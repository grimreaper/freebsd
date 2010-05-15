/*-
 * Copyright (c) 1991 Regents of the University of California.
 * All rights reserved.
 * Copyright (c) 1994 John S. Dyson
 * All rights reserved.
 * Copyright (c) 1994 David Greenman
 * All rights reserved.
 * Copyright (c) 2003 Peter Wemm
 * All rights reserved.
 * Copyright (c) 2005-2010 Alan L. Cox <alc@cs.rice.edu>
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and William Jolitz of UUNET Technologies Inc.
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
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
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
 *	from:	@(#)pmap.c	7.7 (Berkeley)	5/12/91
 */
/*-
 * Copyright (c) 2003 Networks Associates Technology, Inc.
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Jake Burkholder,
 * Safeport Network Services, and Network Associates Laboratories, the
 * Security Research Division of Network Associates, Inc. under
 * DARPA/SPAWAR contract N66001-01-C-8035 ("CBOSS"), as part of the DARPA
 * CHATS research program.
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#include "opt_msgbuf.h"
#include "opt_pmap.h"
#include "opt_vm.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/msgbuf.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/vmmeter.h>
#include <sys/sched.h>
#include <sys/sysctl.h>
#ifdef SMP
#include <sys/smp.h>
#endif

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include <vm/vm_pageout.h>
#include <vm/vm_pager.h>
#include <vm/vm_reserv.h>
#include <vm/uma.h>

#include <machine/cpu.h>
#include <machine/cputypes.h>
#include <machine/md_var.h>
#include <machine/pcb.h>
#include <machine/specialreg.h>
#ifdef SMP
#include <machine/smp.h>
#endif

#ifndef PMAP_SHPGPERPROC
#define PMAP_SHPGPERPROC 200
#endif

#if !defined(DIAGNOSTIC)
#define PMAP_INLINE	__gnu89_inline
#else
#define PMAP_INLINE
#endif

#define PV_STATS
#ifdef PV_STATS
#define PV_STAT(x)	do { x ; } while (0)
#else
#define PV_STAT(x)	do { } while (0)
#endif

#define	CACHE_LINE_FETCH_SIZE	128
#define	PA_LOCK_PAD		CACHE_LINE_FETCH_SIZE

struct vp_lock {
	struct mtx	vp_lock;
	unsigned char	pad[(PA_LOCK_PAD - sizeof(struct mtx))];
};

#define	pa_index(pa)	((pa) >> PDRSHIFT)
#define	pa_to_pvh(pa)	(&pv_table[pa_index(pa)])

#define	PA_LOCKPTR(pa)	&pa_lock[pa_index((pa)) % PA_LOCK_COUNT].vp_lock
#define	PA_LOCK(pa)	mtx_lock(PA_LOCKPTR(pa))
#define	PA_TRYLOCK(pa)	mtx_trylock(PA_LOCKPTR(pa))
#define	PA_UNLOCK(pa)	mtx_unlock(PA_LOCKPTR(pa))
#define	PA_LOCK_ASSERT(pa, a)	mtx_assert(PA_LOCKPTR(pa), (a))

#define	PA_LOCK_COUNT	64

struct mtx pv_lock __aligned(128);
struct vp_lock pa_lock[PA_LOCK_COUNT] __aligned(128);


struct pmap kernel_pmap_store;

vm_offset_t virtual_avail;	/* VA of first avail page (after kernel bss) */
vm_offset_t virtual_end;	/* VA of last avail page (end of kernel AS) */

static int ndmpdp;
static vm_paddr_t dmaplimit;
vm_offset_t kernel_vm_end = VM_MIN_KERNEL_ADDRESS;
pt_entry_t pg_nx;

SYSCTL_NODE(_vm, OID_AUTO, pmap, CTLFLAG_RD, 0, "VM/pmap parameters");

static int pg_ps_enabled = 1;
SYSCTL_INT(_vm_pmap, OID_AUTO, pg_ps_enabled, CTLFLAG_RDTUN, &pg_ps_enabled, 0,
    "Are large page mappings enabled?");

static uint64_t pmap_tryrelock_calls;
SYSCTL_QUAD(_vm_pmap, OID_AUTO, tryrelock_calls, CTLFLAG_RD,
    &pmap_tryrelock_calls, 0, "Number of tryrelock calls");

static int pmap_tryrelock_restart;
SYSCTL_INT(_vm_pmap, OID_AUTO, tryrelock_restart, CTLFLAG_RD,
    &pmap_tryrelock_restart, 0, "Number of tryrelock restarts");


static u_int64_t	KPTphys;	/* phys addr of kernel level 1 */
static u_int64_t	KPDphys;	/* phys addr of kernel level 2 */
u_int64_t		KPDPphys;	/* phys addr of kernel level 3 */
u_int64_t		KPML4phys;	/* phys addr of kernel level 4 */

static u_int64_t	DMPDphys;	/* phys addr of direct mapped level 2 */
static u_int64_t	DMPDPphys;	/* phys addr of direct mapped level 3 */

/*
 * Data for the pv entry allocation mechanism
 */
static int pv_entry_count = 0, pv_entry_max = 0, pv_entry_high_water = 0;
static struct md_page *pv_table;
static int shpgperproc = PMAP_SHPGPERPROC;

/*
 * All those kernel PT submaps that BSD is so fond of
 */
pt_entry_t *CMAP1 = 0;
caddr_t CADDR1 = 0;
struct msgbuf *msgbufp = 0;

/*
 * Crashdump maps.
 */
static caddr_t crashdumpmap;

static void	free_pv_entry(pmap_t pmap, pv_entry_t pv);
static pv_entry_t get_pv_entry(pmap_t locked_pmap);
static void pmap_pv_demote_pde(pmap_t pmap, vm_offset_t va, vm_paddr_t pa,
	struct pv_list_head *pv_list);
static boolean_t pmap_pv_insert_pde(pmap_t pmap, vm_offset_t va, vm_paddr_t pa);
static void	pmap_pv_promote_pde(pmap_t pmap, vm_offset_t va, vm_paddr_t pa);
static void	pmap_pvh_free(struct md_page *pvh, pmap_t pmap, vm_offset_t va);
static pv_entry_t pmap_pvh_remove(struct md_page *pvh, pmap_t pmap,
		    vm_offset_t va);

static int pmap_change_attr_locked(vm_offset_t va, vm_size_t size, int mode);
static boolean_t pmap_demote_pde(pmap_t pmap, pd_entry_t *pde, vm_offset_t va,
	struct pv_list_head *pv_list);
static boolean_t pmap_enter_pde(pmap_t pmap, vm_offset_t va, vm_page_t m,
    vm_prot_t prot);
static vm_page_t pmap_enter_quick_locked(pmap_t pmap, vm_offset_t va,
    vm_page_t m, vm_prot_t prot, vm_page_t mpte);
static void pmap_fill_ptp(pt_entry_t *firstpte, pt_entry_t newpte);
static void pmap_insert_pt_page(pmap_t pmap, vm_page_t mpte);
static void pmap_invalidate_cache_range(vm_offset_t sva, vm_offset_t eva);
static boolean_t pmap_is_modified_pvh(struct md_page *pvh);
static void pmap_kenter_attr(vm_offset_t va, vm_paddr_t pa, int mode);
static vm_page_t pmap_lookup_pt_page(pmap_t pmap, vm_offset_t va);
static void pmap_pde_attr(pd_entry_t *pde, int cache_bits);
static void pmap_promote_pde(pmap_t pmap, pd_entry_t *pde, vm_offset_t va);
static boolean_t pmap_protect_pde(pmap_t pmap, pd_entry_t *pde, vm_offset_t sva,
    vm_prot_t prot);
static void pmap_pte_attr(pt_entry_t *pte, int cache_bits);
static int pmap_remove_pde(pmap_t pmap, pd_entry_t *pdq, vm_offset_t sva,
    vm_page_t *free, struct pv_list_head *pv_list);
static int pmap_remove_pte(pmap_t pmap, pt_entry_t *ptq,
		vm_offset_t sva, pd_entry_t ptepde, vm_page_t *free);
static void pmap_remove_pt_page(pmap_t pmap, vm_page_t mpte);
static void pmap_remove_page(pmap_t pmap, vm_offset_t va, pd_entry_t *pde,
    vm_page_t *free);
static void pmap_remove_entry(struct pmap *pmap, vm_page_t m,
		vm_offset_t va);
static boolean_t pmap_try_insert_pv_entry(pmap_t pmap, vm_offset_t va,
    vm_page_t m);
static void pmap_update_pde(pmap_t pmap, vm_offset_t va, pd_entry_t *pde,
    pd_entry_t newpde);
static void pmap_update_pde_invalidate(vm_offset_t va, pd_entry_t newpde);

static vm_page_t pmap_allocpde(pmap_t pmap, vm_paddr_t pa, vm_offset_t va, int flags);
static vm_page_t pmap_allocpte(pmap_t pmap, vm_paddr_t pa, vm_offset_t va, int flags);

static vm_page_t _pmap_allocpte(pmap_t pmap, vm_paddr_t pa, vm_pindex_t ptepindex, int flags);
static int _pmap_unwire_pte_hold(pmap_t pmap, vm_offset_t va, vm_page_t m,
                vm_page_t* free);
static int pmap_unuse_pt(pmap_t, vm_offset_t, pd_entry_t, vm_page_t *);
static vm_offset_t pmap_kmem_choose(vm_offset_t addr);

CTASSERT(1 << PDESHIFT == sizeof(pd_entry_t));
CTASSERT(1 << PTESHIFT == sizeof(pt_entry_t));


#define LS_MAX		4
struct lock_stack {
	struct mtx *ls_array[LS_MAX];
	int ls_top;
};

static void
ls_init(struct lock_stack *ls)
{

	ls->ls_top = 0;
}

static void
ls_push(struct lock_stack *ls, struct mtx *lock)
{

	KASSERT(ls->ls_top < LS_MAX, ("lock stack overflow"));
	
	ls->ls_array[ls->ls_top] = lock;
	ls->ls_top++;
	mtx_lock(lock);
}


static int
ls_trypush(struct lock_stack *ls, struct mtx *lock)
{

	KASSERT(ls->ls_top < LS_MAX, ("lock stack overflow"));

	if (mtx_trylock(lock) == 0)
		return (0);
	
	ls->ls_array[ls->ls_top] = lock;
	ls->ls_top++;
	return (1);
}

#ifdef notyet
static void
ls_pop(struct lock_stack *ls)
{
	struct mtx *lock;

	KASSERT(ls->ls_top > 0, ("lock stack underflow"));

	ls->ls_top--;
	lock = ls->ls_array[ls->ls_top];
	mtx_unlock(lock);
}
#endif

static void
ls_popa(struct lock_stack *ls)
{
	struct mtx *lock;

	KASSERT(ls->ls_top > 0, ("lock stack underflow"));

	while (ls->ls_top > 0) {
		ls->ls_top--;
		lock = ls->ls_array[ls->ls_top];
		mtx_unlock(lock);
	}
}
#ifdef INVARIANTS
extern void kdb_backtrace(void);
#endif
/*
 * Move the kernel virtual free pointer to the next
 * 2MB.  This is used to help improve performance
 * by using a large (2MB) page for much of the kernel
 * (.text, .data, .bss)
 */
static vm_offset_t
pmap_kmem_choose(vm_offset_t addr)
{
	vm_offset_t newaddr = addr;

	newaddr = (addr + (NBPDR - 1)) & ~(NBPDR - 1);
	return newaddr;
}

/********************/
/* Inline functions */
/********************/

/* Return a non-clipped PD index for a given VA */
static __inline vm_pindex_t
pmap_pde_pindex(vm_offset_t va)
{
	return va >> PDRSHIFT;
}


/* Return various clipped indexes for a given VA */
static __inline vm_pindex_t
pmap_pte_index(vm_offset_t va)
{

	return ((va >> PAGE_SHIFT) & ((1ul << NPTEPGSHIFT) - 1));
}

static __inline vm_pindex_t
pmap_pde_index(vm_offset_t va)
{

	return ((va >> PDRSHIFT) & ((1ul << NPDEPGSHIFT) - 1));
}

static __inline vm_pindex_t
pmap_pdpe_index(vm_offset_t va)
{

	return ((va >> PDPSHIFT) & ((1ul << NPDPEPGSHIFT) - 1));
}

static __inline vm_pindex_t
pmap_pml4e_index(vm_offset_t va)
{

	return ((va >> PML4SHIFT) & ((1ul << NPML4EPGSHIFT) - 1));
}

/* Return a pointer to the PML4 slot that corresponds to a VA */
static __inline pml4_entry_t *
pmap_pml4e(pmap_t pmap, vm_offset_t va)
{

	return (&pmap->pm_pml4[pmap_pml4e_index(va)]);
}

/* Return a pointer to the PDP slot that corresponds to a VA */
static __inline pdp_entry_t *
pmap_pml4e_to_pdpe(pml4_entry_t *pml4e, vm_offset_t va)
{
	pdp_entry_t *pdpe;

	pdpe = (pdp_entry_t *)PHYS_TO_DMAP(*pml4e & PG_FRAME);
	return (&pdpe[pmap_pdpe_index(va)]);
}

/* Return a pointer to the PDP slot that corresponds to a VA */
static __inline pdp_entry_t *
pmap_pdpe(pmap_t pmap, vm_offset_t va)
{
	pml4_entry_t *pml4e;

	pml4e = pmap_pml4e(pmap, va);
	if ((*pml4e & PG_V) == 0)
		return NULL;
	return (pmap_pml4e_to_pdpe(pml4e, va));
}

/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pd_entry_t *
pmap_pdpe_to_pde(pdp_entry_t *pdpe, vm_offset_t va)
{
	pd_entry_t *pde;

	pde = (pd_entry_t *)PHYS_TO_DMAP(*pdpe & PG_FRAME);
	return (&pde[pmap_pde_index(va)]);
}

/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pd_entry_t *
pmap_pde(pmap_t pmap, vm_offset_t va)
{
	pdp_entry_t *pdpe;

	pdpe = pmap_pdpe(pmap, va);
	if (pdpe == NULL || (*pdpe & PG_V) == 0)
		 return NULL;
	return (pmap_pdpe_to_pde(pdpe, va));
}

/* Return a pointer to the PT slot that corresponds to a VA */
static __inline pt_entry_t *
pmap_pde_to_pte(pd_entry_t *pde, vm_offset_t va)
{
	pt_entry_t *pte;

	pte = (pt_entry_t *)PHYS_TO_DMAP(*pde & PG_FRAME);
	return (&pte[pmap_pte_index(va)]);
}

/* Return a pointer to the PT slot that corresponds to a VA */
static __inline pt_entry_t *
pmap_pte(pmap_t pmap, vm_offset_t va)
{
	pd_entry_t *pde;

	pde = pmap_pde(pmap, va);
	if (pde == NULL || (*pde & PG_V) == 0)
		return NULL;
	if ((*pde & PG_PS) != 0)	/* compat with i386 pmap_pte() */
		return ((pt_entry_t *)pde);
	return (pmap_pde_to_pte(pde, va));
}


PMAP_INLINE pt_entry_t *
vtopte(vm_offset_t va)
{
	u_int64_t mask = ((1ul << (NPTEPGSHIFT + NPDEPGSHIFT + NPDPEPGSHIFT + NPML4EPGSHIFT)) - 1);

	return (PTmap + ((va >> PAGE_SHIFT) & mask));
}

static __inline pd_entry_t *
vtopde(vm_offset_t va)
{
	u_int64_t mask = ((1ul << (NPDEPGSHIFT + NPDPEPGSHIFT + NPML4EPGSHIFT)) - 1);

	return (PDmap + ((va >> PDRSHIFT) & mask));
}

/*
 * Try to acquire a physical address lock while a pmap is locked.  If we
 * fail to trylock we unlock and lock the pmap directly and cache the
 * locked pa in *locked.  The caller should then restart their loop in case
 * the virtual to physical mapping has changed.
 */
static int
pa_tryrelock(pmap_t pmap, vm_paddr_t pa, vm_paddr_t *locked)
{
	vm_paddr_t lockpa;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	atomic_add_long((volatile long *)&pmap_tryrelock_calls, 1);
	lockpa = *locked;
	*locked = pa;
	if (lockpa) {
		PA_LOCK_ASSERT(lockpa, MA_OWNED);
		if (PA_LOCKPTR(pa) == PA_LOCKPTR(lockpa))
			return (0);
		PA_UNLOCK(lockpa);
	}
	if (PA_TRYLOCK(pa))
		return 0;
	PMAP_UNLOCK(pmap);
	PA_LOCK(pa);
	PMAP_LOCK(pmap);
	atomic_add_int((volatile int *)&pmap_tryrelock_restart, 1);

	return (EAGAIN);
}

static u_int64_t
allocpages(vm_paddr_t *firstaddr, int n)
{
	u_int64_t ret;

	ret = *firstaddr;
	bzero((void *)ret, n * PAGE_SIZE);
	*firstaddr += n * PAGE_SIZE;
	return (ret);
}

static void
create_pagetables(vm_paddr_t *firstaddr)
{
	int i;

	/* Allocate pages */
	KPTphys = allocpages(firstaddr, NKPT);
	KPML4phys = allocpages(firstaddr, 1);
	KPDPphys = allocpages(firstaddr, NKPML4E);
	KPDphys = allocpages(firstaddr, NKPDPE);

	ndmpdp = (ptoa(Maxmem) + NBPDP - 1) >> PDPSHIFT;
	if (ndmpdp < 4)		/* Minimum 4GB of dirmap */
		ndmpdp = 4;
	DMPDPphys = allocpages(firstaddr, NDMPML4E);
	DMPDphys = allocpages(firstaddr, ndmpdp);
	dmaplimit = (vm_paddr_t)ndmpdp << PDPSHIFT;

	/* Fill in the underlying page table pages */
	/* Read-only from zero to physfree */
	/* XXX not fully used, underneath 2M pages */
	for (i = 0; (i << PAGE_SHIFT) < *firstaddr; i++) {
		((pt_entry_t *)KPTphys)[i] = i << PAGE_SHIFT;
		((pt_entry_t *)KPTphys)[i] |= PG_RW | PG_V | PG_G;
	}

	/* Now map the page tables at their location within PTmap */
	for (i = 0; i < NKPT; i++) {
		((pd_entry_t *)KPDphys)[i] = KPTphys + (i << PAGE_SHIFT);
		((pd_entry_t *)KPDphys)[i] |= PG_RW | PG_V;
	}

	/* Map from zero to end of allocations under 2M pages */
	/* This replaces some of the KPTphys entries above */
	for (i = 0; (i << PDRSHIFT) < *firstaddr; i++) {
		((pd_entry_t *)KPDphys)[i] = i << PDRSHIFT;
		((pd_entry_t *)KPDphys)[i] |= PG_RW | PG_V | PG_PS | PG_G;
	}

	/* And connect up the PD to the PDP */
	for (i = 0; i < NKPDPE; i++) {
		((pdp_entry_t *)KPDPphys)[i + KPDPI] = KPDphys + (i << PAGE_SHIFT);
		((pdp_entry_t *)KPDPphys)[i + KPDPI] |= PG_RW | PG_V | PG_U;
	}

	/* Now set up the direct map space using 2MB pages */
	/* Preset PG_M and PG_A because demotion expects it */
	for (i = 0; i < NPDEPG * ndmpdp; i++) {
		((pd_entry_t *)DMPDphys)[i] = (vm_paddr_t)i << PDRSHIFT;
		((pd_entry_t *)DMPDphys)[i] |= PG_RW | PG_V | PG_PS | PG_G |
		    PG_M | PG_A;
	}

	/* And the direct map space's PDP */
	for (i = 0; i < ndmpdp; i++) {
		((pdp_entry_t *)DMPDPphys)[i] = DMPDphys + (i << PAGE_SHIFT);
		((pdp_entry_t *)DMPDPphys)[i] |= PG_RW | PG_V | PG_U;
	}

	/* And recursively map PML4 to itself in order to get PTmap */
	((pdp_entry_t *)KPML4phys)[PML4PML4I] = KPML4phys;
	((pdp_entry_t *)KPML4phys)[PML4PML4I] |= PG_RW | PG_V | PG_U;

	/* Connect the Direct Map slot up to the PML4 */
	((pdp_entry_t *)KPML4phys)[DMPML4I] = DMPDPphys;
	((pdp_entry_t *)KPML4phys)[DMPML4I] |= PG_RW | PG_V | PG_U;

	/* Connect the KVA slot up to the PML4 */
	((pdp_entry_t *)KPML4phys)[KPML4I] = KPDPphys;
	((pdp_entry_t *)KPML4phys)[KPML4I] |= PG_RW | PG_V | PG_U;
}

/*
 *	Bootstrap the system enough to run with virtual memory.
 *
 *	On amd64 this is called after mapping has already been enabled
 *	and just syncs the pmap module with what has already been done.
 *	[We can't call it easily with mapping off since the kernel is not
 *	mapped with PA == VA, hence we would have to relocate every address
 *	from the linked base (virtual) address "KERNBASE" to the actual
 *	(physical) address starting relative to 0]
 */
