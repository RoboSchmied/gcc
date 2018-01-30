/* -*- C++ -*- modules.  Experimental!
   Copyright (C) 2017-2018 Free Software Foundation, Inc.
   Written by Nathan Sidwell <nathan@acm.org> while at FaceBook

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* Comments in this file have a non-negligible chance of being wrong
   or at least inaccurate.  Due to (a) my misunderstanding, (b)
   ambiguities that I have interpretted differently to original intent
   (c) changes in the specification, (d) my poor wording.  */

/* (Incomplete) Design Notes

   Each namespace-scope decl has a MODULE_INDEX and a MODULE_PURVIEW_P
   flag.  The symbols for a particular module are held located in a
   sparse array hanging off the ns-level binding.  Both global module
   and module-specific are on the same slot.  The current TU is slot
   0.  Imports have non-zero indices, and any indirect import will
   have a smaller index than any module importing it (direct imports
   of course have a higher index than zero).  This scheme has the nice
   property that builtins and the global module get the expected
   MODULE_INDEX of zero and MODULE_PURVIEW_P of false, without needing
   special handling.

   I have not yet decided how to represent the decls for the same
   global-module entity appearing in two different modules.  Two
   distinct decls may get expensive, and we'd certainly need to know
   about duplicates in the case of an inline fn or identical class
   decl. (There is still C++ discussion about exacly how much global
   module state needs dumping.)  I'm thinking every global module decl
   gets a HIDDEN decl in the current TU -- unless it too declares it.
   Such decls can have different default args in different modules,
   but that is likely to be rare.

   A module interface compilation produces a BMI, which is essentially
   a tree serialization using auto-numbered back references.  We can
   generate this in a single pass, walking the namespace graph.
   Inter-module references are by name.

   There is no lazy loading.  This will probably be needed later.  For
   lazy loading, importing a module would populate the binding array
   with markers.  When name-lookup encounters such a marker, it would
   lazily load the entities of that name from that module.  This means
   breaking the tree serialization.

   The MODULE_PURVIEW_P is an explicit flag in the decl.  We could do
   it implicitly by location, as all global module entities must come
   before all module-specific entities.  */

/* MODULE_STAMP is a #define passed in from the Makefile.  When
   present, it is used for version stamping the binary files, and
   indicates experimentalness of the module system.  It is very
   experimental right now.  */
#ifndef MODULE_STAMP
#error "Stahp! What are you doing? This is not ready yet."
#endif

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "cp-tree.h"
#include "stringpool.h"
#include "dumpfile.h"
#include "bitmap.h"
#include "cgraph.h"
#include "tree-iterator.h"
#include "cpplib.h"
#include "incpath.h"
#include "libiberty.h"
#include "tree-diagnostic.h"

/* Id for dumping module information.  */
int module_dump_id;

/* We have a few more special module indices.  */
#define MODULE_INDEX_IMPORTING (~0U)  /* Currently being imported.  */
#define MODULE_INDEX_ERROR (~0U - 1)  /* Previously failed to import.  */

/* Mangling for module files.  */
#define MOD_FNAME_SFX ".nms" /* New Module System.  Honest.  */
#define MOD_FNAME_DOT '-' /* Dots convert to ... */

/* Prefix for section names.  */
#define MOD_SNAME_PFX ".gnu.c++"

static int
get_version ()
{
  /* If the on-disk format changes, update the version number.  */
  int version = 20180101;

#if defined (MODULE_STAMP)
  /* MODULE_STAMP is a decimal encoding YYYYMMDDhhmm or YYYYMMDD in
     local timezone.  Using __TIME__ doesn't work very well with
     boostrapping!  */
  version = -(MODULE_STAMP > 2000LL * 10000 * 10000
	      ? int (MODULE_STAMP - 2000LL * 10000 * 10000)
	      : int (MODULE_STAMP - 2000LL * 10000) * 10000);
#endif
  return version;
}

/* Version to date. */
static unsigned version2date (int v)
{
  if (MODULE_STAMP && v < 0)
    return -v / 10000 + 20000000;
  else
    return v;
}

/* Version to time. */
static unsigned version2time (int v)
{
  if (MODULE_STAMP && v < 0)
    return -v % 10000;
  else
    return 0;
}

typedef char version_string[32];
static void version2string (unsigned version, version_string &out)
{
  unsigned date = version2date (version);
  unsigned time = version2time (version);
  sprintf (out, MODULE_STAMP ? "%04u/%02u/%02u-%02u:%02u" : "%04u/%02u/%02u",
	   date / 10000, (date / 100) % 100, (date % 100),
	   time / 100, time % 100);
}

/* Map from pointer to unsigned and vice-versa.  Entries are
   non-deletable.  */
struct non_null_hash : pointer_hash <void> {
  static bool is_deleted (value_type) {return false;}
  static void remove (value_type) {}
};
typedef simple_hashmap_traits<non_null_hash, unsigned> ptr_uint_traits;
typedef simple_hashmap_traits<int_hash<unsigned,0>,void *> uint_ptr_traits;

typedef hash_map<void *,unsigned,ptr_uint_traits> ptr_uint_hash_map;
typedef hash_map<unsigned,void *,uint_ptr_traits> uint_ptr_hash_map;

/* A data buffer, using trailing array hack.  */

struct data
{
  size_t size;
  char buffer[1];

private:
  unsigned calc_crc () const;

public:
  void set_crc (unsigned *crc_ptr);
  bool check_crc (unsigned *crc_ptr) const;

public:
  /* new & delete semantics don't quite work.  */
  static data *extend (data *, size_t);
  static data *release (data *d)
  {
    free (d);
    return NULL;
  }
};

unsigned
data::calc_crc () const
{
  unsigned crc = 0;
  for (size_t ix = 4; ix < size; ix++)
    crc = crc32_byte (crc, buffer[ix]);
  return crc;
}

void
data::set_crc (unsigned *crc_ptr)
{
  if (crc_ptr)
    {
      gcc_checking_assert (size >= 4);
      unsigned crc = calc_crc ();
      *(unsigned *)buffer = crc;
      *crc_ptr = crc32_unsigned (*crc_ptr, crc);
    }
}

bool
data::check_crc (unsigned *crc_p) const
{
  if (crc_p)
    {
      if (size < 4)
	return false;

      unsigned c_crc = calc_crc ();
      if (c_crc != *(unsigned const *)buffer)
	return false;
      *crc_p = crc32_unsigned (*crc_p, c_crc);
    }

  return true;
}

data *data::extend (data *c, size_t a)
{
  if (!c || a > c->size)
    {
      c = XRESIZEVAR (data, c, offsetof (data, buffer) + a);
      c->size = a;
    }

  return c;
}

/* Encapsulated Lazy Records Of Named Declarations.
   Header: Stunningly Elf32_Ehdr-like
   Sections: Sectional data
     1     .README   : human readable, stunningly STRTAB-like
     2     .imports  : import table
     [3-N) .decls    : decls bound to a name
     N     .bindings : bindings of namespace names
     N+1   .ident    : config data
     N+2    .strtab  : strings, stunningly STRTAB-like
   Index: Section table, stunningly ELF32_Shdr-like.
 */

class elf
{
protected:
  enum private_constants
    {
      /* File kind. */
      ET_NONE = 0,
      EM_NONE = 0,
      OSABI_NONE = 0,

      /* File format. */
      EV_CURRENT = 1,
      CLASS32 = 1,
      DATA2LSB = 1,
      DATA2MSB = 2,

      /* Section numbering.  */
      SHN_UNDEF = 0,
      SHN_LORESERVE = 0xff00,
      SHN_XINDEX = 0xffff,
      SHN_HIRESERVE = 0xffff,

      /* Symbol types.  */
      STT_NOTYPE = 0,
      STB_GLOBAL = 1,

      /* I really hope we do not get BMI files larger than 4GB.  */
      MY_CLASS = CLASS32,
      /* It is host endianness that is relevant.  */
#ifdef WORDS_BIGENDIAN
      MY_ENDIAN = DATA2MSB,
#else
      MY_ENDIAN = DATA2LSB,
#endif
    };

public:
  enum public_constants
    {
      /* Section types.  */
      SHT_NONE = 0,
      SHT_PROGBITS = 1,
      SHT_STRTAB = 3,

      /* Section flags.  */
      SHF_NONE = 0x00,
      SHF_ALLOC = 0x02,
      SHF_STRINGS = 0x20,
    };

protected:
  struct ident
  /* On-disk representation.  */
  {
    uint8_t magic[4];
    uint8_t klass; /* 4:CLASS32 */
    uint8_t data; /* 5:DATA2[LM]SB */
    uint8_t version; /* 6:EV_CURRENT  */
    uint8_t osabi; /* 7:OSABI_NONE */
    uint8_t abiver; /* 8: 0 */
    uint8_t pad[7]; /* 9-15 */
  };
  struct header
  /* On-disk representation.  */
  {
    struct ident ident;
    uint16_t type; /* ET_NONE */
    uint16_t machine; /* EM_NONE */
    uint32_t version; /* EV_CURRENT */
    uint32_t entry; /* 0 */
    uint32_t phoff; /* 0 */
    uint32_t shoff; /* TBD */
    uint32_t flags; 
    uint16_t ehsize; /* sizeof (header) */
    uint16_t phentsize; /* 0 */
    uint16_t phnum;    /* 0 */
    uint16_t shentsize; /* sizeof (section) */
    uint16_t shnum;  /* TBD */
    uint16_t shstrndx; /* TBD */
  };
  struct section
  /* On-disk representation.  */
  {
    uint32_t name; /* String table offset.  */
    uint32_t type; /* TBD */
    uint32_t flags;
    uint32_t addr; /* 0 */
    uint32_t off;  /* TBD */
    uint32_t size; /* TBD */
    uint32_t link; /* TBD */
    uint32_t info;
    uint32_t addralign; /* 0 */
    uint32_t entsize; /* 0, except SHT_STRTAB */
  };
  struct symbol
  /* On-disk representation.  */
  {
    uint32_t name;
    uint32_t offset;
    uint32_t size;
    unsigned char info;
    unsigned char other;
    uint16_t shndx;
  };

protected:
  struct isection
  /* Not the on-disk representation. */
  {
    unsigned short type;   /* Type of section.  */
    unsigned short flags;  /* Section flags.  */
    unsigned name;   /* Index into string section.  */
    unsigned offset; /* File offset.  */
    unsigned size;   /* Size of data.  */
  };

protected:
  FILE *stream;

protected:
  int err;
  vec<isection, va_heap, vl_embed> *sections;

public:
  elf (FILE *stream)
    :stream (stream), err (0), sections (NULL)
  {}
  ~elf ()
  {
    vec_free (sections);
  }

public:
  int get_error () const
  {
    return err;
  }
  void set_error (int e = -1)
  {
    if (!err)
      err = e;
  }
};

/* ELF reader.  */

class elf_in : public elf
{
protected:
  data *strings;

  public:
  elf_in (FILE *s)
    :elf (s), strings (NULL)
  {
  }

protected:
  bool read (void *, size_t);

public:
  data *read (unsigned snum);
  data *find (unsigned type, const char *name);
  void release ()
  {
    strings = data::release (strings);
  }

public:
  bool begin ();
  int end ()
  {
    return get_error ();
  }

public:
  const char *name (unsigned offset)
  {
    return &strings->buffer[offset < strings->size ? offset : 0];
  }
};

/* Elf writer.  */

class elf_out : public elf
{
public:
  /* Builder for string table.  */
  class strtab
  {
    ptr_uint_hash_map ident_map;	/* Map of IDENTIFIERS to offsets. */
    vec<tree, va_gc> *idents;		/* Ordered vector.  */
    vec<const char *, va_gc> *literals; /* Ordered vector.  */
    unsigned size;			/* Next offset.  */

  public:
    strtab (unsigned size = 50)
      :ident_map (size), idents (NULL), literals (NULL), size (0)
    {
      vec_safe_reserve (idents, size);
      vec_safe_reserve (literals, 10);
      name ("");
    }
    ~strtab ()
    {
      vec_free (idents);
      vec_free (literals);
    }

  public:
    unsigned name (const_tree ident);
    unsigned name (const char *literal);
    unsigned write (elf_out *out);
  };

private:
  strtab strings;

public:
  elf_out (FILE *s)
    :elf (s), strings (500)
  {
  }

protected:
  uint32_t pad ();
  unsigned add (unsigned type, unsigned name = 0,
		unsigned off = 0, unsigned size = 0, unsigned flags = SHF_NONE);
  bool write (const void *, size_t);

public:
  unsigned name (const_tree ident)
  {
    return strings.name (ident);
  }
  unsigned name (const char *n)
  {
    return strings.name (n);
  }

public:
  /* Add a section.  */
  unsigned add (unsigned type, unsigned name, const data *,
		unsigned flags = SHF_NONE);

public:
  bool begin ();
  int end ();
};

bool
elf_in::read (void *buffer, size_t size)
{
  if (fread (buffer, 1, size, stream) != size)
    {
      set_error (errno);
      return false;
    }
  return true;
}

data *
elf_in::read (unsigned snum)
{
  if (snum >= sections->length ())
    return NULL;
  const isection *sec = &(*sections)[snum];
  if (fseek (stream, sec->offset, SEEK_SET))
    {
      set_error (errno);
      return NULL;
    }

  data *b = data::extend (NULL, sec->size);
  if (read (b->buffer, b->size))
    return b;
  b = data::release (b);
  return NULL;
}

data *
elf_in::find (unsigned type, const char *n)
{
  unsigned snum = sections->length ();
  while (--snum)
    {
      const isection *isec = &(*sections)[snum];

      if (isec->type == type && !strcmp (n, name (isec->name)))
	return read (snum);
    }

  return NULL;
}

bool
elf_in::begin ()
{
  gcc_checking_assert (!sections);

  header header;
  if (fseek (stream, 0, SEEK_SET)
      || !read (&header, sizeof (header)))
    return false;
  if (header.ident.magic[0] != 0x7f
      || header.ident.magic[1] != 'E'
      || header.ident.magic[2] != 'L'
      || header.ident.magic[3] != 'F')
    {
      error ("not Encapsulated Ledger File");
      return false;
    }

  /* We expect a particular format -- the ELF is not intended to be
     distributable.  */
  if (header.ident.klass != MY_CLASS
      || header.ident.data != MY_ENDIAN
      || header.ident.version != EV_CURRENT)
    {
      error ("unexpected encapsulation format");
      return false;
    }

  /* And the versioning isn't target-specific.  */
  if (header.type != ET_NONE
      || header.machine != EM_NONE
      || header.ident.osabi != OSABI_NONE)
    {
      error ("unexpected encapsulation type");
      return false;
    }

  if (!header.shoff || !header.shnum
      || header.shentsize != sizeof (section))
    {
      error ("section table missing or wrong format");
      return false;
    }
  if (fseek (stream, header.shoff, SEEK_SET))
    {
    section_table_fail:
      set_error (errno);
      error ("cannot read section table");
      return false;
    }

  unsigned strndx = header.shstrndx;
  unsigned shnum = header.shnum;
  vec_alloc (sections, shnum);
  for (unsigned ix = 0; ix != shnum; ix++)
    {
      section section;
      if (fread (&section, 1, sizeof (section), stream) != sizeof (section))
	goto section_table_fail;
      isection isection;
      isection.type = section.type;
      isection.name = section.name;
      isection.offset = section.off;
      isection.size = section.size;
      if (!ix)
	{
	  if (strndx == SHN_XINDEX)
	    strndx = section.link;
	  if (shnum == SHN_XINDEX)
	    {
	      shnum = isection.size;
	      isection.size = 0;
	      if (!shnum)
		goto section_table_fail;
	    }
	  vec_safe_reserve (sections, shnum, true);
	}
      sections->quick_push (isection);
    }

  if (strndx)
    {
      strings = read (strndx);
      /* The string table should be at least one byte, with NUL chars
	 at either end.  */
      if (strings && !(strings->size && !strings->buffer[0]
		       && !strings->buffer[strings->size - 1]))
	strings = data::release (strings);
    }

  if (!strings)
    {
      /* Create a default string table.  */
      strings = data::extend (NULL, 1);
      strings->buffer[0] = 0;
    }

  return true;
}

unsigned
elf_out::strtab::name (const_tree ident)
{
  unsigned result;
  bool existed;
  unsigned *slot
    = &ident_map.get_or_insert (const_cast <tree> (ident), &existed);
  if (existed)
    result = *slot;
  else
    {
      *slot = size;
      vec_safe_push (idents, const_cast <tree> (ident));
      result = size;
      size += IDENTIFIER_LENGTH (ident) + 1;
    }
  return result;
}

unsigned
elf_out::strtab::name (const char *literal)
{
  vec_safe_push (idents, NULL_TREE);
  vec_safe_push (literals, literal);
  unsigned result = size;
  size += strlen (literal) + 1;
  return result;
}

unsigned
elf_out::strtab::write (elf_out *elf)
{
  unsigned off = elf->pad ();
  unsigned shname = name (".strtab");
  unsigned lit_ix = 0;
  for (unsigned ix = 0; ix != idents->length (); ix++)
    {
      unsigned len;
      const char *ptr;

      if (const_tree ident = (*idents)[ix])
	{
	  len = IDENTIFIER_LENGTH (ident);
	  ptr = IDENTIFIER_POINTER (ident);
	}
      else
	{
	  ptr = (*literals)[lit_ix++];
	  len = strlen (ptr);
	}
      if (!elf->write (ptr, len + 1))
	return 0;
    }

  gcc_assert (lit_ix == literals->length ());
  return elf->add (SHT_STRTAB, shname, off, size, SHF_STRINGS);
}

unsigned
elf_out::add (unsigned type, unsigned name, unsigned off, unsigned size,
	      unsigned flags)
{
  isection sec;

  sec.type = type;
  sec.flags = flags;
  sec.name = name;
  sec.offset = off;
  sec.size = size;

  unsigned snum = sections->length ();
  vec_safe_push (sections, sec);
  return snum;
}

bool
elf_out::write (const void *data, size_t size)
{
  if (fwrite (data, 1, size, stream) == size)
    return true;
  set_error (errno);
  return false;
}

unsigned
elf_out::add (unsigned type, unsigned name, const data *data, unsigned flags)
{
  uint32_t off = pad ();

  if (!off || fwrite (data->buffer, 1, data->size, stream) != data->size)
    {
      set_error (errno);
      return 0;
    }

  return add (type, name, off, data->size, flags);
}

bool
elf_out::begin ()
{
  gcc_checking_assert (!sections);
  vec_alloc (sections, 10);

  /* Create the UNDEF section.  */
  add (SHT_NONE);

  /* Write an empty header.  */
  header header;
  memset (&header, 0, sizeof (header));
  return write (&header, sizeof (header));
}

uint32_t elf_out::pad ()
{
  long off = ftell (stream);
  if (off < 0)
    return 0;

  if (unsigned padding = off & 3)
    {
      /* Align the section on disk, should help the necessary copies.  */
      unsigned zero = 0;
      padding = 4 - padding;
      off += padding;
      if (fwrite (&zero, 1, padding, stream) != padding)
	return 0;
    }

  return (uint32_t)off;
}

int
elf_out::end ()
{
  /* Write the string table.  */
  unsigned strndx = strings.write (this);

  uint32_t shoff = pad ();
  unsigned shnum = sections->length ();

  /* Write section table */
  for (unsigned ix = 0; ix != sections->length (); ix++)
    {
      const isection *isec = &(*sections)[ix];
      section section;
      memset (&section, 0, sizeof (section));
      section.name = isec->name;
      section.type = isec->type;
      section.off = isec->offset;
      section.size = isec->size;
      section.flags = isec->flags;
      section.entsize = 0;
      if (isec->flags & SHF_STRINGS)
	section.entsize = 1;

      if (!ix)
	{
	  if (strndx >= SHN_LORESERVE)
	    section.link = strndx;
	  if (shnum >= SHN_LORESERVE)
	    section.size = shnum;
	}

      if (!write (&section, sizeof (section)))
	goto out;
    }

  /* Write header.  */
  if (fseek (stream, 0, SEEK_SET))
    {
      set_error (errno);
      goto out;
    }

  header header;
  memset (&header, 0, sizeof (header));
  header.ident.magic[0] = 0x7f;
  header.ident.magic[1] = 'E';
  header.ident.magic[2] = 'L';
  header.ident.magic[3] = 'F';
  header.ident.klass = MY_CLASS;
  header.ident.data =  MY_ENDIAN;
  header.ident.version = EV_CURRENT;
  header.ident.osabi = OSABI_NONE;
  header.type = ET_NONE;
  header.machine = EM_NONE;
  header.version = EV_CURRENT;
  header.shoff = shoff;
  header.ehsize = sizeof (header);
  header.shentsize = sizeof (section);
  header.shnum = shnum >= SHN_LORESERVE ? unsigned (SHN_XINDEX) : shnum;
  header.shstrndx = strndx >= SHN_LORESERVE ? unsigned (SHN_XINDEX) : strndx;

  write (&header, sizeof (header));

 out:
  return get_error ();
}

