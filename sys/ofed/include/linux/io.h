/*-
 * Copyright (c) 2010 Isilon Systems, Inc.
 * Copyright (c) 2010 iX Systems, Inc.
 * Copyright (c) 2010 Panasas, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef	_LINUX_IO_H_
#define	_LINUX_IO_H_

#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/pmap.h>

static inline uint32_t
__raw_readl(const volatile void *addr)
{
	return *(const volatile uint32_t *)addr;
}

static inline void
__raw_writel(uint32_t b, volatile void *addr)
{
	*(volatile uint32_t *)addr = b;
}

static inline uint64_t
__raw_readq(const volatile void *addr)
{
	return *(const volatile uint64_t *)addr;
}

static inline void
__raw_writeq(uint64_t b, volatile void *addr)
{
	*(volatile uint64_t *)addr = b;
}

/*
 * XXX This is all x86 specific.  It should be bus space access.
 */
#define mmiowb()

#undef writel
static inline void
writel(uint32_t b, void *addr)
{
        *(volatile uint32_t *)addr = b;
}

#undef writeq
static inline void
writeq(uint64_t b, void *addr)
{
        *(volatile uint64_t *)addr = b;
}

#undef writeb
static inline void
writeb(uint8_t b, void *addr)
{
        *(volatile uint8_t *)addr = b;
}

#undef writew
static inline void
writew(uint16_t b, void *addr)
{
        *(volatile uint16_t *)addr = b;
}

#define	ioremap	pmap_mapdev

#define	memset_io(a, b, c)	memset((a), (b), (c))
#define	memcpy_fromio(a, b, c)	memcpy((a), (b), (c))
#define	memcpy_toio(a, b, c)	memcpy((a), (b), (c))

/*
 * iounmap is not defined as pmap_unmapdev requires a length that can
 * not easily be determined on BSD.
 */

#endif	/* _LINUX_IO_H_ */