void
pmap_bootstrap(vm_paddr_t *firstaddr)
{
	vm_offset_t va;
	pt_entry_t *pte, *unused;
	int i;	

	/*
	 * Create an initial set of page tables to run the kernel in.
	 */
	create_pagetables(firstaddr);

	virtual_avail = (vm_offset_t) KERNBASE + *firstaddr;
	virtual_avail = pmap_kmem_choose(virtual_avail);

	virtual_end = VM_MAX_KERNEL_ADDRESS;


	/* XXX do %cr0 as well */
	load_cr4(rcr4() | CR4_PGE | CR4_PSE);
	load_cr3(KPML4phys);

	/*
	 * Initialize the kernel pmap (which is statically allocated).
	 */
	PMAP_LOCK_INIT(kernel_pmap);
	kernel_pmap->pm_pml4 = (pdp_entry_t *)PHYS_TO_DMAP(KPML4phys);
	kernel_pmap->pm_root = NULL;
	kernel_pmap->pm_active = -1;	/* don't allow deactivation */
	TAILQ_INIT(&kernel_pmap->pm_pvchunk);

	/*
	 * Reserve some special page table entries/VA space for temporary
	 * mapping of pages.
	 */
#define	SYSMAP(c, p, v, n)	\
	v = (c)va; va += ((n)*PAGE_SIZE); p = pte; pte += (n);

	va = virtual_avail;
	pte = vtopte(va);

	/*
	 * CMAP1 is only used for the memory test.
	 */
	SYSMAP(caddr_t, CMAP1, CADDR1, 1)

	/*
	 * Crashdump maps.
	 */
	SYSMAP(caddr_t, unused, crashdumpmap, MAXDUMPPGS)

	/*
	 * msgbufp is used to map the system message buffer.
	 */
	SYSMAP(struct msgbuf *, unused, msgbufp, atop(round_page(MSGBUF_SIZE)))

	virtual_avail = va;

	*CMAP1 = 0;

	invltlb();

	/* Initialize the PAT MSR. */
	pmap_init_pat();

		/* Setup page locks. */
	for (i = 0; i < PA_LOCK_COUNT; i++)
		mtx_init(&pa_lock[i].vp_lock, "page lock", NULL,
		    MTX_DEF | MTX_RECURSE | MTX_DUPOK);
	mtx_init(&pv_lock, "pv list lock", NULL, MTX_DEF);

}

/*
 * Setup the PAT MSR.
 */
void
pmap_init_pat(void)
{
	uint64_t pat_msr;

	/* Bail if this CPU doesn't implement PAT. */
	if (!(cpu_feature & CPUID_PAT))
		panic("no PAT??");

#ifdef PAT_WORKS
	/*
	 * Leave the indices 0-3 at the default of WB, WT, UC, and UC-.
	 * Program 4 and 5 as WP and WC.
	 * Leave 6 and 7 as UC and UC-.
	 */
	pat_msr = rdmsr(MSR_PAT);
	pat_msr &= ~(PAT_MASK(4) | PAT_MASK(5));
	pat_msr |= PAT_VALUE(4, PAT_WRITE_PROTECTED) |
	    PAT_VALUE(5, PAT_WRITE_COMBINING);
#else
	/*
	 * Due to some Intel errata, we can only safely use the lower 4
	 * PAT entries.  Thus, just replace PAT Index 2 with WC instead
	 * of UC-.
	 *
	 *   Intel Pentium III Processor Specification Update
	 * Errata E.27 (Upper Four PAT Entries Not Usable With Mode B
	 * or Mode C Paging)
	 *
	 *   Intel Pentium IV  Processor Specification Update
	 * Errata N46 (PAT Index MSB May Be Calculated Incorrectly)
	 */
	pat_msr = rdmsr(MSR_PAT);
	pat_msr &= ~PAT_MASK(2);
	pat_msr |= PAT_VALUE(2, PAT_WRITE_COMBINING);
#endif
	wrmsr(MSR_PAT, pat_msr);
}

/*
 *	Initialize a vm_page's machine-dependent fields.
 */
void
pmap_page_init(vm_page_t m)
{

	TAILQ_INIT(&m->md.pv_list);
	m->md.pat_mode = PAT_WRITE_BACK;
}

struct mtx *
pmap_page_lockptr(vm_page_t m)
{

	KASSERT(m != NULL, ("pmap_page_lockptr: NULL page"));
	return (PA_LOCKPTR(VM_PAGE_TO_PHYS(m)));
}

/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 */
void
pmap_init(void)
{
	vm_page_t mpte;
	vm_size_t s;
	int i, pv_npg;

	/*
	 * Initialize the vm page array entries for the kernel pmap's
	 * page table pages.
	 */ 
	for (i = 0; i < NKPT; i++) {
		mpte = PHYS_TO_VM_PAGE(KPTphys + (i << PAGE_SHIFT));
		KASSERT(mpte >= vm_page_array &&
		    mpte < &vm_page_array[vm_page_array_size],
		    ("pmap_init: page table page is out of range"));
		mpte->pindex = pmap_pde_pindex(KERNBASE) + i;
		mpte->phys_addr = KPTphys + (i << PAGE_SHIFT);
	}

	/*
	 * Initialize the address space (zone) for the pv entries.  Set a
	 * high water mark so that the system can recover from excessive
	 * numbers of pv entries.
	 */
	TUNABLE_INT_FETCH("vm.pmap.shpgperproc", &shpgperproc);
	pv_entry_max = shpgperproc * maxproc + cnt.v_page_count;
	TUNABLE_INT_FETCH("vm.pmap.pv_entries", &pv_entry_max);
	pv_entry_high_water = 9 * (pv_entry_max / 10);

	/*
	 * If the kernel is running in a virtual machine on an AMD Family 10h
	 * processor, then it must assume that MCA is enabled by the virtual
	 * machine monitor.
	 */
	if (vm_guest == VM_GUEST_VM && cpu_vendor_id == CPU_VENDOR_AMD &&
	    CPUID_TO_FAMILY(cpu_id) == 0x10)
		workaround_erratum383 = 1;

	/*
	 * Are large page mappings enabled?
	 */
	TUNABLE_INT_FETCH("vm.pmap.pg_ps_enabled", &pg_ps_enabled);
	if (pg_ps_enabled) {
		KASSERT(MAXPAGESIZES > 1 && pagesizes[1] == 0,
		    ("pmap_init: can't assign to pagesizes[1]"));
		pagesizes[1] = NBPDR;
	}

	/*
	 * Calculate the size of the pv head table for superpages.
	 */
	for (i = 0; phys_avail[i + 1]; i += 2);
	pv_npg = round_2mpage(phys_avail[(i - 2) + 1]) / NBPDR;

	/*
	 * Allocate memory for the pv head table for superpages.
	 */
	s = (vm_size_t)(pv_npg * sizeof(struct md_page));
	s = round_page(s);
	pv_table = (struct md_page *)kmem_alloc(kernel_map, s);
	for (i = 0; i < pv_npg; i++)
		TAILQ_INIT(&pv_table[i].pv_list);
}

static int
pmap_pventry_proc(SYSCTL_HANDLER_ARGS)
{
	int error;

	error = sysctl_handle_int(oidp, oidp->oid_arg1, oidp->oid_arg2, req);
	if (error == 0 && req->newptr) {
		shpgperproc = (pv_entry_max - cnt.v_page_count) / maxproc;
		pv_entry_high_water = 9 * (pv_entry_max / 10);
	}
	return (error);
}
SYSCTL_PROC(_vm_pmap, OID_AUTO, pv_entry_max, CTLTYPE_INT|CTLFLAG_RW, 
    &pv_entry_max, 0, pmap_pventry_proc, "IU", "Max number of PV entries");

static int
pmap_shpgperproc_proc(SYSCTL_HANDLER_ARGS)
{
	int error;

	error = sysctl_handle_int(oidp, oidp->oid_arg1, oidp->oid_arg2, req);
	if (error == 0 && req->newptr) {
		pv_entry_max = shpgperproc * maxproc + cnt.v_page_count;
		pv_entry_high_water = 9 * (pv_entry_max / 10);
	}
	return (error);
}
SYSCTL_PROC(_vm_pmap, OID_AUTO, shpgperproc, CTLTYPE_INT|CTLFLAG_RW, 
    &shpgperproc, 0, pmap_shpgperproc_proc, "IU", "Page share factor per proc");

SYSCTL_NODE(_vm_pmap, OID_AUTO, pde, CTLFLAG_RD, 0,
    "2MB page mapping counters");

static u_long pmap_pde_demotions;
SYSCTL_ULONG(_vm_pmap_pde, OID_AUTO, demotions, CTLFLAG_RD,
    &pmap_pde_demotions, 0, "2MB page demotions");

static u_long pmap_pde_mappings;
SYSCTL_ULONG(_vm_pmap_pde, OID_AUTO, mappings, CTLFLAG_RD,
    &pmap_pde_mappings, 0, "2MB page mappings");

static u_long pmap_pde_p_failures;
SYSCTL_ULONG(_vm_pmap_pde, OID_AUTO, p_failures, CTLFLAG_RD,
    &pmap_pde_p_failures, 0, "2MB page promotion failures");

static u_long pmap_pde_promotions;
SYSCTL_ULONG(_vm_pmap_pde, OID_AUTO, promotions, CTLFLAG_RD,
    &pmap_pde_promotions, 0, "2MB page promotions");


/***************************************************
 * Low level helper routines.....
 ***************************************************/

/*
 * Determine the appropriate bits to set in a PTE or PDE for a specified
 * caching mode.
 */
static int
pmap_cache_bits(int mode, boolean_t is_pde)
{
	int pat_flag, pat_index, cache_bits;

	/* The PAT bit is different for PTE's and PDE's. */
	pat_flag = is_pde ? PG_PDE_PAT : PG_PTE_PAT;

	/* Map the caching mode to a PAT index. */
	switch (mode) {
#ifdef PAT_WORKS
	case PAT_UNCACHEABLE:
		pat_index = 3;
		break;
	case PAT_WRITE_THROUGH:
		pat_index = 1;
		break;
	case PAT_WRITE_BACK:
		pat_index = 0;
		break;
	case PAT_UNCACHED:
		pat_index = 2;
		break;
	case PAT_WRITE_COMBINING:
		pat_index = 5;
		break;
	case PAT_WRITE_PROTECTED:
		pat_index = 4;
		break;
#else
	case PAT_UNCACHED:
	case PAT_UNCACHEABLE:
	case PAT_WRITE_PROTECTED:
		pat_index = 3;
		break;
	case PAT_WRITE_THROUGH:
		pat_index = 1;
		break;
	case PAT_WRITE_BACK:
		pat_index = 0;
		break;
	case PAT_WRITE_COMBINING:
		pat_index = 2;
		break;
#endif
	default:
		panic("Unknown caching mode %d\n", mode);
	}	

	/* Map the 3-bit index value into the PAT, PCD, and PWT bits. */
	cache_bits = 0;
	if (pat_index & 0x4)
		cache_bits |= pat_flag;
	if (pat_index & 0x2)
		cache_bits |= PG_NC_PCD;
	if (pat_index & 0x1)
		cache_bits |= PG_NC_PWT;
	return (cache_bits);
}

/*
 * After changing the page size for the specified virtual address in the page
 * table, flush the corresponding entries from the processor's TLB.  Only the
 * calling processor's TLB is affected.
 *
 * The calling thread must be pinned to a processor.
 */
static void
pmap_update_pde_invalidate(vm_offset_t va, pd_entry_t newpde)
{
	u_long cr4;

	if ((newpde & PG_PS) == 0)
		/* Demotion: flush a specific 2MB page mapping. */
		invlpg(va);
	else if ((newpde & PG_G) == 0)
		/*
		 * Promotion: flush every 4KB page mapping from the TLB
		 * because there are too many to flush individually.
		 */
		invltlb();
	else {
		/*
		 * Promotion: flush every 4KB page mapping from the TLB,
		 * including any global (PG_G) mappings.
		 */
		cr4 = rcr4();
		load_cr4(cr4 & ~CR4_PGE);
		/*
		 * Although preemption at this point could be detrimental to
		 * performance, it would not lead to an error.  PG_G is simply
		 * ignored if CR4.PGE is clear.  Moreover, in case this block
		 * is re-entered, the load_cr4() either above or below will
		 * modify CR4.PGE flushing the TLB.
		 */
		load_cr4(cr4 | CR4_PGE);
	}
}
#ifdef SMP
/*
 * For SMP, these functions have to use the IPI mechanism for coherence.
 *
 * N.B.: Before calling any of the following TLB invalidation functions,
 * the calling processor must ensure that all stores updating a non-
 * kernel page table are globally performed.  Otherwise, another
 * processor could cache an old, pre-update entry without being
 * invalidated.  This can happen one of two ways: (1) The pmap becomes
 * active on another processor after its pm_active field is checked by
 * one of the following functions but before a store updating the page
 * table is globally performed. (2) The pmap becomes active on another
 * processor before its pm_active field is checked but due to
 * speculative loads one of the following functions stills reads the
 * pmap as inactive on the other processor.
 * 
 * The kernel page table is exempt because its pm_active field is
 * immutable.  The kernel page table is always active on every
 * processor.
 */
void
pmap_invalidate_page(pmap_t pmap, vm_offset_t va)
{
	u_int cpumask;
	u_int other_cpus;

	sched_pin();
	if (pmap == kernel_pmap || pmap->pm_active == all_cpus) {
		invlpg(va);
		smp_invlpg(va);
	} else {
		cpumask = PCPU_GET(cpumask);
		other_cpus = PCPU_GET(other_cpus);
		if (pmap->pm_active & cpumask)
			invlpg(va);
		if (pmap->pm_active & other_cpus)
			smp_masked_invlpg(pmap->pm_active & other_cpus, va);
	}
	sched_unpin();
}

void
pmap_invalidate_range(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	u_int cpumask;
	u_int other_cpus;
	vm_offset_t addr;

	sched_pin();
	if (pmap == kernel_pmap || pmap->pm_active == all_cpus) {
		for (addr = sva; addr < eva; addr += PAGE_SIZE)
			invlpg(addr);
		smp_invlpg_range(sva, eva);
	} else {
		cpumask = PCPU_GET(cpumask);
		other_cpus = PCPU_GET(other_cpus);
		if (pmap->pm_active & cpumask)
			for (addr = sva; addr < eva; addr += PAGE_SIZE)
				invlpg(addr);
		if (pmap->pm_active & other_cpus)
			smp_masked_invlpg_range(pmap->pm_active & other_cpus,
			    sva, eva);
	}
	sched_unpin();
}

void
pmap_invalidate_all(pmap_t pmap)
{
	u_int cpumask;
	u_int other_cpus;

	sched_pin();
	if (pmap == kernel_pmap || pmap->pm_active == all_cpus) {
		invltlb();
		smp_invltlb();
	} else {
		cpumask = PCPU_GET(cpumask);
		other_cpus = PCPU_GET(other_cpus);
		if (pmap->pm_active & cpumask)
			invltlb();
		if (pmap->pm_active & other_cpus)
			smp_masked_invltlb(pmap->pm_active & other_cpus);
	}
	sched_unpin();
}

void
pmap_invalidate_cache(void)
{

	sched_pin();
	wbinvd();
	smp_cache_flush();
	sched_unpin();
}

struct pde_action {
	cpumask_t store;	/* processor that updates the PDE */
	cpumask_t invalidate;	/* processors that invalidate their TLB */
	vm_offset_t va;
	pd_entry_t *pde;
	pd_entry_t newpde;
};

static void
pmap_update_pde_action(void *arg)
{
	struct pde_action *act = arg;

	if (act->store == PCPU_GET(cpumask))
		pde_store(act->pde, act->newpde);
}

static void
pmap_update_pde_teardown(void *arg)
{
	struct pde_action *act = arg;

	if ((act->invalidate & PCPU_GET(cpumask)) != 0)
		pmap_update_pde_invalidate(act->va, act->newpde);
}

/*
 * Change the page size for the specified virtual address in a way that
 * prevents any possibility of the TLB ever having two entries that map the
 * same virtual address using different page sizes.  This is the recommended
 * workaround for Erratum 383 on AMD Family 10h processors.  It prevents a
 * machine check exception for a TLB state that is improperly diagnosed as a
 * hardware error.
 */
static void
pmap_update_pde(pmap_t pmap, vm_offset_t va, pd_entry_t *pde, pd_entry_t newpde)
{
	struct pde_action act;
	cpumask_t active, cpumask;

	sched_pin();
	cpumask = PCPU_GET(cpumask);
	if (pmap == kernel_pmap)
		active = all_cpus;
	else
		active = pmap->pm_active;
	if ((active & PCPU_GET(other_cpus)) != 0) {
		act.store = cpumask;
		act.invalidate = active;
		act.va = va;
		act.pde = pde;
		act.newpde = newpde;
		smp_rendezvous_cpus(cpumask | active,
		    smp_no_rendevous_barrier, pmap_update_pde_action,
		    pmap_update_pde_teardown, &act);
	} else {
		pde_store(pde, newpde);
		if ((active & cpumask) != 0)
			pmap_update_pde_invalidate(va, newpde);
	}
	sched_unpin();
}
#else /* !SMP */
/*
 * Normal, non-SMP, invalidation functions.
 * We inline these within pmap.c for speed.
 */
PMAP_INLINE void
pmap_invalidate_page(pmap_t pmap, vm_offset_t va)
{

	if (pmap == kernel_pmap || pmap->pm_active)
		invlpg(va);
}

PMAP_INLINE void
pmap_invalidate_range(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	vm_offset_t addr;

	if (pmap == kernel_pmap || pmap->pm_active)
		for (addr = sva; addr < eva; addr += PAGE_SIZE)
			invlpg(addr);
}

PMAP_INLINE void
pmap_invalidate_all(pmap_t pmap)
{

	if (pmap == kernel_pmap || pmap->pm_active)
		invltlb();
}

PMAP_INLINE void
pmap_invalidate_cache(void)
{

	wbinvd();
}

static void
pmap_update_pde(pmap_t pmap, vm_offset_t va, pd_entry_t *pde, pd_entry_t newpde)
{

	pde_store(pde, newpde);
	if (pmap == kernel_pmap || pmap->pm_active)
		pmap_update_pde_invalidate(va, newpde);
}
#endif /* !SMP */

static void
pmap_invalidate_cache_range(vm_offset_t sva, vm_offset_t eva)
{

	KASSERT((sva & PAGE_MASK) == 0,
	    ("pmap_invalidate_cache_range: sva not page-aligned"));
	KASSERT((eva & PAGE_MASK) == 0,
	    ("pmap_invalidate_cache_range: eva not page-aligned"));

	if (cpu_feature & CPUID_SS)
		; /* If "Self Snoop" is supported, do nothing. */
	else if ((cpu_feature & CPUID_CLFSH) != 0 &&
		 eva - sva < 2 * 1024 * 1024) {

		/*
		 * Otherwise, do per-cache line flush.  Use the mfence
		 * instruction to insure that previous stores are
		 * included in the write-back.  The processor
		 * propagates flush to other processors in the cache
		 * coherence domain.
		 */
		mfence();
		for (; sva < eva; sva += cpu_clflush_line_size)
			clflush(sva);
		mfence();
	} else {

		/*
		 * No targeted cache flush methods are supported by CPU,
		 * or the supplied range is bigger than 2MB.
		 * Globally invalidate cache.
		 */
		pmap_invalidate_cache();
	}
}

/*
 * Are we current address space or kernel?
 */
static __inline int
pmap_is_current(pmap_t pmap)
{
	return (pmap == kernel_pmap ||
	    (pmap->pm_pml4[PML4PML4I] & PG_FRAME) == (PML4pml4e[0] & PG_FRAME));
}

/*
 *	Routine:	pmap_extract
 *	Function:
 *		Extract the physical page address associated
 *		with the given map/virtual_address pair.
 */
vm_paddr_t 
pmap_extract(pmap_t pmap, vm_offset_t va)
{
	vm_paddr_t rtval;
	pt_entry_t *pte;
	pd_entry_t pde, *pdep;

	rtval = 0;
	PMAP_LOCK(pmap);
	pdep = pmap_pde(pmap, va);
	if (pdep != NULL) {
		pde = *pdep;
		if (pde) {
			if ((pde & PG_PS) != 0)
				rtval = (pde & PG_PS_FRAME) | (va & PDRMASK);
			else {
				pte = pmap_pde_to_pte(pdep, va);
				rtval = (*pte & PG_FRAME) | (va & PAGE_MASK);
			}
		}
	}
	PMAP_UNLOCK(pmap);
	return (rtval);
}

/*
 *	Routine:	pmap_extract_and_hold
 *	Function:
 *		Atomically extract and hold the physical page
 *		with the given pmap and virtual address pair
 *		if that mapping permits the given protection.
 */
vm_page_t
pmap_extract_and_hold(pmap_t pmap, vm_offset_t va, vm_prot_t prot)
{
	pd_entry_t pde, *pdep;
	pt_entry_t pte;
	vm_paddr_t pa;
	vm_page_t m;

	pa = 0;
	m = NULL;
	PMAP_LOCK(pmap);
retry:
	pdep = pmap_pde(pmap, va);
	if (pdep != NULL && (pde = *pdep)) {
		if (pde & PG_PS) {
			if ((pde & PG_RW) || (prot & VM_PROT_WRITE) == 0) {
				if (pa_tryrelock(pmap, pde & PG_PS_FRAME, &pa))
					goto retry;

				m = PHYS_TO_VM_PAGE((pde & PG_PS_FRAME) |
				    (va & PDRMASK));
				vm_page_hold(m);
			}
		} else {
			pte = *pmap_pde_to_pte(pdep, va);
			if ((pte & PG_V) &&
			    ((pte & PG_RW) || (prot & VM_PROT_WRITE) == 0)) {
				if (pa_tryrelock(pmap, pte & PG_FRAME, &pa))
					goto retry;
				m = PHYS_TO_VM_PAGE(pte & PG_FRAME);
				vm_page_hold(m);
			}
		}
	}
	if (pa)
		PA_UNLOCK(pa);
	PMAP_UNLOCK(pmap);
	return (m);
}

vm_paddr_t
pmap_kextract(vm_offset_t va)
{
	pd_entry_t pde;
	vm_paddr_t pa;

	if (va >= DMAP_MIN_ADDRESS && va < DMAP_MAX_ADDRESS) {
		pa = DMAP_TO_PHYS(va);
	} else {
		pde = *vtopde(va);
		if (pde & PG_PS) {
			pa = (pde & PG_PS_FRAME) | (va & PDRMASK);
		} else {
			/*
			 * Beware of a concurrent promotion that changes the
			 * PDE at this point!  For example, vtopte() must not
			 * be used to access the PTE because it would use the
			 * new PDE.  It is, however, safe to use the old PDE
			 * because the page table page is preserved by the
			 * promotion.
			 */
			pa = *pmap_pde_to_pte(&pde, va);
			pa = (pa & PG_FRAME) | (va & PAGE_MASK);
		}
	}
	return pa;
}