/* State of a particular module. */
struct GTY(()) module_state {
  /* We always import & export ourselves.  */
  bitmap imports;	/* Transitive modules we're importing.  */
  bitmap exports;	/* Subset of that, that we're exporting.  */

  tree name;		/* Name of the module.  */
  vec<tree, va_gc> *name_parts;  /* Split parts of name.  */

  unsigned mod;		/* Module index.  */
  unsigned crc;		/* CRC we saw reading it in. */

  const char *filename;	/* Filename */
  location_t loc;	/* Its location.  */

  vec<unsigned, va_gc> *remap; /* module no remapping.  */

  bool imported : 1;	/* Imported via import declaration.  */
  bool exported : 1;	/* The import is exported.  */

 public:
  module_state ();
  ~module_state ();
  void release (bool = true);

 public:
  void set_index (unsigned index);
  void set_name (tree name);
  void push_location (const char *filename);
  void pop_location () const;
  void do_import (unsigned index, bool is_export);

 public:
  void dump (FILE *, bool);
};

/* Hash module state by name.  */

struct module_state_hash : ggc_remove <module_state *>
{
  typedef module_state *value_type;
  typedef tree compare_type; /* An identifier.  */

  static hashval_t hash (const value_type m)
  {
    return IDENTIFIER_HASH_VALUE (m->name);
  }
  static bool equal (const value_type existing, compare_type candidate)
  {
    return existing->name == candidate;
  }

  static inline void mark_empty (value_type &p) {p = NULL;}
  static inline bool is_empty (value_type p) {return !p;}

  /* Nothing is deletable.  Everything is insertable.  */
  static bool is_deleted (value_type) { return false; }
  static void mark_deleted (value_type) { gcc_unreachable (); }
};

/* Vector of module state.  If this is non-null, index 0 is
   occupied.  */
static GTY(()) vec<module_state *, va_gc> *modules;

/* Map from identifier to module index. */
static GTY(()) hash_table<module_state_hash> *module_hash;

/* Module search path.  */
static cpp_dir *module_path;

/* Longest module path.  */
static size_t module_path_max;

module_state::module_state ()
  : imports (BITMAP_GGC_ALLOC ()), exports (BITMAP_GGC_ALLOC ()),
    name (NULL_TREE), name_parts (NULL),
    mod (0), crc (0),
    filename (NULL), loc (UNKNOWN_LOCATION),
    remap (NULL)
{
  imported = exported = false;
}

module_state::~module_state ()
{
  release ();
}

/* Free up state.  If ALL is true, we're completely done.  If ALL is
   false, we've completed reading in the module (but have not
   completed parsing).  */

void
module_state::release (bool all)
{
  if (all)
    {
      imports = NULL;
      exports = NULL;
    }

  vec_free (remap);
}

/* We've been assigned INDEX.  Mark the self-import-export bits.  */

void
module_state::set_index (unsigned index)
{
  gcc_checking_assert (mod == ~0u);
  bitmap_set_bit (imports, index);
  bitmap_set_bit (exports, index);
}

/* Set NAME and PARTS fields from incoming NAME.  We glued all the
   identifiers together when parsing, and now we split them up
   again.  */
// FIXME  Perhaps a TREE_VEC of identifiers would be better? Oh well.
void
module_state::set_name (tree name_)
{
  name = name_;

  size_t len = IDENTIFIER_LENGTH (name);
  const char *ptr = IDENTIFIER_POINTER (name);
  char *buffer = NULL;

  const char *dot;
  do
    {
      dot = (const char *)memchr (ptr, '.', len);
      size_t l = dot ? dot - ptr : len;

      gcc_assert (l);

      size_t id_l = l;
      const char *id_p = ptr;

      vec_safe_reserve (name_parts,
			vec_safe_length (name_parts) + 1,
			!name_parts && !dot);
      name_parts->quick_push (get_identifier_with_length (id_p, id_l));
      if (dot)
	l++;
      ptr += l;
      len -= l;
    }
  while (dot);

  XDELETEVEC (buffer);
}

void
module_state::push_location (const char *name)
{
  // FIXME:We want LC_MODULE_ENTER really.
  filename = name;
  linemap_add (line_table, LC_ENTER, false, filename, 0);
  loc = linemap_line_start (line_table, 0, 0);
  input_location = loc;
}

void
module_state::pop_location () const
{
  linemap_add (line_table, LC_LEAVE, false, NULL, 0);
}

/* Return the IDENTIFIER_NODE naming module IX.  This is the name
   including dots.  */

tree
module_name (unsigned ix)
{
  return (*modules)[ix]->name;
}

/* Return the vector of IDENTIFIER_NODES naming module IX.  These are
   individual identifers per sub-module component.  */

vec<tree, va_gc> *
module_name_parts (unsigned ix)
{
  return (*modules)[ix]->name_parts;
}

/* Return the bitmap describing what modules are imported into
   MODULE.  Remember, we always import ourselves.  */

bitmap
module_import_bitmap (unsigned ix)
{
  const module_state *state = (*modules)[ix];

  return state ? state->imports : NULL;
}

/* Return the context that controls what module DECL is in.  That is
   the outermost non-namespace context.  */

tree
module_context (tree decl)
{
  for (;;)
    {
      tree outer = CP_DECL_CONTEXT (decl);
      if (TYPE_P (outer))
	{
	  if (tree name = TYPE_NAME (outer))
	    outer = name;
	  else
	    return NULL_TREE;
	}
      if (TREE_CODE (outer) == NAMESPACE_DECL)
	break;
      decl = outer;
    }
  return decl;
}

/* We've just directly imported INDEX.  Update our import/export
   bitmaps.  IS_EXPORT is true if we're reexporting the module.  */

void
module_state::do_import (unsigned index, bool is_export)
{
  module_state *other = (*modules)[index];

  if (this == (*modules)[0])
    {
      other->imported = true;
      other->exported = is_export;
    }
  bitmap_ior_into (imports, other->exports);
  if (is_export)
    bitmap_ior_into (exports, other->exports);
}

/* Byte serializer base.  */
class bytes {
protected:
  struct data *data;	/* Buffer being read/written.  */
  size_t pos;		/* Position in buffer.  */
  uint32_t bit_val;	/* Bit buffer.  */
  unsigned bit_pos;	/* Next bit in bit buffer.  */

public:
  bytes ()
    :data (NULL), pos (0), bit_val (0), bit_pos (0)
  {}
  ~bytes () 
  {
    gcc_checking_assert (!data);
  }

protected:
  void begin (bool crc)
  {
    gcc_checking_assert (!data);
    pos = crc ? 4 : 0;
  }
public:
  void end ()
  {
    data = data::release (data);
    pos = 0;
  }

protected:
  /* Finish bit packet.  Rewind the bytes not used.  */
  unsigned bit_flush ()
  {
    gcc_assert (bit_pos);
    unsigned bytes = (bit_pos + 7) / 8;
    unuse (4 - bytes);
    bit_pos = 0;
    bit_val = 0;
    return bytes;
  }

protected:
  char *use (unsigned bytes)
  {
    char *res = &data->buffer[pos];
    pos += bytes;
    return res;
  }
  void unuse (unsigned bytes)
  {
    pos -= bytes;
  }
};

class cpm_stream;

/* Byte stream writer.  */
class bytes_out : public bytes {
  /* Bit instrumentation.  */
  unsigned spans[3];
  unsigned lengths[3];
  int is_set;

public:
  bytes_out ()
    : bytes ()
  {
    spans[0] = spans[1] = spans[2] = 0;
    lengths[0] = lengths[1] = lengths[2] = 0;
    is_set = -1;
  }
  ~bytes_out ()
  {
  }

private:
  char *use (unsigned);

public:
  void begin (bool crc_p = false);
  unsigned end (elf_out *, unsigned, unsigned *crc_ptr = NULL, bool = false);

public:
  void raw (unsigned);

public:
  void instrument (cpm_stream *d);

public:
  void b (bool);
  void bflush ();

public:
  void c (unsigned char);
  void i (int);
  void u (unsigned);
  void s (size_t s);
  void wi (HOST_WIDE_INT);
  void wu (unsigned HOST_WIDE_INT);
  void str (const char *, size_t);
  void buf (const char *, size_t);
  void printf (const char *, ...) ATTRIBUTE_PRINTF_2;
};

/* Byte stream reader.  */
class bytes_in : public bytes {
protected:
  bool overrun;

public:
  bytes_in ()
    : bytes (), overrun (false)
  {
  }
  ~bytes_in ()
  {
  }

public:
  bool begin (elf_in *src, const char *name, unsigned *crc_p = NULL);
  bool end (elf_in *src)
  {
    if (overrun)
      src->set_error ();
    bytes::end ();
    return !overrun;
  }
  bool more_p () const
  {
    return pos != data->size;
  }

private:
  const char *use (unsigned bytes, unsigned *avail = NULL)
  {
    unsigned space = data->size - pos;
    if (space < bytes)
      {
	bytes = space;
	if (!avail)
	  {
	    overrun = true;
	    return NULL;
	  }
      }
    if (avail)
      *avail = bytes;
    return bytes::use (bytes);
  }

public:
  bool get_overrun () const
  {
    return overrun;
  }
  void set_overrun ()
  {
    overrun = true;
  }

public:
  unsigned raw ();

public:
  bool b ();
  void bflush ();
private:
  void bfill ();

public:
  int c ();
  int i ();
  unsigned u ();
  size_t s ();
  HOST_WIDE_INT wi ();
  unsigned HOST_WIDE_INT wu ();
  const char *str (size_t * = NULL);
  const char *buf (size_t);
};

/* Finish a set of bools.  */

void
bytes_out::bflush ()
{
  if (bit_pos)
    {
      raw (bit_val);
      lengths[2] += bit_flush ();
    }
  spans[2]++;
  is_set = -1;
}

void
bytes_in::bflush ()
{
  if (bit_pos)
    bit_flush ();
}

/* When reading, we don't know how many bools we'll read in.  So read
   4 bytes-worth, and then rewind when flushing if we didn't need them
   all.  */

void
bytes_in::bfill ()
{
  bit_val = raw ();
}

/* Low level bytes_ins and bytes_outs.  I did think about making these
   templatized, but that started to look error prone, so went with
   type-specific names.
   b - bools,
   i, u - ints/unsigned
   wi/wu - wide ints/unsigned
   s - size_t
   buf - fixed size buffer
   str - variable length string  */

/* Bools are packed into bytes.  You cannot mix bools and non-bools.
   You must call bflush before emitting another type.  So batch your
   bools.

   It may be worth optimizing for most bools being zero.  some kind of
   run-length encoding?  */

void
bytes_out::b (bool x)
{
  if (is_set != x)
    {
      is_set = x;
      spans[x]++;
    }
  lengths[x]++;
  bit_val |= unsigned (x) << bit_pos++;
  if (bit_pos == 32)
    {
      raw (bit_val);
      lengths[2] += bit_flush ();
    }
}

bool
bytes_in::b ()
{
  if (!bit_pos)
    bfill ();
  bool v = (bit_val >> bit_pos++) & 1;
  if (bit_pos == 32)
    bit_flush ();
  return v;
}

/* Exactly 4 bytes.  Used internally for bool packing and crc
   transfer -- hence no crc here.  */

void
bytes_out::raw (unsigned val)
{
  char *ptr = use (4);
  ptr[0] = val;
  ptr[1] = val >> 8;
  ptr[2] = val >> 16;
  ptr[3] = val >> 24;
}

unsigned
bytes_in::raw ()
{
  unsigned val = 0;
  if (const char *ptr = use (4))
    {
      val |= (unsigned char)ptr[0];
      val |= (unsigned char)ptr[1] << 8;
      val |= (unsigned char)ptr[2] << 16;
      val |= (unsigned char)ptr[3] << 24;
    }

  return val;
}

/* Chars are unsigned and written as single bytes. */

void
bytes_out::c (unsigned char v)
{
  *use (1) = v;
}

int
bytes_in::c ()
{
  int v = 0;
  if (const char *ptr = use (1))
    v = (unsigned char)ptr[0];
  return v;
}

/* Ints are written as sleb128.  */

void
bytes_out::i (int v)
{
  unsigned max = (sizeof (v) * 8 + 6) / 7; 
  char *ptr = use (max);
  unsigned count = 0;

  int end = v < 0 ? -1 : 0;
  bool more;
  do
    {
      unsigned byte = v & 127;
      v >>= 6; /* Signed shift.  */
      more = v != end;
      ptr[count++] = byte | (more << 7);
      v >>= 1; /* Signed shift.  */
    }
  while (more);
  unuse (max - count);
}

int
bytes_in::i ()
{
  int v = 0;
  unsigned max = (sizeof (v) * 8 + 6) / 7;
  const char *ptr = use (max, &max);
  unsigned count = 0;

  unsigned bit = 0;
  unsigned byte;
  do
    {
      if (count == max)
	{
	  overrun = true;
	  return 0;
	}
      byte = ptr[count++];
      v |= (byte & 127) << bit;
      bit += 7;
    }
  while (byte & 128);
  unuse (max - count);

  if (byte & 0x40 && bit < sizeof (v) * 8)
    v |= ~(unsigned)0 << bit;

  return v;
}

/* Unsigned are written as uleb128.  */

void
bytes_out::u (unsigned v)
{
  unsigned max = (sizeof (v) * 8 + 6) / 7;
  char *ptr = use (max);
  unsigned count = 0;

  bool more;
  do
    {
      unsigned byte = v & 127;
      v >>= 7;
      more = v != 0;
      ptr[count++] = byte | (more << 7);
    }
  while (more);
  unuse (max - count);
}

unsigned
bytes_in::u ()
{
  unsigned v = 0;
  unsigned max = (sizeof (v) * 8 + 6) / 7;
  const char *ptr = use (max, &max);
  unsigned count = 0;

  unsigned bit = 0;
  unsigned byte;
  do
    {
      if (count == max)
	{
	  overrun = true;
	  return 0;
	}
      byte = ptr[count++];
      v |= (byte & 127) << bit;
      bit += 7;
    }
  while (byte & 128);
  unuse (max - count);

  return v;
}

void
bytes_out::wi (HOST_WIDE_INT v)
{
  unsigned max = (sizeof (v) * 8 + 6) / 7; 
  char *ptr = use (max);
  unsigned count = 0;

  int end = v < 0 ? -1 : 0;
  bool more;
  do
    {
      unsigned byte = v & 127;
      v >>= 6; /* Signed shift.  */
      more = v != end;
      ptr[count++] = byte | (more << 7);
      v >>= 1; /* Signed shift.  */
    }
  while (more);
  unuse (max - count);
}

HOST_WIDE_INT
bytes_in::wi ()
{
  HOST_WIDE_INT v = 0;
  unsigned max = (sizeof (v) * 8 + 6) / 7;
  const char *ptr = use (max, &max);
  unsigned count = 0;

  unsigned bit = 0;
  unsigned byte;
  do
    {
      if (count == max)
	{
	  overrun = true;
	  return 0;
	}
      byte = ptr[count++];
      v |= (unsigned HOST_WIDE_INT)(byte & 127) << bit;
      bit += 7;
    }
  while (byte & 128);
  unuse (max - count);

  if (byte & 0x40 && bit < sizeof (v) * 8)
    v |= ~(unsigned HOST_WIDE_INT)0 << bit;
  return v;
}

inline void
bytes_out::wu (unsigned HOST_WIDE_INT v)
{
  wi ((HOST_WIDE_INT) v);
}

inline unsigned HOST_WIDE_INT
bytes_in::wu ()
{
  return (unsigned HOST_WIDE_INT) wi ();
}

inline void
bytes_out::s (size_t s)
{
  if (sizeof (s) == sizeof (unsigned))
    u (s);
  else
    wu (s);
}

inline size_t
bytes_in::s ()
{
  if (sizeof (size_t) == sizeof (unsigned))
    return u ();
  else
    return wu ();
}

void
bytes_out::buf (const char *buf, size_t len)
{
  memcpy (use (len), buf, len);
}

const char *
bytes_in::buf (size_t len)
{
  const char *ptr = use (len);

  return ptr;
}

/* Strings:
   u:length
   buf:bytes
*/

void
bytes_out::str (const char *string, size_t len)
{
  s (len);
  buf (string, len + 1);
}

const char *
bytes_in::str (size_t *len_p)
{
  size_t len = s ();

  /* We're about to trust some user data.  */
  if (overrun)
    len = 0;
  *len_p = len;
  const char *str = buf (len + 1);
  if (str[len])
    {
      overrun = true;
      str = "";
    }
  return str;
}

void
bytes_out::printf (const char *format, ...)
{
  va_list args;
  size_t len = 500;

 again:
  va_start (args, format);
  char *ptr = use (len);
  size_t actual = vsnprintf (ptr, len, format, args);
  va_end (args);
  if (actual > len)
    {
      unuse (len);
      len = actual;
      goto again;
    }
  unuse (len - actual);
}

bool
bytes_in::begin (elf_in *source, const char *name, unsigned *crc_p)
{
  bytes::begin (crc_p);

  data = source->find (elf::SHT_PROGBITS, name);

  if (!data || !data->check_crc (crc_p))
    {
      data = data::release (data);
      set_overrun ();
      error ("section %qs is missing or corrupted", name);
      return false;
    }
  return true;
}

void
bytes_out::begin (bool crc_p)
{
  bytes::begin (crc_p);
  data = data::extend (0, 200);
}

unsigned
bytes_out::end (elf_out *sink, unsigned name, unsigned *crc_ptr, bool string_p)
{
  data->size = pos;
  data->set_crc (crc_ptr);
  unsigned sec_num = sink->add (string_p ? elf::SHT_STRTAB : elf::SHT_PROGBITS,
				name, data,
				string_p ? elf::SHF_STRINGS : elf::SHF_NONE);
  bytes::end ();

  return sec_num;
}

char *
bytes_out::use (unsigned bytes)
{
  if (data->size < pos + bytes)
    data = data::extend (data, (pos + bytes) * 3/2);
  return bytes::use (bytes);
}

/* Module cpm_stream base.  */
class cpm_stream {
public:
  /* Record tags.  */
  enum record_tag
  {
    /* Module-specific records.  */
    rt_binding,		/* A name-binding.  */
    rt_definition,	/* A definition. */
    rt_identifier,	/* An identifier node.  */
    rt_conv_identifier,	/* A conversion operator name.  */
    rt_type_name,	/* A type name.  */
    rt_typeinfo_var,	/* A typeinfo object.  */
    rt_typeinfo_pseudo, /* A typeinfo pseudo type.  */
    rt_tree_base = 0x100,	/* Tree codes.  */
    rt_ref_base = 0x1000	/* Back-reference indices.  */
  };
  struct gtp {
    const tree *ptr;
    unsigned num;
  };

public:
  static const gtp globals_arys[];
  static vec<tree, va_gc> *globals;
  static unsigned globals_crc;

public:
  module_state *state;

private:
  unsigned tag;

private:
  FILE *d;
  unsigned depth;
  unsigned nesting;
  bool bol;

public:
  cpm_stream (module_state *, cpm_stream *chain = NULL);
  ~cpm_stream ();

protected:
  /* Allocate a new reference index.  */
  unsigned next (unsigned count = 1)
  {
    unsigned res = tag;
    tag += count;
    return res;
  }

public:
  FILE *dumps () const
  {
    return d;
  }
  bool dump () const 
  {
    return d != NULL;
  }
  bool dump (const char *, ...);
  void nest ()
  {
    nesting++;
  }
  void unnest ()
  {
    nesting--;
  }
};

/* Global trees.  */
const cpm_stream::gtp cpm_stream::globals_arys[] =
  {
    {sizetype_tab, stk_type_kind_last},
    {integer_types, itk_none},
    {global_trees, TI_MAX},
    {cp_global_trees, CPTI_MAX},
    {NULL, 0}
  };
vec<tree, va_gc> GTY(()) *cpm_stream::globals;
unsigned cpm_stream::globals_crc;

