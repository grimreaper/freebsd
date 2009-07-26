/*-
 * Copyright (c) 2001 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Klaus Klein.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *	From: $NetBSD: int_fmtio.h,v 1.2 2001/04/26 16:25:21 kleink Exp $
 * $FreeBSD$
 */

#ifndef _MACHINE_INTTYPES_H_
#define _MACHINE_INTTYPES_H_

/*
 * Macros for format specifiers.
 */

#ifdef __powerpc64__
#define PRI64		"l"
#define PRIreg		"l"
#else
#define PRI64		"ll"
#define PRIreg
#endif

/* fprintf(3) macros for signed integers. */

#define	PRId8		"d"	/* int8_t */
#define	PRId16		"d"	/* int16_t */
#define	PRId32		"d"	/* int32_t */
#define	PRId64		PRI64"d" /* int64_t */
#define	PRIdLEAST8	"d"	/* int_least8_t */
#define	PRIdLEAST16	"d"	/* int_least16_t */
#define	PRIdLEAST32	"d"	/* int_least32_t */
#define	PRIdLEAST64	PRI64"d" /* int_least64_t */
#define	PRIdFAST8	"d"	/* int_fast8_t */
#define	PRIdFAST16	"d"	/* int_fast16_t */
#define	PRIdFAST32	"d"	/* int_fast32_t */
#define	PRIdFAST64	PRI64"d" /* int_fast64_t */
#define	PRIdMAX		"jd"	/* intmax_t */
#define	PRIdPTR		PRIreg"d" /* intptr_t */

#define	PRIi8		"i"	/* int8_t */
#define	PRIi16		"i"	/* int16_t */
#define	PRIi32		"i"	/* int32_t */
#define	PRIi64		PRI64"i" /* int64_t */
#define	PRIiLEAST8	"i"	/* int_least8_t  */
#define	PRIiLEAST16	"i"	/* int_least16_t */
#define	PRIiLEAST32	"i"	/* int_least32_t */
#define	PRIiLEAST64	PRI64"i" /* int_least64_t */
#define	PRIiFAST8	"i"	/* int_fast8_t */
#define	PRIiFAST16	"i"	/* int_fast16_t */
#define	PRIiFAST32	"i"	/* int_fast32_t */
#define	PRIiFAST64	PRI64"i" /* int_fast64_t */
#define	PRIiMAX		"ji"	/* intmax_t */
#define	PRIiPTR		PRIreg"i" /* intptr_t */

/* fprintf(3) macros for unsigned integers. */

#define	PRIo8		"o"	/* uint8_t */
#define	PRIo16		"o"	/* uint16_t */
#define	PRIo32		"o"	/* uint32_t */
#define	PRIo64		PRI64"o" /* uint64_t */
#define	PRIoLEAST8	"o"	/* uint_least8_t */
#define	PRIoLEAST16	"o"	/* uint_least16_t */
#define	PRIoLEAST32	"o"	/* uint_least32_t */
#define	PRIoLEAST64	PRI64"o" /* uint_least64_t */
#define	PRIoFAST8	"o"	/* uint_fast8_t */
#define	PRIoFAST16	"o"	/* uint_fast16_t */
#define	PRIoFAST32	"o"	/* uint_fast32_t */
#define	PRIoFAST64	PRI64"o" /* uint_fast64_t */
#define	PRIoMAX		"jo"	/* uintmax_t */
#define	PRIoPTR		PRIreg"o" /* uintptr_t */

#define	PRIu8		"u"	/* uint8_t */
#define	PRIu16		"u"	/* uint16_t */
#define	PRIu32		"u"	/* uint32_t */
#define	PRIu64		PRI64"u" /* uint64_t */
#define	PRIuLEAST8	"u"	/* uint_least8_t */
#define	PRIuLEAST16	"u"	/* uint_least16_t */
#define	PRIuLEAST32	"u"	/* uint_least32_t */
#define	PRIuLEAST64	PRI64"u" /* uint_least64_t */
#define	PRIuFAST8	"u"	/* uint_fast8_t */
#define	PRIuFAST16	"u"	/* uint_fast16_t */
#define	PRIuFAST32	"u"	/* uint_fast32_t */
#define	PRIuFAST64	PRI64"u" /* uint_fast64_t */
#define	PRIuMAX		"ju"	/* uintmax_t */
#define	PRIuPTR		PRIreg"u" /* uintptr_t */

#define	PRIx8		"x"	/* uint8_t */
#define	PRIx16		"x"	/* uint16_t */
#define	PRIx32		"x"	/* uint32_t */
#define	PRIx64		PRI64"x" /* uint64_t */
#define	PRIxLEAST8	"x"	/* uint_least8_t */
#define	PRIxLEAST16	"x"	/* uint_least16_t */
#define	PRIxLEAST32	"x"	/* uint_least32_t */
#define	PRIxLEAST64	PRI64"x" /* uint_least64_t */
#define	PRIxFAST8	"x"	/* uint_fast8_t */
#define	PRIxFAST16	"x"	/* uint_fast16_t */
#define	PRIxFAST32	"x"	/* uint_fast32_t */
#define	PRIxFAST64	PRI64"x" /* uint_fast64_t */
#define	PRIxMAX		"jx"	/* uintmax_t */
#define	PRIxPTR		PRIreg"x" /* uintptr_t */