/***************************************************
 * Low level mapping routines.....
 ***************************************************/

/*
 * Add a wired page to the kva.
 * Note: not SMP coherent.
 */
PMAP_INLINE void 
pmap_kenter(vm_offset_t va, vm_paddr_t pa)
{
	pt_entry_t *pte;

	pte = vtopte(va);
	pte_store(pte, pa | PG_RW | PG_V | PG_G);
}

static __inline void
pmap_kenter_attr(vm_offset_t va, vm_paddr_t pa, int mode)
{
	pt_entry_t *pte;

	pte = vtopte(va);
	pte_store(pte, pa | PG_RW | PG_V | PG_G | pmap_cache_bits(mode, 0));
}

/*
 * Remove a page from the kernel pagetables.
 * Note: not SMP coherent.
 */
PMAP_INLINE void
pmap_kremove(vm_offset_t va)
{
	pt_entry_t *pte;

	pte = vtopte(va);
	pte_clear(pte);
}

/*
 *	Used to map a range of physical addresses into kernel
 *	virtual address space.
 *
 *	The value passed in '*virt' is a suggested virtual address for
 *	the mapping. Architectures which can support a direct-mapped
 *	physical to virtual region can return the appropriate address
 *	within that region, leaving '*virt' unchanged. Other
 *	architectures should map the pages starting at '*virt' and
 *	update '*virt' with the first usable address after the mapped
 *	region.
 */
vm_offset_t
pmap_map(vm_offset_t *virt, vm_paddr_t start, vm_paddr_t end, int prot)
{
	return PHYS_TO_DMAP(start);
}


/*
 * Add a list of wired pages to the kva
 * this routine is only used for temporary
 * kernel mappings that do not need to have
 * page modification or references recorded.
 * Note that old mappings are simply written
 * over.  The page *must* be wired.
 * Note: SMP coherent.  Uses a ranged shootdown IPI.
 */
void
pmap_qenter(vm_offset_t sva, vm_page_t *ma, int count)
{
	pt_entry_t *endpte, oldpte, *pte;

	oldpte = 0;
	pte = vtopte(sva);
	endpte = pte + count;
	while (pte < endpte) {
		oldpte |= *pte;
		pte_store(pte, VM_PAGE_TO_PHYS(*ma) | PG_G |
		    pmap_cache_bits((*ma)->md.pat_mode, 0) | PG_RW | PG_V);
		pte++;
		ma++;
	}
	if ((oldpte & PG_V) != 0)
		pmap_invalidate_range(kernel_pmap, sva, sva + count *
		    PAGE_SIZE);
}

/*
 * This routine tears out page mappings from the
 * kernel -- it is meant only for temporary mappings.
 * Note: SMP coherent.  Uses a ranged shootdown IPI.
 */
void
pmap_qremove(vm_offset_t sva, int count)
{
	vm_offset_t va;

	va = sva;
	while (count-- > 0) {
		pmap_kremove(va);
		va += PAGE_SIZE;
	}
	pmap_invalidate_range(kernel_pmap, sva, va);
}

/***************************************************
 * Page table page management routines.....
 ***************************************************/
static __inline void
pmap_free_zero_pages(vm_page_t free)
{
	vm_page_t m;

	while (free != NULL) {
		m = free;
		free = m->right;
		/* Preserve the page's PG_ZERO setting. */
		vm_page_free_toq(m);
	}
}

/*
 * Schedule the specified unused page table page to be freed.  Specifically,
 * add the page to the specified list of pages that will be released to the
 * physical memory manager after the TLB has been updated.
 */
static __inline void
pmap_add_delayed_free_list(vm_page_t m, vm_page_t *free, boolean_t set_PG_ZERO)
{

	if (set_PG_ZERO)
		m->flags |= PG_ZERO;
	else
		m->flags &= ~PG_ZERO;
	m->right = *free;
	*free = m;
}
	
/*
 * Inserts the specified page table page into the specified pmap's collection
 * of idle page table pages.  Each of a pmap's page table pages is responsible
 * for mapping a distinct range of virtual addresses.  The pmap's collection is
 * ordered by this virtual address range.
 */
static void
pmap_insert_pt_page(pmap_t pmap, vm_page_t mpte)
{
	vm_page_t root;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	root = pmap->pm_root;
	if (root == NULL) {
		mpte->left = NULL;
		mpte->right = NULL;
	} else {
		root = vm_page_splay(mpte->pindex, root);
		if (mpte->pindex < root->pindex) {
			mpte->left = root->left;
			mpte->right = root;
			root->left = NULL;
		} else if (mpte->pindex == root->pindex)
			panic("pmap_insert_pt_page: pindex already inserted");
		else {
			mpte->right = root->right;
			mpte->left = root;
			root->right = NULL;
		}
	}
	pmap->pm_root = mpte;
}

/*
 * Looks for a page table page mapping the specified virtual address in the
 * specified pmap's collection of idle page table pages.  Returns NULL if there
 * is no page table page corresponding to the specified virtual address.
 */
static vm_page_t
pmap_lookup_pt_page(pmap_t pmap, vm_offset_t va)
{
	vm_page_t mpte;
	vm_pindex_t pindex = pmap_pde_pindex(va);

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	if ((mpte = pmap->pm_root) != NULL && mpte->pindex != pindex) {
		mpte = vm_page_splay(pindex, mpte);
		if ((pmap->pm_root = mpte)->pindex != pindex)
			mpte = NULL;
	}
	return (mpte);
}

/*
 * Removes the specified page table page from the specified pmap's collection
 * of idle page table pages.  The specified page table page must be a member of
 * the pmap's collection.
 */
static void
pmap_remove_pt_page(pmap_t pmap, vm_page_t mpte)
{
	vm_page_t root;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	if (mpte != pmap->pm_root) {
		root = vm_page_splay(mpte->pindex, pmap->pm_root);
		KASSERT(mpte == root,
		    ("pmap_remove_pt_page: mpte %p is missing from pmap %p",
		    mpte, pmap));
	}
	if (mpte->left == NULL)
		root = mpte->right;
	else {
		root = vm_page_splay(mpte->pindex, mpte->left);
		root->right = mpte->right;
	}
	pmap->pm_root = root;
}

/*
 * This routine unholds page table pages, and if the hold count
 * drops to zero, then it decrements the wire count.
 */
static __inline int
pmap_unwire_pte_hold(pmap_t pmap, vm_offset_t va, vm_page_t m, vm_page_t *free)
{

	--m->wire_count;
	if (m->wire_count == 0)
		return _pmap_unwire_pte_hold(pmap, va, m, free);
	else
		return 0;
}

static int 
_pmap_unwire_pte_hold(pmap_t pmap, vm_offset_t va, vm_page_t m, 
    vm_page_t *free)
{

	/*
	 * unmap the page table page
	 */
	if (m->pindex >= (NUPDE + NUPDPE)) {
		/* PDP page */
		pml4_entry_t *pml4;
		pml4 = pmap_pml4e(pmap, va);
		*pml4 = 0;
	} else if (m->pindex >= NUPDE) {
		/* PD page */
		pdp_entry_t *pdp;
		pdp = pmap_pdpe(pmap, va);
		*pdp = 0;
	} else {
		/* PTE page */
		pd_entry_t *pd;
		pd = pmap_pde(pmap, va);
		*pd = 0;
	}
	--pmap->pm_stats.resident_count;
	if (m->pindex < NUPDE) {
		/* We just released a PT, unhold the matching PD */
		vm_page_t pdpg;

		pdpg = PHYS_TO_VM_PAGE(*pmap_pdpe(pmap, va) & PG_FRAME);
		pmap_unwire_pte_hold(pmap, va, pdpg, free);
	}
	if (m->pindex >= NUPDE && m->pindex < (NUPDE + NUPDPE)) {
		/* We just released a PD, unhold the matching PDP */
		vm_page_t pdppg;

		pdppg = PHYS_TO_VM_PAGE(*pmap_pml4e(pmap, va) & PG_FRAME);
		pmap_unwire_pte_hold(pmap, va, pdppg, free);
	}

	/*
	 * This is a release store so that the ordinary store unmapping
	 * the page table page is globally performed before TLB shoot-
	 * down is begun.
	 */
	atomic_subtract_rel_int(&cnt.v_wire_count, 1);

	/* 
	 * Put page on a list so that it is released after
	 * *ALL* TLB shootdown is done
	 */
	pmap_add_delayed_free_list(m, free, TRUE);
	
	return 1;
}

/*
 * After removing a page table entry, this routine is used to
 * conditionally free the page, and manage the hold/wire counts.
 */
static int
pmap_unuse_pt(pmap_t pmap, vm_offset_t va, pd_entry_t ptepde, vm_page_t *free)
{
	vm_page_t mpte;

	if (va >= VM_MAXUSER_ADDRESS)
		return 0;
	KASSERT(ptepde != 0, ("pmap_unuse_pt: ptepde != 0"));
	mpte = PHYS_TO_VM_PAGE(ptepde & PG_FRAME);
	return pmap_unwire_pte_hold(pmap, va, mpte, free);
}

void
pmap_pinit0(pmap_t pmap)
{

	PMAP_LOCK_INIT(pmap);
	pmap->pm_pml4 = (pml4_entry_t *)PHYS_TO_DMAP(KPML4phys);
	pmap->pm_root = NULL;
	pmap->pm_active = 0;
	TAILQ_INIT(&pmap->pm_pvchunk);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);
}

/*
 * Initialize a preallocated and zeroed pmap structure,
 * such as one in a vmspace structure.
 */
int
pmap_pinit(pmap_t pmap)
{
	vm_page_t pml4pg;
	static vm_pindex_t color;

	PMAP_LOCK_INIT(pmap);

	/*
	 * allocate the page directory page
	 */
	while ((pml4pg = vm_page_alloc(NULL, color++, VM_ALLOC_NOOBJ |
	    VM_ALLOC_NORMAL | VM_ALLOC_WIRED | VM_ALLOC_ZERO)) == NULL)
		VM_WAIT;

	pmap->pm_pml4 = (pml4_entry_t *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(pml4pg));

	if ((pml4pg->flags & PG_ZERO) == 0)
		pagezero(pmap->pm_pml4);

	/* Wire in kernel global address entries. */
	pmap->pm_pml4[KPML4I] = KPDPphys | PG_RW | PG_V | PG_U;
	pmap->pm_pml4[DMPML4I] = DMPDPphys | PG_RW | PG_V | PG_U;

	/* install self-referential address mapping entry(s) */
	pmap->pm_pml4[PML4PML4I] = VM_PAGE_TO_PHYS(pml4pg) | PG_V | PG_RW | PG_A | PG_M;

	pmap->pm_root = NULL;
	pmap->pm_active = 0;
	TAILQ_INIT(&pmap->pm_pvchunk);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);

	return (1);
}

/*
 * this routine is called if the page table page is not
 * mapped correctly.
 *
 * Note: If a page allocation fails at page table level two or three,
 * one or two pages may be held during the wait, only to be released
 * afterwards.  This conservative approach is easily argued to avoid
 * race conditions.
 */
static vm_page_t
_pmap_allocpte(pmap_t pmap, vm_paddr_t pa, vm_pindex_t ptepindex, int flags)
{
	vm_page_t m, pdppg, pdpg;

	KASSERT((flags & (M_NOWAIT | M_WAITOK)) == M_NOWAIT ||
	    (flags & (M_NOWAIT | M_WAITOK)) == M_WAITOK,
	    ("_pmap_allocpte: flags is neither M_NOWAIT nor M_WAITOK"));

	/*
	 * Allocate a page table page.
	 */
	if ((m = vm_page_alloc(NULL, ptepindex, VM_ALLOC_NOOBJ |
	    VM_ALLOC_WIRED | VM_ALLOC_ZERO)) == NULL) {
		if (flags & M_WAITOK) {
			PMAP_UNLOCK(pmap);
			PA_UNLOCK(pa);
			VM_WAIT;
			PA_LOCK(pa);
			PMAP_LOCK(pmap);
		}

		/*
		 * Indicate the need to retry.  While waiting, the page table
		 * page may have been allocated.
		 */
		return (NULL);
	}
	if ((m->flags & PG_ZERO) == 0)
		pmap_zero_page(m);

	/*
	 * Map the pagetable page into the process address space, if
	 * it isn't already there.
	 */

	if (ptepindex >= (NUPDE + NUPDPE)) {
		pml4_entry_t *pml4;
		vm_pindex_t pml4index;

		/* Wire up a new PDPE page */
		pml4index = ptepindex - (NUPDE + NUPDPE);
		pml4 = &pmap->pm_pml4[pml4index];
		*pml4 = VM_PAGE_TO_PHYS(m) | PG_U | PG_RW | PG_V | PG_A | PG_M;

	} else if (ptepindex >= NUPDE) {
		vm_pindex_t pml4index;
		vm_pindex_t pdpindex;
		pml4_entry_t *pml4;
		pdp_entry_t *pdp;

		/* Wire up a new PDE page */
		pdpindex = ptepindex - NUPDE;
		pml4index = pdpindex >> NPML4EPGSHIFT;

		pml4 = &pmap->pm_pml4[pml4index];
		if ((*pml4 & PG_V) == 0) {
			/* Have to allocate a new pdp, recurse */
			if (_pmap_allocpte(pmap, pa, NUPDE + NUPDPE + pml4index,
			    flags) == NULL) {
				--m->wire_count;
				atomic_subtract_int(&cnt.v_wire_count, 1);
				vm_page_free_zero(m);
				return (NULL);
			}
		} else {
			/* Add reference to pdp page */
			pdppg = PHYS_TO_VM_PAGE(*pml4 & PG_FRAME);
			pdppg->wire_count++;
		}
		pdp = (pdp_entry_t *)PHYS_TO_DMAP(*pml4 & PG_FRAME);

		/* Now find the pdp page */
		pdp = &pdp[pdpindex & ((1ul << NPDPEPGSHIFT) - 1)];
		*pdp = VM_PAGE_TO_PHYS(m) | PG_U | PG_RW | PG_V | PG_A | PG_M;

	} else {
		vm_pindex_t pml4index;
		vm_pindex_t pdpindex;
		pml4_entry_t *pml4;
		pdp_entry_t *pdp;
		pd_entry_t *pd;

		/* Wire up a new PTE page */
		pdpindex = ptepindex >> NPDPEPGSHIFT;
		pml4index = pdpindex >> NPML4EPGSHIFT;

		/* First, find the pdp and check that its valid. */
		pml4 = &pmap->pm_pml4[pml4index];
		if ((*pml4 & PG_V) == 0) {
			/* Have to allocate a new pd, recurse */
			if (_pmap_allocpte(pmap, pa, NUPDE + pdpindex,
			    flags) == NULL) {
				--m->wire_count;
				atomic_subtract_int(&cnt.v_wire_count, 1);
				vm_page_free_zero(m);
				return (NULL);
			}
			pdp = (pdp_entry_t *)PHYS_TO_DMAP(*pml4 & PG_FRAME);
			pdp = &pdp[pdpindex & ((1ul << NPDPEPGSHIFT) - 1)];
		} else {
			pdp = (pdp_entry_t *)PHYS_TO_DMAP(*pml4 & PG_FRAME);
			pdp = &pdp[pdpindex & ((1ul << NPDPEPGSHIFT) - 1)];
			if ((*pdp & PG_V) == 0) {
				/* Have to allocate a new pd, recurse */
				if (_pmap_allocpte(pmap, pa, NUPDE + pdpindex,
				    flags) == NULL) {
					--m->wire_count;
					atomic_subtract_int(&cnt.v_wire_count,
					    1);
					vm_page_free_zero(m);
					return (NULL);
				}
			} else {
				/* Add reference to the pd page */
				pdpg = PHYS_TO_VM_PAGE(*pdp & PG_FRAME);
				pdpg->wire_count++;
			}
		}
		pd = (pd_entry_t *)PHYS_TO_DMAP(*pdp & PG_FRAME);

		/* Now we know where the page directory page is */
		pd = &pd[ptepindex & ((1ul << NPDEPGSHIFT) - 1)];
		*pd = VM_PAGE_TO_PHYS(m) | PG_U | PG_RW | PG_V | PG_A | PG_M;
	}

	pmap->pm_stats.resident_count++;

	return m;
}

static vm_page_t
pmap_allocpde(pmap_t pmap, vm_paddr_t pa, vm_offset_t va, int flags)
{
	vm_pindex_t pdpindex, ptepindex;
	pdp_entry_t *pdpe;
	vm_page_t pdpg;

	KASSERT((flags & (M_NOWAIT | M_WAITOK)) == M_NOWAIT ||
	    (flags & (M_NOWAIT | M_WAITOK)) == M_WAITOK,
	    ("pmap_allocpde: flags is neither M_NOWAIT nor M_WAITOK"));
retry:
	pdpe = pmap_pdpe(pmap, va);
	if (pdpe != NULL && (*pdpe & PG_V) != 0) {
		/* Add a reference to the pd page. */
		pdpg = PHYS_TO_VM_PAGE(*pdpe & PG_FRAME);
		pdpg->wire_count++;
	} else {
		/* Allocate a pd page. */
		ptepindex = pmap_pde_pindex(va);
		pdpindex = ptepindex >> NPDPEPGSHIFT;
		pdpg = _pmap_allocpte(pmap, pa, NUPDE + pdpindex, flags);
		if (pdpg == NULL && (flags & M_WAITOK))
			goto retry;
	}
	return (pdpg);
}

static vm_page_t
pmap_allocpte(pmap_t pmap, vm_paddr_t pa, vm_offset_t va, int flags)
{
	vm_pindex_t ptepindex;
	pd_entry_t *pd;
	vm_page_t m;
	struct pv_list_head pv_list;

	KASSERT((flags & (M_NOWAIT | M_WAITOK)) == M_NOWAIT ||
	    (flags & (M_NOWAIT | M_WAITOK)) == M_WAITOK,
	    ("pmap_allocpte: flags is neither M_NOWAIT nor M_WAITOK"));

	/*
	 * Calculate pagetable page index
	 */
	ptepindex = pmap_pde_pindex(va);
retry:
	/*
	 * Get the page directory entry
	 */
	pd = pmap_pde(pmap, va);

	/*
	 * This supports switching from a 2MB page to a
	 * normal 4K page.
	 */
	if (pd != NULL && (*pd & (PG_PS | PG_V)) == (PG_PS | PG_V)) {
		TAILQ_INIT(&pv_list);
		if (!pmap_demote_pde(pmap, pd, va, &pv_list)) {
			/*
			 * Invalidation of the 2MB page mapping may have caused
			 * the deallocation of the underlying PD page.
			 */
			pd = NULL;
		}
	}

	/*
	 * If the page table page is mapped, we just increment the
	 * hold count, and activate it.
	 */
	if (pd != NULL && (*pd & PG_V) != 0) {
		m = PHYS_TO_VM_PAGE(*pd & PG_FRAME);
		m->wire_count++;
	} else {
		/*
		 * Here if the pte page isn't mapped, or if it has been
		 * deallocated.
		 */
		m = _pmap_allocpte(pmap, pa, ptepindex, flags);
		if (m == NULL && (flags & M_WAITOK))
			goto retry;
	}
	return (m);
}


/***************************************************
 * Pmap allocation/deallocation routines.
 ***************************************************/

/*
 * Release any resources held by the given physical map.
 * Called when a pmap initialized by pmap_pinit is being released.
 * Should only be called if the map contains no valid mappings.
 */
void
pmap_release(pmap_t pmap)
{
	vm_page_t m;

	KASSERT(pmap->pm_stats.resident_count == 0,
	    ("pmap_release: pmap resident count %ld != 0",
	    pmap->pm_stats.resident_count));
	KASSERT(pmap->pm_root == NULL,
	    ("pmap_release: pmap has reserved page table page(s)"));

	m = PHYS_TO_VM_PAGE(pmap->pm_pml4[PML4PML4I] & PG_FRAME);

	pmap->pm_pml4[KPML4I] = 0;	/* KVA */
	pmap->pm_pml4[DMPML4I] = 0;	/* Direct Map */
	pmap->pm_pml4[PML4PML4I] = 0;	/* Recursive Mapping */

	m->wire_count--;
	atomic_subtract_int(&cnt.v_wire_count, 1);
	vm_page_free_zero(m);
	PMAP_LOCK_DESTROY(pmap);
}

static int
kvm_size(SYSCTL_HANDLER_ARGS)
{
	unsigned long ksize = VM_MAX_KERNEL_ADDRESS - VM_MIN_KERNEL_ADDRESS;

	return sysctl_handle_long(oidp, &ksize, 0, req);
}
SYSCTL_PROC(_vm, OID_AUTO, kvm_size, CTLTYPE_LONG|CTLFLAG_RD, 
    0, 0, kvm_size, "LU", "Size of KVM");

static int
kvm_free(SYSCTL_HANDLER_ARGS)
{
	unsigned long kfree = VM_MAX_KERNEL_ADDRESS - kernel_vm_end;

	return sysctl_handle_long(oidp, &kfree, 0, req);
}
SYSCTL_PROC(_vm, OID_AUTO, kvm_free, CTLTYPE_LONG|CTLFLAG_RD, 
    0, 0, kvm_free, "LU", "Amount of KVM free");

/*
 * grow the number of kernel page table entries, if needed
 */