cpm_stream::cpm_stream (module_state *state_, cpm_stream *chain)
  : state (state_), tag (rt_ref_base), d (chain ? chain->d : NULL),
    depth (chain ? chain->depth + 1 : 0), nesting (0), bol (true)
{
  gcc_assert (MAX_TREE_CODES <= rt_ref_base - rt_tree_base);
  if (!chain)
    d = dump_begin (module_dump_id, NULL);

  if (!globals)
    {
      /* Construct the global tree array.  This is an array of unique
	 global trees (& types).  */
      // FIXME:Some slots are lazily allocated, we must move them to
      // the end and not stream them here.  They must be locatable via
      // some other means.
      unsigned crc = 0;
      hash_table<nofree_ptr_hash<tree_node> > hash (200);
      vec_alloc (globals, 200);

      dump () && dump ("+Creating globals");
      /* Insert the TRANSLATION_UNIT_DECL.  */
      *hash.find_slot (DECL_CONTEXT (global_namespace), INSERT)
	= DECL_CONTEXT (global_namespace);
      globals->quick_push (DECL_CONTEXT (global_namespace));

      for (unsigned jx = 0; globals_arys[jx].ptr; jx++)
	{
	  const tree *ptr = globals_arys[jx].ptr;
	  unsigned limit = globals_arys[jx].num;
	  
	  for (unsigned ix = 0; ix != limit; ix++, ptr++)
	    {
	      dump () && !(ix & 31) && dump ("") && dump ("+\t%u:%u:", jx,ix);
	      unsigned v = 0;
	      if (tree val = *ptr)
		{
		  tree *slot = hash.find_slot (val, INSERT);
		  if (!*slot)
		    {
		      *slot = val;
		      crc = crc32_unsigned (crc, globals->length ());
		      vec_safe_push (globals, val);
		      v++;
		      if (CODE_CONTAINS_STRUCT (TREE_CODE (val), TS_TYPED))
			{
			  val = TREE_TYPE (val);
			  if (val)
			    {
			      slot = hash.find_slot (val, INSERT);
			      if (!*slot)
				{
				  *slot = val;
				  crc = crc32_unsigned (crc, globals->length ());
				  vec_safe_push (globals, val);
				  v++;
				}
			    }
			}
		    }
		}
	      dump () && dump ("+%u", v);
	    }
	}
      gcc_checking_assert (hash.elements () == globals->length ());
      globals_crc = crc32_unsigned (crc, globals->length ());
      dump () && dump ("") &&dump ("Created %u unique globals, crc=%x",
				   globals->length (), globals_crc);
    }
}

cpm_stream::~cpm_stream ()
{
  if (!depth && d)
    dump_end (module_dump_id, d);
}

static bool
dump_nested_name (tree t, FILE *d)
{
  if (t && TYPE_P (t))
    t = TYPE_NAME (t);

  if (t && DECL_P (t))
    {
      if (t == global_namespace)
	;
      else if (tree ctx = DECL_CONTEXT (t))
	if (TREE_CODE (ctx) == TRANSLATION_UNIT_DECL
	    || dump_nested_name (ctx, d))
	  fputs ("::", d);
      t = DECL_NAME (t);
    }

  if (t)
    switch (TREE_CODE (t))
      {
      case IDENTIFIER_NODE:
	fwrite (IDENTIFIER_POINTER (t), 1, IDENTIFIER_LENGTH (t), d);
	return true;

      case STRING_CST:
	fwrite (TREE_STRING_POINTER (t), 1, TREE_STRING_LENGTH (t) - 1, d);
	return true;

      case INTEGER_CST:
	print_hex (wi::to_wide (t), d);
	return true;

      default:
	break;
      }

  return false;
}

/* Specialized printfy thing.  */

bool
cpm_stream::dump (const char *format, ...)
{
  va_list args;
  bool no_nl = format[0] == '+';
  format += no_nl;

  if (bol)
    {
      if (depth)
	{
	  /* Module import indenting.  */
	  const char *indent = ">>>>";
	  const char *dots   = ">...>";
	  if (depth > strlen (indent))
	    indent = dots;
	  else
	    indent += strlen (indent) - depth;
	  fputs (indent, d);
	}
      if (nesting)
	{
	  /* Tree indenting.  */
	  const char *indent = "      ";
	  const char *dots  =  "   ... ";
	  if (nesting > strlen (indent))
	    indent = dots;
	  else
	    indent += strlen (indent) - nesting;
	  fputs (indent, d);
	}
      bol = false;
    }

  va_start (args, format);
  while (const char *esc = strchr (format, '%'))
    {
      fwrite (format, 1, (size_t)(esc - format), d);
      format = ++esc;
      switch (*format++)
	{
	case 'C': /* Code */
	  {
	    tree_code code = (tree_code)va_arg (args, unsigned);
	    fputs (get_tree_code_name (code), d);
	    break;
	  }
	case 'I': /* Identifier.  */
	  {
	    tree t = va_arg (args, tree);
	    dump_nested_name (t, d);
	    break;
	  }
	case 'N': /* Name.  */
	  {
	    tree t = va_arg (args, tree);
	    fputc ('\'', d);
	    dump_nested_name (t, d);
	    fputc ('\'', d);
	    break;
	  }
	case 'M': /* Mangled name */
	  {
	    tree t = va_arg (args, tree);
	    if (t && TYPE_P (t))
	      t = TYPE_NAME (t);
	    if (t && HAS_DECL_ASSEMBLER_NAME_P (t)
		&& DECL_ASSEMBLER_NAME_SET_P (t))
	      {
		fputc ('(', d);
		fputs (IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (t)), d);
		fputc (')', d);
	      }
	    break;
	  }
	case 'P': /* Pair.  */
	  {
	    tree ctx = va_arg (args, tree);
	    tree name = va_arg (args, tree);
	    fputc ('\'', d);
	    dump_nested_name (ctx, d);
	    if (ctx && ctx != global_namespace)
	      fputs ("::", d);
	    dump_nested_name (name, d);
	    fputc ('\'', d);
	    break;
	  }
	case 'R': /* Ratio */
	  {
	    unsigned a = va_arg (args, unsigned);
	    unsigned b = va_arg (args, unsigned);
	    fprintf (d, "%.1f", (float) a / (b + !b));
	    break;
	  }
	case 'U': /* long unsigned.  */
	  {
	    unsigned long u = va_arg (args, unsigned long);
	    fprintf (d, "%lu", u);
	    break;
	  }
	case 'V': /* Verson.  */
	  {
	    unsigned v = va_arg (args, unsigned);
	    version_string string;

	    version2string (v, string);
	    fputs (string, d);
	    break;
	  }
	case 'p': /* Pointer. */
	  {
	    void *p = va_arg (args, void *);
	    fprintf (d, "%p", p);
	    break;
	  }
	case 's': /* String. */
	  {
	    const char *s = va_arg (args, char *);
	    fputs (s, d);
	    break;
	  }
	case 'u': /* Unsigned.  */
	  {
	    unsigned u = va_arg (args, unsigned);
	    fprintf (d, "%u", u);
	    break;
	  }
	case 'x': /* Hex. */
	  {
	    unsigned x = va_arg (args, unsigned);
	    fprintf (d, "%x", x);
	    break;
	  }
	default:
	  gcc_unreachable ();
	}
    }
  fputs (format, d);
  va_end (args);
  if (!no_nl)
    {
      bol = true;
      fputc ('\n', d);
    }
  return true;
}

/* cpm_stream cpms_out.  */
class cpms_out : public cpm_stream {
public: // FIXME
  elf_out elf;
  bytes_out w;

private:
  ptr_uint_hash_map tree_map; /* trees to ids  */

  /* Tree instrumentation. */
  unsigned unique;
  unsigned refs;
  unsigned nulls;
  unsigned records;

public:
  cpms_out (FILE *, module_state *);
  ~cpms_out ();

public:
  bool begin ();
  bool end ();
  void write ();

private:
  void instrument ();
  void header (unsigned crc);
  void imports (unsigned *crc_p);

public:
  void tag_binding (tree ns, tree name, module_binding_vec *);
  void maybe_tag_definition (tree decl);
  void tag_definition (tree node, tree maybe_template);

private:
  void tag (record_tag rt)
  {
    records++;
    w.u (rt);
  }
  unsigned insert (tree);
  void start (tree_code, tree);
  void loc (location_t);
  void core_bools (tree);
  void core_vals (tree);
  void lang_type_bools (tree);
  void lang_type_vals (tree);
  void lang_decl_bools (tree);
  void lang_decl_vals (tree);
  void tree_node_raw (tree_code, tree);
  int tree_node_special (tree);
  void chained_decls (tree);
  void tree_vec (vec<tree, va_gc> *);
  void tree_pair_vec (vec<tree_pair_s, va_gc> *);
  void define_function (tree, tree);
  void define_var (tree, tree);
  void define_class (tree, tree);
  void define_enum (tree, tree);

public:
  void tree_node (tree);
  vec<tree, va_gc> *bindings (bytes_out *, vec<tree, va_gc> *nest, tree ns);
};

cpms_out::cpms_out (FILE *s, module_state *state)
  :cpm_stream (state), elf (s), w ()
{
  unique = refs = nulls = 0;
  records = 0;
  dump () && dump ("Writing module %I", state->name);
}

cpms_out::~cpms_out ()
{
}

// FIXME collate
void
bytes_out::instrument (cpm_stream *d)
{
  if (data)
    d->dump ("Wrote %U bytes", data->size);
  d->dump ("Wrote %u bits in %u bytes", lengths[0] + lengths[1],
	   lengths[2]);
  for (unsigned ix = 0; ix < 2; ix++)
    d->dump ("  %u %s spans of %R bits", spans[ix],
	     ix ? "one" : "zero", lengths[ix], spans[ix]);
  d->dump ("  %u blocks with %R bits padding", spans[2],
	   lengths[2] * 8 - (lengths[0] + lengths[1]), spans[2]);
}

void
cpms_out::instrument ()
{
  if (dump ())
    {
      dump ("");
      w.instrument (this);
      dump ("Wrote %u trees", unique + refs + nulls);
      dump ("  %u unique", unique);
      dump ("  %u references", refs);
      dump ("  %u nulls", nulls);
      dump ("Wrote %u records", records);
    }
}

/* Cpm_Stream in.  */
class cpms_in : public cpm_stream {
  elf_in elf;
  bytes_in r;

private:
  uint_ptr_hash_map tree_map; /* ids to trees  */

public:
  cpms_in (FILE *, module_state *, cpms_in *);
  ~cpms_in ();

public:
  bool begin ();
  unsigned read (unsigned *crc_ptr);
  bool end ();

private:
  bool header (unsigned *crc_ptr);
  bool imports (unsigned *crc_ptr);

public:
  bool tag_binding ();
  tree tag_definition ();

private:
  unsigned insert (tree);
  tree finish_type (tree);

private:
  tree start (tree_code);
  tree finish (tree);
  location_t loc ();
  bool core_bools (tree);
  bool core_vals (tree);
  bool lang_type_bools (tree);
  bool lang_type_vals (tree);
  bool lang_decl_bools (tree);
  bool lang_decl_vals (tree);
  bool tree_node_raw (tree_code, tree, tree, tree);
  tree tree_node_special (unsigned);
  tree chained_decls ();
  vec<tree, va_gc> *tree_vec ();
  vec<tree_pair_s, va_gc> *tree_pair_vec ();
  tree define_function (tree, tree);
  tree define_var (tree, tree);
  tree define_class (tree, tree);
  tree define_enum (tree, tree);

public:
  tree tree_node ();
};

cpms_in::cpms_in (FILE *s, module_state *state, cpms_in *from)
  :cpm_stream (state, from), elf (s), r ()
{
  dump () && dump ("Importing %I", state->name);
}

cpms_in::~cpms_in ()
{
}

unsigned
cpms_out::insert (tree t)
{
  unsigned tag = next ();
  bool existed = tree_map.put (t, tag);
  gcc_assert (!existed);
  return tag;
}

unsigned
cpms_in::insert (tree t)
{
  unsigned tag = next ();
  bool existed = tree_map.put (tag, t);
  gcc_assert (!existed);
  return tag;
}

static unsigned do_module_import (location_t, tree, bool module_unit_p,
				  bool import_export_p,
				  cpms_in * = NULL, unsigned * = NULL);

/* Header section.  This determines if the current compilation is
   compatible with the serialized module.  I.e. the contents are just
   confirming things we already know (or failing to match).

   raw:version
   raw:crc
   u:module-name
   u:<target-triplet>
   u:<host-triplet>
   u:globals_length
   raw:globals_crc
   // FIXME CPU,ABI and similar tags
   // FIXME Optimization and similar tags
*/

void
cpms_out::header (unsigned inner_crc)
{
  w.begin (true);

  /* Write version and inner crc as raw values, for easier
     debug inspection.  */
  dump () && dump ("Writing version=%u, inner_crc=%x",
		   get_version (), inner_crc);
  w.raw (unsigned (get_version ()));
  w.raw (inner_crc);

  w.u (elf.name (state->name));

  /* Configuration. */
  dump () && dump ("Writing target='%s', host='%s'",
		   TARGET_MACHINE, HOST_MACHINE);
  unsigned target = elf.name (TARGET_MACHINE);
  unsigned host = (!strcmp (TARGET_MACHINE, HOST_MACHINE)
		   ? target : elf.name (HOST_MACHINE));
  w.u (target);
  w.u (host);

  /* Global tree information.  We write the globals crc separately,
     rather than mix it directly into the overall crc, as it is used
     to ensure data match between instances of the compiler, not
     integrity of the file.  */
  dump () && dump ("Writing globals=%u, crc=%x",
		   globals->length (), globals_crc);
  w.u (globals->length ());
  w.raw (globals_crc);

  /* Now generate CRC, we'll have incorporated the inner CRC because
     of its serialization above.  */
  unsigned crc = 0;
  w.end (&elf, elf.name (MOD_SNAME_PFX ".ident"), &crc);
  dump () && dump ("Writing CRC=%x", crc);
  state->crc = crc;
}

// FIXME: What we really want to do at this point is read the header
// and then allow hooking out to rebuild it if there are
// checksum/version mismatches.  Other errors should barf.

bool
cpms_in::header (unsigned *crc_ptr)
{
  unsigned crc = 0;
  if (!r.begin (&elf, MOD_SNAME_PFX ".ident", &crc))
    return false;

  dump () && dump ("Reading CRC=%x", crc);
  if (crc_ptr && crc != *crc_ptr)
    {
      error ("module %qE CRC mismatch", state->name);
    fail:
      r.set_overrun ();
      return r.end (&elf);
    }
  state->crc = crc;

  /* Check version.  */
  int my_ver = get_version ();
  int their_ver = int (r.raw ());
  dump () && dump  (my_ver == their_ver ? "Version %V"
		    : "Expecting %V found %V", my_ver, their_ver);
  if (their_ver != my_ver)
    {
      int my_date = version2date (my_ver);
      int their_date = version2date (their_ver);
      int my_time = version2time (my_ver);
      int their_time = version2time (their_ver);
      version_string my_string, their_string;

      version2string (my_ver, my_string);
      version2string (their_ver, their_string);

      if (my_date != their_date)
	{
	  /* Dates differ, decline.  */
	  error ("file is version %s, this is version %s",
		 their_string, my_string);
	  goto fail;
	}
      else if (my_time != their_time)
	/* Times differ, give it a go.  */
	warning (0, "file is version %s, compiler is version %s,"
		 " perhaps close enough?", their_string, my_string);
    }

  /* Read and ignore the inner crc.  We only wrote it to mix it into
     the crc.  */
  r.raw ();

  /* Check module name.  */
  const char *their_name = elf.name (r.u ());
  if (strlen (their_name) != IDENTIFIER_LENGTH (state->name)
      || memcmp (their_name, IDENTIFIER_POINTER (state->name),
		 IDENTIFIER_LENGTH (state->name)))
    {
      error ("module %qs found, expected module %qE", their_name, state->name);
      goto fail;
    }

  /* Check target & host.  */
  const char *their_target = elf.name (r.u ());
  const char *their_host = elf.name (r.u ());
  dump () && dump ("Read target='%s', host='%s'", their_target, their_host);
  if (strcmp (their_target, TARGET_MACHINE)
      || strcmp (their_host, HOST_MACHINE))
    {
      error ("target & host is %qs:%qs, expected %qs:%qs",
	     their_target, TARGET_MACHINE, their_host, HOST_MACHINE);
      goto fail;
    }

  /* Check global trees.  */
  unsigned their_glength = r.u ();
  unsigned their_gcrc = r.raw ();
  dump () && dump ("Read globals=%u, crc=%x", their_glength, their_gcrc);
  if (their_glength != globals->length ()
      || their_gcrc != globals_crc)
    {
      error ("global tree mismatch");
      goto fail;
    }

  if (r.more_p ())
    goto fail;

  return r.end (&elf);
}

/* Imports
   {
     b:imported
     b:exported
     u:index
     u:crc
     u:name
   } imports[N]

   We need the complete set, so that we can build up the remapping
   vector on subsequent import.  */

// FIXME: Should we stream the pathname to the import?

void
cpms_out::imports (unsigned *crc_p)
{
  w.begin (true);

  w.u (modules->length ());
  for (unsigned ix = MODULE_INDEX_IMPORT_BASE; ix < modules->length (); ix++)
    {
      module_state *state = (*modules)[ix];
      dump () && dump ("Writing %simport %I (crc=%x)",
		       state->exported ? "export " :
		       state->imported ? "" : "indirect ",
		       state->name, state->crc);
      w.b (state->imported);
      w.b (state->exported);
      w.bflush ();
      w.u (state->crc);
      unsigned name = elf.name (state->name);
      w.u (name);
    }
  w.end (&elf, elf.name (MOD_SNAME_PFX ".imports"), crc_p);
}

bool
cpms_in::imports (unsigned *crc_p)
{
  if (!r.begin (&elf, MOD_SNAME_PFX ".imports", crc_p))
    return false;

  unsigned imports = r.u ();
  gcc_assert (!state->remap);
  state->remap = NULL;
  vec_safe_reserve (state->remap, imports);

  for (unsigned ix = MODULE_INDEX_IMPORT_BASE; ix--;)
    state->remap->quick_push (0);
  for (unsigned ix = MODULE_INDEX_IMPORT_BASE; ix < imports; ix++)
    {
      bool imported = r.b ();
      bool exported = r.b ();
      r.bflush ();
      unsigned crc = r.u ();
      tree name = get_identifier (elf.name (r.u ()));

      dump () && dump ("Begin nested %simport %I",
		       exported ? "export " : imported ? "" : "indirect ", name);
      unsigned new_ix = do_module_import (UNKNOWN_LOCATION, name,
					  /*unit_p=*/false,
					  /*import_p=*/imported, this, &crc);
      if (new_ix == MODULE_INDEX_ERROR)
	goto fail;
      state->remap->quick_push (new_ix);
      dump () && dump ("Completed nested import %I #%u", name, new_ix);
      if (imported)
	{
	  dump () && dump ("Direct %simport %I %u",
			   exported ? "export " : "", name, new_ix);
	  state->do_import (new_ix, exported);
	}
    }

  if (r.more_p ())
    {
    fail:
      r.set_overrun ();
    }

  return r.end (&elf);
}

/* BINDING is a vector of decls bound in namespace NS.  Write out the
   binding and definitions of things in the binding list.
   NS must have already have a binding somewhere.

   tree:ns
   tree:name
   tree:binding*
   tree:NULL
*/

void
cpms_out::tag_binding (tree ns, tree name, module_binding_vec *binding)
{
  dump () && dump ("Writing %N bindings for %N", ns, name);
  tag (rt_binding);
  tree_node (ns);
  tree_node (name);

  for (unsigned ix = binding->length (); ix--;)
    {
      tree decl = (*binding)[ix];

      if (DECL_IS_BUILTIN (decl))
	// FIXME: write the builtin
	continue;

      if (TREE_CODE (decl) == CONST_DECL)
	{
	  gcc_assert (TREE_CODE (CP_DECL_CONTEXT (decl)) == ENUMERAL_TYPE);
	  continue;
	}

      tree_node (decl);
    }
  tree_node (NULL_TREE);

  // FIXME:Write during binding?
  while (binding->length ())
    {
      tree decl = binding->pop ();
      if (!DECL_IS_BUILTIN (decl) && TREE_CODE (decl) != CONST_DECL)
	maybe_tag_definition (decl);
    }
}

bool
cpms_in::tag_binding ()
{
  tree ns = tree_node ();
  tree name = tree_node ();
  tree type = NULL_TREE;
  tree decls = NULL_TREE;

  dump () && dump ("Reading %N binding for %N", ns, name);

  while (tree decl = tree_node ())
    {
      if (TREE_CODE (decl) == TYPE_DECL)
	{
	  if (type)
	    r.set_overrun ();
	  type = decl;
	}
      else if (decls
	       || (TREE_CODE (decl) == TEMPLATE_DECL
		   && TREE_CODE (DECL_TEMPLATE_RESULT (decl)) == FUNCTION_DECL))
	{
	  if (!DECL_DECLARES_FUNCTION_P (decl)
	      || (decls
		  && TREE_CODE (decls) != OVERLOAD
		  && TREE_CODE (decls) != FUNCTION_DECL))
	    r.set_overrun ();
	  decls = ovl_make (decl, decls);
	  if (DECL_MODULE_EXPORT_P (decl))
	    OVL_EXPORT_P (decls) = true;
	}
      else
	decls = decl;
    }

  if (r.get_overrun ())
    return false;

  if (!decls && !type)
    return true;

  return push_module_binding (ns, name, state->mod, decls, type);
}

