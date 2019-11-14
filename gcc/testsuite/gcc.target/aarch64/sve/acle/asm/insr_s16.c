/* { dg-final { check-function-bodies "**" "" "-DCHECK_ASM" } } */

#include "test_sve_acle.h"

/*
** insr_w0_s16_tied1:
**	insr	z0\.h, w0
**	ret
*/
TEST_UNIFORM_ZX (insr_w0_s16_tied1, svint16_t, int16_t,
		 z0 = svinsr_n_s16 (z0, x0),
		 z0 = svinsr (z0, x0))

/*
** insr_w0_s16_untied:
**	movprfx	z0, z1
**	insr	z0\.h, w0
**	ret
*/
TEST_UNIFORM_ZX (insr_w0_s16_untied, svint16_t, int16_t,
		 z0 = svinsr_n_s16 (z1, x0),
		 z0 = svinsr (z1, x0))

/*
** insr_0_s16_tied1:
**	insr	z0\.h, wzr
**	ret
*/
TEST_UNIFORM_Z (insr_0_s16_tied1, svint16_t,
		z0 = svinsr_n_s16 (z0, 0),
		z0 = svinsr (z0, 0))

/*
** insr_0_s16_untied:
**	movprfx	z0, z1
**	insr	z0\.h, wzr
**	ret
*/
TEST_UNIFORM_Z (insr_0_s16_untied, svint16_t,
		z0 = svinsr_n_s16 (z1, 0),
		z0 = svinsr (z1, 0))

/*
** insr_1_s16:
** (
**	mov	(w[0-9]+), #?1
**	insr	z0\.h, \1
** |
**	movi	v([0-9]+)\.4h, 0x1
**	insr	z0\.h, h\2
** )
**	ret
*/
TEST_UNIFORM_Z (insr_1_s16, svint16_t,
		z0 = svinsr_n_s16 (z0, 1),
		z0 = svinsr (z0, 1))
