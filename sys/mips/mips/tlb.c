/*-
 * Copyright (c) 2004-2010 Juli Mallett <jmallett@FreeBSD.org>
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/pcpu.h>

#include <vm/vm.h>
#include <vm/vm_page.h>

#include <machine/pte.h>
#include <machine/tlb.h>

#if 0
/*
 * PageMask must increment in steps of 2 bits.
 */
COMPILE_TIME_ASSERT(POPCNT(TLBMASK_MASK) % 2 == 0);
#endif

static inline void
tlb_probe(void)
{
	__asm __volatile ("tlbp" : : : "memory");
	mips_barrier();
}

static inline void
tlb_read(void)
{
	__asm __volatile ("tlbr" : : : "memory");
	mips_barrier();
}

static inline void
tlb_write_indexed(void)
{
	__asm __volatile ("tlbwi" : : : "memory");
	mips_barrier();
}

static inline void
tlb_write_random(void)
{
	__asm __volatile ("tlbwr" : : : "memory");
	mips_barrier();
}

static void tlb_invalidate_one(unsigned);

void
tlb_invalidate_address(struct pmap *pmap, vm_offset_t va)
{
	register_t s;
	int i;

	va &= ~PAGE_MASK;

	s = intr_disable();
	mips_wr_entryhi(TLBHI_ENTRY(va, pmap_asid(pmap)));
	tlb_probe();
	i = mips_rd_index();
	if (i >= 0)
		tlb_invalidate_one(i);
	intr_restore(s);
}

void
tlb_invalidate_all(void)
{
	register_t s;
	unsigned i;

	s = intr_disable();
	for (i = mips_rd_wired(); i < num_tlbentries; i++)
		tlb_invalidate_one(i);
	intr_restore(s);
}

void
tlb_update(struct pmap *pmap, vm_offset_t va, pt_entry_t pte)
{
	register_t asid;
	register_t s;
	int i;

	va &= ~PAGE_MASK;

	s = intr_disable();
	asid = mips_rd_entryhi();
	mips_wr_entryhi(TLBHI_ENTRY(va, pmap_asid(pmap)));
	tlb_probe();
	i = mips_rd_index();
	mips_wr_entrylo0(pte);
	mips_wr_entrylo1(pte + TLBLO_PFN_ODD);
	if (i >= 0)
		tlb_write_indexed();
	else
		tlb_write_random();
	mips_wr_entryhi(asid);
	intr_restore(s);
}

static void
tlb_invalidate_one(unsigned i)
{
	/* XXX an invalid ASID? */
	mips_wr_entryhi(TLBHI_ENTRY(MIPS_KSEG0_START + (i * PAGE_SIZE), 0));
	mips_wr_entrylo0(0);
	mips_wr_entrylo1(0);
	mips_wr_index(i);
	tlb_write_indexed();
}