/* Stream a function definition.  */

void
cpms_out::define_function (tree decl, tree)
{
  tree_node (DECL_RESULT (decl));
  tree_node (DECL_INITIAL (decl));
  tree_node (DECL_SAVED_TREE (decl));
  if (DECL_DECLARED_CONSTEXPR_P (decl))
    tree_node (find_constexpr_fundef (decl));
}

tree
cpms_in::define_function (tree decl, tree maybe_template)
{
  tree result = tree_node ();
  tree initial = tree_node ();
  tree saved = tree_node ();
  tree constexpr_body = (DECL_DECLARED_CONSTEXPR_P (decl)
			 ? tree_node () : NULL_TREE);

  if (r.get_overrun ())
    return NULL_TREE;

  if (TREE_CODE (CP_DECL_CONTEXT (maybe_template)) == NAMESPACE_DECL)
    {
      unsigned mod = MAYBE_DECL_MODULE_INDEX (maybe_template);
      if (mod != state->mod)
	{
	  error ("unexpected definition of %q#D", decl);
	  r.set_overrun ();
	  return NULL_TREE;
	}
      if (!MAYBE_DECL_MODULE_PURVIEW_P (maybe_template)
	  && DECL_SAVED_TREE (decl))
	return decl; // FIXME check same
    }

  DECL_RESULT (decl) = result;
  DECL_INITIAL (decl) = initial;
  DECL_SAVED_TREE (decl) = saved;
  if (constexpr_body)
    register_constexpr_fundef (decl, constexpr_body);

  current_function_decl = decl;
  allocate_struct_function (decl, false);
  cfun->language = ggc_cleared_alloc<language_function> ();
  cfun->language->base.x_stmt_tree.stmts_are_full_exprs_p = 1;
  set_cfun (NULL);
  current_function_decl = NULL_TREE;

  if (!DECL_TEMPLATE_INFO (decl) || DECL_USE_TEMPLATE (decl))
    {
      comdat_linkage (decl);
      note_vague_linkage_fn (decl);
      cgraph_node::finalize_function (decl, false);
    }

  return decl;
}

/* Stream a variable definition.  */

void
cpms_out::define_var (tree decl, tree)
{
  tree_node (DECL_INITIAL (decl));
}

tree
cpms_in::define_var (tree decl, tree)
{
  tree init = tree_node ();

  if (r.get_overrun ())
    return NULL;

  DECL_INITIAL (decl) = init;

  return decl;
}

/* A chained set of decls.  */

void
cpms_out::chained_decls (tree decls)
{
  for (; decls; decls = DECL_CHAIN (decls))
    tree_node (decls);
  tree_node (NULL_TREE);
}

tree
cpms_in::chained_decls ()
{
  tree decls = NULL_TREE;
  for (tree *chain = &decls; chain && !r.get_overrun ();)
    if (tree decl = tree_node ())
      {
	if (!DECL_P (decl))
	  r.set_overrun ();
	else
	  {
	    gcc_assert (!DECL_CHAIN (decl));
	    *chain = decl;
	    chain = &DECL_CHAIN (decl);
	  }
      }
    else
      chain = NULL;
  return decls;
}

/* A vector of trees.  */

void
cpms_out::tree_vec (vec<tree, va_gc> *v)
{
  unsigned len = vec_safe_length (v);
  w.u (len);
  if (len)
    for (unsigned ix = 0; ix != len; ix++)
      tree_node ((*v)[ix]);
}

vec<tree, va_gc> *
cpms_in::tree_vec ()
{
  vec<tree, va_gc> *v = NULL;
  if (unsigned len = r.u ())
    {
      vec_alloc (v, len);
      for (unsigned ix = 0; ix != len; ix++)
	v->quick_push (tree_node ());
    }
  return v;
}

/* A vector of tree pairs.  */

void
cpms_out::tree_pair_vec (vec<tree_pair_s, va_gc> *v)
{
  unsigned len = vec_safe_length (v);
  w.u (len);
  if (len)
    for (unsigned ix = 0; ix != len; ix++)
      {
	tree_pair_s const &s = (*v)[ix];
	tree_node (s.purpose);
	tree_node (s.value);
      }
}

vec<tree_pair_s, va_gc> *
cpms_in::tree_pair_vec ()
{
  vec<tree_pair_s, va_gc> *v = NULL;
  if (unsigned len = r.u ())
    {
      vec_alloc (v, len);
      for (unsigned ix = 0; ix != len; ix++)
	{
	  tree_pair_s s;
	  s.purpose = tree_node ();
	  s.value = tree_node ();
	  v->quick_push (s);
      }
    }
  return v;
}

/* Stream a class definition.  */

void
cpms_out::define_class (tree type, tree maybe_template)
{
  gcc_assert (TYPE_MAIN_VARIANT (type) == type);

  chained_decls (TYPE_FIELDS (type));
  tree_node (TYPE_VFIELD (type));
  tree_node (TYPE_BINFO (type));
  if (TYPE_LANG_SPECIFIC (type))
    {
      tree_node (CLASSTYPE_PRIMARY_BINFO (type));
      tree_vec (CLASSTYPE_VBASECLASSES (type));
      tree as_base = CLASSTYPE_AS_BASE (type);
      if (as_base && as_base != type)
	tag_definition (CLASSTYPE_AS_BASE (type), NULL_TREE);
      else
	tree_node (as_base);
      tree_vec (CLASSTYPE_MEMBER_VEC (type));
      tree_node (CLASSTYPE_FRIEND_CLASSES (type));
      tree_node (CLASSTYPE_LAMBDA_EXPR (type));

      if (TYPE_CONTAINS_VPTR_P (type))
	{
	  tree_vec (CLASSTYPE_PURE_VIRTUALS (type));
	  tree_pair_vec (CLASSTYPE_VCALL_INDICES (type));
	  tree_node (CLASSTYPE_KEY_METHOD (type));
	}

      tree vtables = CLASSTYPE_VTABLES (type);
      chained_decls (vtables);
      /* Write the vtable initializers.  */
      for (; vtables; vtables = TREE_CHAIN (vtables))
	tree_node (DECL_INITIAL (vtables));
    }

  if (TREE_CODE (maybe_template) == TEMPLATE_DECL)
    tree_node (CLASSTYPE_DECL_LIST (type));

  // lang->nested_udts

  /* Now define all the members.  */
  for (tree member = TYPE_FIELDS (type); member; member = TREE_CHAIN (member))
    maybe_tag_definition (member);

  /* End of definitions.  */
  tree_node (NULL_TREE);
}

/* Nop sorted needed for resorting the member vec.  */

static void
nop (void *, void *)
{
}

tree
cpms_in::define_class (tree type, tree maybe_template)
{
  gcc_assert (TYPE_MAIN_VARIANT (type) == type);

  tree fields = chained_decls ();
  tree vfield = tree_node ();
  tree binfo = tree_node ();
  vec<tree, va_gc> *member_vec = NULL;
  tree primary = NULL_TREE;
  tree as_base = NULL_TREE;
  vec<tree, va_gc> *vbases = NULL;
  vec<tree, va_gc> *pure_virts = NULL;
  vec<tree_pair_s, va_gc> *vcall_indices = NULL;
  tree key_method = NULL_TREE;
  tree vtables = NULL_TREE;
  tree lambda = NULL_TREE;
  tree friends = NULL_TREE;

  if (TYPE_LANG_SPECIFIC (type))
    {
      primary = tree_node ();
      vbases = tree_vec ();
      as_base = tree_node ();
      member_vec = tree_vec ();
      friends = tree_node ();
      lambda = tree_node ();

      /* TYPE_VBASECLASSES is not set yet, so TYPE_CONTAINS_VPTR will
	 malfunction.  */
      if (TYPE_POLYMORPHIC_P (type) || vbases)
	{
	  pure_virts = tree_vec ();
	  vcall_indices = tree_pair_vec ();
	  key_method = tree_node ();
	}
      vtables = chained_decls ();
    }

  tree decl_list = NULL_TREE;
  if (TREE_CODE (maybe_template) == TEMPLATE_DECL)
    decl_list = tree_node ();

  // lang->nested_udts

  // FIXME: Sanity check stuff

  if (r.get_overrun ())
    return NULL_TREE;

  TYPE_FIELDS (type) = fields;
  TYPE_VFIELD (type) = vfield;
  TYPE_BINFO (type) = binfo;

  if (TYPE_LANG_SPECIFIC (type))
    {
      CLASSTYPE_PRIMARY_BINFO (type) = primary;
      CLASSTYPE_VBASECLASSES (type) = vbases;
      CLASSTYPE_AS_BASE (type) = as_base;

      CLASSTYPE_FRIEND_CLASSES (type) = friends;
      CLASSTYPE_LAMBDA_EXPR (type) = lambda;

      CLASSTYPE_MEMBER_VEC (type) = member_vec;
      CLASSTYPE_PURE_VIRTUALS (type) = pure_virts;
      CLASSTYPE_VCALL_INDICES (type) = vcall_indices;

      CLASSTYPE_KEY_METHOD (type) = key_method;
      if (!key_method && TYPE_CONTAINS_VPTR_P (type))
	vec_safe_push (keyed_classes, type);

      CLASSTYPE_VTABLES (type) = vtables;
      /* Read the vtable initializers.  */
      for (; vtables; vtables = TREE_CHAIN (vtables))
	DECL_INITIAL (vtables) = tree_node ();

      CLASSTYPE_DECL_LIST (type) = decl_list;

      /* Resort the member vector.  */
      resort_type_member_vec (member_vec, NULL, nop, NULL);
    }

  /* Propagate to all variants.  */
  fixup_type_variants (type);

  /* Now define all the members.  */
  while (tree_node ())
    if (r.get_overrun ())
      break;

  return type;
}

/* Stream an enum definition.  */

void
cpms_out::define_enum (tree type, tree)
{
  gcc_assert (TYPE_MAIN_VARIANT (type) == type);

  tree_node (TYPE_VALUES (type));
  tree_node (TYPE_MIN_VALUE (type));
  tree_node (TYPE_MAX_VALUE (type));
}

tree
cpms_in::define_enum (tree type, tree)
{
  gcc_assert (TYPE_MAIN_VARIANT (type) == type);

  tree values = tree_node ();
  tree min = tree_node ();
  tree max = tree_node ();

  if (r.get_overrun ())
    return NULL_TREE;

  TYPE_VALUES (type) = values;
  TYPE_MIN_VALUE (type) = min;
  TYPE_MAX_VALUE (type) = max;

  if (!ENUM_IS_SCOPED (type))
    {
      /* Inject the members into the containing scope.  */
      tree ctx = CP_DECL_CONTEXT (TYPE_NAME (type));
      unsigned mod_ix = DECL_MODULE_INDEX (TYPE_NAME (type));

      if (TREE_CODE (ctx) == NAMESPACE_DECL)
	for (; values; values = TREE_CHAIN (values))
	  {
	    tree cst = TREE_VALUE (values);

	    // FIXME: mark the CST as exported so lookup will find
	    // it.  It'd probably better for this cst's context to be
	    // the ENUM itself, even though it needs to be in the
	    // containing scope.
	    DECL_MODULE_EXPORT_P (cst) = true;
	    push_module_binding (ctx, DECL_NAME (cst), mod_ix, cst, NULL);
	  }
      else
	insert_late_enum_def_bindings (ctx, type);
    }

  return type;
}

/* Write out DECL's definition, if importers need it.  */

void
cpms_out::maybe_tag_definition (tree t)
{
  tree maybe_template = NULL_TREE;

 again:
  switch (TREE_CODE (t))
    {
    default:
      return;

    case TEMPLATE_DECL:
      maybe_template = t;
      t = DECL_TEMPLATE_RESULT (t);
      goto again;

    case TYPE_DECL:
      if (!DECL_IMPLICIT_TYPEDEF_P (t))
	return;

      if (TREE_CODE (TREE_TYPE (t)) == ENUMERAL_TYPE
	  ? !TYPE_VALUES (TREE_TYPE (t))
	  : !TYPE_FIELDS (TREE_TYPE (t)))
	return;
      break;

    case FUNCTION_DECL:
      if (!DECL_INITIAL (t))
	/* Not defined.  */
	return;
      if (DECL_TEMPLATE_INFO (t))
	{
	  if (DECL_USE_TEMPLATE (t) & 1)
	    return;
	}
      else if (!DECL_DECLARED_INLINE_P (t))
	return;
      break;

    case VAR_DECL:
      if (!DECL_INITIAL (t))
	/* Nothing to define.  */
	return;

      if (!TREE_CONSTANT (t))
	return;
      break;
    }

  tag_definition (t, maybe_template);
}

/* Write out T's definition  */

void
cpms_out::tag_definition (tree t, tree maybe_template)
{
  dump () && dump ("Writing%s definition for %C:%N%M",
		   maybe_template ? " template" : "", TREE_CODE (t), t, t);

  if (!maybe_template)
    maybe_template = t;
  tag (rt_definition);
  tree_node (maybe_template);

 again:
  switch (TREE_CODE (t))
    {
    default:
      // FIXME:Other things
      gcc_unreachable ();

    case FUNCTION_DECL:
      define_function (t, maybe_template);
      break;

    case VAR_DECL:
      define_var (t, maybe_template);
      break;

    case TYPE_DECL:
      if (DECL_IMPLICIT_TYPEDEF_P (t))
	{
	  t = TREE_TYPE (t);
	  goto again;
	}

      // FIXME:Actual typedefs
      gcc_unreachable ();

    case RECORD_TYPE:
    case UNION_TYPE:
      define_class (t, maybe_template);
      break;

    case ENUMERAL_TYPE:
      define_enum (t, maybe_template);
      break;
    }
}

tree
cpms_in::tag_definition ()
{
  tree t = tree_node ();
  dump () && dump ("Reading definition for %C:%N%M", TREE_CODE (t), t, t);

  if (r.get_overrun ())
    return NULL_TREE;

  tree maybe_template = t;
  if (TREE_CODE (t) == TEMPLATE_DECL)
    t = DECL_TEMPLATE_RESULT (t);

 again:
  switch (TREE_CODE (t))
    {
    default:
      // FIXME: read other things
      t = NULL_TREE;
      break;

    case FUNCTION_DECL:
      t = define_function (t, maybe_template);
      break;

    case VAR_DECL:
      t = define_var (t, maybe_template);
      break;

    case TYPE_DECL:
      if (DECL_IMPLICIT_TYPEDEF_P (t))
	{
	  t = TREE_TYPE (t);
	  goto again;
	}
      // FIXME:Actual typedefs
      t = NULL_TREE;
      break;

    case RECORD_TYPE:
    case UNION_TYPE:
      t = define_class (t, maybe_template);
      break;

    case ENUMERAL_TYPE:
      t = define_enum (t, maybe_template);
      break;
    }

  return t;
}

/* Read & write locations.  */

void
cpms_out::loc (location_t)
{
  // FIXME:Do something
}

location_t
cpms_in::loc ()
{
  // FIXME:Do something^-1
  return UNKNOWN_LOCATION;
}

/* Start tree write.  Write information to allocate the receiving
   node.  */

void
cpms_out::start (tree_code code, tree t)
{
  switch (code)
    {
    default:
      if (TREE_CODE_CLASS (code) == tcc_vl_exp)
	w.u (VL_EXP_OPERAND_LENGTH (t));
      break;
    case IDENTIFIER_NODE:
      gcc_unreachable ();
      break;
    case TREE_BINFO:
      w.u (BINFO_N_BASE_BINFOS (t));
      break;
    case TREE_VEC:
      w.u (TREE_VEC_LENGTH (t));
      break;
    case STRING_CST:
      w.str (TREE_STRING_POINTER (t), TREE_STRING_LENGTH (t));
      break;
    case VECTOR_CST:
      w.u (VECTOR_CST_LOG2_NPATTERNS (t));
      w.u (VECTOR_CST_NELTS_PER_PATTERN (t));
      break;
    case INTEGER_CST:
      w.u (TREE_INT_CST_NUNITS (t));
      w.u (TREE_INT_CST_EXT_NUNITS (t));
      w.u (TREE_INT_CST_OFFSET_NUNITS (t));
      break;
    case OMP_CLAUSE:
      gcc_unreachable (); // FIXME:
    }
}

/* Start tree read.  Allocate the receiving node.  */

tree
cpms_in::start (tree_code code)
{
  tree t = NULL_TREE;

  // FIXME: should we checksum the numbers we use to allocate with?
  switch (code)
    {
    default:
      if (TREE_CODE_CLASS (code) == tcc_vl_exp)
	{
	  unsigned ops = r.u ();
	  t = build_vl_exp (code, ops);
	}
      else
	t = make_node (code);
      break;
    case IDENTIFIER_NODE:
      gcc_unreachable ();
      break;
    case STRING_CST:
      {
	size_t l;
	const char *str = r.str (&l);
	t = build_string (l, str);
      }
      break;
    case TREE_BINFO:
      t = make_tree_binfo (r.u ());
      break;
    case TREE_VEC:
      t = make_tree_vec (r.u ());
      break;
    case VECTOR_CST:
      t = make_vector (r.u (), r.u ());
      break;
    case INTEGER_CST:
      {
	unsigned n = r.u ();
	unsigned e = r.u ();
	t = make_int_cst (n, e);
	TREE_INT_CST_OFFSET_NUNITS(t) = r.u ();
      }
      break;
    case OMP_CLAUSE:
      gcc_unreachable (); // FIXME:
    }

  return t;
}

/* Semantic processing.  Add to symbol table etc.  Return
   possibly-remapped tree.  */

tree
cpms_in::finish (tree t)
{
  if (TYPE_P (t))
    {
      bool on_pr_list = false;
      if (POINTER_TYPE_P (t))
	{
	  on_pr_list = t->type_non_common.minval != NULL;

	  t->type_non_common.minval = NULL;

	  tree probe = TREE_TYPE (t);
	  for (probe = (TREE_CODE (t) == POINTER_TYPE
			? TYPE_POINTER_TO (probe)
			: TYPE_REFERENCE_TO (probe));
	       probe;
	       probe = (TREE_CODE (t) == POINTER_TYPE
			? TYPE_NEXT_PTR_TO (probe)
			: TYPE_NEXT_REF_TO (probe)))
	    if (TYPE_MODE_RAW (probe) == TYPE_MODE_RAW (t)
		&& (TYPE_REF_CAN_ALIAS_ALL (probe)
		    == TYPE_REF_CAN_ALIAS_ALL (t)))
	      return probe;
	}

      tree remap = finish_type (t);
      if (remap == t && on_pr_list)
	{
	  tree to_type = TREE_TYPE (remap);
	  gcc_assert ((TREE_CODE (remap) == POINTER_TYPE
		       ? TYPE_POINTER_TO (to_type)
		       : TYPE_REFERENCE_TO (to_type)) != remap);
	  if (TREE_CODE (remap) == POINTER_TYPE)
	    {
	      TYPE_NEXT_PTR_TO (remap) = TYPE_POINTER_TO (to_type);
	      TYPE_POINTER_TO (to_type) = remap;
	    }
	  else
	    {
	      TYPE_NEXT_REF_TO (remap) = TYPE_REFERENCE_TO (to_type);
	      TYPE_REFERENCE_TO (to_type) = remap;
	    }
	}
      return remap;
    }

  if (DECL_P (t) && !MAYBE_DECL_MODULE_PURVIEW_P (t))
    {
      // FIXME:Revisit
      tree ctx = CP_DECL_CONTEXT (t);

      if (TREE_CODE (ctx) == NAMESPACE_DECL)
	{
	  /* A global-module decl.  See if there's already a duplicate.  */
	  tree old = merge_global_decl (ctx, state->mod, t);

	  if (!old)
	    error ("failed to merge %#qD", t);
	  else
	    dump () && dump ("%s decl %N%M, (%p)",
			     old == t ? "New" : "Existing",
			     old, old, (void *)old);

	  return old;
	}
    }

  if (TREE_CODE (t) == TEMPLATE_INFO)
    /* We're not a pending template in this TU.  */
    TI_PENDING_TEMPLATE_FLAG (t) = 0;

  if (TREE_CODE (t) == INTEGER_CST)
    {
      // FIXME:Remap small ints
      // FIXME:other consts too
    }

  return t;
}

/* The structure streamers access the raw fields, because the
   alternative, of using the accessor macros can require using
   different accessors for the same underlying field, depending on the
   tree code.  That's both confusing and annoying.  */

/* Read & write the core boolean flags.  */

