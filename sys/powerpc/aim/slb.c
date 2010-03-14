/*-
 * Copyright (c) 2010 Nathan Whitehorn
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
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

#include <sys/param.h>
#include <sys/kdb.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/vmparam.h>

struct slb *
va_to_slb_entry(pmap_t pm, vm_offset_t va)
{
	uint64_t slbe, i;

	slbe = (uintptr_t)va >> ADDR_SR_SHFT;
	slbe = (slbe << SLBE_ESID_SHIFT) | SLBE_VALID;

	for (i = 0; i < sizeof(pm->pm_slb)/sizeof(pm->pm_slb[0]); i++) {
		if (pm->pm_slb[i].slbe == (slbe | i))
			return &pm->pm_slb[i];
	}

	/* XXX: Have a long list for processes mapping more than 16 GB */

	return (0);
}

uint64_t
va_to_vsid(pmap_t pm, vm_offset_t va)
{
	struct slb *entry;

	entry = va_to_slb_entry(pm, va);

	/*
	 * If there is no vsid for this VA, we need to add a new entry
	 * to the PMAP's segment table.
	 * 
	 * XXX We assume (for now) that we are not mapping large pages.
	 */

	if (entry == NULL)
		return (allocate_vsid(pm, (uintptr_t)va >> ADDR_SR_SHFT, 0));

	return ((entry->slbv & SLBV_VSID_MASK) >> SLBV_VSID_SHIFT);
}

uintptr_t moea64_get_unique_vsid(void);

uint64_t
allocate_vsid(pmap_t pm, uint64_t esid, int large)
{
	uint64_t vsid;
	struct slb slb_entry;

	vsid = moea64_get_unique_vsid();

	slb_entry.slbe = (esid << SLBE_ESID_SHIFT) | SLBE_VALID;
	slb_entry.slbv = vsid << SLBV_VSID_SHIFT;

	if (large)
		slb_entry.slbv |= SLBV_L;

	/*
	 * Someone probably wants this soon, and it may be a wired
	 * SLB mapping, so pre-spill this entry.
	 */
	slb_insert(pm, &slb_entry, 1);

	return (vsid);
}

#ifdef NOTYET /* We don't have a back-up list. Spills are a bad idea. */
/* Lock entries mapping kernel text and stacks */

#define SLB_SPILLABLE(slbe) \
	(((slbe & SLBE_ESID_MASK) < VM_MIN_KERNEL_ADDRESS && \
	    (slbe & SLBE_ESID_MASK) > SEGMENT_LENGTH) || \
	    (slbe & SLBE_ESID_MASK) > VM_MAX_KERNEL_ADDRESS)
#else
#define SLB_SPILLABLE(slbe) 0
#endif

void
slb_insert(pmap_t pm, struct slb *slb_entry, int prefer_empty)
{
	uint64_t slbe, slbv;
	int i, j, to_spill;

	to_spill = -1;
	slbv = slb_entry->slbv;
	slbe = slb_entry->slbe;

	/* Hunt for a likely candidate */

	for (i = mftb() % 64, j = 0; j < 64; j++, i = (i+1) % 64) {
		if (pm == kernel_pmap && i == USER_SR)
				continue;

		if (to_spill == 0 && (pm->pm_slb[i].slbe & SLBE_VALID) &&
		    (pm != kernel_pmap || SLB_SPILLABLE(pm->pm_slb[i].slbe))) {
			to_spill = i;
			if (!prefer_empty)
				break;
		}

		if (!(pm->pm_slb[i].slbe & SLBE_VALID)) {
			to_spill = i;
			break;
		}
	}

	if (to_spill < 0)
		panic("SLB spill on ESID %#lx, but no available candidates!\n",
		   (slbe & SLBE_ESID_MASK) >> SLBE_ESID_SHIFT);

	pm->pm_slb[to_spill].slbv = slbv;
	pm->pm_slb[to_spill].slbe = slbe | to_spill;

	if (pm == kernel_pmap && pmap_bootstrapped) {
		/* slbie not required */
		__asm __volatile ("slbmte %0, %1" :: 
		    "r"(kernel_pmap->pm_slb[i].slbv),
		    "r"(kernel_pmap->pm_slb[i].slbe)); 
	}
}

