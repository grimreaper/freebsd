/*-
 * Copyright (c) 1987, 1991 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)endian.h	7.8 (Berkeley) 4/3/91
 * $FreeBSD$
 */

#ifndef _MACHINE_ENDIAN_H_
#define	_MACHINE_ENDIAN_H_

#include <sys/cdefs.h>
#include <sys/_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Define the order of 32-bit words in 64-bit words.
 */
#define	_QUAD_HIGHWORD 1
#define	_QUAD_LOWWORD 0

/*
 * Definitions for byte order, according to byte significance from low
 * address to high.
 */
#define	_LITTLE_ENDIAN	1234	/* LSB first: i386, vax */
#define	_BIG_ENDIAN	4321	/* MSB first: 68000, ibm, net */
#define	_PDP_ENDIAN	3412	/* LSB first in word, MSW first in long */

#define	_BYTE_ORDER	_LITTLE_ENDIAN

/*
 * Deprecated variants that don't have enough underscores to be useful in more
 * strict namespaces.
 */
#if __BSD_VISIBLE
#define	LITTLE_ENDIAN	_LITTLE_ENDIAN
#define	BIG_ENDIAN	_BIG_ENDIAN
#define	PDP_ENDIAN	_PDP_ENDIAN
#define	BYTE_ORDER	_BYTE_ORDER
#endif

#if defined(__GNUCLIKE_ASM) && defined(__GNUCLIKE_BUILTIN_CONSTANT_P)

static __inline __uint32_t
__byte_swap_int_var(__uint32_t x)
{
	register __uint32_t _x;


	_x = x;
	__asm ("bswap %0" : "+r" (_x));
	return (_x);
}

#ifdef __OPTIMIZE__

#define	__byte_swap_int_const(x) \
	((((x) & 0xff000000) >> 24) | \
	 (((x) & 0x00ff0000) >>  8) | \
	 (((x) & 0x0000ff00) <<  8) | \
	 (((x) & 0x000000ff) << 24))
#define	__bswap32(x)	(__builtin_constant_p(x) ? \
	__byte_swap_int_const(x) : __byte_swap_int_var(x))

#else	/* __OPTIMIZE__ */

#define	__bswap32(x)	__byte_swap_int_var(x)

#endif	/* __OPTIMIZE__ */

static __inline __uint64_t
__byte_swap_long_var(__uint64_t x)
{
	register __uint64_t _x;

	_x = x;
	__asm ("bswap %0" : "+r" (_x));
	return (_x);
}

#ifdef __OPTIMIZE__

#define	__byte_swap_long_const(x) \
	((__uint64_t)(((__uint64_t)x >> 56) |		\
	 (((__uint64_t)x >> 40) & 0xff00) |		\
	 (((__uint64_t)x >> 24) & 0xff0000) |		\
	 (((__uint64_t)x >> 8) & 0xff000000) |		\
	 (((__uint64_t)x << 8) & (0xfful << 32)) |	\
	 (((__uint64_t)x << 24) & (0xfful << 40)) |	\
	 (((__uint64_t)x << 40) & (0xfful << 48)) |	\
	 (((__uint64_t)x << 56))))

#define	__bswap64(x)	(__builtin_constant_p(x) ? \
	__byte_swap_long_const(x) : __byte_swap_long_var(x))

#else	/* __OPTIMIZE__ */

#define	__bswap64(x)	__byte_swap_long_var(x)

#endif	/* __OPTIMIZE__ */

#define	__byte_swap_short_const(_x)	(__uint16_t)((_x) << 8 | (_x) >> 8)

static __inline __uint16_t
__byte_swap_short_var(__uint16_t x)
{
	return __byte_swap_short_const(x);
}

#define	__bswap16(x)	(__builtin_constant_p(x) ? \
	__byte_swap_short_const(x) : __byte_swap_short_var(x))

#define	__htonl(x)	__bswap32(x)
#define	__htons(x)	__bswap16(x)
#define	__ntohl(x)	__bswap32(x)
#define	__ntohs(x)	__bswap16(x)

#else /* !(__GNUCLIKE_ASM && __GNUCLIKE_BUILTIN_CONSTANT_P) */

/*
 * No optimizations are available for this compiler.  Fall back to
 * non-optimized functions by defining the constant usually used to prevent
 * redefinition.
 */
#define	_BYTEORDER_FUNC_DEFINED

#endif /* __GNUCLIKE_ASM && __GNUCLIKE_BUILTIN_CONSTANT_P */

#ifdef __cplusplus
}
#endif

#endif /* !_MACHINE_ENDIAN_H_ */