void
cpms_out::core_bools (tree t)
{
#define WB(X) (w.b (X))
  tree_code code = TREE_CODE (t);

  WB (t->base.side_effects_flag);
  WB (t->base.constant_flag);
  WB (t->base.addressable_flag);
  WB (t->base.volatile_flag);
  WB (t->base.readonly_flag);
  WB (t->base.asm_written_flag);
  WB (t->base.nowarning_flag);
  // visited is zero
  WB (t->base.used_flag); // FIXME: should we be dumping this?
  WB (t->base.nothrow_flag);
  WB (t->base.static_flag);
  WB (t->base.public_flag);
  WB (t->base.private_flag);
  WB (t->base.protected_flag);
  WB (t->base.deprecated_flag);
  WB (t->base.default_def_flag);

  switch (code)
    {
    case TREE_VEC:
    case INTEGER_CST:
    case CALL_EXPR:
    case SSA_NAME:
    case MEM_REF:
    case TARGET_MEM_REF:
      /* These use different base.u fields.  */
      break;

    case BLOCK:
      WB (t->block.abstract_flag);
      /* FALLTHROUGH  */

    default:
      WB (t->base.u.bits.lang_flag_0);
      WB (t->base.u.bits.lang_flag_1);
      WB (t->base.u.bits.lang_flag_2);
      WB (t->base.u.bits.lang_flag_3);
      WB (t->base.u.bits.lang_flag_4);
      WB (t->base.u.bits.lang_flag_5);
      WB (t->base.u.bits.lang_flag_6);
      WB (t->base.u.bits.saturating_flag);
      WB (t->base.u.bits.unsigned_flag);
      WB (t->base.u.bits.packed_flag);
      WB (t->base.u.bits.user_align);
      WB (t->base.u.bits.nameless_flag);
      WB (t->base.u.bits.atomic_flag);
      break;
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_COMMON))
    {
      WB (t->type_common.no_force_blk_flag);
      WB (t->type_common.needs_constructing_flag);
      WB (t->type_common.transparent_aggr_flag);
      WB (t->type_common.restrict_flag);
      WB (t->type_common.string_flag);
      WB (t->type_common.lang_flag_0);
      WB (t->type_common.lang_flag_1);
      WB (t->type_common.lang_flag_2);
      WB (t->type_common.lang_flag_3);
      WB (t->type_common.lang_flag_4);
      WB (t->type_common.lang_flag_5);
      WB (t->type_common.lang_flag_6);
      WB (t->type_common.typeless_storage);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_COMMON))
    {
      WB (t->decl_common.nonlocal_flag);
      WB (t->decl_common.virtual_flag);
      WB (t->decl_common.ignored_flag);
      WB (t->decl_common.abstract_flag);
      WB (t->decl_common.artificial_flag);
      WB (t->decl_common.preserve_flag);
      WB (t->decl_common.debug_expr_is_from);
      WB (t->decl_common.lang_flag_0);
      WB (t->decl_common.lang_flag_1);
      WB (t->decl_common.lang_flag_2);
      WB (t->decl_common.lang_flag_3);
      WB (t->decl_common.lang_flag_4);
      WB (t->decl_common.lang_flag_5);
      WB (t->decl_common.lang_flag_6);
      WB (t->decl_common.lang_flag_7);
      WB (t->decl_common.lang_flag_8);
      WB (t->decl_common.decl_flag_0);
      /* static variables become external.  */
      WB (t->decl_common.decl_flag_1
	  || (code == VAR_DECL && TREE_STATIC (t)
	      && !DECL_WEAK (t) && !DECL_VTABLE_OR_VTT_P (t)));
      WB (t->decl_common.decl_flag_2);
      WB (t->decl_common.decl_flag_3);
      WB (t->decl_common.gimple_reg_flag);
      WB (t->decl_common.decl_by_reference_flag);
      WB (t->decl_common.decl_read_flag);
      WB (t->decl_common.decl_nonshareable_flag);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WITH_VIS))
    {
      WB (t->decl_with_vis.defer_output);
      WB (t->decl_with_vis.hard_register);
      WB (t->decl_with_vis.common_flag);
      WB (t->decl_with_vis.in_text_section);
      WB (t->decl_with_vis.in_constant_pool);
      WB (t->decl_with_vis.dllimport_flag);
      WB (t->decl_with_vis.weak_flag);
      WB (t->decl_with_vis.seen_in_bind_expr);
      WB (t->decl_with_vis.comdat_flag);
      WB (t->decl_with_vis.visibility_specified);
      WB (t->decl_with_vis.comdat_flag);
      WB (t->decl_with_vis.init_priority_p);
      WB (t->decl_with_vis.shadowed_for_var_p);
      WB (t->decl_with_vis.cxx_constructor);
      WB (t->decl_with_vis.cxx_destructor);
      WB (t->decl_with_vis.final);
      WB (t->decl_with_vis.regdecl_flag);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_FUNCTION_DECL))
    {
      WB (t->function_decl.static_ctor_flag);
      WB (t->function_decl.static_dtor_flag);
      WB (t->function_decl.uninlinable);
      WB (t->function_decl.possibly_inlined);
      WB (t->function_decl.novops_flag);
      WB (t->function_decl.returns_twice_flag);
      WB (t->function_decl.malloc_flag);
      WB (t->function_decl.operator_new_flag);
      WB (t->function_decl.declared_inline_flag);
      WB (t->function_decl.no_inline_warning_flag);
      WB (t->function_decl.no_instrument_function_entry_exit);
      WB (t->function_decl.no_limit_stack);
      WB (t->function_decl.disregard_inline_limits);
      WB (t->function_decl.pure_flag);
      WB (t->function_decl.looping_const_or_pure_flag);
      WB (t->function_decl.has_debug_args_flag);
      WB (t->function_decl.tm_clone_flag);
      WB (t->function_decl.versioned_function);
    }
#undef WB
}

bool
cpms_in::core_bools (tree t)
{
#define RB(X) ((X) = r.b ())
  tree_code code = TREE_CODE (t);

  RB (t->base.side_effects_flag);
  RB (t->base.constant_flag);
  RB (t->base.addressable_flag);
  RB (t->base.volatile_flag);
  RB (t->base.readonly_flag);
  RB (t->base.asm_written_flag);
  RB (t->base.nowarning_flag);
  // visited is zero
  RB (t->base.used_flag);
  RB (t->base.nothrow_flag);
  RB (t->base.static_flag);
  RB (t->base.public_flag);
  RB (t->base.private_flag);
  RB (t->base.protected_flag);
  RB (t->base.deprecated_flag);
  RB (t->base.default_def_flag);

  switch (code)
    {
    case TREE_VEC:
    case INTEGER_CST:
    case CALL_EXPR:
    case SSA_NAME:
    case MEM_REF:
    case TARGET_MEM_REF:
      /* These use different base.u fields.  */
      break;

    case BLOCK:
      RB (t->block.abstract_flag);
      /* FALLTHROUGH.  */

    default:
      RB (t->base.u.bits.lang_flag_0);
      RB (t->base.u.bits.lang_flag_1);
      RB (t->base.u.bits.lang_flag_2);
      RB (t->base.u.bits.lang_flag_3);
      RB (t->base.u.bits.lang_flag_4);
      RB (t->base.u.bits.lang_flag_5);
      RB (t->base.u.bits.lang_flag_6);
      RB (t->base.u.bits.saturating_flag);
      RB (t->base.u.bits.unsigned_flag);
      RB (t->base.u.bits.packed_flag);
      RB (t->base.u.bits.user_align);
      RB (t->base.u.bits.nameless_flag);
      RB (t->base.u.bits.atomic_flag);
      break;
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_COMMON))
    {
      RB (t->type_common.no_force_blk_flag);
      RB (t->type_common.needs_constructing_flag);
      RB (t->type_common.transparent_aggr_flag);
      RB (t->type_common.restrict_flag);
      RB (t->type_common.string_flag);
      RB (t->type_common.lang_flag_0);
      RB (t->type_common.lang_flag_1);
      RB (t->type_common.lang_flag_2);
      RB (t->type_common.lang_flag_3);
      RB (t->type_common.lang_flag_4);
      RB (t->type_common.lang_flag_5);
      RB (t->type_common.lang_flag_6);
      RB (t->type_common.typeless_storage);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_COMMON))
    {
      RB (t->decl_common.nonlocal_flag);
      RB (t->decl_common.virtual_flag);
      RB (t->decl_common.ignored_flag);
      RB (t->decl_common.abstract_flag);
      RB (t->decl_common.artificial_flag);
      RB (t->decl_common.preserve_flag);
      RB (t->decl_common.debug_expr_is_from);
      RB (t->decl_common.lang_flag_0);
      RB (t->decl_common.lang_flag_1);
      RB (t->decl_common.lang_flag_2);
      RB (t->decl_common.lang_flag_3);
      RB (t->decl_common.lang_flag_4);
      RB (t->decl_common.lang_flag_5);
      RB (t->decl_common.lang_flag_6);
      RB (t->decl_common.lang_flag_7);
      RB (t->decl_common.lang_flag_8);
      RB (t->decl_common.decl_flag_0);
      RB (t->decl_common.decl_flag_1);
      RB (t->decl_common.decl_flag_2);
      RB (t->decl_common.decl_flag_3);
      RB (t->decl_common.gimple_reg_flag);
      RB (t->decl_common.decl_by_reference_flag);
      RB (t->decl_common.decl_read_flag);
      RB (t->decl_common.decl_nonshareable_flag);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WITH_VIS))
    {
      RB (t->decl_with_vis.defer_output);
      RB (t->decl_with_vis.hard_register);
      RB (t->decl_with_vis.common_flag);
      RB (t->decl_with_vis.in_text_section);
      RB (t->decl_with_vis.in_constant_pool);
      RB (t->decl_with_vis.dllimport_flag);
      RB (t->decl_with_vis.weak_flag);
      RB (t->decl_with_vis.seen_in_bind_expr);
      RB (t->decl_with_vis.comdat_flag);
      RB (t->decl_with_vis.visibility_specified);
      RB (t->decl_with_vis.comdat_flag);
      RB (t->decl_with_vis.init_priority_p);
      RB (t->decl_with_vis.shadowed_for_var_p);
      RB (t->decl_with_vis.cxx_constructor);
      RB (t->decl_with_vis.cxx_destructor);
      RB (t->decl_with_vis.final);
      RB (t->decl_with_vis.regdecl_flag);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_FUNCTION_DECL))
    {
      RB (t->function_decl.static_ctor_flag);
      RB (t->function_decl.static_dtor_flag);
      RB (t->function_decl.uninlinable);
      RB (t->function_decl.possibly_inlined);
      RB (t->function_decl.novops_flag);
      RB (t->function_decl.returns_twice_flag);
      RB (t->function_decl.malloc_flag);
      RB (t->function_decl.operator_new_flag);
      RB (t->function_decl.declared_inline_flag);
      RB (t->function_decl.no_inline_warning_flag);
      RB (t->function_decl.no_instrument_function_entry_exit);
      RB (t->function_decl.no_limit_stack);
      RB (t->function_decl.disregard_inline_limits);
      RB (t->function_decl.pure_flag);
      RB (t->function_decl.looping_const_or_pure_flag);
      RB (t->function_decl.has_debug_args_flag);
      RB (t->function_decl.tm_clone_flag);
      RB (t->function_decl.versioned_function);
    }
#undef RB
  return !r.get_overrun ();
}

void
cpms_out::lang_decl_bools (tree t)
{
#define WB(X) (w.b (X))
  const struct lang_decl *lang = DECL_LANG_SPECIFIC (t);

  WB (lang->u.base.language == lang_cplusplus);
  WB ((lang->u.base.use_template >> 0) & 1);
  WB ((lang->u.base.use_template >> 1) & 1);
  /* Vars stop being not really extern */
  WB (lang->u.base.not_really_extern
      && (TREE_CODE (t) != VAR_DECL
	  || DECL_VTABLE_OR_VTT_P (t) || DECL_WEAK (t)));
  WB (lang->u.base.initialized_in_class);
  WB (lang->u.base.repo_available_p);
  WB (lang->u.base.threadprivate_or_deleted_p);
  WB (lang->u.base.anticipated_p);
  WB (lang->u.base.friend_or_tls);
  WB (lang->u.base.odr_used);
  WB (lang->u.base.concept_p);
  WB (lang->u.base.var_declared_inline_p);
  WB (lang->u.base.module_purview_p);
  switch (lang->u.base.selector)
    {
    case lds_fn:  /* lang_decl_fn.  */
      WB (lang->u.fn.global_ctor_p);
      WB (lang->u.fn.global_dtor_p);
      WB (lang->u.fn.static_function);
      WB (lang->u.fn.pure_virtual);
      WB (lang->u.fn.defaulted_p);
      WB (lang->u.fn.has_in_charge_parm_p);
      WB (lang->u.fn.has_vtt_parm_p);
      /* There shouldn't be a pending inline at this point.  */
      gcc_assert (!lang->u.fn.pending_inline_p);
      WB (lang->u.fn.nonconverting);
      WB (lang->u.fn.thunk_p);
      WB (lang->u.fn.this_thunk_p);
      WB (lang->u.fn.hidden_friend_p);
      WB (lang->u.fn.omp_declare_reduction_p);
      /* FALLTHROUGH.  */
    case lds_min:  /* lang_decl_min.  */
      /* No bools.  */
      break;
    case lds_ns:  /* lang_decl_ns.  */
      /* No bools.  */
      break;
    case lds_parm:  /* lang_decl_parm.  */
      /* No bools.  */
      break;
    default:
      gcc_unreachable ();
    }
#undef WB
}

bool
cpms_in::lang_decl_bools (tree t)
{
#define RB(X) ((X) = r.b ())
  struct lang_decl *lang = DECL_LANG_SPECIFIC (t);

  lang->u.base.language = r.b () ? lang_cplusplus : lang_c;
  unsigned v;
  v = r.b () << 0;
  v |= r.b () << 1;
  lang->u.base.use_template = v;
  RB (lang->u.base.not_really_extern);
  RB (lang->u.base.initialized_in_class);
  RB (lang->u.base.repo_available_p);
  RB (lang->u.base.threadprivate_or_deleted_p);
  RB (lang->u.base.anticipated_p);
  RB (lang->u.base.friend_or_tls);
  RB (lang->u.base.odr_used);
  RB (lang->u.base.concept_p);
  RB (lang->u.base.var_declared_inline_p);
  RB (lang->u.base.module_purview_p);
  switch (lang->u.base.selector)
    {
    case lds_fn:  /* lang_decl_fn.  */
      RB (lang->u.fn.global_ctor_p);
      RB (lang->u.fn.global_dtor_p);
      RB (lang->u.fn.static_function);
      RB (lang->u.fn.pure_virtual);
      RB (lang->u.fn.defaulted_p);
      RB (lang->u.fn.has_in_charge_parm_p);
      RB (lang->u.fn.has_vtt_parm_p);
      RB (lang->u.fn.nonconverting);
      RB (lang->u.fn.thunk_p);
      RB (lang->u.fn.this_thunk_p);
      RB (lang->u.fn.hidden_friend_p);
      RB (lang->u.fn.omp_declare_reduction_p);
      /* FALLTHROUGH.  */
    case lds_min:  /* lang_decl_min.  */
      /* No bools.  */
      break;
    case lds_ns:  /* lang_decl_ns.  */
      /* No bools.  */
      break;
    case lds_parm:  /* lang_decl_parm.  */
      /* No bools.  */
      break;
    default:
      gcc_unreachable ();
    }
#undef RB
  return !r.get_overrun ();
}

void
cpms_out::lang_type_bools (tree t)
{
#define WB(X) (w.b (X))
  const struct lang_type *lang = TYPE_LANG_SPECIFIC (t);

  WB (lang->has_type_conversion);
  WB (lang->has_copy_ctor);
  WB (lang->has_default_ctor);
  WB (lang->const_needs_init);
  WB (lang->ref_needs_init);
  WB (lang->has_const_copy_assign);
  WB ((lang->use_template >> 0) & 1);
  WB ((lang->use_template >> 1) & 1);

  WB (lang->has_mutable);
  WB (lang->com_interface);
  WB (lang->non_pod_class);
  WB (lang->nearly_empty_p);
  WB (lang->user_align);
  WB (lang->has_copy_assign);
  WB (lang->has_new);
  WB (lang->has_array_new);
  WB ((lang->gets_delete >> 0) & 1);
  WB ((lang->gets_delete >> 1) & 1);
  // Interfaceness is recalculated upon reading.  May have to revisit?
  // lang->interface_only
  // lang->interface_unknown
  WB (lang->contains_empty_class_p);
  WB (lang->anon_aggr);
  WB (lang->non_zero_init);
  WB (lang->empty_p);
  WB (lang->vec_new_uses_cookie);
  WB (lang->declared_class);
  WB (lang->diamond_shaped);
  WB (lang->repeated_base);
  gcc_assert (!lang->being_defined);
  WB (lang->debug_requested);
  WB (lang->fields_readonly);
  WB (lang->ptrmemfunc_flag);
  WB (lang->was_anonymous);
  WB (lang->lazy_default_ctor);
  WB (lang->lazy_copy_ctor);
  WB (lang->lazy_copy_assign);
  WB (lang->lazy_destructor);
  WB (lang->has_const_copy_ctor);
  WB (lang->has_complex_copy_ctor);
  WB (lang->has_complex_copy_assign);
  WB (lang->non_aggregate);
  WB (lang->has_complex_dflt);
  WB (lang->has_list_ctor);
  WB (lang->non_std_layout);
  WB (lang->is_literal);
  WB (lang->lazy_move_ctor);
  WB (lang->lazy_move_assign);
  WB (lang->has_complex_move_ctor);
  WB (lang->has_complex_move_assign);
  WB (lang->has_constexpr_ctor);
  WB (lang->unique_obj_representations);
  WB (lang->unique_obj_representations_set);
#undef WB
}

bool
cpms_in::lang_type_bools (tree t)
{
#define RB(X) ((X) = r.b ())
  struct lang_type *lang = TYPE_LANG_SPECIFIC (t);

  RB (lang->has_type_conversion);
  RB (lang->has_copy_ctor);
  RB (lang->has_default_ctor);
  RB (lang->const_needs_init);
  RB (lang->ref_needs_init);
  RB (lang->has_const_copy_assign);
  unsigned v;
  v = r.b () << 0;
  v |= r.b () << 1;
  lang->use_template = v;

  RB (lang->has_mutable);
  RB (lang->com_interface);
  RB (lang->non_pod_class);
  RB (lang->nearly_empty_p);
  RB (lang->user_align);
  RB (lang->has_copy_assign);
  RB (lang->has_new);
  RB (lang->has_array_new);
  v = r.b () << 0;
  v |= r.b () << 1;
  lang->gets_delete = v;
  // lang->interface_only
  // lang->interface_unknown
  lang->interface_unknown = true; // Redetermine interface
  RB (lang->contains_empty_class_p);
  RB (lang->anon_aggr);
  RB (lang->non_zero_init);
  RB (lang->empty_p);
  RB (lang->vec_new_uses_cookie);
  RB (lang->declared_class);
  RB (lang->diamond_shaped);
  RB (lang->repeated_base);
  gcc_assert (!lang->being_defined);
  RB (lang->debug_requested);
  RB (lang->fields_readonly);
  RB (lang->ptrmemfunc_flag);
  RB (lang->was_anonymous);
  RB (lang->lazy_default_ctor);
  RB (lang->lazy_copy_ctor);
  RB (lang->lazy_copy_assign);
  RB (lang->lazy_destructor);
  RB (lang->has_const_copy_ctor);
  RB (lang->has_complex_copy_ctor);
  RB (lang->has_complex_copy_assign);
  RB (lang->non_aggregate);
  RB (lang->has_complex_dflt);
  RB (lang->has_list_ctor);
  RB (lang->non_std_layout);
  RB (lang->is_literal);
  RB (lang->lazy_move_ctor);
  RB (lang->lazy_move_assign);
  RB (lang->has_complex_move_ctor);
  RB (lang->has_complex_move_assign);
  RB (lang->has_constexpr_ctor);
  RB (lang->unique_obj_representations);
  RB (lang->unique_obj_representations_set);
#undef RB
  return !r.get_overrun ();
}

/* Read & write the core values and pointers.  */