#define	PRIX8		"X"	/* uint8_t */
#define	PRIX16		"X"	/* uint16_t */
#define	PRIX32		"X"	/* uint32_t */
#define	PRIX64		PRI64"X" /* uint64_t */
#define	PRIXLEAST8	"X"	/* uint_least8_t */
#define	PRIXLEAST16	"X"	/* uint_least16_t */
#define	PRIXLEAST32	"X"	/* uint_least32_t */
#define	PRIXLEAST64	PRI64"X" /* uint_least64_t */
#define	PRIXFAST8	"X"	/* uint_fast8_t */
#define	PRIXFAST16	"X"	/* uint_fast16_t */
#define	PRIXFAST32	"X"	/* uint_fast32_t */
#define	PRIXFAST64	PRI64"X" /* uint_fast64_t */
#define	PRIXMAX		"jX"	/* uintmax_t */
#define	PRIXPTR		PRIreg"X" /* uintptr_t */

/* fscanf(3) macros for signed integers. */

#define	SCNd8		"hhd"	/* int8_t */
#define	SCNd16		"hd"	/* int16_t */
#define	SCNd32		"d"	/* int32_t */
#define	SCNd64		PRI64"d" /* int64_t */
#define	SCNdLEAST8	"hhd"	/* int_least8_t */
#define	SCNdLEAST16	"hd"	/* int_least16_t */
#define	SCNdLEAST32	"d"	/* int_least32_t */
#define	SCNdLEAST64	PRI64"d" /* int_least64_t */
#define	SCNdFAST8	"d"	/* int_fast8_t */
#define	SCNdFAST16	"d"	/* int_fast16_t */
#define	SCNdFAST32	"d"	/* int_fast32_t */
#define	SCNdFAST64	PRI64"d" /* int_fast64_t */
#define	SCNdMAX		"jd"	/* intmax_t */
#define	SCNdPTR		PRIreg"d" /* intptr_t */

#define	SCNi8		"hhi"	/* int8_t */
#define	SCNi16		"hi"	/* int16_t */
#define	SCNi32		"i"	/* int32_t */
#define	SCNi64		PRI64"i" /* int64_t */
#define	SCNiLEAST8	"hhi"	/* int_least8_t */
#define	SCNiLEAST16	"hi"	/* int_least16_t */
#define	SCNiLEAST32	"i"	/* int_least32_t */
#define	SCNiLEAST64	PRI64"i" /* int_least64_t */
#define	SCNiFAST8	"i"	/* int_fast8_t */
#define	SCNiFAST16	"i"	/* int_fast16_t */
#define	SCNiFAST32	"i"	/* int_fast32_t */
#define	SCNiFAST64	PRI64"i" /* int_fast64_t */
#define	SCNiMAX		"ji"	/* intmax_t */
#define	SCNiPTR		PRIreg"i" /* intptr_t */

/* fscanf(3) macros for unsigned integers. */

#define	SCNo8		"hho"	/* uint8_t */
#define	SCNo16		"ho"	/* uint16_t */
#define	SCNo32		"o"	/* uint32_t */
#define	SCNo64		PRI64"o" /* uint64_t */
#define	SCNoLEAST8	"hho"	/* uint_least8_t */
#define	SCNoLEAST16	"ho"	/* uint_least16_t */
#define	SCNoLEAST32	"o"	/* uint_least32_t */
#define	SCNoLEAST64	PRI64"o" /* uint_least64_t */
#define	SCNoFAST8	"o"	/* uint_fast8_t */
#define	SCNoFAST16	"o"	/* uint_fast16_t */
#define	SCNoFAST32	"o"	/* uint_fast32_t */
#define	SCNoFAST64	PRI64"o" /* uint_fast64_t */
#define	SCNoMAX		"jo"	/* uintmax_t */
#define	SCNoPTR		PRIreg"o" /* uintptr_t */

#define	SCNu8		"hhu"	/* uint8_t */
#define	SCNu16		"hu"	/* uint16_t */
#define	SCNu32		"u"	/* uint32_t */
#define	SCNu64		PRI64"u" /* uint64_t */
#define	SCNuLEAST8	"hhu"	/* uint_least8_t */
#define	SCNuLEAST16	"hu"	/* uint_least16_t */
#define	SCNuLEAST32	"u"	/* uint_least32_t */
#define	SCNuLEAST64	PRI64"u" /* uint_least64_t */
#define	SCNuFAST8	"u"	/* uint_fast8_t */
#define	SCNuFAST16	"u"	/* uint_fast16_t */
#define	SCNuFAST32	"u"	/* uint_fast32_t */
#define	SCNuFAST64	PRI64"u" /* uint_fast64_t */
#define	SCNuMAX		"ju"	/* uintmax_t */
#define	SCNuPTR		PRIreg"u" /* uintptr_t */

#define	SCNx8		"hhx"	/* uint8_t */
#define	SCNx16		"hx"	/* uint16_t */
#define	SCNx32		"x"	/* uint32_t */
#define	SCNx64		PRI64"x" /* uint64_t */
#define	SCNxLEAST8	"hhx"	/* uint_least8_t */
#define	SCNxLEAST16	"hx"	/* uint_least16_t */
#define	SCNxLEAST32	"x"	/* uint_least32_t */
#define	SCNxLEAST64	PRI64"x" /* uint_least64_t */
#define	SCNxFAST8	"x"	/* uint_fast8_t */
#define	SCNxFAST16	"x"	/* uint_fast16_t */
#define	SCNxFAST32	"x"	/* uint_fast32_t */
#define	SCNxFAST64	PRI64"x" /* uint_fast64_t */
#define	SCNxMAX		"jx"	/* uintmax_t */
#define	SCNxPTR		PRIreg"x" /* uintptr_t */

#endif /* !_MACHINE_INTTYPES_H_ */