void
pmap_growkernel(vm_offset_t addr)
{
	vm_paddr_t paddr;
	vm_page_t nkpg;
	pd_entry_t *pde, newpdir;
	pdp_entry_t *pdpe;

	mtx_assert(&kernel_map->system_mtx, MA_OWNED);

	/*
	 * Return if "addr" is within the range of kernel page table pages
	 * that were preallocated during pmap bootstrap.  Moreover, leave
	 * "kernel_vm_end" and the kernel page table as they were.
	 *
	 * The correctness of this action is based on the following
	 * argument: vm_map_findspace() allocates contiguous ranges of the
	 * kernel virtual address space.  It calls this function if a range
	 * ends after "kernel_vm_end".  If the kernel is mapped between
	 * "kernel_vm_end" and "addr", then the range cannot begin at
	 * "kernel_vm_end".  In fact, its beginning address cannot be less
	 * than the kernel.  Thus, there is no immediate need to allocate
	 * any new kernel page table pages between "kernel_vm_end" and
	 * "KERNBASE".
	 */
	if (KERNBASE < addr && addr <= KERNBASE + NKPT * NBPDR)
		return;

	addr = roundup2(addr, NBPDR);
	if (addr - 1 >= kernel_map->max_offset)
		addr = kernel_map->max_offset;
	while (kernel_vm_end < addr) {
		pdpe = pmap_pdpe(kernel_pmap, kernel_vm_end);
		if ((*pdpe & PG_V) == 0) {
			/* We need a new PDP entry */
			nkpg = vm_page_alloc(NULL, kernel_vm_end >> PDPSHIFT,
			    VM_ALLOC_INTERRUPT | VM_ALLOC_NOOBJ |
			    VM_ALLOC_WIRED | VM_ALLOC_ZERO);
			if (nkpg == NULL)
				panic("pmap_growkernel: no memory to grow kernel");
			if ((nkpg->flags & PG_ZERO) == 0)
				pmap_zero_page(nkpg);
			paddr = VM_PAGE_TO_PHYS(nkpg);
			*pdpe = (pdp_entry_t)
				(paddr | PG_V | PG_RW | PG_A | PG_M);
			continue; /* try again */
		}
		pde = pmap_pdpe_to_pde(pdpe, kernel_vm_end);
		if ((*pde & PG_V) != 0) {
			kernel_vm_end = (kernel_vm_end + NBPDR) & ~PDRMASK;
			if (kernel_vm_end - 1 >= kernel_map->max_offset) {
				kernel_vm_end = kernel_map->max_offset;
				break;                       
			}
			continue;
		}

		nkpg = vm_page_alloc(NULL, pmap_pde_pindex(kernel_vm_end),
		    VM_ALLOC_INTERRUPT | VM_ALLOC_NOOBJ | VM_ALLOC_WIRED |
		    VM_ALLOC_ZERO);
		if (nkpg == NULL)
			panic("pmap_growkernel: no memory to grow kernel");
		if ((nkpg->flags & PG_ZERO) == 0)
			pmap_zero_page(nkpg);
		paddr = VM_PAGE_TO_PHYS(nkpg);
		newpdir = (pd_entry_t) (paddr | PG_V | PG_RW | PG_A | PG_M);
		pde_store(pde, newpdir);

		kernel_vm_end = (kernel_vm_end + NBPDR) & ~PDRMASK;
		if (kernel_vm_end - 1 >= kernel_map->max_offset) {
			kernel_vm_end = kernel_map->max_offset;
			break;                       
		}
	}
}


/***************************************************
 * page management routines.
 ***************************************************/

CTASSERT(sizeof(struct pv_chunk) == PAGE_SIZE);
CTASSERT(_NPCM == 3);
CTASSERT(_NPCPV == 168);

static __inline struct pv_chunk *
pv_to_chunk(pv_entry_t pv)
{

	return (struct pv_chunk *)((uintptr_t)pv & ~(uintptr_t)PAGE_MASK);
}

#define PV_PMAP(pv) (pv_to_chunk(pv)->pc_pmap)

#define	PC_FREE0	0xfffffffffffffffful
#define	PC_FREE1	0xfffffffffffffffful
#define	PC_FREE2	0x000000fffffffffful

static uint64_t pc_freemask[_NPCM] = { PC_FREE0, PC_FREE1, PC_FREE2 };

SYSCTL_INT(_vm_pmap, OID_AUTO, pv_entry_count, CTLFLAG_RD, &pv_entry_count, 0,
	"Current number of pv entries");

#ifdef PV_STATS
static int pc_chunk_count, pc_chunk_allocs, pc_chunk_frees, pc_chunk_tryfail;

SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_count, CTLFLAG_RD, &pc_chunk_count, 0,
	"Current number of pv entry chunks");
SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_allocs, CTLFLAG_RD, &pc_chunk_allocs, 0,
	"Current number of pv entry chunks allocated");
SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_frees, CTLFLAG_RD, &pc_chunk_frees, 0,
	"Current number of pv entry chunks frees");
SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_tryfail, CTLFLAG_RD, &pc_chunk_tryfail, 0,
	"Number of times tried to get a chunk page but failed.");

static long pv_entry_frees, pv_entry_allocs;
static int pv_entry_spare;

SYSCTL_LONG(_vm_pmap, OID_AUTO, pv_entry_frees, CTLFLAG_RD, &pv_entry_frees, 0,
	"Current number of pv entry frees");
SYSCTL_LONG(_vm_pmap, OID_AUTO, pv_entry_allocs, CTLFLAG_RD, &pv_entry_allocs, 0,
	"Current number of pv entry allocs");
SYSCTL_INT(_vm_pmap, OID_AUTO, pv_entry_spare, CTLFLAG_RD, &pv_entry_spare, 0,
	"Current number of spare pv entries");

static int pmap_collect_inactive, pmap_collect_active;

SYSCTL_INT(_vm_pmap, OID_AUTO, pmap_collect_inactive, CTLFLAG_RD, &pmap_collect_inactive, 0,
	"Current number times pmap_collect called on inactive queue");
SYSCTL_INT(_vm_pmap, OID_AUTO, pmap_collect_active, CTLFLAG_RD, &pmap_collect_active, 0,
	"Current number times pmap_collect called on active queue");
#endif

/*
 * We are in a serious low memory condition.  Resort to
 * drastic measures to free some pages so we can allocate
 * another pv entry chunk.  This is normally called to
 * unmap inactive pages, and if necessary, active pages.
 *
 * We do not, however, unmap 2mpages because subsequent accesses will
 * allocate per-page pv entries until repromotion occurs, thereby
 * exacerbating the shortage of free pv entries.
 */
#ifdef nomore
static void
pmap_collect(pmap_t locked_pmap, struct vpgqueues *vpq)
{
	struct md_page *pvh;
	pd_entry_t *pde;
	pmap_t pmap;
	pt_entry_t *pte, tpte;
	pv_entry_t next_pv, pv;
	vm_offset_t va;
	vm_page_t m, free;

	TAILQ_FOREACH(m, &vpq->pl, pageq) {
		if (m->hold_count || m->busy)
			continue;
		TAILQ_FOREACH_SAFE(pv, &m->md.pv_list, pv_list, next_pv) {
			pmap = PV_PMAP(pv);
			va = pv->pv_va;
			/* Avoid deadlock and lock recursion. */
			if (pmap > locked_pmap)
				PMAP_LOCK(pmap);
			else if (pmap != locked_pmap && !PMAP_TRYLOCK(pmap))
				continue;
			pmap->pm_stats.resident_count--;
			pde = pmap_pde(pmap, va);
			KASSERT((*pde & PG_PS) == 0, ("pmap_collect: found"
			    " a 2mpage in page %p's pv list", m));
			pte = pmap_pde_to_pte(pde, va);
			tpte = pte_load_clear(pte);
			KASSERT((tpte & PG_W) == 0,
			    ("pmap_collect: wired pte %#lx", tpte));
			if (tpte & PG_A)
				vm_page_flag_set(m, PG_REFERENCED);
			if ((tpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
				vm_page_dirty(m);
			free = NULL;
			pmap_unuse_pt(pmap, va, *pde, &free);
			pmap_invalidate_page(pmap, va);
			pmap_free_zero_pages(free);
			TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);
			if (TAILQ_EMPTY(&m->md.pv_list)) {
				pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
				if (TAILQ_EMPTY(&pvh->pv_list))
					vm_page_flag_clear(m, PG_WRITEABLE);
			}
			free_pv_entry(pmap, pv);
			if (pmap != locked_pmap)
				PMAP_UNLOCK(pmap);
		}
	}
}
#endif

/*
 * free the pv_entry back to the free list
 */
static void
free_pv_entry(pmap_t pmap, pv_entry_t pv)
{
	vm_page_t m;
	struct pv_chunk *pc;
	int idx, field, bit;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	mtx_lock(&pv_lock);
	PV_STAT(pv_entry_frees++);
	PV_STAT(pv_entry_spare++);
	pv_entry_count--;
	pc = pv_to_chunk(pv);
	idx = pv - &pc->pc_pventry[0];
	field = idx / 64;
	bit = idx % 64;
	pc->pc_map[field] |= 1ul << bit;
	/* move to head of list */
	TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
	if (pc->pc_map[0] != PC_FREE0 || pc->pc_map[1] != PC_FREE1 ||
	    pc->pc_map[2] != PC_FREE2) {
		TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
		mtx_unlock(&pv_lock);
		return;
	}
	PV_STAT(pv_entry_spare -= _NPCPV);
	PV_STAT(pc_chunk_count--);
	PV_STAT(pc_chunk_frees++);
	/* entire chunk is free, return it */
	m = PHYS_TO_VM_PAGE(DMAP_TO_PHYS((vm_offset_t)pc));
	dump_drop_page(m->phys_addr);
	mtx_unlock(&pv_lock);
	KASSERT(m->wire_count == 1, ("wire_count == %d", m->wire_count));
	m->wire_count--;
	atomic_subtract_int(&cnt.v_wire_count, 1);
	vm_page_free(m);
}

/*
 * get a new pv_entry, allocating a block from the system
 * when needed.
 */
static pv_entry_t
get_pv_entry(pmap_t pmap)
{
	static const struct timeval printinterval = { 60, 0 };
	static struct timeval lastprint;
	static vm_pindex_t colour;
	struct vpgqueues *pq;
	int bit, field;
	pv_entry_t pv;
	struct pv_chunk *pc;
	vm_page_t m;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	mtx_lock(&pv_lock);
	PV_STAT(pv_entry_allocs++);
	pv_entry_count++;
	if (pv_entry_count > pv_entry_high_water)
		if (ratecheck(&lastprint, &printinterval))
			printf("Approaching the limit on PV entries, consider "
			    "increasing either the vm.pmap.shpgperproc or the "
			    "vm.pmap.pv_entry_max sysctl.\n");
	pq = NULL;
	pc = TAILQ_FIRST(&pmap->pm_pvchunk);
	if (pc != NULL) {
		for (field = 0; field < _NPCM; field++) {
			if (pc->pc_map[field]) {
				bit = bsfq(pc->pc_map[field]);
				break;
			}
		}
		if (field < _NPCM) {
			pv = &pc->pc_pventry[field * 64 + bit];
			pc->pc_map[field] &= ~(1ul << bit);
			/* If this was the last item, move it to tail */
			if (pc->pc_map[0] == 0 && pc->pc_map[1] == 0 &&
			    pc->pc_map[2] == 0) {
				TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
				TAILQ_INSERT_TAIL(&pmap->pm_pvchunk, pc, pc_list);
			}
			PV_STAT(pv_entry_spare--);
			mtx_unlock(&pv_lock);
			return (pv);
		}
	}
	/* No free items, allocate another chunk */
	m = vm_page_alloc(NULL, colour, (pq == &vm_page_queues[PQ_ACTIVE] ?
	    VM_ALLOC_SYSTEM : VM_ALLOC_NORMAL) | VM_ALLOC_NOOBJ |
	    VM_ALLOC_WIRED);
	if (m == NULL) {
		pv_entry_count--;
		PV_STAT(pc_chunk_tryfail++);
		mtx_unlock(&pv_lock);
		return (NULL);
	}
	PV_STAT(pc_chunk_count++);
	PV_STAT(pc_chunk_allocs++);
	colour++;
	dump_add_page(m->phys_addr);
	pc = (void *)PHYS_TO_DMAP(m->phys_addr);
	pc->pc_pmap = pmap;
	pc->pc_map[0] = PC_FREE0 & ~1ul;	/* preallocated bit 0 */
	pc->pc_map[1] = PC_FREE1;
	pc->pc_map[2] = PC_FREE2;
	pv = &pc->pc_pventry[0];
	TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
	PV_STAT(pv_entry_spare += _NPCPV - 1);

	mtx_unlock(&pv_lock);
	return (pv);
}

static void
pmap_pv_list_free(pmap_t pmap, struct pv_list_head *pv_list)
{
	pv_entry_t pv;

	while (!TAILQ_EMPTY(pv_list)) {
		pv = TAILQ_FIRST(pv_list);
		TAILQ_REMOVE(pv_list, pv, pv_list);
		free_pv_entry(pmap, pv);
	}
}

static boolean_t
pmap_pv_list_alloc(pmap_t pmap, int count, struct pv_list_head *pv_list)
{
	pv_entry_t pv;
	int i;
	boolean_t slept;

	slept = FALSE;
	for (i = 0; i < count; i++) {
		while ((pv = get_pv_entry(pmap)) == NULL) {
			PMAP_UNLOCK(pmap);
			slept = TRUE;
			VM_WAIT;
			PMAP_LOCK(pmap);
		}
		TAILQ_INSERT_HEAD(pv_list, pv, pv_list);
	}

	return (slept);
}

static boolean_t
pmap_pv_list_try_alloc(pmap_t pmap, int count, struct pv_list_head *pv_list)
{
	pv_entry_t pv;
	int i;
	boolean_t success;

	success = TRUE;
	for (i = 0; i < count; i++) {
		if ((pv = get_pv_entry(pmap)) == NULL) {
			success = FALSE;
			pmap_pv_list_free(pmap, pv_list);
			goto done;
		}
		TAILQ_INSERT_HEAD(pv_list, pv, pv_list);
	}
done:
	return (success);
}

/*
 * First find and then remove the pv entry for the specified pmap and virtual
 * address from the specified pv list.  Returns the pv entry if found and NULL
 * otherwise.  This operation can be performed on pv lists for either 4KB or
 * 2MB page mappings.
 */
static __inline pv_entry_t
pmap_pvh_remove(struct md_page *pvh, pmap_t pmap, vm_offset_t va)
{
	pv_entry_t pv;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	TAILQ_FOREACH(pv, &pvh->pv_list, pv_list) {
		if (pmap == PV_PMAP(pv) && va == pv->pv_va) {
			TAILQ_REMOVE(&pvh->pv_list, pv, pv_list);
			break;
		}
	}
	return (pv);
}

/*
 * After demotion from a 2MB page mapping to 512 4KB page mappings,
 * destroy the pv entry for the 2MB page mapping and reinstantiate the pv
 * entries for each of the 4KB page mappings.
 */
static void
pmap_pv_demote_pde(pmap_t pmap, vm_offset_t va, vm_paddr_t pa,
	struct pv_list_head *pv_list)
{
	struct md_page *pvh;
	pv_entry_t pv;
	vm_offset_t va_last;
	vm_page_t m;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	PA_LOCK_ASSERT(pa, MA_OWNED);
	KASSERT((pa & PDRMASK) == 0,
	    ("pmap_pv_demote_pde: pa is not 2mpage aligned"));

	 /* Transfer the 2mpage's pv entry for this mapping to the first
	  *  page's pv list.
	  */
	pvh = pa_to_pvh(pa);
	va = trunc_2mpage(va);
	pv = pmap_pvh_remove(pvh, pmap, va);
	KASSERT(pv != NULL, ("pmap_pv_demote_pde: pv not found"));
	m = PHYS_TO_VM_PAGE(pa);
#ifdef INVARIANTS
		if (va == 0) {
			printf("inserting va==0\n");
			kdb_backtrace();
		}
#endif
	vm_page_lock(m);
	TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_list);
	vm_page_unlock(m);
	
	/* Instantiate the remaining NPTEPG - 1 pv entries. */
	va_last = va + NBPDR - PAGE_SIZE;
	do {
		m++;
		KASSERT((m->flags & (PG_FICTITIOUS | PG_UNMANAGED)) == 0,
		    ("pmap_pv_demote_pde: page %p is not managed", m));
		va += PAGE_SIZE;
		pv = TAILQ_FIRST(pv_list);
		TAILQ_REMOVE(pv_list, pv, pv_list);
#ifdef INVARIANTS
		if (va == 0) {
			printf("inserting va==0\n");
			kdb_backtrace();
		}
#endif		
		pv->pv_va = va;
		vm_page_lock(m);
		TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_list);
		vm_page_unlock(m);
	} while (va < va_last);

}

/*
 * After promotion from 512 4KB page mappings to a single 2MB page mapping,
 * replace the many pv entries for the 4KB page mappings by a single pv entry
 * for the 2MB page mapping.
 */
static void
pmap_pv_promote_pde(pmap_t pmap, vm_offset_t va, vm_paddr_t pa)
{
	struct md_page *pvh;
	pv_entry_t pv;
	vm_offset_t va_last;
	vm_page_t m;

	PA_LOCK_ASSERT(pa, MA_OWNED);
	KASSERT((pa & PDRMASK) == 0,
	    ("pmap_pv_promote_pde: pa is not 2mpage aligned"));

	/*
	 * Transfer the first page's pv entry for this mapping to the
	 * 2mpage's pv list.  Aside from avoiding the cost of a call
	 * to get_pv_entry(), a transfer avoids the possibility that
	 * get_pv_entry() calls pmap_collect() and that pmap_collect()
	 * removes one of the mappings that is being promoted.
	 */
	m = PHYS_TO_VM_PAGE(pa);
	va = trunc_2mpage(va);
	pv = pmap_pvh_remove(&m->md, pmap, va);
	KASSERT(pv != NULL, ("pmap_pv_promote_pde: pv not found"));
	pvh = pa_to_pvh(pa);
	TAILQ_INSERT_TAIL(&pvh->pv_list, pv, pv_list);
	/* Free the remaining NPTEPG - 1 pv entries. */
	va_last = va + NBPDR - PAGE_SIZE;
	do {
		m++;
		va += PAGE_SIZE;
		pmap_pvh_free(&m->md, pmap, va);
	} while (va < va_last);
}

/*
 * First find and then destroy the pv entry for the specified pmap and virtual
 * address.  This operation can be performed on pv lists for either 4KB or 2MB
 * page mappings.
 */
static void
pmap_pvh_free(struct md_page *pvh, pmap_t pmap, vm_offset_t va)
{
	pv_entry_t pv;

	pv = pmap_pvh_remove(pvh, pmap, va);
	KASSERT(pv != NULL, ("pmap_pvh_free: pv not found"));
	free_pv_entry(pmap, pv);
}

static void
pmap_remove_entry(pmap_t pmap, vm_page_t m, vm_offset_t va)
{
	struct md_page *pvh;

	vm_page_lock_assert(m, MA_OWNED);

	pmap_pvh_free(&m->md, pmap, va);
	if (TAILQ_EMPTY(&m->md.pv_list)) {
		pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
		if (TAILQ_EMPTY(&pvh->pv_list))
			vm_page_flag_clear(m, PG_WRITEABLE);
	}
}

/*
 * Conditionally create a pv entry.
 */
static boolean_t
pmap_try_insert_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m)
{
	pv_entry_t pv;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	vm_page_lock_assert(m, MA_OWNED);
	if (pv_entry_count < pv_entry_high_water && 
	    (pv = get_pv_entry(pmap)) != NULL) {
#ifdef INVARIANTS
		if (va == 0) {
			printf("inserting va==0\n");
			kdb_backtrace();
		}
#endif		
		pv->pv_va = va;
		TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_list);
		return (TRUE);
	} else
		return (FALSE);
}

/*
 * Create the pv entry for a 2MB page mapping.
 */