void
cpms_out::core_vals (tree t)
{
#define WU(X) (w.u (X))
#define WT(X) (tree_node (X))
  tree_code code = TREE_CODE (t);

  switch (code)
    {
    case TREE_VEC:
    case INTEGER_CST:
      /* Length written earlier.  */
      break;
    case CALL_EXPR:
      WU (t->base.u.ifn);
      break;
    case SSA_NAME:
    case MEM_REF:
    case TARGET_MEM_REF:
      /* We shouldn't meet these.  */
      gcc_unreachable ();

    default:
      break;
    }

  /* The ordering here is that in tree-core.h & cp-tree.h.  */
  if (CODE_CONTAINS_STRUCT (code, TS_BASE))
    { /* Nothing to do.  */ }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPED))
    WT (t->typed.type);

  if (CODE_CONTAINS_STRUCT (code, TS_COMMON))
    {
      /* Whether TREE_CHAIN is dumped depends on who's containing it.  */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_INT_CST))
    {
      unsigned num = TREE_INT_CST_EXT_NUNITS (t);
      for (unsigned ix = 0; ix != num; ix++)
	w.wu (TREE_INT_CST_ELT (t, ix));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_REAL_CST))
    gcc_unreachable (); // FIXME
  
  if (CODE_CONTAINS_STRUCT (code, TS_FIXED_CST))
    gcc_unreachable (); // FIXME
  
  if (CODE_CONTAINS_STRUCT (code, TS_VECTOR))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_STRING))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_COMPLEX))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_IDENTIFIER))
    gcc_unreachable (); /* Should never meet.  */

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_MINIMAL))
    {
      /* decl_minimal.name & decl_minimal.context already read in.  */
      loc (t->decl_minimal.locus);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_COMMON))
    {
      WU (t->decl_common.mode);
      WU (t->decl_common.off_align);
      WU (t->decl_common.align);

      WT (t->decl_common.size_unit);
      WT (t->decl_common.attributes);
      switch (code)
	// FIXME: Perhaps this should be done with the later
	// polymorphic check?
	{
	default:
	  break;
	case VAR_DECL:
	  if (TREE_CODE (DECL_CONTEXT (t)) != FUNCTION_DECL)
	    break;
	  /* FALLTHROUGH */
	case PARM_DECL:
	case CONST_DECL:
	  WT (t->decl_common.initial);
	  break;
	}
      /* decl_common.initial, decl_common.abstract_origin.  */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WRTL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_NON_COMMON))
    {
      /* decl_non_common.result. */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_PARM_DECL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WITH_VIS))
    {
      WT (t->decl_with_vis.assembler_name);
      WU (t->decl_with_vis.visibility);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_VAR_DECL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (code, TS_FIELD_DECL))
    {
      WT (t->field_decl.offset);
      WT (t->field_decl.bit_field_type);
      WT (t->field_decl.qualifier);
      WT (t->field_decl.bit_offset);
      WT (t->field_decl.fcontext);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_LABEL_DECL))
    {
      WU (t->label_decl.label_decl_uid);
      WU (t->label_decl.eh_landing_pad_nr);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_RESULT_DECL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (code, TS_CONST_DECL))
    { /* No extra fields.  */ }

  if (CODE_CONTAINS_STRUCT (code, TS_FUNCTION_DECL))
    {
      chained_decls (t->function_decl.arguments);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TRANSLATION_UNIT_DECL))
    gcc_unreachable (); /* Should never meet.  */

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_COMMON))
    {
      /* By construction we want to make sure we have the canonical
	 and main variants already in the type table, so emit them
	 now.  */
      WT (t->type_common.main_variant);
      WT (t->type_common.canonical);

      /* type_common.next_variant is internally manipulated.  */
      /* type_common.pointer_to, type_common.reference_to.  */

      WU (t->type_common.precision);
      WU (t->type_common.contains_placeholder_bits);
      WU (t->type_common.mode);
      WU (t->type_common.align);

      WT (t->type_common.size);
      WT (t->type_common.size_unit);
      WT (t->type_common.attributes);
      WT (t->type_common.name);
      WT (t->type_common.context);

      WT (t->type_common.common.chain); /* TYPE_STUB_DECL.  */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_WITH_LANG_SPECIFIC))
    { /* Nothing to do.  */ }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_NON_COMMON))
    {
      /* Records and unions hold FIELDS, VFIELD & BINFO on these
	 things.  */
      if (!RECORD_OR_UNION_CODE_P (code) && code != ENUMERAL_TYPE)
	{
	  /* Don't write the cached values vector.  */
	  if (!TYPE_CACHED_VALUES_P (t))
	    WT (t->type_non_common.values);
	  /* POINTER and REFERENCE types hold NEXT_{PTR,REF}_TO */
	  if (POINTER_TYPE_P (t))
	    {
	      /* We need to record whether we're on the
		 TYPE_{POINTER,REFERENCE}_TO list of the type we refer
		 to.  Do that by recording NULL or self reference
		 here.  */
	      tree probe = TREE_TYPE (t);
	      for (probe = (TREE_CODE (t) == POINTER_TYPE
			    ? TYPE_POINTER_TO (probe)
			    : TYPE_REFERENCE_TO (probe));
		   probe && probe != t;
		   probe = (TREE_CODE (t) == POINTER_TYPE
			    ? TYPE_NEXT_PTR_TO (probe)
			    : TYPE_NEXT_REF_TO (probe)))
		continue;
	      WT (probe);
	    }
	  else
	    WT (t->type_non_common.minval);
	  WT (t->type_non_common.maxval);
	}
      WT (t->type_non_common.lang_1);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_LIST))
    {
      WT (t->list.purpose);
      WT (t->list.value);
      WT (t->list.common.chain);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_VEC))
    for (unsigned ix = TREE_VEC_LENGTH (t); ix--;)
      WT (TREE_VEC_ELT (t, ix));

  if (TREE_CODE_CLASS (code) == tcc_vl_exp)
    for (unsigned ix = VL_EXP_OPERAND_LENGTH (t); --ix;)
      WT (TREE_OPERAND (t, ix));
  else if (CODE_CONTAINS_STRUCT (code, TS_EXP)
	   /* FIXME:For some reason, some tcc_expression nodes do not claim
	      to contain TS_EXP.  I think this is a bug. */
	   || TREE_CODE_CLASS (code) == tcc_expression
	   || TREE_CODE_CLASS (code) == tcc_binary
	   || TREE_CODE_CLASS (code) == tcc_unary)
    for (unsigned ix = TREE_OPERAND_LENGTH (t); ix--;)
      WT (TREE_OPERAND (t, ix));

  if (CODE_CONTAINS_STRUCT (code, TS_SSA_NAME))
    gcc_unreachable (); /* Should not see.  */

  if (CODE_CONTAINS_STRUCT (code, TS_BLOCK))
    {
      WT (t->block.supercontext);
      chained_decls (t->block.vars);
      WT (t->block.abstract_origin);
      // FIXME nonlocalized_vars, fragment_origin, fragment_chain
      WT (t->block.subblocks);
      WT (t->block.chain);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_BINFO))
    {
      WT (t->binfo.offset);
      WT (t->binfo.vtable);
      WT (t->binfo.virtuals);
      WT (t->binfo.vptr_field);
      WT (t->binfo.inheritance);
      WT (t->binfo.vtt_subvtt);
      WT (t->binfo.vtt_vptr);
      gcc_assert (BINFO_N_BASE_BINFOS (t)
		  == vec_safe_length (BINFO_BASE_ACCESSES (t)));
      tree_vec (BINFO_BASE_ACCESSES (t));
      if (unsigned num = BINFO_N_BASE_BINFOS (t))
	for (unsigned ix = 0; ix != num; ix++)
	  WT (BINFO_BASE_BINFO (t, ix));
    }

  if (CODE_CONTAINS_STRUCT (code, TS_STATEMENT_LIST))
    {
      for (tree_stmt_iterator iter = tsi_start (t);
	   !tsi_end_p (iter); tsi_next (&iter))
	if (tree stmt = tsi_stmt (iter))
	  WT (stmt);
      WT (NULL_TREE);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_CONSTRUCTOR))
    {
      unsigned len = vec_safe_length (t->constructor.elts);
      WU (len);
      if (len)
	for (unsigned ix = 0; ix != len; ix++)
	  {
	    const constructor_elt &elt = (*t->constructor.elts)[ix];

	    WT (elt.index);
	    WT (elt.value);
	  }
    }

  if (CODE_CONTAINS_STRUCT (code, TS_OMP_CLAUSE))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_OPTIMIZATION))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_TARGET_OPTION))
    gcc_unreachable (); // FIXME

  /* Now the C++-specific nodes.  These are disjoint. While we could
     use CODE directly, going via cp_tree_node_structure makes it
     easy to see whether we're missing cases.  */
  switch (cp_tree_node_structure (code))
    {
    case TS_CP_GENERIC:
      break;

    case TS_CP_TPI:
      WU (((lang_tree_node *)t)->tpi.index);
      WU (((lang_tree_node *)t)->tpi.level);
      WU (((lang_tree_node *)t)->tpi.orig_level);
      WT (((lang_tree_node *)t)->tpi.decl);
      break;
      
    case TS_CP_PTRMEM:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_OVERLOAD:
      WT (((lang_tree_node *)t)->overload.function);
      WT (t->common.chain);
      break;
      
    case TS_CP_MODULE_VECTOR:
      gcc_unreachable (); /* Should never see.  */
      break;

    case TS_CP_BASELINK:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_TEMPLATE_DECL:
      WT (((lang_tree_node *)t)->template_decl.arguments);
      WT (((lang_tree_node *)t)->template_decl.result);
      break;

    case TS_CP_DEFAULT_ARG:
      gcc_unreachable (); /* Should never see.  */
      break;

    case TS_CP_DEFERRED_NOEXCEPT:
      WT (((lang_tree_node *)t)->deferred_noexcept.pattern);
      WT (((lang_tree_node *)t)->deferred_noexcept.args);
      break;

    case TS_CP_IDENTIFIER:
      gcc_unreachable (); /* Should never see.  */
      break;

    case TS_CP_STATIC_ASSERT:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_ARGUMENT_PACK_SELECT:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_TRAIT_EXPR:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_LAMBDA_EXPR:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_TEMPLATE_INFO:
      // TI_TEMPLATE -> TYPE
      WT (t->common.chain); // TI_ARGS
      // FIXME typedefs_needing_access_checking
      break;

    case TS_CP_CONSTRAINT_INFO:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_USERDEF_LITERAL:
      gcc_unreachable (); // FIXME
      break;
    }

#undef WT
#undef WU
}

bool
cpms_in::core_vals (tree t)
{
#define RU(X) ((X) = r.u ())
#define RUC(T,X) ((X) = T (r.u ()))
#define RT(X) ((X) = tree_node ())
  tree_code code = TREE_CODE (t);

  switch (code)
    {
    case TREE_VEC:
    case INTEGER_CST:
      /* Length read earlier.  */
      break;
    case CALL_EXPR:
      RUC (internal_fn, t->base.u.ifn);
      break;
    case SSA_NAME:
    case MEM_REF:
    case TARGET_MEM_REF:
      /* We shouldn't meet these.  */
      return false;

    default:
      break;
    }

  /* The ordering here is that in tree-core.h & cp-tree.h.  */
  if (CODE_CONTAINS_STRUCT (code, TS_BASE))
    { /* Nothing to do.  */ }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPED))
    RT (t->typed.type);

  if (CODE_CONTAINS_STRUCT (code, TS_COMMON))
    {
      /* Whether TREE_CHAIN is dumped depends on who's containing it.  */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_INT_CST))
    {
      unsigned num = TREE_INT_CST_EXT_NUNITS (t);
      for (unsigned ix = 0; ix != num; ix++)
	TREE_INT_CST_ELT (t, ix) = r.wu ();
    }

  if (CODE_CONTAINS_STRUCT (code, TS_REAL_CST))
    gcc_unreachable (); // FIXME
  
  if (CODE_CONTAINS_STRUCT (code, TS_FIXED_CST))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_VECTOR))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_STRING))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_COMPLEX))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_IDENTIFIER))
    return false; /* Should never meet.  */

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_MINIMAL))
    {
      /* decl_minimal.name & decl_minimal.context already read in.  */
      /* Don't zap the locus just yet, we don't record it correctly
	 and thus lose all location information.  */
      /* t->decl_minimal.locus = */
      loc ();
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_COMMON))
    {
      RUC (machine_mode, t->decl_common.mode);
      RU (t->decl_common.off_align);
      RU (t->decl_common.align);

      RT (t->decl_common.size_unit);
      RT (t->decl_common.attributes);
      switch (code)
	// FIXME: Perhaps this should be done with the later
	// polymorphic check?
	{
	default:
	  break;
	case VAR_DECL:
	  if (TREE_CODE (DECL_CONTEXT (t)) != FUNCTION_DECL)
	    break;
	  /* FALLTHROUGH */
	case PARM_DECL:
	case CONST_DECL:
	  RT (t->decl_common.initial);
	  break;
	}
      /* decl_common.initial, decl_common.abstract_origin.  */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WRTL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (TREE_CODE (t), TS_DECL_NON_COMMON))
    {
      /* decl_non_common.result. */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_PARM_DECL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (code, TS_DECL_WITH_VIS))
    {
      RT (t->decl_with_vis.assembler_name);
      RUC (symbol_visibility, t->decl_with_vis.visibility);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_VAR_DECL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (code, TS_FIELD_DECL))
    {
      RT (t->field_decl.offset);
      RT (t->field_decl.bit_field_type);
      RT (t->field_decl.qualifier);
      RT (t->field_decl.bit_offset);
      RT (t->field_decl.fcontext);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_LABEL_DECL))
    {
      RU (t->label_decl.label_decl_uid);
      RU (t->label_decl.eh_landing_pad_nr);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_RESULT_DECL))
    {} // FIXME?

  if (CODE_CONTAINS_STRUCT (code, TS_CONST_DECL))
    { /* No extra fields.  */ }

  if (CODE_CONTAINS_STRUCT (code, TS_FUNCTION_DECL))
    {
      t->function_decl.arguments = chained_decls ();
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TRANSLATION_UNIT_DECL))
    return false;

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_COMMON))
    {
      RT (t->type_common.main_variant);
      RT (t->type_common.canonical);

      /* type_common.next_variant is internally manipulated.  */
      /* type_common.pointer_to, type_common.reference_to.  */

      RU (t->type_common.precision);
      RU (t->type_common.contains_placeholder_bits);
      RUC (machine_mode, t->type_common.mode);
      RU (t->type_common.align);

      RT (t->type_common.size);
      RT (t->type_common.size_unit);
      RT (t->type_common.attributes);
      RT (t->type_common.name);
      RT (t->type_common.context);

      RT (t->type_common.common.chain); /* TYPE_STUB_DECL.  */
    }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_WITH_LANG_SPECIFIC))
    { /* Nothing to do.  */ }

  if (CODE_CONTAINS_STRUCT (code, TS_TYPE_NON_COMMON))
    {
      /* Records and unions hold FIELDS, VFIELD & BINFO on these
	 things.  */
      if (!RECORD_OR_UNION_CODE_P (code) && code != ENUMERAL_TYPE)
	{
	  if (!TYPE_CACHED_VALUES_P (t))
	    RT (t->type_non_common.values);
	  else
	    /* Clear the type cached values.  */
	    TYPE_CACHED_VALUES_P (t) = 0;

	  /* POINTER and REFERENCE types hold NEXT_{PTR,REF}_TO.  We
	     store a marker there to indicate whether we're on the
	     referred to type's pointer/reference to list.  */
	  RT (t->type_non_common.minval);
	  if (POINTER_TYPE_P (t) && t->type_non_common.minval
	      && t->type_non_common.minval != t)
	    {
	      t->type_non_common.minval = NULL_TREE;
	      r.set_overrun ();
	    }
	  RT (t->type_non_common.maxval);
	}
      RT (t->type_non_common.lang_1);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_LIST))
    {
      RT (t->list.purpose);
      RT (t->list.value);
      RT (t->list.common.chain);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_VEC))
    for (unsigned ix = TREE_VEC_LENGTH (t); ix--;)
      RT (TREE_VEC_ELT (t, ix));

  if (TREE_CODE_CLASS (code) == tcc_vl_exp)
    for (unsigned ix = VL_EXP_OPERAND_LENGTH (t); --ix;)
      RT (TREE_OPERAND (t, ix));
  else if (CODE_CONTAINS_STRUCT (code, TS_EXP)
	   /* See comment in cpms_out::core_vals.  */
	   || TREE_CODE_CLASS (code) == tcc_expression
	   || TREE_CODE_CLASS (code) == tcc_binary
	   || TREE_CODE_CLASS (code) == tcc_unary)
    for (unsigned ix = TREE_OPERAND_LENGTH (t); ix--;)
      RT (TREE_OPERAND (t, ix));

  if (CODE_CONTAINS_STRUCT (code, TS_SSA_NAME))
    return false;

  if (CODE_CONTAINS_STRUCT (code, TS_BLOCK))
    {
      RT (t->block.supercontext);
      t->block.vars = chained_decls ();
      RT (t->block.abstract_origin);
      // FIXME nonlocalized_vars, fragment_origin, fragment_chain
      RT (t->block.subblocks);
      RT (t->block.chain);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_BINFO))
    {
      RT (t->binfo.offset);
      RT (t->binfo.vtable);
      RT (t->binfo.virtuals);
      RT (t->binfo.vptr_field);
      RT (t->binfo.inheritance);
      RT (t->binfo.vtt_subvtt);
      RT (t->binfo.vtt_vptr);
      BINFO_BASE_ACCESSES (t) = tree_vec ();
      if (BINFO_BASE_ACCESSES (t))
	{
	  unsigned num = BINFO_BASE_ACCESSES (t)->length ();
	  for (unsigned ix = 0; ix != num; ix++)
	    BINFO_BASE_APPEND (t, tree_node ());
	}
    }


  if (CODE_CONTAINS_STRUCT (code, TS_STATEMENT_LIST))
    {
      tree_stmt_iterator iter = tsi_start (t);
      for (tree stmt; RT (stmt);)
	tsi_link_after (&iter, stmt, TSI_CONTINUE_LINKING);
    }

  if (CODE_CONTAINS_STRUCT (code, TS_CONSTRUCTOR))
    {
      if (unsigned len = r.u ())
	{
	  vec_alloc (t->constructor.elts, len);
	  for (unsigned ix = 0; ix != len; ix++)
	    {
	      constructor_elt elt;

	      RT (elt.index);
	      RT (elt.value);
	      t->constructor.elts->quick_push (elt);
	    }
	}
    }

  if (CODE_CONTAINS_STRUCT (code, TS_OMP_CLAUSE))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_OPTIMIZATION))
    gcc_unreachable (); // FIXME

  if (CODE_CONTAINS_STRUCT (code, TS_TARGET_OPTION))
    gcc_unreachable (); // FIXME

  /* Now the C++-specific nodes.  These are disjoint. While we could
     use CODE directly, going via cp_tree_node_structure makes it
     easy to see whether we're missing cases.  */
  switch (cp_tree_node_structure (code))
    {
    case TS_CP_GENERIC:
      break;

    case TS_CP_TPI:
      RU (((lang_tree_node *)t)->tpi.index);
      RU (((lang_tree_node *)t)->tpi.level);
      RU (((lang_tree_node *)t)->tpi.orig_level);
      RT (((lang_tree_node *)t)->tpi.decl);
      break;

    case TS_CP_PTRMEM:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_OVERLOAD:
      RT (((lang_tree_node *)t)->overload.function);
      RT (t->common.chain);
      break;

    case TS_CP_MODULE_VECTOR:
      return false;

    case TS_CP_BASELINK:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_TEMPLATE_DECL:
      RT (((lang_tree_node *)t)->template_decl.arguments);
      RT (((lang_tree_node *)t)->template_decl.result);
      break;

    case TS_CP_DEFAULT_ARG:
      return false;

    case TS_CP_DEFERRED_NOEXCEPT:
      RT (((lang_tree_node *)t)->deferred_noexcept.pattern);
      RT (((lang_tree_node *)t)->deferred_noexcept.args);
      break;

    case TS_CP_IDENTIFIER:
      return false; /* Should never see.  */

    case TS_CP_STATIC_ASSERT:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_ARGUMENT_PACK_SELECT:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_TRAIT_EXPR:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_LAMBDA_EXPR:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_TEMPLATE_INFO:
      // TI_TEMPLATE -> TYPE
      RT (t->common.chain); // TI_ARGS
      // FIXME typedefs_needing_access_checking
      break;

    case TS_CP_CONSTRAINT_INFO:
      gcc_unreachable (); // FIXME
      break;

    case TS_CP_USERDEF_LITERAL:
      gcc_unreachable (); // FIXME
      break;
    }

#undef RT
#undef RM
#undef RU
  return !r.get_overrun ();
}

void
cpms_out::lang_decl_vals (tree t)
{
  const struct lang_decl *lang = DECL_LANG_SPECIFIC (t);
#define WU(X) (w.u (X))
#define WT(X) (tree_node (X))
  /* Module index already written.  */
  switch (lang->u.base.selector)
    {
    case lds_fn:  /* lang_decl_fn.  */
      if (DECL_NAME (t) && IDENTIFIER_OVL_OP_P (DECL_NAME (t)))
	WU (lang->u.fn.ovl_op_code);
      if (lang->u.fn.thunk_p)
	w.wi (lang->u.fn.u5.fixed_offset);
      else
	WT (lang->u.fn.u5.cloned_function);
      /* FALLTHROUGH.  */
    case lds_min:  /* lang_decl_min.  */
      WT (lang->u.min.template_info);
      WT (lang->u.min.access);
      break;
    case lds_ns:  /* lang_decl_ns.  */
      break;
    case lds_parm:  /* lang_decl_parm.  */
      WU (lang->u.parm.level);
      WU (lang->u.parm.index);
      break;
    default:
      gcc_unreachable ();
    }
#undef WU
#undef WT
}

