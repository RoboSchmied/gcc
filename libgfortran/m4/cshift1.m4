`/* Implementation of the CSHIFT intrinsic
   Copyright 2003, 2007, 2009, 2012 Free Software Foundation, Inc.
   Contributed by Feng Wang <wf_cs@yahoo.com>

This file is part of the GNU Fortran runtime library (libgfortran).

Libgfortran is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public
License as published by the Free Software Foundation; either
version 3 of the License, or (at your option) any later version.

Ligbfortran is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

#include "libgfortran.h"
#include <stdlib.h>
#include <assert.h>
#include <string.h>'

include(iparm.m4)dnl

`#if defined (HAVE_'atype_name`)

static void
cshift1 (gfc_array_char * const restrict ret, 
	const gfc_array_char * const restrict array,
	const 'atype` * const restrict h, 
	const 'atype_name` * const restrict pwhich)
{
  /* r.* indicates the return array.  */
  index_type rstride[GFC_MAX_DIMENSIONS];
  index_type rstride0;
  index_type roffset;
  char *rptr;
  char *dest;
  /* s.* indicates the source array.  */
  index_type sstride[GFC_MAX_DIMENSIONS];
  index_type sstride0;
  index_type soffset;
  const char *sptr;
  const char *src;
  /* h.* indicates the shift array.  */
  index_type hstride[GFC_MAX_DIMENSIONS];
  index_type hstride0;
  const 'atype_name` *hptr;

  index_type count[GFC_MAX_DIMENSIONS];
  index_type extent[GFC_MAX_DIMENSIONS];
  index_type dim;
  index_type len;
  index_type n;
  int which;
  'atype_name` sh;
  index_type arraysize;
  index_type size;

  if (pwhich)
    which = *pwhich - 1;
  else
    which = 0;

  if (which < 0 || (which + 1) > GFC_DESCRIPTOR_RANK (array))
    runtime_error ("Argument ''`DIM''` is out of range in call to ''`CSHIFT''`");

  size = GFC_DESCRIPTOR_SIZE(array);

  arraysize = size0 ((array_t *)array);

  if (ret->base_addr == NULL)
    {
      int i;

      ret->base_addr = internal_malloc_size (size * arraysize);
      ret->offset = 0;
      ret->dtype = array->dtype;
      for (i = 0; i < GFC_DESCRIPTOR_RANK (array); i++)
        {
	  index_type ub, str;

          ub = GFC_DESCRIPTOR_EXTENT(array,i) - 1;

          if (i == 0)
            str = 1;
          else
	    str = GFC_DESCRIPTOR_EXTENT(ret,i-1) *
	      GFC_DESCRIPTOR_STRIDE(ret,i-1);

	  GFC_DIMENSION_SET(ret->dim[i], 0, ub, str);
        }
    }
  else if (unlikely (compile_options.bounds_check))
    {
      bounds_equal_extents ((array_t *) ret, (array_t *) array,
				 "return value", "CSHIFT");
    }

  if (unlikely (compile_options.bounds_check))
    {
      bounds_reduced_extents ((array_t *) h, (array_t *) array, which,
      			      "SHIFT argument", "CSHIFT");
    }

  if (arraysize == 0)
    return;

  extent[0] = 1;
  count[0] = 0;
  n = 0;

  /* Initialized for avoiding compiler warnings.  */
  roffset = size;
  soffset = size;
  len = 0;

  for (dim = 0; dim < GFC_DESCRIPTOR_RANK (array); dim++)
    {
      if (dim == which)
        {
          roffset = GFC_DESCRIPTOR_STRIDE_BYTES(ret,dim);
          if (roffset == 0)
            roffset = size;
          soffset = GFC_DESCRIPTOR_STRIDE_BYTES(array,dim);
          if (soffset == 0)
            soffset = size;
          len = GFC_DESCRIPTOR_EXTENT(array,dim);
        }
      else
        {
          count[n] = 0;
          extent[n] = GFC_DESCRIPTOR_EXTENT(array,dim);
          rstride[n] = GFC_DESCRIPTOR_STRIDE_BYTES(ret,dim);
          sstride[n] = GFC_DESCRIPTOR_STRIDE_BYTES(array,dim);

          hstride[n] = GFC_DESCRIPTOR_STRIDE(h,n);
          n++;
        }
    }
  if (sstride[0] == 0)
    sstride[0] = size;
  if (rstride[0] == 0)
    rstride[0] = size;
  if (hstride[0] == 0)
    hstride[0] = 1;

  dim = GFC_DESCRIPTOR_RANK (array);
  rstride0 = rstride[0];
  sstride0 = sstride[0];
  hstride0 = hstride[0];
  rptr = ret->base_addr;
  sptr = array->base_addr;
  hptr = h->base_addr;

  while (rptr)
    {
      /* Do the shift for this dimension.  */
      sh = *hptr;
      sh = (div (sh, len)).rem;
      if (sh < 0)
        sh += len;

      src = &sptr[sh * soffset];
      dest = rptr;

      for (n = 0; n < len; n++)
        {
          memcpy (dest, src, size);
          dest += roffset;
          if (n == len - sh - 1)
            src = sptr;
          else
            src += soffset;
        }

      /* Advance to the next section.  */
      rptr += rstride0;
      sptr += sstride0;
      hptr += hstride0;
      count[0]++;
      n = 0;
      while (count[n] == extent[n])
        {
          /* When we get to the end of a dimension, reset it and increment
             the next dimension.  */
          count[n] = 0;
          /* We could precalculate these products, but this is a less
             frequently used path so probably not worth it.  */
          rptr -= rstride[n] * extent[n];
          sptr -= sstride[n] * extent[n];
	  hptr -= hstride[n] * extent[n];
          n++;
          if (n >= dim - 1)
            {
              /* Break out of the loop.  */
              rptr = NULL;
              break;
            }
          else
            {
              count[n]++;
              rptr += rstride[n];
              sptr += sstride[n];
	      hptr += hstride[n];
            }
        }
    }
}

void cshift1_'atype_kind` (gfc_array_char * const restrict, 
	const gfc_array_char * const restrict,
	const 'atype` * const restrict, 
	const 'atype_name` * const restrict);
export_proto(cshift1_'atype_kind`);

void
cshift1_'atype_kind` (gfc_array_char * const restrict ret,
	const gfc_array_char * const restrict array,
	const 'atype` * const restrict h, 
	const 'atype_name` * const restrict pwhich)
{
  cshift1 (ret, array, h, pwhich);
}


void cshift1_'atype_kind`_char (gfc_array_char * const restrict ret, 
	GFC_INTEGER_4,
	const gfc_array_char * const restrict array,
	const 'atype` * const restrict h, 
	const 'atype_name` * const restrict pwhich,
	GFC_INTEGER_4);
export_proto(cshift1_'atype_kind`_char);

void
cshift1_'atype_kind`_char (gfc_array_char * const restrict ret,
	GFC_INTEGER_4 ret_length __attribute__((unused)),
	const gfc_array_char * const restrict array,
	const 'atype` * const restrict h, 
	const 'atype_name` * const restrict pwhich,
	GFC_INTEGER_4 array_length __attribute__((unused)))
{
  cshift1 (ret, array, h, pwhich);
}


void cshift1_'atype_kind`_char4 (gfc_array_char * const restrict ret, 
	GFC_INTEGER_4,
	const gfc_array_char * const restrict array,
	const 'atype` * const restrict h, 
	const 'atype_name` * const restrict pwhich,
	GFC_INTEGER_4);
export_proto(cshift1_'atype_kind`_char4);

void
cshift1_'atype_kind`_char4 (gfc_array_char * const restrict ret,
	GFC_INTEGER_4 ret_length __attribute__((unused)),
	const gfc_array_char * const restrict array,
	const 'atype` * const restrict h, 
	const 'atype_name` * const restrict pwhich,
	GFC_INTEGER_4 array_length __attribute__((unused)))
{
  cshift1 (ret, array, h, pwhich);
}

#endif'