static boolean_t
pmap_pv_insert_pde(pmap_t pmap, vm_offset_t va, vm_paddr_t pa)
{
	struct md_page *pvh;
	pv_entry_t pv;

	PA_LOCK_ASSERT(pa, MA_OWNED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	if (pv_entry_count < pv_entry_high_water && 
	    (pv = get_pv_entry(pmap)) != NULL) {
#ifdef INVARIANTS
		if (va == 0) {
			printf("inserting va==0\n");
			kdb_backtrace();
		}
#endif		
		pv->pv_va = va;
		pvh = pa_to_pvh(pa);
		TAILQ_INSERT_TAIL(&pvh->pv_list, pv, pv_list);
		return (TRUE);
	} else
		return (FALSE);
}

/*
 * Fills a page table page with mappings to consecutive physical pages.
 */
static void
pmap_fill_ptp(pt_entry_t *firstpte, pt_entry_t newpte)
{
	pt_entry_t *pte;

	for (pte = firstpte; pte < firstpte + NPTEPG; pte++) {
		*pte = newpte;
		newpte += PAGE_SIZE;
	}
}

/*
 * Tries to demote a 2MB page mapping.  If demotion fails, the 2MB page
 * mapping is invalidated.
 */
static boolean_t
pmap_demote_pde(pmap_t pmap, pd_entry_t *pde, vm_offset_t va,
	struct pv_list_head *pv_list)
{
	pd_entry_t newpde, oldpde;
	pt_entry_t *firstpte, newpte;
	vm_paddr_t mptepa;
	vm_page_t free, mpte;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	oldpde = *pde;
	KASSERT((oldpde & (PG_PS | PG_V)) == (PG_PS | PG_V),
	    ("pmap_demote_pde: oldpde is missing PG_PS and/or PG_V"));
	mpte = pmap_lookup_pt_page(pmap, va);
	if (mpte != NULL)
		pmap_remove_pt_page(pmap, mpte);
	else {
		KASSERT((oldpde & PG_W) == 0,
		    ("pmap_demote_pde: page table page for a wired mapping"
		    " is missing"));

		/*
		 * Invalidate the 2MB page mapping and return "failure" if the
		 * mapping was never accessed or the allocation of the new
		 * page table page fails.  If the 2MB page mapping belongs to
		 * the direct map region of the kernel's address space, then
		 * the page allocation request specifies the highest possible
		 * priority (VM_ALLOC_INTERRUPT).  Otherwise, the priority is
		 * normal.  Page table pages are preallocated for every other
		 * part of the kernel address space, so the direct map region
		 * is the only part of the kernel address space that must be
		 * handled here.
		 */
		if ((oldpde & PG_A) == 0 || (mpte = vm_page_alloc(NULL,
		    pmap_pde_pindex(va), (va >= DMAP_MIN_ADDRESS && va <
		    DMAP_MAX_ADDRESS ? VM_ALLOC_INTERRUPT : VM_ALLOC_NORMAL) |
		    VM_ALLOC_NOOBJ | VM_ALLOC_WIRED)) == NULL) {
			free = NULL;
			pmap_remove_pde(pmap, pde, trunc_2mpage(va), &free, pv_list);
			pmap_invalidate_page(pmap, trunc_2mpage(va));
			pmap_free_zero_pages(free);
			CTR2(KTR_PMAP, "pmap_demote_pde: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return (FALSE);
		}
		if (va < VM_MAXUSER_ADDRESS)
			pmap->pm_stats.resident_count++;
	}
	if (TAILQ_EMPTY(pv_list) && ((oldpde & PG_MANAGED) != 0)) {
		if (pmap_pv_list_try_alloc(pmap, NPTEPG-1, pv_list) == FALSE)
			return (FALSE);
	}
	mptepa = VM_PAGE_TO_PHYS(mpte);
	firstpte = (pt_entry_t *)PHYS_TO_DMAP(mptepa);
	newpde = mptepa | PG_M | PG_A | (oldpde & PG_U) | PG_RW | PG_V;
	KASSERT((oldpde & PG_A) != 0,
	    ("pmap_demote_pde: oldpde is missing PG_A"));
	KASSERT((oldpde & (PG_M | PG_RW)) != PG_RW,
	    ("pmap_demote_pde: oldpde is missing PG_M"));
	newpte = oldpde & ~PG_PS;
	if ((newpte & PG_PDE_PAT) != 0)
		newpte ^= PG_PDE_PAT | PG_PTE_PAT;

	/*
	 * If the page table page is new, initialize it.
	 */
	if (mpte->wire_count == 1) {
		mpte->wire_count = NPTEPG;
		pmap_fill_ptp(firstpte, newpte);
	}
	KASSERT((*firstpte & PG_FRAME) == (newpte & PG_FRAME),
	    ("pmap_demote_pde: firstpte and newpte map different physical"
	    " addresses"));

	/*
	 * If the mapping has changed attributes, update the page table
	 * entries.
	 */
	if ((*firstpte & PG_PTE_PROMOTE) != (newpte & PG_PTE_PROMOTE))
		pmap_fill_ptp(firstpte, newpte);

	/*
	 * Demote the mapping.  This pmap is locked.  The old PDE has
	 * PG_A set.  If the old PDE has PG_RW set, it also has PG_M
	 * set.  Thus, there is no danger of a race with another
	 * processor changing the setting of PG_A and/or PG_M between
	 * the read above and the store below. 
	 */
	if (workaround_erratum383)
		pmap_update_pde(pmap, va, pde, newpde);
	else
		pde_store(pde, newpde);

	/*
	 * Invalidate a stale recursive mapping of the page table page.
	 */
	if (va >= VM_MAXUSER_ADDRESS)
		pmap_invalidate_page(pmap, (vm_offset_t)vtopte(va));

	/*
	 * Demote the pv entry.  This depends on the earlier demotion
	 * of the mapping.  Specifically, the (re)creation of a per-
	 * page pv entry might trigger the execution of pmap_collect(),
	 * which might reclaim a newly (re)created per-page pv entry
	 * and destroy the associated mapping.  In order to destroy
	 * the mapping, the PDE must have already changed from mapping
	 * the 2mpage to referencing the page table page.
	 */
	if ((oldpde & PG_MANAGED) != 0)
		pmap_pv_demote_pde(pmap, va, oldpde & PG_PS_FRAME, pv_list);

	pmap_pde_demotions++;
	CTR2(KTR_PMAP, "pmap_demote_pde: success for va %#lx"
	    " in pmap %p", va, pmap);
	return (TRUE);
}

/*
 * pmap_remove_pde: do the things to unmap a superpage in a process
 */
static int
pmap_remove_pde(pmap_t pmap, pd_entry_t *pdq, vm_offset_t sva,
    vm_page_t *free, struct pv_list_head *pv_list)
{
	struct md_page *pvh;
	pd_entry_t oldpde;
	vm_offset_t eva, va;
	vm_page_t m, mpte;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT((sva & PDRMASK) == 0,
	    ("pmap_remove_pde: sva is not 2mpage aligned"));
	oldpde = pte_load_clear(pdq);
	if (oldpde & PG_W)
		pmap->pm_stats.wired_count -= NBPDR / PAGE_SIZE;

	/*
	 * Machines that don't support invlpg, also don't support
	 * PG_G.
	 */
	if (oldpde & PG_G)
		pmap_invalidate_page(kernel_pmap, sva);
	pmap->pm_stats.resident_count -= NBPDR / PAGE_SIZE;
	if (oldpde & PG_MANAGED) {
		pvh = pa_to_pvh(oldpde & PG_PS_FRAME);
		pmap_pvh_free(pvh, pmap, sva);
		eva = sva + NBPDR;
		for (va = sva, m = PHYS_TO_VM_PAGE(oldpde & PG_PS_FRAME);
		    va < eva; va += PAGE_SIZE, m++) {
			/*
			 * XXX do we need to individually lock each page? 
			 *
			 */
			if ((oldpde & (PG_M | PG_RW)) == (PG_M | PG_RW))
				vm_page_dirty(m);
			if (oldpde & PG_A)
				vm_page_flag_set(m, PG_REFERENCED);
			if (TAILQ_EMPTY(&m->md.pv_list) &&
			    TAILQ_EMPTY(&pvh->pv_list))
				vm_page_flag_clear(m, PG_WRITEABLE);
		}
	}
	if (pmap == kernel_pmap) {
		if (!pmap_demote_pde(pmap, pdq, sva, pv_list))
			panic("pmap_remove_pde: failed demotion");
	} else {
		mpte = pmap_lookup_pt_page(pmap, sva);
		if (mpte != NULL) {
			pmap_remove_pt_page(pmap, mpte);
			pmap->pm_stats.resident_count--;
			KASSERT(mpte->wire_count == NPTEPG,
			    ("pmap_remove_pde: pte page wire count error"));
			mpte->wire_count = 0;
			pmap_add_delayed_free_list(mpte, free, FALSE);
			atomic_subtract_int(&cnt.v_wire_count, 1);
		}
	}
	return (pmap_unuse_pt(pmap, sva, *pmap_pdpe(pmap, sva), free));
}


/*
 * pmap_remove_pte: do the things to unmap a page in a process
 */
static int
pmap_remove_pte(pmap_t pmap, pt_entry_t *ptq, vm_offset_t va, 
    pd_entry_t ptepde, vm_page_t *free)
{
	pt_entry_t oldpte;
	vm_page_t m;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	oldpte = pte_load_clear(ptq);
	if (oldpte & PG_W)
		pmap->pm_stats.wired_count -= 1;
	/*
	 * Machines that don't support invlpg, also don't support
	 * PG_G.
	 */
	if (oldpte & PG_G)
		pmap_invalidate_page(kernel_pmap, va);
	pmap->pm_stats.resident_count -= 1;
	if (oldpte & PG_MANAGED) {
		m = PHYS_TO_VM_PAGE(oldpte & PG_FRAME);
		vm_page_lock_assert(m, MA_OWNED);
		if ((oldpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
			vm_page_dirty(m);
		if (oldpte & PG_A)
			vm_page_flag_set(m, PG_REFERENCED);
		pmap_remove_entry(pmap, m, va);
	}
	return (pmap_unuse_pt(pmap, va, ptepde, free));
}

/*
 * Remove a single page from a process address space
 */
static void
pmap_remove_page(pmap_t pmap, vm_offset_t va, pd_entry_t *pde, vm_page_t *free)
{
	pt_entry_t *pte;
	vm_paddr_t pa = 0;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	if ((*pde & PG_V) == 0)
		return;
	pte = pmap_pde_to_pte(pde, va);
	if ((*pte & PG_V) == 0)
		return;
	if  (*pte & PG_MANAGED)
		(void)pa_tryrelock(pmap, *pte & PG_FRAME, &pa);

	pmap_remove_pte(pmap, pte, va, *pde, free);
	if (pa)
		PA_UNLOCK(pa);
	pmap_invalidate_page(pmap, va);
}

static void
pmap_prealloc_pv_list(pmap_t pmap, vm_offset_t sva, vm_offset_t eva,
	struct pv_list_head *pv_list)
{
	vm_offset_t va_next;
	pml4_entry_t *pml4e;
	pdp_entry_t *pdpe;
	pd_entry_t ptpaddr, *pde;
	pt_entry_t *pte;
	int i, alloc_count;

	alloc_count = 0;
	PMAP_LOCK(pmap);
	for (; sva < eva; sva = va_next) {

		pml4e = pmap_pml4e(pmap, sva);
		if ((*pml4e & PG_V) == 0) {
			va_next = (sva + NBPML4) & ~PML4MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		pdpe = pmap_pml4e_to_pdpe(pml4e, sva);
		if ((*pdpe & PG_V) == 0) {
			va_next = (sva + NBPDP) & ~PDPMASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		/*
		 * Calculate index for next page table.
		 */
		va_next = (sva + NBPDR) & ~PDRMASK;
		if (va_next < sva)
			va_next = eva;

		pde = pmap_pdpe_to_pde(pdpe, sva);
		ptpaddr = *pde;

		/*
		 * Weed out invalid mappings.
		 */
		if (ptpaddr == 0)
			continue;

		/*
		 * Check for large page.
		 */
		if ((ptpaddr & PG_PS) != 0) {
			alloc_count++;
			continue;
		}
		/*
		 * Limit our scan to either the end of the va represented
		 * by the current page table page, or to the end of the
		 * range being removed.
		 */
		if (va_next > eva)
			va_next = eva;

		for (pte = pmap_pde_to_pte(pde, sva); sva != va_next; pte++,
		    sva += PAGE_SIZE) {
			if (*pte == 0)
				continue;
		}
	}
	for (i = 0; i < alloc_count; i++)
		pmap_pv_list_alloc(pmap, NPTEPG-1, pv_list);

	PMAP_UNLOCK(pmap);
}

/*
 *	Remove the given range of addresses from the specified map.
 *
 *	It is assumed that the start and end are properly
 *	rounded to the page size.
 */
void
pmap_remove(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	vm_offset_t va_next;
	pml4_entry_t *pml4e;
	pdp_entry_t *pdpe;
	pd_entry_t ptpaddr, *pde;
	pt_entry_t *pte;
	vm_paddr_t pa;
	vm_page_t free = NULL;
	struct pv_list_head pv_list;
	int anyvalid;

	/*
	 * Perform an unsynchronized read.  This is, however, safe.
	 */
	if (pmap->pm_stats.resident_count == 0)
		return;

	pa = anyvalid = 0;
	TAILQ_INIT(&pv_list);

	/*
	 * pre-allocate pvs
	 *
	 */
	if ((pmap == kernel_pmap) &&
	    (sva + PAGE_SIZE != eva)) 
		pmap_prealloc_pv_list(pmap, sva, eva, &pv_list);

	PMAP_LOCK(pmap);
restart:
	/*
	 * special handling of removing one page.  a very
	 * common operation and easy to short circuit some
	 * code.
	 */
	if (sva + PAGE_SIZE == eva) {
		pde = pmap_pde(pmap, sva);
		if (pde && (*pde & PG_PS) == 0) {
			pmap_remove_page(pmap, sva, pde, &free);
			goto out;
		}
	}

	for (; sva < eva; sva = va_next) {

		if (pmap->pm_stats.resident_count == 0)
			break;

		pml4e = pmap_pml4e(pmap, sva);
		if ((*pml4e & PG_V) == 0) {
			va_next = (sva + NBPML4) & ~PML4MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		pdpe = pmap_pml4e_to_pdpe(pml4e, sva);
		if ((*pdpe & PG_V) == 0) {
			va_next = (sva + NBPDP) & ~PDPMASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		/*
		 * Calculate index for next page table.
		 */
		va_next = (sva + NBPDR) & ~PDRMASK;
		if (va_next < sva)
			va_next = eva;

		pde = pmap_pdpe_to_pde(pdpe, sva);
		ptpaddr = *pde;

		/*
		 * Weed out invalid mappings.
		 */
		if (ptpaddr == 0)
			continue;

		/*
		 * Check for large page.
		 */
		if ((ptpaddr & PG_PS) != 0) {
			if (pa_tryrelock(pmap, ptpaddr & PG_FRAME, &pa)) {
				va_next = sva;
				continue;
			}

			/*
			 * Are we removing the entire large page?  If not,
			 * demote the mapping and fall through.
			 */
			if (sva + NBPDR == va_next && eva >= va_next) {
				/*
				 * The TLB entry for a PG_G mapping is
				 * invalidated by pmap_remove_pde().
				 */
				if ((ptpaddr & PG_G) == 0)
					anyvalid = 1;
				pmap_remove_pde(pmap, pde, sva, &free, &pv_list);
				continue;
			} else if (!pmap_demote_pde(pmap, pde, sva, &pv_list)) {
				/* The large page mapping was destroyed. */
				continue;
			} else
				ptpaddr = *pde;
		}

		/*
		 * Limit our scan to either the end of the va represented
		 * by the current page table page, or to the end of the
		 * range being removed.
		 */
		if (va_next > eva)
			va_next = eva;

		for (pte = pmap_pde_to_pte(pde, sva); sva != va_next; pte++,
		    sva += PAGE_SIZE) {
			int ret;

			if (*pte == 0)
				continue;

			if  ((*pte & PG_MANAGED) &&
			    pa_tryrelock(pmap, *pte & PG_FRAME, &pa))
				goto restart;

			/*
			 * The TLB entry for a PG_G mapping is invalidated
			 * by pmap_remove_pte().
			 */
			if ((*pte & PG_G) == 0)
				anyvalid = 1;
			ret = pmap_remove_pte(pmap, pte, sva, ptpaddr, &free);

			if (ret)
				break;
		}
	}
out:
	if (pa)
		PA_UNLOCK(pa);
	if (anyvalid)
		pmap_invalidate_all(pmap);
	if (!TAILQ_EMPTY(&pv_list))
		pmap_pv_list_free(pmap, &pv_list);

	PMAP_UNLOCK(pmap);
	pmap_free_zero_pages(free);
}

/*
 *	Routine:	pmap_remove_all
 *	Function:
 *		Removes this physical page from
 *		all physical maps in which it resides.
 *		Reflects back modify bits to the pager.
 *
 *	Notes:
 *		Original versions of this routine were very
 *		inefficient because they iteratively called
 *		pmap_remove (slow...)
 */

void
pmap_remove_all(vm_page_t m)
{
	struct md_page *pvh;
	pv_entry_t pv;
	pmap_t pmap;
	pt_entry_t *pte, tpte;
	pd_entry_t *pde;
	vm_offset_t va;
	vm_page_t free;
	struct pv_list_head pv_list;
	
	TAILQ_INIT(&pv_list);
	KASSERT((m->flags & PG_FICTITIOUS) == 0,
	    ("pmap_remove_all: page %p is fictitious", m));
	vm_page_lock_assert(m, MA_OWNED);
	pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
	while ((pv = TAILQ_FIRST(&pvh->pv_list)) != NULL) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		va = pv->pv_va;
		pde = pmap_pde(pmap, va);
		(void)pmap_demote_pde(pmap, pde, va, &pv_list);
		PMAP_UNLOCK(pmap);
	}
	while ((pv = TAILQ_FIRST(&m->md.pv_list)) != NULL) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		pmap->pm_stats.resident_count--;
		pde = pmap_pde(pmap, pv->pv_va);
		KASSERT((*pde & PG_PS) == 0, ("pmap_remove_all: found"
		    " a 2mpage in page %p's pv list", m));
		pte = pmap_pde_to_pte(pde, pv->pv_va);
		tpte = pte_load_clear(pte);
		if (tpte & PG_W)
			pmap->pm_stats.wired_count--;
		if (tpte & PG_A)
			vm_page_flag_set(m, PG_REFERENCED);

		/*
		 * Update the vm_page_t clean and reference bits.
		 */
		if ((tpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
			vm_page_dirty(m);
		free = NULL;
		pmap_unuse_pt(pmap, pv->pv_va, *pde, &free);
		pmap_invalidate_page(pmap, pv->pv_va);
		pmap_free_zero_pages(free);
		TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);
		free_pv_entry(pmap, pv);
		PMAP_UNLOCK(pmap);
	}
	vm_page_flag_clear(m, PG_WRITEABLE);
}

/*
 * pmap_protect_pde: do the things to protect a 2mpage in a process
 */
static boolean_t
pmap_protect_pde(pmap_t pmap, pd_entry_t *pde, vm_offset_t sva, vm_prot_t prot)
{
	pd_entry_t newpde, oldpde;
	vm_offset_t eva, va;
	vm_page_t m;
	boolean_t anychanged;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT((sva & PDRMASK) == 0,
	    ("pmap_protect_pde: sva is not 2mpage aligned"));
	anychanged = FALSE;
retry:
	oldpde = newpde = *pde;
	if (oldpde & PG_MANAGED) {
		eva = sva + NBPDR;
		for (va = sva, m = PHYS_TO_VM_PAGE(oldpde & PG_PS_FRAME);
		    va < eva; va += PAGE_SIZE, m++) {
			/*
			 * In contrast to the analogous operation on a 4KB page
			 * mapping, the mapping's PG_A flag is not cleared and
			 * the page's PG_REFERENCED flag is not set.  The
			 * reason is that pmap_demote_pde() expects that a 2MB
			 * page mapping with a stored page table page has PG_A
			 * set.
			 */
			if ((oldpde & (PG_M | PG_RW)) == (PG_M | PG_RW))
				vm_page_dirty(m);
		}
	}
	if ((prot & VM_PROT_WRITE) == 0)
		newpde &= ~(PG_RW | PG_M);
	if ((prot & VM_PROT_EXECUTE) == 0)
		newpde |= pg_nx;
	if (newpde != oldpde) {
		if (!atomic_cmpset_long(pde, oldpde, newpde))
			goto retry;
		if (oldpde & PG_G)
			pmap_invalidate_page(pmap, sva);
		else
			anychanged = TRUE;
	}
	return (anychanged);
}

/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 */
void
pmap_protect(pmap_t pmap, vm_offset_t sva, vm_offset_t eva, vm_prot_t prot)
{
	vm_offset_t va_next;
	pml4_entry_t *pml4e;
	pdp_entry_t *pdpe;
	pd_entry_t ptpaddr, *pde;
	pt_entry_t *pte;
	int anychanged;
	vm_paddr_t pa;
	struct pv_list_head pv_list;

	if ((prot & VM_PROT_READ) == VM_PROT_NONE) {
		pmap_remove(pmap, sva, eva);
		return;
	}

	if ((prot & (VM_PROT_WRITE|VM_PROT_EXECUTE)) ==
	    (VM_PROT_WRITE|VM_PROT_EXECUTE))
		return;

	TAILQ_INIT(&pv_list);
	pa = 0;
	anychanged = 0;
	PMAP_LOCK(pmap);
restart:	
	for (; sva < eva; sva = va_next) {

		pml4e = pmap_pml4e(pmap, sva);
		if ((*pml4e & PG_V) == 0) {
			va_next = (sva + NBPML4) & ~PML4MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		pdpe = pmap_pml4e_to_pdpe(pml4e, sva);
		if ((*pdpe & PG_V) == 0) {
			va_next = (sva + NBPDP) & ~PDPMASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		va_next = (sva + NBPDR) & ~PDRMASK;
		if (va_next < sva)
			va_next = eva;

		pde = pmap_pdpe_to_pde(pdpe, sva);
		ptpaddr = *pde;

		/*
		 * Weed out invalid mappings.
		 */
		if (ptpaddr == 0)
			continue;

		/*
		 * Check for large page.
		 */
		if ((ptpaddr & PG_PS) != 0) {
			/*
			 * Are we protecting the entire large page?  If not,
			 * demote the mapping and fall through.
			 */
			if (sva + NBPDR == va_next && eva >= va_next) {
				/*
				 * The TLB entry for a PG_G mapping is
				 * invalidated by pmap_protect_pde().
				 */
				if (pmap_protect_pde(pmap, pde, sva, prot))
					anychanged = 1;
				continue;
			} else if (!pmap_demote_pde(pmap, pde, sva, &pv_list)) {
				/* The large page mapping was destroyed. */
				continue;
			}
		}

		if (va_next > eva)
			va_next = eva;

		for (pte = pmap_pde_to_pte(pde, sva); sva != va_next; pte++,
		    sva += PAGE_SIZE) {
			pt_entry_t obits, pbits;
			vm_page_t m;

retry:
			obits = pbits = *pte;
			if ((pbits & PG_V) == 0)
				continue;
			if (pbits & PG_MANAGED) {
				m = NULL;
				if (pa_tryrelock(pmap, pbits & PG_FRAME, &pa))
					goto restart;
				if (pbits & PG_A) {
					m = PHYS_TO_VM_PAGE(pbits & PG_FRAME);
					vm_page_flag_set(m, PG_REFERENCED);
					pbits &= ~PG_A;
				}
				if ((pbits & (PG_M | PG_RW)) == (PG_M | PG_RW)) {
					if (m == NULL)
						m = PHYS_TO_VM_PAGE(pbits &
						    PG_FRAME);
					vm_page_dirty(m);
				}
			}

			if ((prot & VM_PROT_WRITE) == 0)
				pbits &= ~(PG_RW | PG_M);
			if ((prot & VM_PROT_EXECUTE) == 0)
				pbits |= pg_nx;

			if (pbits != obits) {
				if (!atomic_cmpset_long(pte, obits, pbits))
					goto retry;
				if (obits & PG_G)
					pmap_invalidate_page(pmap, sva);
				else
					anychanged = 1;
			}
		}
	}
	if (pa)
		PA_UNLOCK(pa);
	if (anychanged)
		pmap_invalidate_all(pmap);
	PMAP_UNLOCK(pmap);
}

/*
 * Tries to promote the 512, contiguous 4KB page mappings that are within a
 * single page table page (PTP) to a single 2MB page mapping.  For promotion
 * to occur, two conditions must be met: (1) the 4KB page mappings must map
 * aligned, contiguous physical memory and (2) the 4KB page mappings must have
 * identical characteristics. 
 */
static void
pmap_promote_pde(pmap_t pmap, pd_entry_t *pde, vm_offset_t va)
{
	pd_entry_t newpde;
	pt_entry_t *firstpte, oldpte, pa, *pte;
	vm_offset_t oldpteva;
	vm_page_t mpte;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	/*
	 * Examine the first PTE in the specified PTP.  Abort if this PTE is
	 * either invalid, unused, or does not map the first 4KB physical page
	 * within a 2MB page. 
	 */
	firstpte = (pt_entry_t *)PHYS_TO_DMAP(*pde & PG_FRAME);
setpde:
	newpde = *firstpte;
	if ((newpde & ((PG_FRAME & PDRMASK) | PG_A | PG_V)) != (PG_A | PG_V)) {
		pmap_pde_p_failures++;
		CTR2(KTR_PMAP, "pmap_promote_pde: failure for va %#lx"
		    " in pmap %p", va, pmap);
		return;
	}
	if ((newpde & (PG_M | PG_RW)) == PG_RW) {
		/*
		 * When PG_M is already clear, PG_RW can be cleared without
		 * a TLB invalidation.
		 */
		if (!atomic_cmpset_long(firstpte, newpde, newpde & ~PG_RW))
			goto setpde;
		newpde &= ~PG_RW;
	}

	/*
	 * Examine each of the other PTEs in the specified PTP.  Abort if this
	 * PTE maps an unexpected 4KB physical page or does not have identical
	 * characteristics to the first PTE.
	 */
	pa = (newpde & (PG_PS_FRAME | PG_A | PG_V)) + NBPDR - PAGE_SIZE;
	for (pte = firstpte + NPTEPG - 1; pte > firstpte; pte--) {
setpte:
		oldpte = *pte;
		if ((oldpte & (PG_FRAME | PG_A | PG_V)) != pa) {
			pmap_pde_p_failures++;
			CTR2(KTR_PMAP, "pmap_promote_pde: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return;
		}
		if ((oldpte & (PG_M | PG_RW)) == PG_RW) {
			/*
			 * When PG_M is already clear, PG_RW can be cleared
			 * without a TLB invalidation.
			 */
			if (!atomic_cmpset_long(pte, oldpte, oldpte & ~PG_RW))
				goto setpte;
			oldpte &= ~PG_RW;
			oldpteva = (oldpte & PG_FRAME & PDRMASK) |
			    (va & ~PDRMASK);
			CTR2(KTR_PMAP, "pmap_promote_pde: protect for va %#lx"
			    " in pmap %p", oldpteva, pmap);
		}
		if ((oldpte & PG_PTE_PROMOTE) != (newpde & PG_PTE_PROMOTE)) {
			pmap_pde_p_failures++;
			CTR2(KTR_PMAP, "pmap_promote_pde: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return;
		}
		pa -= PAGE_SIZE;
	}

	/*
	 * Save the page table page in its current state until the PDE
	 * mapping the superpage is demoted by pmap_demote_pde() or
	 * destroyed by pmap_remove_pde(). 
	 */
	mpte = PHYS_TO_VM_PAGE(*pde & PG_FRAME);
	KASSERT(mpte >= vm_page_array &&
	    mpte < &vm_page_array[vm_page_array_size],
	    ("pmap_promote_pde: page table page is out of range"));
	KASSERT(mpte->pindex == pmap_pde_pindex(va),
	    ("pmap_promote_pde: page table page's pindex is wrong"));
	pmap_insert_pt_page(pmap, mpte);

	/*
	 * Promote the pv entries.
	 */
	if ((newpde & PG_MANAGED) != 0)
		pmap_pv_promote_pde(pmap, va, newpde & PG_PS_FRAME);

	/*
	 * Propagate the PAT index to its proper position.
	 */
	if ((newpde & PG_PTE_PAT) != 0)
		newpde ^= PG_PDE_PAT | PG_PTE_PAT;

	/*
	 * Map the superpage.
	 */
	if (workaround_erratum383)
		pmap_update_pde(pmap, va, pde, PG_PS | newpde);
	else
		pde_store(pde, PG_PS | newpde);

	pmap_pde_promotions++;
	CTR2(KTR_PMAP, "pmap_promote_pde: success for va %#lx"
	    " in pmap %p", va, pmap);
}

/*
 *	Insert the given physical page (p) at
 *	the specified virtual address (v) in the
 *	target physical map with the protection requested.
 *
 *	If specified, the page will be wired down, meaning
 *	that the related pte can not be reclaimed.
 *
 *	NB:  This is the only routine which MAY NOT lazy-evaluate
 *	or lose information.  That is, this routine must actually
 *	insert this page into the given map NOW.
 */
void
pmap_enter(pmap_t pmap, vm_offset_t va, vm_prot_t access, vm_page_t m,
    vm_prot_t prot, boolean_t wired)
{
	vm_paddr_t pa;
	pd_entry_t *pde;
	pt_entry_t *pte;
	vm_paddr_t opa, lockedpa;
	pt_entry_t origpte, newpte;
	vm_page_t mpte, om;
	boolean_t invlva, opalocked;
	pv_entry_t pv;
	struct lock_stack ls;

	va = trunc_page(va);

	KASSERT(va <= VM_MAX_KERNEL_ADDRESS, ("pmap_enter: toobig"));
	KASSERT(va < UPT_MIN_ADDRESS || va >= UPT_MAX_ADDRESS,
	    ("pmap_enter: invalid to pmap_enter page table pages (va: 0x%lx)", va));

	mpte = NULL;
	pv = NULL;
	lockedpa = pa = VM_PAGE_TO_PHYS(m);
	opa = 0;
	opalocked = FALSE;
	ls_init(&ls);
	ls_push(&ls, PA_LOCKPTR(lockedpa));
	ls_push(&ls, PMAP_LOCKPTR(pmap));
	if ((m->flags & (PG_FICTITIOUS | PG_UNMANAGED)) == 0) {
		while ((pv = get_pv_entry(pmap)) == NULL) {
			ls_popa(&ls);
			VM_WAIT;
			ls_push(&ls, PA_LOCKPTR(lockedpa));
			ls_push(&ls, PMAP_LOCKPTR(pmap));
		}
	}
	
restart:
	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
	if (va < VM_MAXUSER_ADDRESS && mpte == NULL)
		mpte = pmap_allocpte(pmap, lockedpa, va, M_WAITOK);

	pde = pmap_pde(pmap, va);
	if (pde != NULL && (*pde & PG_V) != 0) {
		if ((*pde & PG_PS) != 0)
			panic("pmap_enter: attempted pmap_enter on 2MB page");
		pte = pmap_pde_to_pte(pde, va);
	} else
		panic("pmap_enter: invalid page directory va=%#lx", va);

	om = NULL;
	origpte = *pte;
	if (opa && (opa != (origpte & PG_FRAME))) {
		ls_popa(&ls);
		ls_push(&ls, PA_LOCKPTR(lockedpa));
		ls_push(&ls, PMAP_LOCKPTR(pmap));
		opalocked = FALSE;
		opa = 0;
		goto restart;
	}

	opa = origpte & PG_FRAME;
	if (opa && (opa != lockedpa) && (opalocked == FALSE)) {
		opalocked = TRUE;
		if (ls_trypush(&ls, PA_LOCKPTR(opa)) == 0) {
			ls_popa(&ls);
			if ((uintptr_t)PA_LOCKPTR(lockedpa) <
			    (uintptr_t)PA_LOCKPTR(opa)) {
				ls_push(&ls, PA_LOCKPTR(lockedpa));
				ls_push(&ls, PA_LOCKPTR(opa));
			} else {
				ls_push(&ls, PA_LOCKPTR(opa));
				ls_push(&ls, PA_LOCKPTR(lockedpa));
			}
			ls_push(&ls, PMAP_LOCKPTR(pmap));
			goto restart;
		}
	}
	    
	/*
	 * Mapping has not changed, must be protection or wiring change.
	 */
	if (origpte && (opa == pa)) {
		/*
		 * Wiring change, just update stats. We don't worry about
		 * wiring PT pages as they remain resident as long as there
		 * are valid mappings in them. Hence, if a user page is wired,
		 * the PT page will be also.
		 */
		if (wired && ((origpte & PG_W) == 0))
			pmap->pm_stats.wired_count++;
		else if (!wired && (origpte & PG_W))
			pmap->pm_stats.wired_count--;

		/*
		 * Remove extra pte reference
		 */
		if (mpte)
			mpte->wire_count--;

		/*
		 * We might be turning off write access to the page,
		 * so we go ahead and sense modify status.
		 */
		if (origpte & PG_MANAGED) {
			om = m;
			pa |= PG_MANAGED;
		}
		goto validate;
	} 
	
	/*
	 * Mapping has changed, invalidate old range and fall through to
	 * handle validating new mapping.
	 */
	if (opa) {
		if (origpte & PG_W)
			pmap->pm_stats.wired_count--;
		if (origpte & PG_MANAGED) {
			om = PHYS_TO_VM_PAGE(opa);
			vm_page_lock_assert(om, MA_OWNED);
			pmap_remove_entry(pmap, om, va);
		}
		if (mpte != NULL) {
			mpte->wire_count--;
			KASSERT(mpte->wire_count > 0,
			    ("pmap_enter: missing reference to page table page,"
			     " va: 0x%lx", va));
		}
	} else
		pmap->pm_stats.resident_count++;

	/*
	 * Enter on the PV list if part of our managed memory.
	 */
	if ((m->flags & (PG_FICTITIOUS | PG_UNMANAGED)) == 0) {
		KASSERT(va < kmi.clean_sva || va >= kmi.clean_eva,
		    ("pmap_enter: managed mapping within the clean submap"));
#ifdef INVARIANTS
		if (va == 0) {
			printf("inserting va==0\n");
			kdb_backtrace();
		}
#endif		
		pv->pv_va = va;
		TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_list);
		pa |= PG_MANAGED;
		pv = NULL;
	}

	/*
	 * Increment counters
	 */
	if (wired)
		pmap->pm_stats.wired_count++;

validate:
	vm_page_lock_assert(m, MA_OWNED);
	/*
	 * Now validate mapping with desired protection/wiring.
	 */
	newpte = (pt_entry_t)(pa | pmap_cache_bits(m->md.pat_mode, 0) | PG_V);
	if ((prot & VM_PROT_WRITE) != 0) {
		newpte |= PG_RW;
		vm_page_flag_set(m, PG_WRITEABLE);
	}
	if ((prot & VM_PROT_EXECUTE) == 0)
		newpte |= pg_nx;
	if (wired)
		newpte |= PG_W;
	if (va < VM_MAXUSER_ADDRESS)
		newpte |= PG_U;
	if (pmap == kernel_pmap)
		newpte |= PG_G;

	/*
	 * if the mapping or permission bits are different, we need
	 * to update the pte.
	 */
	if ((origpte & ~(PG_M|PG_A)) != newpte) {
		newpte |= PG_A;
		if ((access & VM_PROT_WRITE) != 0)
			newpte |= PG_M;
		if (origpte & PG_V) {
			invlva = FALSE;
			origpte = pte_load_store(pte, newpte);
			if (origpte & PG_A) {
				if (origpte & PG_MANAGED)
					vm_page_flag_set(om, PG_REFERENCED);
				if (opa != VM_PAGE_TO_PHYS(m) || ((origpte &
				    PG_NX) == 0 && (newpte & PG_NX)))
					invlva = TRUE;
			}
			if ((origpte & (PG_M | PG_RW)) == (PG_M | PG_RW)) {
				if ((origpte & PG_MANAGED) != 0)
					vm_page_dirty(om);
				if ((newpte & PG_RW) == 0)
					invlva = TRUE;
			}
			if (invlva)
				pmap_invalidate_page(pmap, va);
		} else
			pte_store(pte, newpte);
	}

	/*
	 * If both the page table page and the reservation are fully
	 * populated, then attempt promotion.
	 */
	if ((mpte == NULL || mpte->wire_count == NPTEPG) &&
	    pg_ps_enabled && vm_reserv_level_iffullpop(m) == 0)
		pmap_promote_pde(pmap, pde, va);

	if (pv != NULL)
		free_pv_entry(pmap, pv);
	ls_popa(&ls);
}

/*
 * Tries to create a 2MB page mapping.  Returns TRUE if successful and FALSE
 * otherwise.  Fails if (1) a page table page cannot be allocated without
 * blocking, (2) a mapping already exists at the specified virtual address, or
 * (3) a pv entry cannot be allocated without reclaiming another pv entry. 
 */
static boolean_t
pmap_enter_pde(pmap_t pmap, vm_offset_t va, vm_page_t m, vm_prot_t prot)
{
	pd_entry_t *pde, newpde;
	vm_page_t free, mpde;
	vm_paddr_t pa;

	vm_page_lock_assert(m, MA_OWNED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	pa = VM_PAGE_TO_PHYS(m);
	if ((mpde = pmap_allocpde(pmap, pa, va, M_NOWAIT)) == NULL) {
		CTR2(KTR_PMAP, "pmap_enter_pde: failure for va %#lx"
		    " in pmap %p", va, pmap);
		return (FALSE);
	}
	pde = (pd_entry_t *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(mpde));
	pde = &pde[pmap_pde_index(va)];
	if ((*pde & PG_V) != 0) {
		KASSERT(mpde->wire_count > 1,
		    ("pmap_enter_pde: mpde's wire count is too low"));
		mpde->wire_count--;
		CTR2(KTR_PMAP, "pmap_enter_pde: failure for va %#lx"
		    " in pmap %p", va, pmap);
		return (FALSE);
	}
	newpde = VM_PAGE_TO_PHYS(m) | pmap_cache_bits(m->md.pat_mode, 1) |
	    PG_PS | PG_V;
	if ((m->flags & (PG_FICTITIOUS | PG_UNMANAGED)) == 0) {
		newpde |= PG_MANAGED;

		/*
		 * Abort this mapping if its PV entry could not be created.
		 */
		if (!pmap_pv_insert_pde(pmap, va, VM_PAGE_TO_PHYS(m))) {
			free = NULL;
			if (pmap_unwire_pte_hold(pmap, va, mpde, &free)) {
				pmap_invalidate_page(pmap, va);
				pmap_free_zero_pages(free);
			}
			CTR2(KTR_PMAP, "pmap_enter_pde: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return (FALSE);
		}
	}
	if ((prot & VM_PROT_EXECUTE) == 0)
		newpde |= pg_nx;
	if (va < VM_MAXUSER_ADDRESS)
		newpde |= PG_U;

	/*
	 * Increment counters.
	 */
	pmap->pm_stats.resident_count += NBPDR / PAGE_SIZE;

	/*
	 * Map the superpage.
	 */
	pde_store(pde, newpde);

	pmap_pde_mappings++;
	CTR2(KTR_PMAP, "pmap_enter_pde: success for va %#lx"
	    " in pmap %p", va, pmap);
	return (TRUE);
}

/*
 * Maps a sequence of resident pages belonging to the same object.
 * The sequence begins with the given page m_start.  This page is
 * mapped at the given virtual address start.  Each subsequent page is
 * mapped at a virtual address that is offset from start by the same
 * amount as the page is offset from m_start within the object.  The
 * last page in the sequence is the page with the largest offset from
 * m_start that can be mapped at a virtual address less than the given
 * virtual address end.  Not every virtual page between start and end
 * is mapped; only those for which a resident page exists with the
 * corresponding offset from m_start are mapped.
 */
void
pmap_enter_object(pmap_t pmap, vm_offset_t start, vm_offset_t end,
    vm_page_t m_start, vm_prot_t prot)
{
	vm_offset_t va;
	vm_page_t m, mpte;
	vm_pindex_t diff, psize;

	VM_OBJECT_LOCK_ASSERT(m_start->object, MA_OWNED);
	psize = atop(end - start);
	mpte = NULL;
	m = m_start;
	while (m != NULL && (diff = m->pindex - m_start->pindex) < psize) {
		va = start + ptoa(diff);
		vm_page_lock(m);
		PMAP_LOCK(pmap);
		if ((va & PDRMASK) == 0 && va + NBPDR <= end &&
		    (VM_PAGE_TO_PHYS(m) & PDRMASK) == 0 &&
		    pg_ps_enabled && vm_reserv_level_iffullpop(m) == 0 &&
		    pmap_enter_pde(pmap, va, m, prot))
			m = &m[NBPDR / PAGE_SIZE - 1];
		else
			mpte = pmap_enter_quick_locked(pmap, va, m, prot,
			    mpte);
		PMAP_UNLOCK(pmap);
		vm_page_unlock(m);
		m = TAILQ_NEXT(m, listq);
	}
}

/*
 * this code makes some *MAJOR* assumptions:
 * 1. Current pmap & pmap exists.
 * 2. Not wired.
 * 3. Read access.
 * 4. No page table pages.
 * but is *MUCH* faster than pmap_enter...
 */

void
pmap_enter_quick(pmap_t pmap, vm_offset_t va, vm_page_t m, vm_prot_t prot)
{

	vm_page_lock_assert(m, MA_OWNED);
	PMAP_LOCK(pmap);
	(void) pmap_enter_quick_locked(pmap, va, m, prot, NULL);
	PMAP_UNLOCK(pmap);
}

static vm_page_t
pmap_enter_quick_locked(pmap_t pmap, vm_offset_t va, vm_page_t m,
    vm_prot_t prot, vm_page_t mpte)
{
	vm_page_t free;
	pt_entry_t *pte;
	vm_paddr_t pa;

	KASSERT(va < kmi.clean_sva || va >= kmi.clean_eva ||
	    (m->flags & (PG_FICTITIOUS | PG_UNMANAGED)) != 0,
	    ("pmap_enter_quick_locked: managed mapping within the clean submap"));
	vm_page_lock_assert(m, MA_OWNED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
	if (va < VM_MAXUSER_ADDRESS) {
		vm_pindex_t ptepindex;
		pd_entry_t *ptepa;

		/*
		 * Calculate pagetable page index
		 */
		ptepindex = pmap_pde_pindex(va);
		if (mpte && (mpte->pindex == ptepindex)) {
			mpte->wire_count++;
		} else {
			/*
			 * Get the page directory entry
			 */
			ptepa = pmap_pde(pmap, va);

			/*
			 * If the page table page is mapped, we just increment
			 * the hold count, and activate it.
			 */
			if (ptepa && (*ptepa & PG_V) != 0) {
				if (*ptepa & PG_PS)
					return (NULL);
				mpte = PHYS_TO_VM_PAGE(*ptepa & PG_FRAME);
				mpte->wire_count++;
			} else {
				pa = VM_PAGE_TO_PHYS(m);
				mpte = _pmap_allocpte(pmap, pa, ptepindex,
				    M_NOWAIT);
				if (mpte == NULL)
					return (mpte);
			}
		}
		pte = (pt_entry_t *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(mpte));
		pte = &pte[pmap_pte_index(va)];
	} else {
		mpte = NULL;
		pte = vtopte(va);
	}
	if (*pte) {
		if (mpte != NULL) {
			mpte->wire_count--;
			mpte = NULL;
		}
		return (mpte);
	}

	/*
	 * Enter on the PV list if part of our managed memory.
	 */
	if ((m->flags & (PG_FICTITIOUS | PG_UNMANAGED)) == 0 &&
	    !pmap_try_insert_pv_entry(pmap, va, m)) {
		if (mpte != NULL) {
			free = NULL;
			if (pmap_unwire_pte_hold(pmap, va, mpte, &free)) {
				pmap_invalidate_page(pmap, va);
				pmap_free_zero_pages(free);
			}
			mpte = NULL;
		}
		return (mpte);
	}

	/*
	 * Increment counters
	 */
	pmap->pm_stats.resident_count++;

	pa = VM_PAGE_TO_PHYS(m) | pmap_cache_bits(m->md.pat_mode, 0);
	if ((prot & VM_PROT_EXECUTE) == 0)
		pa |= pg_nx;

	/*
	 * Now validate mapping with RO protection
	 */
	if (m->flags & (PG_FICTITIOUS|PG_UNMANAGED))
		pte_store(pte, pa | PG_V | PG_U);
	else
		pte_store(pte, pa | PG_V | PG_U | PG_MANAGED);
	return mpte;
}

/*
 * Make a temporary mapping for a physical address.  This is only intended
 * to be used for panic dumps.
 */
void *
pmap_kenter_temporary(vm_paddr_t pa, int i)
{
	vm_offset_t va;

	va = (vm_offset_t)crashdumpmap + (i * PAGE_SIZE);
	pmap_kenter(va, pa);
	invlpg(va);
	return ((void *)crashdumpmap);
}

/*
 * This code maps large physical mmap regions into the
 * processor address space.  Note that some shortcuts
 * are taken, but the code works.
 */
void
pmap_object_init_pt(pmap_t pmap, vm_offset_t addr, vm_object_t object,
    vm_pindex_t pindex, vm_size_t size)
{
	pd_entry_t *pde;
	vm_paddr_t pa, ptepa;
	vm_page_t p, pdpg;
	int pat_mode;

	VM_OBJECT_LOCK_ASSERT(object, MA_OWNED);
	KASSERT(object->type == OBJT_DEVICE || object->type == OBJT_SG,
	    ("pmap_object_init_pt: non-device object"));
	if ((addr & (NBPDR - 1)) == 0 && (size & (NBPDR - 1)) == 0) {
		if (!vm_object_populate(object, pindex, pindex + atop(size)))
			return;
		p = vm_page_lookup(object, pindex);
		KASSERT(p->valid == VM_PAGE_BITS_ALL,
		    ("pmap_object_init_pt: invalid page %p", p));
		pat_mode = p->md.pat_mode;

		/*
		 * Abort the mapping if the first page is not physically
		 * aligned to a 2MB page boundary.
		 */
		ptepa = VM_PAGE_TO_PHYS(p);
		if (ptepa & (NBPDR - 1))
			return;

		/*
		 * Skip the first page.  Abort the mapping if the rest of
		 * the pages are not physically contiguous or have differing
		 * memory attributes.
		 */
		p = TAILQ_NEXT(p, listq);
		for (pa = ptepa + PAGE_SIZE; pa < ptepa + size;
		    pa += PAGE_SIZE) {
			KASSERT(p->valid == VM_PAGE_BITS_ALL,
			    ("pmap_object_init_pt: invalid page %p", p));
			if (pa != VM_PAGE_TO_PHYS(p) ||
			    pat_mode != p->md.pat_mode)
				return;
			p = TAILQ_NEXT(p, listq);
		}

		/*
		 * Map using 2MB pages.  Since "ptepa" is 2M aligned and
		 * "size" is a multiple of 2M, adding the PAT setting to "pa"
		 * will not affect the termination of this loop.
		 */ 
		PMAP_LOCK(pmap);
		for (pa = ptepa | pmap_cache_bits(pat_mode, 1); pa < ptepa +
		    size; pa += NBPDR) {
			pa = VM_PAGE_TO_PHYS(p);
			pdpg = pmap_allocpde(pmap, pa, addr, M_NOWAIT);
			if (pdpg == NULL) {
				/*
				 * The creation of mappings below is only an
				 * optimization.  If a page directory page
				 * cannot be allocated without blocking,
				 * continue on to the next mapping rather than
				 * blocking.
				 */
				addr += NBPDR;
				continue;
			}
			pde = (pd_entry_t *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(pdpg));
			pde = &pde[pmap_pde_index(addr)];
			if ((*pde & PG_V) == 0) {
				pde_store(pde, pa | PG_PS | PG_M | PG_A |
				    PG_U | PG_RW | PG_V);
				pmap->pm_stats.resident_count += NBPDR /
				    PAGE_SIZE;
				pmap_pde_mappings++;
			} else {
				/* Continue on if the PDE is already valid. */
				pdpg->wire_count--;
				KASSERT(pdpg->wire_count > 0,
				    ("pmap_object_init_pt: missing reference "
				    "to page directory page, va: 0x%lx", addr));
			}
			addr += NBPDR;
		}
		PMAP_UNLOCK(pmap);
	}
}

/*
 *	Routine:	pmap_change_wiring
 *	Function:	Change the wiring attribute for a map/virtual-address
 *			pair.
 *	In/out conditions:
 *			The mapping must already exist in the pmap.
 */
void
pmap_change_wiring(pmap_t pmap, vm_offset_t va, boolean_t wired)
{
	pd_entry_t *pde;
	pt_entry_t *pte;
	vm_paddr_t pa;
	boolean_t slept;
	struct pv_list_head pv_list;

	TAILQ_INIT(&pv_list);

	/*
	 * Wiring is not a hardware characteristic so there is no need to
	 * invalidate TLB.
	 */
	pa = 0;
	PMAP_LOCK(pmap);
retry:
	slept = FALSE;
	pde = pmap_pde(pmap, va);
	if ((*pde & PG_PS) && (!wired != ((*pde & PG_W) == 0))) {
		if (TAILQ_EMPTY(&pv_list))
			slept = pmap_pv_list_alloc(pmap, NPTEPG-1, &pv_list);
		if (slept)
			goto retry;
		
		if (pa_tryrelock(pmap, *pde & PG_FRAME, &pa))
			goto retry;
	}
	if ((*pde & PG_PS) != 0) {
		if (!wired != ((*pde & PG_W) == 0)) {
			if (!pmap_demote_pde(pmap, pde, va, &pv_list))
				panic("pmap_change_wiring: demotion failed");
		} else
			goto out;
	}
	pte = pmap_pde_to_pte(pde, va);
	if (wired && (*pte & PG_W) == 0) {
		pmap->pm_stats.wired_count++;
		atomic_set_long(pte, PG_W);
	} else if (!wired && (*pte & PG_W) != 0) {
		pmap->pm_stats.wired_count--;
		atomic_clear_long(pte, PG_W);
	}
out:
	if (pa)
		PA_UNLOCK(pa);
	if (!TAILQ_EMPTY(&pv_list))
		pmap_pv_list_free(pmap, &pv_list);
	PMAP_UNLOCK(pmap);
}

/*
 *	Copy the range specified by src_addr/len
 *	from the source map to the range dst_addr/len
 *	in the destination map.
 *
 *	This routine is only advisory and need not do anything.
 */

void
pmap_copy(pmap_t dst_pmap, pmap_t src_pmap, vm_offset_t dst_addr, vm_size_t len,
    vm_offset_t src_addr)
{
	vm_page_t   free;
	vm_offset_t addr;
	vm_offset_t end_addr = src_addr + len;
	vm_offset_t va_next;
	vm_paddr_t pa;

	if (dst_addr != src_addr)
		return;

	free = NULL;
	if (dst_pmap < src_pmap) {
		PMAP_LOCK(dst_pmap);
		PMAP_LOCK(src_pmap);
	} else {
		PMAP_LOCK(src_pmap);
		PMAP_LOCK(dst_pmap);
	}
	for (addr = src_addr; addr < end_addr; addr = va_next) {
		pt_entry_t *src_pte, *dst_pte;
		vm_page_t dstmpde, dstmpte, srcmpte;
		pml4_entry_t *pml4e;
		pdp_entry_t *pdpe;
		pd_entry_t srcptepaddr, *pde;

		KASSERT(addr < UPT_MIN_ADDRESS,
		    ("pmap_copy: invalid to pmap_copy page tables"));

		pml4e = pmap_pml4e(src_pmap, addr);
		if ((*pml4e & PG_V) == 0) {
			va_next = (addr + NBPML4) & ~PML4MASK;
			if (va_next < addr)
				va_next = end_addr;
			continue;
		}

		pdpe = pmap_pml4e_to_pdpe(pml4e, addr);
		if ((*pdpe & PG_V) == 0) {
			va_next = (addr + NBPDP) & ~PDPMASK;
			if (va_next < addr)
				va_next = end_addr;
			continue;
		}

		va_next = (addr + NBPDR) & ~PDRMASK;
		if (va_next < addr)
			va_next = end_addr;

		pde = pmap_pdpe_to_pde(pdpe, addr);
		srcptepaddr = *pde;
		if (srcptepaddr == 0)
			continue;
			
		if (srcptepaddr & PG_PS) {
			pa = srcptepaddr & PG_PS_FRAME;
			if (PA_TRYLOCK(pa) == 0)
				continue;

			dstmpde = pmap_allocpde(dst_pmap, pa, addr, M_NOWAIT);
			if (dstmpde == NULL)
				break;
			pde = (pd_entry_t *)
			    PHYS_TO_DMAP(VM_PAGE_TO_PHYS(dstmpde));
			pde = &pde[pmap_pde_index(addr)];
			if (*pde == 0 && ((srcptepaddr & PG_MANAGED) == 0 ||
			    pmap_pv_insert_pde(dst_pmap, addr, srcptepaddr &
			    PG_PS_FRAME))) {
				*pde = srcptepaddr & ~PG_W;
				dst_pmap->pm_stats.resident_count +=
				    NBPDR / PAGE_SIZE;
			} else
				dstmpde->wire_count--;
			PA_UNLOCK(pa);
			continue;
		}

		srcptepaddr &= PG_FRAME;
		srcmpte = PHYS_TO_VM_PAGE(srcptepaddr);
		KASSERT(srcmpte->wire_count > 0,
		    ("pmap_copy: source page table page is unused"));

		if (va_next > end_addr)
			va_next = end_addr;

		src_pte = (pt_entry_t *)PHYS_TO_DMAP(srcptepaddr);
		src_pte = &src_pte[pmap_pte_index(addr)];
		dstmpte = NULL;
		while (addr < va_next) {
			pt_entry_t ptetemp;
			ptetemp = *src_pte;
			/*
			 * we only virtual copy managed pages
			 */
			if ((ptetemp & PG_MANAGED) != 0) {
				pa = ptetemp & PG_FRAME;
				if (PA_TRYLOCK(pa) == 0)
					break;
				if (dstmpte != NULL &&
				    dstmpte->pindex == pmap_pde_pindex(addr))
					dstmpte->wire_count++;
				else if ((dstmpte = pmap_allocpte(dst_pmap,
					    pa, addr, M_NOWAIT)) == NULL) {
					PA_UNLOCK(pa);
					goto out;
				}
				dst_pte = (pt_entry_t *)
				    PHYS_TO_DMAP(VM_PAGE_TO_PHYS(dstmpte));
				dst_pte = &dst_pte[pmap_pte_index(addr)];
				if (*dst_pte == 0 &&
				    pmap_try_insert_pv_entry(dst_pmap, addr,
				    PHYS_TO_VM_PAGE(ptetemp & PG_FRAME))) {
					/*
					 * Clear the wired, modified, and
					 * accessed (referenced) bits
					 * during the copy.
					 */
					*dst_pte = ptetemp & ~(PG_W | PG_M |
					    PG_A);
					dst_pmap->pm_stats.resident_count++;
	 			} else {
					free = NULL;
					if (pmap_unwire_pte_hold(dst_pmap,
					    addr, dstmpte, &free)) {
					    	pmap_invalidate_page(dst_pmap,
					 	    addr);
				    	    	pmap_free_zero_pages(free);
					}
					goto out;
				}
				PA_UNLOCK(pa);
				if (dstmpte->wire_count >= srcmpte->wire_count)
					break;
			}
			addr += PAGE_SIZE;
			src_pte++;
		}
	}
out:
	PMAP_UNLOCK(src_pmap);
	PMAP_UNLOCK(dst_pmap);
}	

/*
 *	pmap_zero_page zeros the specified hardware page by mapping 
 *	the page into KVM and using bzero to clear its contents.
 */
void
pmap_zero_page(vm_page_t m)
{
	vm_offset_t va = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m));

	pagezero((void *)va);
}

/*
 *	pmap_zero_page_area zeros the specified hardware page by mapping 
 *	the page into KVM and using bzero to clear its contents.
 *
 *	off and size may not cover an area beyond a single hardware page.
 */
void
pmap_zero_page_area(vm_page_t m, int off, int size)
{
	vm_offset_t va = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m));

	if (off == 0 && size == PAGE_SIZE)
		pagezero((void *)va);
	else
		bzero((char *)va + off, size);
}

/*
 *	pmap_zero_page_idle zeros the specified hardware page by mapping 
 *	the page into KVM and using bzero to clear its contents.  This
 *	is intended to be called from the vm_pagezero process only and
 *	outside of Giant.
 */
void
pmap_zero_page_idle(vm_page_t m)
{
	vm_offset_t va = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m));

	pagezero((void *)va);
}

/*
 *	pmap_copy_page copies the specified (machine independent)
 *	page by mapping the page into virtual memory and using
 *	bcopy to copy the page, one machine dependent page at a
 *	time.
 */
void
pmap_copy_page(vm_page_t msrc, vm_page_t mdst)
{
	vm_offset_t src = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(msrc));
	vm_offset_t dst = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(mdst));

	pagecopy((void *)src, (void *)dst);
}

/*
 * Returns true if the pmap's pv is one of the first
 * 16 pvs linked to from this page.  This count may
 * be changed upwards or downwards in the future; it
 * is only necessary that true be returned for a small
 * subset of pmaps for proper page aging.
 */
boolean_t
pmap_page_exists_quick(pmap_t pmap, vm_page_t m)
{
	struct md_page *pvh;
	pv_entry_t pv;
	int loops = 0;

	if (m->flags & PG_FICTITIOUS)
		return FALSE;

	vm_page_lock_assert(m, MA_OWNED);
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		if (PV_PMAP(pv) == pmap) {
			return TRUE;
		}
		loops++;
		if (loops >= 16)
			break;
	}
	if (loops < 16) {
		pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
		TAILQ_FOREACH(pv, &pvh->pv_list, pv_list) {
			if (PV_PMAP(pv) == pmap)
				return (TRUE);
			loops++;
			if (loops >= 16)
				break;
		}
	}
	return (FALSE);
}

/*
 * Returns TRUE if the given page is mapped individually or as part of
 * a 2mpage.  Otherwise, returns FALSE.
 */
boolean_t
pmap_page_is_mapped(vm_page_t m)
{
	struct md_page *pvh;

	if ((m->flags & (PG_FICTITIOUS | PG_UNMANAGED)) != 0)
		return (FALSE);
	vm_page_lock_assert(m, MA_OWNED);
	if (TAILQ_EMPTY(&m->md.pv_list)) {
		pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
		return (!TAILQ_EMPTY(&pvh->pv_list));
	} else
		return (TRUE);
}

/*
 * Remove all pages from specified address space
 * this aids process exit speeds.  Also, this code
 * is special cased for current process only, but
 * can have the more generic (and slightly slower)
 * mode enabled.  This is much faster than pmap_remove
 * in the case of running down an entire address space.
 */
void
pmap_remove_pages(pmap_t pmap)
{
	pd_entry_t ptepde;
	pt_entry_t *pte, tpte;
	vm_page_t free = NULL;
	vm_page_t m, mpte, mt;
	vm_paddr_t pa;
	pv_entry_t pv;
	struct md_page *pvh;
	struct pv_chunk *pc, *npc;
	int field, idx, iter;
	int64_t bit;
	uint64_t inuse, bitmask;
	int allfree;

	if (pmap != vmspace_pmap(curthread->td_proc->p_vmspace)) {
		printf("warning: pmap_remove_pages called with non-current pmap\n");
		return;
	}
	pa = 0;
	iter = 0;
	PMAP_LOCK(pmap);
restart:
	TAILQ_FOREACH_SAFE(pc, &pmap->pm_pvchunk, pc_list, npc) {
		allfree = 1;
		for (field = 0; field < _NPCM; field++) {
			iter++;
			inuse = (~(pc->pc_map[field])) & pc_freemask[field];
			while (inuse != 0) {
				bit = bsfq(inuse);
				bitmask = 1UL << bit;
				idx = field * 64 + bit;
				pv = &pc->pc_pventry[idx];
				inuse &= ~bitmask;

				pte = pmap_pdpe(pmap, pv->pv_va);
				ptepde = *pte;
				pte = pmap_pdpe_to_pde(pte, pv->pv_va);
				tpte = *pte;
				if ((tpte & (PG_PS | PG_V)) == PG_V) {
					ptepde = tpte;
					pte = (pt_entry_t *)PHYS_TO_DMAP(tpte &
					    PG_FRAME);
					pte = &pte[pmap_pte_index(pv->pv_va)];
					tpte = *pte & ~PG_PTE_PAT;
				}
				if ((tpte & PG_V) == 0)
					panic("bad pte tpte=%ld va=%lx idx=%d iter=%d",
					    tpte, pv->pv_va, idx, iter);

/*
 * We cannot remove wired pages from a process' mapping at this time
 */
				if (tpte & PG_W) {
					allfree = 0;
					continue;
				}

				if (pa_tryrelock(pmap, tpte & PG_FRAME, &pa))
					goto restart;

				m = PHYS_TO_VM_PAGE(tpte & PG_FRAME);
				KASSERT(m->phys_addr == (tpte & PG_FRAME),
				    ("vm_page_t %p phys_addr mismatch %016jx %016jx",
				    m, (uintmax_t)m->phys_addr,
				    (uintmax_t)tpte));

				KASSERT(m < &vm_page_array[vm_page_array_size],
					("pmap_remove_pages: bad tpte %#jx",
					(uintmax_t)tpte));

				pte_clear(pte);

				/*
				 * Update the vm_page_t clean/reference bits.
				 */
				if ((tpte & (PG_M | PG_RW)) == (PG_M | PG_RW)) {
					if ((tpte & PG_PS) != 0) {
						for (mt = m; mt < &m[NBPDR / PAGE_SIZE]; mt++)
							vm_page_dirty(mt);
					} else
						vm_page_dirty(m);
				}
				mtx_lock(&pv_lock);
				/* Mark free */
				PV_STAT(pv_entry_frees++);
				PV_STAT(pv_entry_spare++);
				pv_entry_count--;
				mtx_unlock(&pv_lock);
				pc->pc_map[field] |= bitmask;
				if ((tpte & PG_PS) != 0) {
					pmap->pm_stats.resident_count -= NBPDR / PAGE_SIZE;
					pvh = pa_to_pvh(tpte & PG_PS_FRAME);
					TAILQ_REMOVE(&pvh->pv_list, pv, pv_list);
					if (TAILQ_EMPTY(&pvh->pv_list)) {
						for (mt = m; mt < &m[NBPDR / PAGE_SIZE]; mt++)
							if (TAILQ_EMPTY(&mt->md.pv_list))
								vm_page_flag_clear(mt, PG_WRITEABLE);
					}
					mpte = pmap_lookup_pt_page(pmap, pv->pv_va);
					if (mpte != NULL) {
						pmap_remove_pt_page(pmap, mpte);
						pmap->pm_stats.resident_count--;
						KASSERT(mpte->wire_count == NPTEPG,
						    ("pmap_remove_pages: pte page wire count error"));
						mpte->wire_count = 0;
						pmap_add_delayed_free_list(mpte, &free, FALSE);
						atomic_subtract_int(&cnt.v_wire_count, 1);
					}
				} else {
					pmap->pm_stats.resident_count--;
					TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);
					if (TAILQ_EMPTY(&m->md.pv_list)) {
						pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
						if (TAILQ_EMPTY(&pvh->pv_list))
							vm_page_flag_clear(m, PG_WRITEABLE);
					}
				}
				pmap_unuse_pt(pmap, pv->pv_va, ptepde, &free);
			}
		}
		if (allfree) {
			mtx_lock(&pv_lock);
			PV_STAT(pv_entry_spare -= _NPCPV);
			PV_STAT(pc_chunk_count--);
			PV_STAT(pc_chunk_frees++);
			TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
			m = PHYS_TO_VM_PAGE(DMAP_TO_PHYS((vm_offset_t)pc));
			dump_drop_page(m->phys_addr);
			mtx_unlock(&pv_lock);
			KASSERT(m->wire_count == 1,
			    ("wire_count == %d", m->wire_count));
			m->wire_count = 0;
			atomic_subtract_int(&cnt.v_wire_count, 1);
			vm_page_free(m);
		}
	}
	if (pa)
		PA_UNLOCK(pa);

	pmap_invalidate_all(pmap);
	PMAP_UNLOCK(pmap);
	pmap_free_zero_pages(free);
}

/*
 *	pmap_is_modified:
 *
 *	Return whether or not the specified physical page was modified
 *	in any physical maps.
 */
boolean_t
pmap_is_modified(vm_page_t m)
{

	if (m->flags & PG_FICTITIOUS)
		return (FALSE);
	if (pmap_is_modified_pvh(&m->md))
		return (TRUE);
	return (pmap_is_modified_pvh(pa_to_pvh(VM_PAGE_TO_PHYS(m))));
}

/*
 * Returns TRUE if any of the given mappings were used to modify
 * physical memory.  Otherwise, returns FALSE.  Both page and 2mpage
 * mappings are supported.
 */
static boolean_t
pmap_is_modified_pvh(struct md_page *pvh)
{
	pv_entry_t pv;
	pt_entry_t *pte;
	pmap_t pmap;
	boolean_t rv;

	rv = FALSE;
	TAILQ_FOREACH(pv, &pvh->pv_list, pv_list) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		pte = pmap_pte(pmap, pv->pv_va);
		rv = (*pte & (PG_M | PG_RW)) == (PG_M | PG_RW);
		PMAP_UNLOCK(pmap);
		if (rv)
			break;
	}
	return (rv);
}

/*
 *	pmap_is_prefaultable:
 *
 *	Return whether or not the specified virtual address is elgible
 *	for prefault.
 */
boolean_t
pmap_is_prefaultable(pmap_t pmap, vm_offset_t addr)
{
	pd_entry_t *pde;
	pt_entry_t *pte;
	boolean_t rv;

	rv = FALSE;
	PMAP_LOCK(pmap);
	pde = pmap_pde(pmap, addr);
	if (pde != NULL && (*pde & (PG_PS | PG_V)) == PG_V) {
		pte = pmap_pde_to_pte(pde, addr);
		rv = (*pte & PG_V) == 0;
	}
	PMAP_UNLOCK(pmap);
	return (rv);
}

/*
 * Clear the write and modified bits in each of the given page's mappings.
 */
void
pmap_remove_write(vm_page_t m)
{
	struct md_page *pvh;
	pmap_t pmap;
	pv_entry_t next_pv, pv;
	pd_entry_t *pde;
	pt_entry_t oldpte, *pte;
	vm_offset_t va;
	struct pv_list_head pv_list;

	if ((m->flags & PG_FICTITIOUS) != 0 ||
	    (m->flags & PG_WRITEABLE) == 0)
		return;

	TAILQ_INIT(&pv_list);
	vm_page_lock_assert(m, MA_OWNED);
	pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
	TAILQ_FOREACH_SAFE(pv, &pvh->pv_list, pv_list, next_pv) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		va = pv->pv_va;
		pde = pmap_pde(pmap, va);
		if ((*pde & PG_RW) != 0)
			(void)pmap_demote_pde(pmap, pde, va, &pv_list);
		PMAP_UNLOCK(pmap);
	}
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		pde = pmap_pde(pmap, pv->pv_va);
		KASSERT((*pde & PG_PS) == 0, ("pmap_clear_write: found"
		    " a 2mpage in page %p's pv list", m));
		pte = pmap_pde_to_pte(pde, pv->pv_va);
retry:
		oldpte = *pte;
		if (oldpte & PG_RW) {
			if (!atomic_cmpset_long(pte, oldpte, oldpte &
			    ~(PG_RW | PG_M)))
				goto retry;
			if ((oldpte & PG_M) != 0)
				vm_page_dirty(m);
			pmap_invalidate_page(pmap, pv->pv_va);
		}
		PMAP_UNLOCK(pmap);
	}
	vm_page_flag_clear(m, PG_WRITEABLE);
}