bool
cpms_in::lang_decl_vals (tree t)
{
  struct lang_decl *lang = DECL_LANG_SPECIFIC (t);
#define RU(X) ((X) = r.u ())
#define RT(X) ((X) = tree_node ())

  /* Module index already read.  */

  switch (lang->u.base.selector)
    {
    case lds_fn:  /* lang_decl_fn.  */
      {
	if (DECL_NAME (t) && IDENTIFIER_OVL_OP_P (DECL_NAME (t)))
	  {
	    unsigned code = r.u ();

	    /* Check consistency.  */
	    if (code >= OVL_OP_MAX
		|| (ovl_op_info[IDENTIFIER_ASSIGN_OP_P (DECL_NAME (t))][code]
		    .ovl_op_code) == OVL_OP_ERROR_MARK)
	      r.set_overrun ();
	    else
	      lang->u.fn.ovl_op_code = code;
	  }

	if (lang->u.fn.thunk_p)
	  lang->u.fn.u5.fixed_offset = r.wi ();
	else
	  RT (lang->u.fn.u5.cloned_function);
      }
      /* FALLTHROUGH.  */
    case lds_min:  /* lang_decl_min.  */
      RT (lang->u.min.template_info);
      RT (lang->u.min.access);
      break;
    case lds_ns:  /* lang_decl_ns.  */
      break;
    case lds_parm:  /* lang_decl_parm.  */
      RU (lang->u.parm.level);
      RU (lang->u.parm.index);
      break;
    default:
      gcc_unreachable ();
    }
#undef RU
#undef RT
  return !r.get_overrun ();
}

/* Most of the value contents of lang_type is streamed in
   define_class.  */

void
cpms_out::lang_type_vals (tree t)
{
  const struct lang_type *lang = TYPE_LANG_SPECIFIC (t);
#define WU(X) (w.u (X))
#define WT(X) (tree_node (X))
  WU (lang->align);
  WT (lang->befriending_classes);
#undef WU
#undef WT
}

bool
cpms_in::lang_type_vals (tree t)
{
  struct lang_type *lang = TYPE_LANG_SPECIFIC (t);
#define RU(X) ((X) = r.u ())
#define RT(X) ((X) = tree_node ())
  RU (lang->align);
  RT (lang->befriending_classes);
#undef RU
#undef RT
  return !r.get_overrun ();
}

/* The raw tree node.  We've already dealt with the code, and in the
   case of decls, determining name, context & module.  Stream the
   bools and vals without post-processing.  */

void
cpms_out::tree_node_raw (tree_code code, tree t)
{
  tree_code_class klass = TREE_CODE_CLASS (code);
  bool specific = false;

  if (klass == tcc_type || klass == tcc_declaration)
    {
      if (klass == tcc_declaration)
	specific = DECL_LANG_SPECIFIC (t) != NULL;
      else if (TYPE_MAIN_VARIANT (t) == t)
	specific = TYPE_LANG_SPECIFIC (t) != NULL;
      else
	gcc_assert (TYPE_LANG_SPECIFIC (t)
		    == TYPE_LANG_SPECIFIC (TYPE_MAIN_VARIANT (t)));
      w.b (specific);
      if (specific && code == VAR_DECL)
	w.b (DECL_DECOMPOSITION_P (t));
    }

  core_bools (t);
  if (specific)
    {
      if (klass == tcc_type)
	lang_type_bools (t);
      else
	lang_decl_bools (t);
    }
  w.bflush ();

  core_vals (t);
  if (specific)
    {
      if (klass == tcc_type)
	lang_type_vals (t);
      else
	lang_decl_vals (t);
    }
}

bool
cpms_in::tree_node_raw (tree_code code, tree t, tree name, tree ctx)
{
  tree_code_class klass = TREE_CODE_CLASS (code);
  bool specific = false;
  bool lied = false;

  if (klass == tcc_type || klass == tcc_declaration)
    {
      specific = r.b ();
      if (specific
	  &&  (klass == tcc_type
	       ? !maybe_add_lang_type_raw (t)
	       : !maybe_add_lang_decl_raw (t, code == VAR_DECL && r.b ())))
	  lied = true;
    }

  if (!core_bools (t))
    lied = true;
  else if (specific)
    {
      if (klass == tcc_type
	  ? !lang_type_bools (t)
	  : !lang_decl_bools (t))
	lied = true;
    }
  r.bflush ();
  if (lied || r.get_overrun ())
    return false;

  if (klass == tcc_declaration)
    {
      DECL_CONTEXT (t) = ctx;
      DECL_NAME (t) = name;
      if (ctx && (TREE_CODE (ctx) == NAMESPACE_DECL
		  || TREE_CODE (ctx) == TRANSLATION_UNIT_DECL))
	// FIXME:maybe retrofit lang_decl?
	DECL_MODULE_INDEX (t) = state->mod;
    }

  if (!core_vals (t))
    return false;

  if (specific)
    {
      if (klass == tcc_type)
	{
	  gcc_assert (TYPE_MAIN_VARIANT (t) == t);
	  if (!lang_type_vals (t))
	    return false;
	}
      else
	{
	  if (!lang_decl_vals (t))
	    return false;
	}
    }
  else if (klass == tcc_type)
    TYPE_LANG_SPECIFIC (t) = TYPE_LANG_SPECIFIC (TYPE_MAIN_VARIANT (t));

  return true;
}

/* If tree has special streaming semantics, do that.  */

int
cpms_out::tree_node_special (tree t)
{
  if (unsigned *val = tree_map.get (t))
    {
      refs++;
      w.u (*val);
      dump () && dump ("Wrote:%u referenced %C:%N%M", *val,
		       TREE_CODE (t), t, t);
      return true;
    }

  if (TREE_CODE_CLASS (TREE_CODE (t)) == tcc_type && TYPE_NAME (t))
    {
      tree name = TYPE_NAME (t);

      gcc_assert (TREE_CODE (name) == TYPE_DECL);
      if (DECL_TINFO_P (name))
	{
	  unsigned ix = get_pseudo_tinfo_index (t);

	  /* Make sure we're identifying this exact variant.  */
	  gcc_assert (get_pseudo_tinfo_type (ix) == t);
	  w.u (rt_typeinfo_pseudo);
	  w.u (ix);
	  unsigned tag = insert (t);
	  dump () && dump ("Wrote:%u typeinfo pseudo %u %N", tag, ix, t);
	  return true;
	}
      else if (!tree_map.get (name))
	{
	  /* T is a named type whose name we have not met yet.  Write the
	     type name as an interstitial, and then start over.  */
	  dump () && dump ("Writing interstitial type name %C:%N%M",
			   TREE_CODE (name), name, name);
	  /* Make sure this is not a named builtin. We should find
	     those some other way to be canonically correct.  */
	  gcc_assert (TREE_TYPE (DECL_NAME (name)) != t
		      || DECL_SOURCE_LOCATION (name) != BUILTINS_LOCATION);
	  w.u (rt_type_name);
	  tree_node (name);
	  dump () && dump ("Wrote interstitial type name %C:%N%M",
			   TREE_CODE (name), name, name);
	  /* The type could be a variant of TREE_TYPE (name).  */
	  return -1;
	}
    }

  if (TREE_CODE (t) == VAR_DECL && DECL_TINFO_P (t))
    {
      /* T is a typeinfo object.  These need recreating by the loader.
	 The type it is for is stashed on the name's TREE_TYPE.  */
      tree type = TREE_TYPE (DECL_NAME (t));
      dump () && dump ("Writing typeinfo %M for %N", t, type);
      w.u (rt_typeinfo_var);
      tree_node (type);
      unsigned tag = insert (t);
      dump () && dump ("Wrote:%u typeinfo %M for %N", tag, t, type);
      unnest ();
      return true;
    }

  if (TREE_CODE (t) == IDENTIFIER_NODE)
    {
      /* An identifier node.  Stream the name or type.  */
      bool conv_op = IDENTIFIER_CONV_OP_P (t);

      w.u (conv_op ? rt_conv_identifier : rt_identifier);
      if (conv_op)
	tree_node (TREE_TYPE (t));
      else
	w.str (IDENTIFIER_POINTER (t), IDENTIFIER_LENGTH (t));

      unsigned tag = insert (t);
      dump () && dump ("Written:%u %sidentifier:%N",
		       tag, conv_op ? "conv_op_" : "",
		       conv_op ? TREE_TYPE (t) : t);
      return true;
    }

  return false;  /* Not a special snowflake. */
}

tree
cpms_in::tree_node_special (unsigned tag)
{
  if (tag >= rt_ref_base)
    {
      tree res;

      if (tag - rt_ref_base < globals->length ())
	res = (*globals)[tag - rt_ref_base];
      else
	{
	  tree *val = (tree *)tree_map.get (tag);
	  if (!val || !*val)
	    {
	      error ("unknown tree reference %qd", tag);
	      r.set_overrun ();
	      return NULL_TREE;
	    }
	  res = *val;
	}
      dump () && dump ("Read:%u found %C:%N%M", tag,
		       TREE_CODE (res), res, res);
      return res;
    }

  if (tag == rt_type_name)
    {
      /* An interstitial type name.  Read the name and then start
	 over.  */
      tree name = tree_node ();
      if (!name || TREE_CODE (name) != TYPE_DECL)
	r.set_overrun ();
      else
	dump () && dump ("Read interstitial type name %C:%N%M",
			 TREE_CODE (name), name, name);
      return NULL_TREE;
    }

  if (tag == rt_typeinfo_var)
    {
      /* A typeinfo.  Get the type and recreate the var decl.  */
      tree var = NULL_TREE, type = tree_node ();
      if (!type || !TYPE_P (type))
	r.set_overrun ();
      else
	{
	  var = get_tinfo_decl (type);
	  unsigned tag = insert (var);
	  dump () && dump ("Created:%u typeinfo var %M for %N",
			   tag, var, type);
	}
      return var;
    }

  if (tag == rt_typeinfo_pseudo)
    {
      /* A pseuto typeinfo.  Get the index and recreate the pseudo.  */
      unsigned ix = r.u ();
      tree type = NULL_TREE;

      if (ix >= 1000)
	r.set_overrun ();
      else
	type = get_pseudo_tinfo_type (ix);

      unsigned tag = insert (type);
      dump () && dump ("Created:%u typeinfo pseudo %u %N", tag, ix, type);
      return type;
    }

  if (tag == rt_definition)
    {
      /* An immediate definition.  */
      tree res = tag_definition ();
      if (res)
	dump () && dump ("Read immediate definition %C:%N%M",
			 TREE_CODE (res), res, res);
      else
	gcc_assert (r.get_overrun ());
      return res;
    }

  if (tag == rt_identifier)
    {
      size_t l;
      const char *str = r.str (&l);
      tree id = get_identifier_with_length (str, l);
      tag = insert (id);
      dump () && dump ("Read:%u identifier:%N", tag, id);
      return id;
    }

  if (tag == rt_conv_identifier)
    {
      tree t = tree_node ();
      if (!t || !TYPE_P (t))
	{
	  error ("bad conversion operator");
	  r.set_overrun ();
	  t = void_type_node;
	}
      tree id = make_conv_op_name (t);
      tag = insert (id);
      dump () && dump ("Read:%u conv_op_identifier:%N", tag, t);
      return id;
    }

  return NULL_TREE;
}

/* Write either the decl (as a declaration) itself (and create a
   mapping for it), or write the existing mapping or write null.  This
   is essentially the lisp self-referential structure pretty-printer,
   except that we implicitly number every node, so need neither two
   passes, nor explicit labelling.
*/

void
cpms_out::tree_node (tree t)
{
  if (!t)
    {
      nulls++;
      w.u (0);
      return;
    }

  nest ();
 again:
  if (int special = tree_node_special (t))
    {
      if (special < 0)
	goto again;
      unnest ();
      return;
    }

  /* Generic node streaming.  */    
  tree_code code = TREE_CODE (t);
  tree_code_class klass = TREE_CODE_CLASS (code);
  gcc_assert (rt_tree_base + code < rt_ref_base);

  unique++;
  w.u (rt_tree_base + code);

  bool body = true;
  if (klass == tcc_declaration)
    {
      /* Write out ctx, name & maybe import reference info.  */
      tree_node (DECL_CONTEXT (t));
      tree_node (DECL_NAME (t));

      tree module_ctx = module_context (t);
      unsigned node_module = 0;
      if (module_ctx)
	node_module = MAYBE_DECL_MODULE_INDEX (module_ctx);
      w.u (node_module);
      if (node_module)
	{
	  tree_node (DECL_DECLARES_FUNCTION_P (t) ? TREE_TYPE (t) : NULL_TREE);
	  dump () && dump ("Writing imported %N@%I", t,
			   module_name (node_module));
	  body = false;
	}
    }

  if (body)
    start (code, t);

  unsigned tag = insert (t);
  dump () && dump ("Writing:%u %C:%N%M%s", tag, TREE_CODE (t), t, t,
		   klass == tcc_declaration && DECL_MODULE_EXPORT_P (t)
		   ? " (exported)": "");

  if (body)
    tree_node_raw (code, t);
  else if (TREE_TYPE (t))
    {
      tree type = TREE_TYPE (t);
      bool existed;
      unsigned *val = &tree_map.get_or_insert (type, &existed);
      if (!existed)
	{
	  tag = next ();
	  *val = tag;
	  dump () && dump ("Writing:%u %C:%N%M imported type", tag,
			   TREE_CODE (type), type, type);
	}
      w.u (existed);
    }

  unnest ();
}

/* Read in a tree using TAG.  TAG is either a back reference, or a
   TREE_CODE for a new TREE.  For any tree that is a DECL, this does
   not read in a definition (initial value, class defn, function body,
   instantiations, whatever).  Return true on success.  Sets *TP to
   error_mark_node if TAG is totally bogus.  */

tree
cpms_in::tree_node ()
{
  unsigned tag = r.u ();

  if (!tag)
    return NULL_TREE;

  nest ();
 again:
  tree res = tree_node_special (tag);
  if (!res && !r.get_overrun ())
    {
      if (tag == rt_type_name)
	{
	  tag = r.u ();
	  goto again;
	}

      if (tag < rt_tree_base || tag >= rt_tree_base + MAX_TREE_CODES)
	{
	  error (tag < rt_tree_base ? "unexpected key %qd"
		 : "unknown tree code %qd" , tag);
	  r.set_overrun ();
	}
    }

  if (res || r.get_overrun ())
    {
      unnest ();
      return res;
    }

  tree_code code = tree_code (tag - rt_tree_base);
  tree_code_class klass = TREE_CODE_CLASS (code);
  tree t = NULL_TREE;

  bool body = true;
  tree name = NULL_TREE;
  tree ctx = NULL_TREE;

  if (klass == tcc_declaration)
    {
      unsigned node_module;

      ctx = tree_node ();
      name = tree_node ();
      if (!r.get_overrun ())
	{
	  unsigned incoming = r.u ();

	  // FIXME:and here 
	  if (incoming < state->remap->length ())
	    node_module = (*state->remap)[incoming];

	  if (incoming >= state->remap->length ()
	      || ((incoming != 0) != (node_module != state->mod)))
	    r.set_overrun ();
	}

      if (r.get_overrun ())
	{
	  unnest ();
	  return NULL_TREE;
	}

      gcc_assert (node_module || !state->mod);

      if (node_module != state->mod)
	{
	  tree type = tree_node ();

	  if (TREE_CODE (ctx) == TRANSLATION_UNIT_DECL)
	    ctx = global_namespace;
	  t = lookup_by_ident (ctx, node_module, name, type, code);
	  if (!t)
	    error ("failed to find %<%E%s%E@%E%>",
		   ctx, &"::"[2 * (ctx == global_namespace)],
		   name, module_name (node_module));
	  dump () && dump ("Importing %P@%I",
			   ctx, name, module_name (node_module));
	  body = false;
	}
    }

  if (body)
    t = start (code);

  /* Insert into map.  */
  tag = insert (t);
  dump () && dump ("%s:%u %C:%N", body ? "Reading" : "Imported", tag,
		   code, name);

  if (body)
    {
      if (!tree_node_raw (code, t, name, ctx))
	goto barf;
    }
  else if (!t)
    goto barf;
  else if (TREE_TYPE (t) && !r.u ())
    {
      tree type = TREE_TYPE (t);
      tag = insert (type);
      dump () && dump ("Read:%u %C:%N%M imported type", tag,
		       TREE_CODE (type), type, type);
    }

  if (r.get_overrun ())
    {
    barf:
      tree_map.put (tag, NULL_TREE);
      r.set_overrun ();
      unnest ();
      return NULL_TREE;
    }

  if (body)
    {
      tree found = finish (t);

      if (found != t)
	{
	  /* Update the mapping.  */
	  t = found;
	  tree_map.put (tag, t);
	  dump () && dump ("Index %u remapping %C:%N%M", tag,
			   t ? TREE_CODE (t) : ERROR_MARK, t, t);
	}
    }

  unnest ();
  return t;
}

/* Rebuild a streamed in type.  */
// FIXME: c++-specific types are not in the canonical type hash.
// Perhaps that should be changed?

tree
cpms_in::finish_type (tree type)
{
  tree main = TYPE_MAIN_VARIANT (type);

  if (main != type)
    {
      /* See if we have this type already on the variant
	 list.  This could only happen if the originally read in main
	 variant was remapped, but we don't have that knowledge.
	 FIXME: Determine if this is a problem, and then maybe fix
	 it?  That would avoid a fruitless search along the variant
	 chain.  */
      for (tree probe = main; probe; probe = TYPE_NEXT_VARIANT (probe))
	{
	  if (!check_base_type (type, probe))
	    continue;

	  if (TYPE_QUALS (type) != TYPE_QUALS (probe))
	    continue;

	  if (FUNC_OR_METHOD_TYPE_P (type))
	    {
	      if (!comp_except_specs (TYPE_RAISES_EXCEPTIONS (type),
				      TYPE_RAISES_EXCEPTIONS (probe),
				      ce_exact))
		continue;

	      if (type_memfn_rqual (type) != type_memfn_rqual (probe))
		continue;
	    }
	  
	  dump () && dump ("Type %p already found as %p variant of %p",
			   (void *)type, (void *)probe, (void *)main);
	  free_node (type);
	  type = probe;
	  goto found_variant;
	}

      /* Splice it into the variant list.  */
      dump () && dump ("Type %p added as variant of %p",
		       (void *)type, (void *)main);
      TYPE_NEXT_VARIANT (type) = TYPE_NEXT_VARIANT (main);
      TYPE_NEXT_VARIANT (main) = type;
      if (RECORD_OR_UNION_CODE_P (TREE_CODE (type)))
	{
	  /* The main variant might already have been defined, copy
	     the bits of its definition that we need.  */
	  TYPE_BINFO (type) = TYPE_BINFO (main);
	  TYPE_VFIELD (type) = TYPE_VFIELD (main);
	  TYPE_FIELDS (type) = TYPE_FIELDS (main);
	}

      /* CANONICAL_TYPE is either already correctly remapped.  Or
         correctly already us.  */
      // FIXME:Are we sure about this?
    found_variant:;
    }
  else if (TREE_CODE (type) == TEMPLATE_TYPE_PARM
	   || TREE_CODE (type) == TEMPLATE_TEMPLATE_PARM)
    {
      tree canon = canonical_type_parameter (type);
      if (TYPE_CANONICAL (type) == type)
	type = canon;
      else
	TYPE_CANONICAL (type) = canon;
      dump () && dump ("Adding template type %p with canonical %p",
		       (void *)type, (void *)canon);
    }
  else if (!TYPE_STRUCTURAL_EQUALITY_P (type)
	   && !TYPE_NAME (type))
    {
      gcc_assert (TYPE_ALIGN (type));
      hashval_t hash = type_hash_canon_hash (type);
      /* type_hash_canon frees type, if we find it already.  */
      type = type_hash_canon (hash, type);
      // FIXME: This is where it'd be nice to determine if type
      // was already found.  See above.
      dump () && dump ("Adding type %p with canonical %p",
		       (void *)main, (void *)type);
    }

  return type;
}

/* Walk the bindings of NS, writing out the bindings for the current
   TU.   */

// FIXME: There's a problem with namespaces themselves.  We need to
// know whether the namespace itself is exported, which happens if
// it's explicitly opened in the purview.  (It may exist because of
// being opened in the global module.)  Need flag on namespace,
// perhaps simple as DECL_MODULE_EXPORT.

/* The binding section is a serialized tree.  Each non-namespace
   binding is a <stroff,shnum> tuple.  Each namespace contains a list
   of non-namespace bindings, zero, a list of namespace bindings,
   zero. */

vec<tree, va_gc> *
cpms_out::bindings (bytes_out *bind, vec<tree, va_gc> *nest, tree ns)
{
  dump () && dump ("Walking namespace %N", ns);

  gcc_checking_assert (!LOOKUP_FOUND_P (ns));
  vec<tree, va_gc> *inner = NULL;
  vec_alloc (inner, 10);

  module_binding_vec *module_bindings = NULL;
  vec_alloc (module_bindings, 10);
  hash_table<named_decl_hash>::iterator end
    (DECL_NAMESPACE_BINDINGS (ns)->end ());
  for (hash_table<named_decl_hash>::iterator iter
	 (DECL_NAMESPACE_BINDINGS (ns)->begin ()); iter != end; ++iter)
    {
      tree binding = *iter;

      if (TREE_CODE (binding) == MODULE_VECTOR)
	binding = MODULE_VECTOR_CLUSTER (binding, 0).slots[0];

      if (!binding)
	continue;

      module_bindings = extract_module_bindings (module_bindings, binding);
      if (!module_bindings->length ())
	continue;

      tree first = (*module_bindings)[0];
      if (TREE_CODE (first) == NAMESPACE_DECL
	  && !DECL_NAMESPACE_ALIAS (first))
	vec_safe_push (inner, module_bindings->pop ());
      else
	{
	  /* Emit open scopes.  */
	  if (!LOOKUP_FOUND_P (ns))
	    {
	      LOOKUP_FOUND_P (ns) = true;
	      for (unsigned ix = 0; ix != nest->length (); ix++)
		{
		  tree outer = (*nest)[ix];
		  if (!LOOKUP_FOUND_P (outer))
		    {
		      LOOKUP_FOUND_P (outer) = true;
		      bind->u (elf.name (DECL_NAME (outer)));
		      bind->u (0); /* It had no bindings of its own.  */
		    }
		}
	      
	      bind->u (elf.name (DECL_NAME (ns)));
	    }
	  bind->u (elf.name (DECL_NAME (first)));
	  tag_binding (ns, DECL_NAME (first), module_bindings);
	  // FIXME, here we'd emit the section number containing the
	  // declaration.
	}
      gcc_checking_assert (!module_bindings->length ());
    }
  vec_free (module_bindings);
  /* Mark end of non-namespace bindings.  */
  if (LOOKUP_FOUND_P (ns))
    bind->u (0);
  vec_safe_push (nest, ns);
  while (inner->length ())
    nest = bindings (bind, nest, inner->pop ());
  vec_free (inner);
  nest->pop ();
  /* Mark end of namespace bindings.  */
  if (LOOKUP_FOUND_P (ns))
    {
      LOOKUP_FOUND_P (ns) = false;
      bind->u (0);
    }
  dump () && dump ("Walked namespace %N", ns);
  return nest;
}

bool
cpms_out::begin ()
{
  return elf.begin ();
}

bool
cpms_out::end ()
{
  int e = elf.end ();
  if (e)
    error ("failed to write module %qE (%qs): %s",
	   state->name, state->filename,
	   e >= 0 ? xstrerror (e) : "Bad file data");
  return e == 0;
}

void
cpms_out::write ()
{
  /* Write README.  */
  {
    w.begin ();
    version_string string;
    version2string (get_version (), string);
    w.printf ("version:%s%c", string, 0);
    w.printf ("module:%s%c", IDENTIFIER_POINTER (state->name), 0);
    for (unsigned ix = modules->length (); ix-- > MODULE_INDEX_IMPORT_BASE;)
      {
	module_state *state = (*modules)[ix];
	if (state->imported)
	  w.printf ("import:%s%c", IDENTIFIER_POINTER (state->name), 0);
      }
    /* Create as STRTAB so that:
         readelf -p.gnu.c++.README X.nms
       works.  */
    w.end (&elf, elf.name (MOD_SNAME_PFX ".README"), NULL, /*strings=*/true);
  }

  unsigned crc = 0;
  imports (&crc); /* Import table. */

  /* Prepare the globals. */
  {
    unsigned limit = globals->length ();
    for (unsigned ix = 0; ix != limit; ix++)
      {
	tree val = (*globals)[ix];
	bool existed;
	unsigned *slot = &tree_map.get_or_insert (val, &existed);
	gcc_checking_assert (!existed);
	*slot = next ();
      }
  }

  bytes_out bind;
  bind.begin (true);

  w.begin (true);

  /* Write decls.  */
  vec<tree, va_gc> *nest = NULL;
  vec_alloc (nest, 30);
  nest = bindings (&bind, nest, global_namespace);
  gcc_assert (!nest->length ());
  vec_free (nest);

  w.end (&elf, elf.name (MOD_SNAME_PFX ".decls"), &crc);
  bind.end (&elf, elf.name (MOD_SNAME_PFX ".bindings"), &crc);

  header (crc);

  instrument ();
}

bool
cpms_in::begin ()
{
  return elf.begin ();
}

bool
cpms_in::end ()
{
  int e = elf.end ();
  if (e)
    {
      /* strerror and friends returns capitalized strings.  */
      char const *err = e >= 0 ? xstrerror (e) : "Bad file data";
      char c = TOLOWER (err[0]);
      error ("%c%s", c, err + 1);
    }
  return e == 0;
}

unsigned
cpms_in::read (unsigned *crc_ptr)
{
  bool ok = header (crc_ptr);
  unsigned crc = 0;

  if (ok)
    ok = imports (&crc);

  unsigned ix = 0;
  if (state != (*modules)[0])
    {
      ix = modules->length ();
      if (ix == MODULE_INDEX_LIMIT)
	{
	  sorry ("too many modules loaded (limit is %u)", ix);
	  ok = false;
	  ix = 0;
	}
      else
	{
	  vec_safe_push (modules, state);
	  state->set_index (ix);
	}
    }
  state->mod = ix;
  if (ok)
    (*state->remap)[0] = ix;

  dump () && dump ("Assigning %N module index %u", state->name, ix);

  /* Just reserve the tag space.  No need to actually insert them in
     the map.  */
  next (globals->length ());

  if (!r.begin (&elf, MOD_SNAME_PFX ".decls", &crc))
    return MODULE_INDEX_ERROR;

  while (ok && r.more_p ())
    {
      int rt = r.u ();

      switch (rt)
	{
	case rt_binding:
	  ok = tag_binding ();
	  break;
	case rt_definition:
	  ok = tag_definition () != NULL_TREE;
	  break;

	default:
	  error (rt < rt_tree_base ? "unknown key %qd"
		 : rt < rt_ref_base ? "unexpected tree code %qd"
		 : "unexpected tree reference %qd",rt);
	  ok = false;
	  break;
	}
    }

  if (!ok)
    r.set_overrun ();

  r.end (&elf);

  return state->mod;
}

static GTY(()) tree proclaimer;
static int export_depth; /* -1 for singleton export.  */

/* Nest a module export level.  Return true if we were already in a
   level.  */

int
push_module_export (bool singleton, tree proclaiming)
{
  int previous = export_depth;

  if (proclaiming)
    {
      proclaimer = proclaimer;
      export_depth = -2;
    }
  else if (singleton)
    export_depth = -1;
  else
    export_depth = +1;
  return previous;
}

/* Unnest a module export level.  */

void
pop_module_export (int previous)
{
  proclaimer = NULL;
  export_depth = previous;
}

int
module_exporting_level ()
{
  return export_depth;
}

/* Set the module EXPORT and INDEX fields on DECL.  */

void
decl_set_module (tree decl)
{
  // FIXME: check ill-formed linkage
  if (export_depth)
    DECL_MODULE_EXPORT_P (decl) = true;

  if (modules && (*modules)[0]->name)
    {
      retrofit_lang_decl (decl);
      DECL_MODULE_PURVIEW_P (decl) = true;
    }
}

/* Return true iff we're in the purview of a named module.  */

bool
module_purview_p ()
{
  return modules && (*modules)[0]->name;
}

/* Return true iff we're the interface TU (this also means we're in a
   module purview.  */

bool
module_interface_p ()
{
  return modules && (*modules)[0]->exported;
}

/* Convert a module name into a file name.  The name is malloced.
 */

static char *
module_to_filename (tree id, size_t &len)
{
  size_t id_len = IDENTIFIER_LENGTH (id);
  const char *id_chars = IDENTIFIER_POINTER (id);

  size_t sfx_len = strlen (MOD_FNAME_SFX);
  len = id_len + sfx_len;

  char *buffer = XNEWVEC (char, id_len + sfx_len + 1);
  memcpy (buffer, id_chars, id_len);
  memcpy (buffer + id_len, MOD_FNAME_SFX, sfx_len + 1);

  char dot = MOD_FNAME_DOT;
  if (dot != '.')
    for (char *ptr = buffer; id_len--; ptr++)
      if (*ptr == '.')
	*ptr = dot;

  return buffer;
}

/* Search the module path for a binary module file called NAME.
   Updates NAME with path found.  */

static FILE *
search_module_path (char *&name, size_t name_len, tree mname)
{
  char *buffer = XNEWVEC (char, module_path_max + name_len + 2);
  bool once = false;

 again:
  if (!IS_ABSOLUTE_PATH (name))
    for (const cpp_dir *dir = module_path; dir; dir = dir->next)
      {
	size_t len = 0;
	/* Don't prepend '.'.  */
	if (dir->len != 1 || dir->name[0] != '.')
	  {
	    memcpy (buffer, dir->name, dir->len);
	    len = dir->len;
	    buffer[len++] = DIR_SEPARATOR;
	  }
	memcpy (buffer + len, name, name_len + 1);

	if (FILE *stream = fopen (buffer, "rb"))
	  {
	    XDELETE (name);
	    name = buffer;
	    return stream;
	  }
      }
  else if (FILE *stream = fopen (buffer, "rb"))
    return stream;

  if (once)
    {
      inform (input_location, "module wrapper failed to install BMI");
      return NULL;
    }

  once = true;

  const char *str_name = IDENTIFIER_POINTER (mname);
  // FIXME: reference main source file here?  See below too.
  inform (UNKNOWN_LOCATION, "invoking module wrapper to install %qs",
	  str_name);

  /* wrapper <module-name> <module-bmi-file> <source-file> <this-file>
     We may want to pass the multilib directory fragment too.
     We may want to provide entire chain of imports.  */

  unsigned len = 0;
  const char *argv[6];
  argv[len++] = flag_module_wrapper;
  argv[len++] = str_name;
  argv[len++] = name;
  argv[len++] = main_input_filename;
  argv[len++] = expand_location (input_location).file;

  if (!quiet_flag)
    {
      if (pp_needs_newline (global_dc->printer))
	{
	  pp_needs_newline (global_dc->printer) = false;
	  fprintf (stderr, "\n");
	}
      fprintf (stderr, "%s module wrapper:", progname);
      for (unsigned ix = 0; ix != len; ix++)
	fprintf (stderr, "%s%s", &" "[!ix], argv[ix]);
      fprintf (stderr, "\n");
      fflush (stderr);
    }
  argv[len] = NULL;

  int err;
  const char *errmsg = NULL;
  int status;
  pex_obj *pex = pex_init (0, progname, NULL);
  if (pex)
    errmsg = pex_run (pex, PEX_LAST | PEX_SEARCH, argv[0],
		      const_cast <char **> (argv), NULL, NULL, &err);

  if (!pex || (!errmsg && !pex_get_status (pex, 1, &status)))
    errmsg = "cannot invoke";
  pex_free (pex);

  diagnostic_set_last_function (global_dc, (diagnostic_info *) NULL);

  if (errmsg)
    {
      errno = err;
      error_at (UNKNOWN_LOCATION, "%s %qs %m", errmsg, argv[0]);
    }
  else if (WIFSIGNALED (status))
    error_at (UNKNOWN_LOCATION, "module wrapper %qs died by signal %s",
	      argv[0], strsignal (WTERMSIG (status)));
  else if (WIFEXITED (status) && WEXITSTATUS (status) != 0)
    error_at (UNKNOWN_LOCATION, "module wrapper %qs exit status %d",
	      argv[0], WEXITSTATUS (status));
  else
    {
      inform (UNKNOWN_LOCATION, "completed module wrapper to install %qs",
	      str_name); 
      goto again;
    }

  return NULL;
}

static FILE *
make_module_file (char *&name, size_t name_len)
{
  size_t root_len = 0;
  if (flag_module_root)
    {
      root_len = strlen (flag_module_root);

      char *buffer = XNEWVEC (char, root_len + name_len + 2);
      memcpy (buffer, flag_module_root, root_len);
      buffer[root_len] = DIR_SEPARATOR;
      memcpy (buffer + root_len + 1, name, name_len + 1);

      XDELETE (name);
      name = buffer;
    }

  FILE *stream = fopen (name, "wb");
  if (!stream && root_len && errno == ENOENT)
    {
      /* Try and create the missing directories.  */
      char *base = name + root_len + 1;
      char *end = base + name_len;

      for (;; base++)
	{
	  /* There can only be one dir separator!  */
	  base = (char *)memchr (base, DIR_SEPARATOR, end - base);
	  if (!base)
	    break;

	  *base = 0;
	  int failed = mkdir (name, S_IRWXU | S_IRWXG | S_IRWXO);
	  *base = DIR_SEPARATOR;
	  if (failed
	      /* Maybe racing with another creator (of a *different*
		 submodule).  */
	      && errno != EEXIST)
	    return NULL;
	}

      /* Have another go.  */
      stream = fopen (name, "wb");
    }

  return stream;
}

/* Import the module NAME into the current TU.  This includes the TU's
   interface and as implementation.  Returns index of imported
   module (or MODULE_INDEX_ERROR).  */

unsigned
do_module_import (location_t loc, tree name, bool module_unit_p,
		  bool import_export_p, cpms_in *from, unsigned *crc_ptr)
{
  if (!module_hash)
    {
      module_hash = hash_table<module_state_hash>::create_ggc (31);
      vec_safe_reserve (modules, 20);
      module_state *current = new (ggc_alloc <module_state> ()) module_state ();
      modules->quick_push (current);
    }

  module_state **slot
    = module_hash->find_slot_with_hash (name, IDENTIFIER_HASH_VALUE (name),
					INSERT);
  module_state *state = *slot;

  if (state)
    /* We already know a module called NAME.  */
    switch (state->mod)
      {
      case MODULE_INDEX_IMPORTING:
	error_at (loc, "circular dependency of module %qE", name);
	return MODULE_INDEX_ERROR;
      case MODULE_INDEX_ERROR:
	error_at (loc, "already failed to read module %qE", name);
	return MODULE_INDEX_ERROR;
      case 0:
	/* Cannot import the current module.  */
	error_at (loc, "already declared as module %qE", name);
	return MODULE_INDEX_ERROR;
      default:
	if (module_unit_p)
	  {
	    /* Cannot be interface/implementation of an imported
	       module.  */
	    error_at (loc, "module %qE already imported", name);
	    return MODULE_INDEX_ERROR;
	  }
      }
  else
    {
      if (module_unit_p)
	state = (*modules)[0];
      else
	state = new (ggc_alloc<module_state> ()) module_state ();

      if (state->name)
	{
	  /* Already declared the module.  */
	  error_at (loc, "already declared as module %qE", state->name);
	  return MODULE_INDEX_ERROR;
	}

      state->set_name (name);
      *slot = state;

      if (module_unit_p && import_export_p)
	{
	  /* We're the exporting module unit, so not loading anything.  */
	  state->exported = true;
	}
      else if (!module_unit_p && !import_export_p)
	{
	  /* The ordering of the import table implies that indirect
	     imports should have already been loaded.  */
	  error ("indirect import %qE not present", name);
	  return MODULE_INDEX_ERROR;
	}
      else
	{
          /* First look in the module file map. If not found, fall back to the
             default mapping. */
          char *filename = NULL;
	  size_t filename_len;

	  if (char **slot = module_files.get (IDENTIFIER_POINTER (name)))
	    {
	      filename_len = strlen (*slot);
	      filename = XNEWVEC (char, filename_len + 1);
	      memcpy (filename, *slot, filename_len + 1);
	    }

          if (!filename)
            filename = module_to_filename (name, filename_len);

	  if (FILE *stream = search_module_path (filename, filename_len, name))
	    {
	      gcc_assert (global_namespace == current_scope ());
	      const char *str_name = IDENTIFIER_POINTER (state->name);
	      location_t saved_loc = input_location;

	      state->push_location (filename);

	      if (!quiet_flag)
		{
		  fprintf (stderr, " importing:%s(%s)",
			   str_name, state->filename);
		  fflush (stderr);
		  pp_needs_newline (global_dc->printer) = true;
		  diagnostic_set_last_function (global_dc,
						(diagnostic_info *) NULL);
		}

	      /* Note, read_module succeeds or never returns.  */
	      state->mod = MODULE_INDEX_IMPORTING;
	      {
		cpms_in in (stream, state, from);
		if (in.begin ())
		  in.read (crc_ptr);
		if (!in.end ())
		  /* Failure to read a module is going to cause big
		     problems, so bail out now.  */
		  fatal_error (input_location, "failed to read module %qE",
			       state->name);
		gcc_assert (state->mod <= modules->length ());
	      }

	      if (!quiet_flag)
		{
		  fprintf (stderr, " imported:%s(#%d)", str_name, state->mod);
		  fflush (stderr);
		  pp_needs_newline (global_dc->printer) = true;
		  diagnostic_set_last_function (global_dc,
						(diagnostic_info *) NULL);
		}

	      state->pop_location ();
	      input_location = saved_loc;

	      fclose (stream);
	    }
	  else
	    {
	      error_at (loc, "cannot find module %qE (%qs): %m",
			name, filename);
	      XDELETE (filename);
	    }
	}
    }

  return state->mod;
}

/* Import the module NAME into the current TU and maybe re-export it.  */

void
import_module (const cp_expr &name, tree)
{
  gcc_assert (global_namespace == current_scope ());
  unsigned index = do_module_import (name.get_location (), *name,
				     /*unit_p=*/false, /*import_p=*/true);
  if (index != MODULE_INDEX_ERROR)
    (*modules)[0]->do_import (index, export_depth != 0);
  gcc_assert (global_namespace == current_scope ());
}

/* Declare the name of the current module to be NAME.  EXPORTING_p is
   true if this TU is the exporting module unit.  */

void
declare_module (const cp_expr &name, bool exporting_p, tree)
{
  gcc_assert (global_namespace == current_scope ());

  unsigned index = do_module_import
    (name.get_location (), *name, true, exporting_p);
  gcc_assert (!index || index == MODULE_INDEX_ERROR);
}

/* Convert the module search path.  */

void
init_module_processing ()
{
  module_path = get_added_cpp_dirs (INC_CXX_MPATH);
  for (const cpp_dir *path = module_path; path; path = path->next)
    if (path->len > module_path_max)
      module_path_max = path->len;

  if (!flag_module_wrapper)
    {
      flag_module_wrapper = getenv ("CXX_MODULE_WRAPPER");
      if (!flag_module_wrapper)
	flag_module_wrapper = "false";
    }
}

/* Finalize the module at end of parsing.  */

void
finish_module ()
{
  if (modules)
    {
      /* GC can clean up the detritus.  */
      module_hash = NULL;
      for (unsigned ix = modules->length (); --ix >= MODULE_INDEX_IMPORT_BASE;)
	(*modules)[ix]->release ();
    }

  if (!module_interface_p ())
    {
      if (module_output)
	error ("-fmodule-output specified for non-module interface compilation");
      return;
    }

  module_state *state = (*modules)[0];
  char *filename;
  size_t filename_len;
  if (module_output)
    {
      filename_len = strlen (module_output);
      filename = XNEWVEC (char, filename_len + 1);
      memcpy (filename, module_output, filename_len + 1);
    }
  else
    filename = module_to_filename (state->name, filename_len);

  /* We don't bail out early with errorcount, because we want to
     remove the BMI file in the case of previous errors.  */
  if (FILE *stream = make_module_file (filename, filename_len))
    {
      if (!errorcount)
	{
	  cpms_out out (stream, state);

	  if (out.begin ())
	    out.write ();
	  out.end ();
	}
      fclose (stream);
    }
  else
    error_at (state->loc, "cannot open module interface %qE (%qs): %m",
	      state->name, filename);

  if (errorcount)
    unlink (filename);

  // FIXME filename is in module state?
  XDELETE (filename);
  (*modules)[0]->release ();
}

#include "gt-cp-module.h"

/* Use of vec<unsigned, va_gc> caused these fns to be needed.  */
void gt_ggc_mx (unsigned int &) {}
void gt_pch_nx (unsigned int &) {}
void gt_pch_nx (unsigned int *, void (*)(void *, void *), void *) {}