/*
 *	pmap_ts_referenced:
 *
 *	Return a count of reference bits for a page, clearing those bits.
 *	It is not necessary for every reference bit to be cleared, but it
 *	is necessary that 0 only be returned when there are truly no
 *	reference bits set.
 *
 *	XXX: The exact number of bits to check and clear is a matter that
 *	should be tested and standardized at some point in the future for
 *	optimal aging of shared pages.
 */
int
pmap_ts_referenced(vm_page_t m)
{
	struct md_page *pvh;
	pv_entry_t pv, pvf, pvn;
	pmap_t pmap;
	pd_entry_t oldpde, *pde;
	pt_entry_t *pte;
	vm_offset_t va;
	int rtval = 0;
	struct pv_list_head pv_list;

	if (m->flags & PG_FICTITIOUS)
		return (rtval);

	TAILQ_INIT(&pv_list);
	vm_page_lock_assert(m, MA_OWNED);
	pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
	TAILQ_FOREACH_SAFE(pv, &pvh->pv_list, pv_list, pvn) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		va = pv->pv_va;
		pde = pmap_pde(pmap, va);
		oldpde = *pde;
		if ((oldpde & PG_A) != 0) {
			if (pmap_demote_pde(pmap, pde, va, &pv_list)) {
				if ((oldpde & PG_W) == 0) {
					/*
					 * Remove the mapping to a single page
					 * so that a subsequent access may
					 * repromote.  Since the underlying
					 * page table page is fully populated,
					 * this removal never frees a page
					 * table page.
					 */
					va += VM_PAGE_TO_PHYS(m) - (oldpde &
					    PG_PS_FRAME);
					pmap_remove_page(pmap, va, pde, NULL);
					rtval++;
					if (rtval > 4) {
						PMAP_UNLOCK(pmap);
						return (rtval);
					}
				}
			}
		}
		PMAP_UNLOCK(pmap);
	}
	if ((pv = TAILQ_FIRST(&m->md.pv_list)) != NULL) {
		pvf = pv;
		do {
			pvn = TAILQ_NEXT(pv, pv_list);
			TAILQ_REMOVE(&m->md.pv_list, pv, pv_list);
			TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_list);
			pmap = PV_PMAP(pv);
			PMAP_LOCK(pmap);
			pde = pmap_pde(pmap, pv->pv_va);
			KASSERT((*pde & PG_PS) == 0, ("pmap_ts_referenced:"
			    " found a 2mpage in page %p's pv list", m));
			pte = pmap_pde_to_pte(pde, pv->pv_va);
			if ((*pte & PG_A) != 0) {
				atomic_clear_long(pte, PG_A);
				pmap_invalidate_page(pmap, pv->pv_va);
				rtval++;
				if (rtval > 4)
					pvn = NULL;
			}
			PMAP_UNLOCK(pmap);
		} while ((pv = pvn) != NULL && pv != pvf);
	}
	return (rtval);
}

/*
 *	Clear the modify bits on the specified physical page.
 */
void
pmap_clear_modify(vm_page_t m)
{
	struct md_page *pvh;
	pmap_t pmap;
	pv_entry_t next_pv, pv;
	pd_entry_t oldpde, *pde;
	pt_entry_t oldpte, *pte;
	vm_offset_t va;
	struct pv_list_head pv_list;

	if ((m->flags & PG_FICTITIOUS) != 0)
		return;
	TAILQ_INIT(&pv_list);
	vm_page_lock_assert(m, MA_OWNED);
	pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
	TAILQ_FOREACH_SAFE(pv, &pvh->pv_list, pv_list, next_pv) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		va = pv->pv_va;
		pde = pmap_pde(pmap, va);
		oldpde = *pde;
		if ((oldpde & PG_RW) != 0) {
			if (pmap_demote_pde(pmap, pde, va, &pv_list)) {
				if ((oldpde & PG_W) == 0) {
					/*
					 * Write protect the mapping to a
					 * single page so that a subsequent
					 * write access may repromote.
					 */
					va += VM_PAGE_TO_PHYS(m) - (oldpde &
					    PG_PS_FRAME);
					pte = pmap_pde_to_pte(pde, va);
					oldpte = *pte;
					if ((oldpte & PG_V) != 0) {
						while (!atomic_cmpset_long(pte,
						    oldpte,
						    oldpte & ~(PG_M | PG_RW)))
							oldpte = *pte;
						vm_page_dirty(m);
						pmap_invalidate_page(pmap, va);
					}
				}
			}
		}
		PMAP_UNLOCK(pmap);
	}
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		pde = pmap_pde(pmap, pv->pv_va);
		KASSERT((*pde & PG_PS) == 0, ("pmap_clear_modify: found"
		    " a 2mpage in page %p's pv list", m));
		pte = pmap_pde_to_pte(pde, pv->pv_va);
		if ((*pte & (PG_M | PG_RW)) == (PG_M | PG_RW)) {
			atomic_clear_long(pte, PG_M);
			pmap_invalidate_page(pmap, pv->pv_va);
		}
		PMAP_UNLOCK(pmap);
	}
}

/*
 *	pmap_clear_reference:
 *
 *	Clear the reference bit on the specified physical page.
 */
void
pmap_clear_reference(vm_page_t m)
{
	struct md_page *pvh;
	pmap_t pmap;
	pv_entry_t next_pv, pv;
	pd_entry_t oldpde, *pde;
	pt_entry_t *pte;
	vm_offset_t va;
	struct pv_list_head pv_list;

	if ((m->flags & PG_FICTITIOUS) != 0)
		return;
	TAILQ_INIT(&pv_list);
	vm_page_lock_assert(m, MA_OWNED);
	pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
	TAILQ_FOREACH_SAFE(pv, &pvh->pv_list, pv_list, next_pv) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		va = pv->pv_va;
		pde = pmap_pde(pmap, va);
		oldpde = *pde;
		if ((oldpde & PG_A) != 0) {
			if (pmap_demote_pde(pmap, pde, va, &pv_list)) {
				/*
				 * Remove the mapping to a single page so
				 * that a subsequent access may repromote.
				 * Since the underlying page table page is
				 * fully populated, this removal never frees
				 * a page table page.
				 */
				va += VM_PAGE_TO_PHYS(m) - (oldpde &
				    PG_PS_FRAME);
				pmap_remove_page(pmap, va, pde, NULL);
			}
		}
		PMAP_UNLOCK(pmap);
	}
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_list) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		pde = pmap_pde(pmap, pv->pv_va);
		KASSERT((*pde & PG_PS) == 0, ("pmap_clear_reference: found"
		    " a 2mpage in page %p's pv list", m));
		pte = pmap_pde_to_pte(pde, pv->pv_va);
		if (*pte & PG_A) {
			atomic_clear_long(pte, PG_A);
			pmap_invalidate_page(pmap, pv->pv_va);
		}
		PMAP_UNLOCK(pmap);
	}
}

/*
 * Miscellaneous support routines follow
 */

/* Adjust the cache mode for a 4KB page mapped via a PTE. */
static __inline void
pmap_pte_attr(pt_entry_t *pte, int cache_bits)
{
	u_int opte, npte;

	/*
	 * The cache mode bits are all in the low 32-bits of the
	 * PTE, so we can just spin on updating the low 32-bits.
	 */
	do {
		opte = *(u_int *)pte;
		npte = opte & ~PG_PTE_CACHE;
		npte |= cache_bits;
	} while (npte != opte && !atomic_cmpset_int((u_int *)pte, opte, npte));
}

/* Adjust the cache mode for a 2MB page mapped via a PDE. */
static __inline void
pmap_pde_attr(pd_entry_t *pde, int cache_bits)
{
	u_int opde, npde;

	/*
	 * The cache mode bits are all in the low 32-bits of the
	 * PDE, so we can just spin on updating the low 32-bits.
	 */
	do {
		opde = *(u_int *)pde;
		npde = opde & ~PG_PDE_CACHE;
		npde |= cache_bits;
	} while (npde != opde && !atomic_cmpset_int((u_int *)pde, opde, npde));
}

/*
 * Map a set of physical memory pages into the kernel virtual
 * address space. Return a pointer to where it is mapped. This
 * routine is intended to be used for mapping device memory,
 * NOT real memory.
 */
void *
pmap_mapdev_attr(vm_paddr_t pa, vm_size_t size, int mode)
{
	vm_offset_t va, offset;
	vm_size_t tmpsize;

	/*
	 * If the specified range of physical addresses fits within the direct
	 * map window, use the direct map. 
	 */
	if (pa < dmaplimit && pa + size < dmaplimit) {
		va = PHYS_TO_DMAP(pa);
		if (!pmap_change_attr(va, size, mode))
			return ((void *)va);
	}
	offset = pa & PAGE_MASK;
	size = roundup(offset + size, PAGE_SIZE);
	va = kmem_alloc_nofault(kernel_map, size);
	if (!va)
		panic("pmap_mapdev: Couldn't alloc kernel virtual memory");
	pa = trunc_page(pa);
	for (tmpsize = 0; tmpsize < size; tmpsize += PAGE_SIZE)
		pmap_kenter_attr(va + tmpsize, pa + tmpsize, mode);
	pmap_invalidate_range(kernel_pmap, va, va + tmpsize);
	pmap_invalidate_cache_range(va, va + tmpsize);
	return ((void *)(va + offset));
}

void *
pmap_mapdev(vm_paddr_t pa, vm_size_t size)
{

	return (pmap_mapdev_attr(pa, size, PAT_UNCACHEABLE));
}

void *
pmap_mapbios(vm_paddr_t pa, vm_size_t size)
{

	return (pmap_mapdev_attr(pa, size, PAT_WRITE_BACK));
}

void
pmap_unmapdev(vm_offset_t va, vm_size_t size)
{
	vm_offset_t base, offset, tmpva;

	/* If we gave a direct map region in pmap_mapdev, do nothing */
	if (va >= DMAP_MIN_ADDRESS && va < DMAP_MAX_ADDRESS)
		return;
	base = trunc_page(va);
	offset = va & PAGE_MASK;
	size = roundup(offset + size, PAGE_SIZE);
	for (tmpva = base; tmpva < (base + size); tmpva += PAGE_SIZE)
		pmap_kremove(tmpva);
	pmap_invalidate_range(kernel_pmap, va, tmpva);
	kmem_free(kernel_map, base, size);
}

/*
 * Sets the memory attribute for the specified page.
 */
void
pmap_page_set_memattr(vm_page_t m, vm_memattr_t ma)
{

	m->md.pat_mode = ma;

	/*
	 * If "m" is a normal page, update its direct mapping.  This update
	 * can be relied upon to perform any cache operations that are
	 * required for data coherence.
	 */
	if ((m->flags & PG_FICTITIOUS) == 0 &&
	    pmap_change_attr(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m)), PAGE_SIZE,
	    m->md.pat_mode))
		panic("memory attribute change on the direct map failed");
}

/*
 * Changes the specified virtual address range's memory type to that given by
 * the parameter "mode".  The specified virtual address range must be
 * completely contained within either the direct map or the kernel map.  If
 * the virtual address range is contained within the kernel map, then the
 * memory type for each of the corresponding ranges of the direct map is also
 * changed.  (The corresponding ranges of the direct map are those ranges that
 * map the same physical pages as the specified virtual address range.)  These
 * changes to the direct map are necessary because Intel describes the
 * behavior of their processors as "undefined" if two or more mappings to the
 * same physical page have different memory types.
 *
 * Returns zero if the change completed successfully, and either EINVAL or
 * ENOMEM if the change failed.  Specifically, EINVAL is returned if some part
 * of the virtual address range was not mapped, and ENOMEM is returned if
 * there was insufficient memory available to complete the change.  In the
 * latter case, the memory type may have been changed on some part of the
 * virtual address range or the direct map.
 */
int
pmap_change_attr(vm_offset_t va, vm_size_t size, int mode)
{
	int error;

	PMAP_LOCK(kernel_pmap);
	error = pmap_change_attr_locked(va, size, mode);
	PMAP_UNLOCK(kernel_pmap);
	return (error);
}

static int
pmap_change_attr_locked(vm_offset_t va, vm_size_t size, int mode)
{
	vm_offset_t base, offset, tmpva;
	vm_paddr_t pa_start, pa_end;
	pd_entry_t *pde;
	pt_entry_t *pte;
	int cache_bits_pte, cache_bits_pde, error;
	boolean_t changed;
	struct pv_list_head pv_list;

	PMAP_LOCK_ASSERT(kernel_pmap, MA_OWNED);
	base = trunc_page(va);
	offset = va & PAGE_MASK;
	size = roundup(offset + size, PAGE_SIZE);

	/*
	 * Only supported on kernel virtual addresses, including the direct
	 * map but excluding the recursive map.
	 */
	if (base < DMAP_MIN_ADDRESS)
		return (EINVAL);

	cache_bits_pde = pmap_cache_bits(mode, 1);
	cache_bits_pte = pmap_cache_bits(mode, 0);
	changed = FALSE;
	TAILQ_INIT(&pv_list);

	/*
	 * Pages that aren't mapped aren't supported.  Also break down 2MB pages
	 * into 4KB pages if required.
	 */
	for (tmpva = base; tmpva < base + size; ) {
		pde = pmap_pde(kernel_pmap, tmpva);
		if (*pde == 0)
			return (EINVAL);
		if (*pde & PG_PS) {
			/*
			 * If the current 2MB page already has the required
			 * memory type, then we need not demote this page. Just
			 * increment tmpva to the next 2MB page frame.
			 */
			if ((*pde & PG_PDE_CACHE) == cache_bits_pde) {
				tmpva = trunc_2mpage(tmpva) + NBPDR;
				continue;
			}

			/*
			 * If the current offset aligns with a 2MB page frame
			 * and there is at least 2MB left within the range, then
			 * we need not break down this page into 4KB pages.
			 */
			if ((tmpva & PDRMASK) == 0 &&
			    tmpva + PDRMASK < base + size) {
				tmpva += NBPDR;
				continue;
			}
			if (!pmap_demote_pde(kernel_pmap, pde, tmpva, &pv_list))
				return (ENOMEM);
		}
		pte = pmap_pde_to_pte(pde, tmpva);
		if (*pte == 0)
			return (EINVAL);
		tmpva += PAGE_SIZE;
	}
	error = 0;

	/*
	 * Ok, all the pages exist, so run through them updating their
	 * cache mode if required.
	 */
	pa_start = pa_end = 0;
	for (tmpva = base; tmpva < base + size; ) {
		pde = pmap_pde(kernel_pmap, tmpva);
		if (*pde & PG_PS) {
			if ((*pde & PG_PDE_CACHE) != cache_bits_pde) {
				pmap_pde_attr(pde, cache_bits_pde);
				changed = TRUE;
			}
			if (tmpva >= VM_MIN_KERNEL_ADDRESS) {
				if (pa_start == pa_end) {
					/* Start physical address run. */
					pa_start = *pde & PG_PS_FRAME;
					pa_end = pa_start + NBPDR;
				} else if (pa_end == (*pde & PG_PS_FRAME))
					pa_end += NBPDR;
				else {
					/* Run ended, update direct map. */
					error = pmap_change_attr_locked(
					    PHYS_TO_DMAP(pa_start),
					    pa_end - pa_start, mode);
					if (error != 0)
						break;
					/* Start physical address run. */
					pa_start = *pde & PG_PS_FRAME;
					pa_end = pa_start + NBPDR;
				}
			}
			tmpva = trunc_2mpage(tmpva) + NBPDR;
		} else {
			pte = pmap_pde_to_pte(pde, tmpva);
			if ((*pte & PG_PTE_CACHE) != cache_bits_pte) {
				pmap_pte_attr(pte, cache_bits_pte);
				changed = TRUE;
			}
			if (tmpva >= VM_MIN_KERNEL_ADDRESS) {
				if (pa_start == pa_end) {
					/* Start physical address run. */
					pa_start = *pte & PG_FRAME;
					pa_end = pa_start + PAGE_SIZE;
				} else if (pa_end == (*pte & PG_FRAME))
					pa_end += PAGE_SIZE;
				else {
					/* Run ended, update direct map. */
					error = pmap_change_attr_locked(
					    PHYS_TO_DMAP(pa_start),
					    pa_end - pa_start, mode);
					if (error != 0)
						break;
					/* Start physical address run. */
					pa_start = *pte & PG_FRAME;
					pa_end = pa_start + PAGE_SIZE;
				}
			}
			tmpva += PAGE_SIZE;
		}
	}
	if (error == 0 && pa_start != pa_end)
		error = pmap_change_attr_locked(PHYS_TO_DMAP(pa_start),
		    pa_end - pa_start, mode);

	/*
	 * Flush CPU caches if required to make sure any data isn't cached that
	 * shouldn't be, etc.
	 */
	if (changed) {
		pmap_invalidate_range(kernel_pmap, base, tmpva);
		pmap_invalidate_cache_range(base, tmpva);
	}
	return (error);
}

/*
 * perform the pmap work for mincore
 */
int
pmap_mincore(pmap_t pmap, vm_offset_t addr)
{
	pd_entry_t *pdep;
	pt_entry_t pte;
	vm_paddr_t pa;
	vm_page_t m;
	int val = 0;
	
	PMAP_LOCK(pmap);
	pdep = pmap_pde(pmap, addr);
	if (pdep != NULL && (*pdep & PG_V)) {
		if (*pdep & PG_PS) {
			pte = *pdep;
			val = MINCORE_SUPER;
			/* Compute the physical address of the 4KB page. */
			pa = ((*pdep & PG_PS_FRAME) | (addr & PDRMASK)) &
			    PG_FRAME;
		} else {
			pte = *pmap_pde_to_pte(pdep, addr);
			pa = pte & PG_FRAME;
		}
	} else {
		pte = 0;
		pa = 0;
	}
	PMAP_UNLOCK(pmap);

	if (pte != 0) {
		val |= MINCORE_INCORE;
		if ((pte & PG_MANAGED) == 0)
			return val;

		m = PHYS_TO_VM_PAGE(pa);

		/*
		 * Modified by us
		 */
		if ((pte & (PG_M | PG_RW)) == (PG_M | PG_RW))
			val |= MINCORE_MODIFIED|MINCORE_MODIFIED_OTHER;
		else {
			/*
			 * Modified by someone else
			 */
			vm_page_lock(m);
			if (m->dirty || pmap_is_modified(m))
				val |= MINCORE_MODIFIED_OTHER;
			vm_page_unlock(m);
		}
		/*
		 * Referenced by us
		 */
		if (pte & PG_A)
			val |= MINCORE_REFERENCED|MINCORE_REFERENCED_OTHER;
		else {
			/*
			 * Referenced by someone else
			 */
			vm_page_lock(m);
			if ((m->flags & PG_REFERENCED) ||
			    pmap_ts_referenced(m)) {
				val |= MINCORE_REFERENCED_OTHER;
				vm_page_flag_set(m, PG_REFERENCED);
			}
			vm_page_unlock(m);
		}
	} 
	return val;
}

void
pmap_activate(struct thread *td)
{
	pmap_t	pmap, oldpmap;
	u_int64_t  cr3;

	critical_enter();
	pmap = vmspace_pmap(td->td_proc->p_vmspace);
	oldpmap = PCPU_GET(curpmap);
#ifdef SMP
if (oldpmap)	/* XXX FIXME */
	atomic_clear_int(&oldpmap->pm_active, PCPU_GET(cpumask));
	atomic_set_int(&pmap->pm_active, PCPU_GET(cpumask));
#else
if (oldpmap)	/* XXX FIXME */
	oldpmap->pm_active &= ~PCPU_GET(cpumask);
	pmap->pm_active |= PCPU_GET(cpumask);
#endif
	cr3 = DMAP_TO_PHYS((vm_offset_t)pmap->pm_pml4);
	td->td_pcb->pcb_cr3 = cr3;
	load_cr3(cr3);
	critical_exit();
}

vm_offset_t
pmap_addr_hint(vm_object_t obj, vm_offset_t addr, vm_size_t size)
{

	if ((obj == NULL) || (size < NBPDR) ||
	    (obj->type != OBJT_DEVICE && obj->type != OBJT_SG)) {
		return addr;
	}

	addr = (addr + (NBPDR - 1)) & ~(NBPDR - 1);
	return addr;
}

/*
 *	Increase the starting virtual address of the given mapping if a
 *	different alignment might result in more superpage mappings.
 */
void
pmap_align_superpage(vm_object_t object, vm_ooffset_t offset,
    vm_offset_t *addr, vm_size_t size)
{
	vm_offset_t superpage_offset;

	if (size < NBPDR)
		return;
	if (object != NULL && (object->flags & OBJ_COLORED) != 0)
		offset += ptoa(object->pg_color);
	superpage_offset = offset & PDRMASK;
	if (size - ((NBPDR - superpage_offset) & PDRMASK) < NBPDR ||
	    (*addr & PDRMASK) == superpage_offset)
		return;
	if ((*addr & PDRMASK) < superpage_offset)
		*addr = (*addr & ~PDRMASK) + superpage_offset;
	else
		*addr = ((*addr + PDRMASK) & ~PDRMASK) + superpage_offset;
}
