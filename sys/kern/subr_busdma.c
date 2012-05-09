/*-
 * Copyright (c) 2012 Marcel Moolenaar
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/busdma.h>
#include <sys/malloc.h>
#include <machine/stdarg.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

struct busdma_tag {
	struct busdma_tag *dt_chain;
	struct busdma_tag *dt_child;
	struct busdma_tag *dt_parent;
	device_t	dt_device;
	bus_addr_t	dt_minaddr;
	bus_addr_t	dt_maxaddr;
	bus_addr_t	dt_align;
	bus_addr_t	dt_bndry;
	bus_size_t	dt_maxsz;
	u_int		dt_nsegs;
	bus_size_t	dt_maxsegsz;
};

struct busdma_seg {
	TAILQ_ENTRY(busdma_seg) ds_chain;
	bus_addr_t	ds_baddr;
	vm_paddr_t	ds_paddr;
	vm_offset_t	ds_vaddr;
	vm_size_t	ds_size;
};

struct busdma_mem {
	struct busdma_tag *dm_tag;
	TAILQ_HEAD(,busdma_seg) dm_seg;
	u_int		dm_nsegs;
};

static struct busdma_tag busdma_root_tag = {
	.dt_maxaddr = ~0UL,
	.dt_align = 1,
	.dt_maxsz = ~0UL,
	.dt_nsegs = ~0U,
	.dt_maxsegsz = ~0UL,
};

static MALLOC_DEFINE(M_BUSDMA_MEM, "busdma_mem", "busdma mem structures");
static MALLOC_DEFINE(M_BUSDMA_SEG, "busdma_seg", "busdma seg structures");
static MALLOC_DEFINE(M_BUSDMA_TAG, "busdma_tag", "busdma tag structures");

static void
_busdma_tag_dump(const char *func, device_t dev, struct busdma_tag *tag)
{

	printf("[%s: %s: tag=%p (minaddr=%jx, maxaddr=%jx, align=%jx, "
	    "bndry=%jx, maxsz=%jx, nsegs=%u, maxsegsz=%jx)]\n",
	    func, (dev != NULL) ? device_get_nameunit(dev) : "*", tag,
	    (uintmax_t)tag->dt_minaddr, (uintmax_t)tag->dt_maxaddr,
	    (uintmax_t)tag->dt_align, (uintmax_t)tag->dt_bndry,
	    (uintmax_t)tag->dt_maxsz,
	    tag->dt_nsegs, (uintmax_t)tag->dt_maxsegsz);
}

static void
_busdma_mem_dump(const char *func, struct busdma_mem *mem) 
{
	struct busdma_seg *seg;

	printf("[%s: %s: mem=%p (tag=%p, nsegs=%u)", func,
	    device_get_nameunit(mem->dm_tag->dt_device), mem,
	    mem->dm_tag, mem->dm_nsegs);
	TAILQ_FOREACH(seg, &mem->dm_seg, ds_chain)
		printf(", {size=%jx, paddr=%jx, vaddr=%jx}", seg->ds_size,
		    seg->ds_paddr, seg->ds_vaddr);
	printf("]\n");
}

static struct busdma_tag *
_busdma_tag_get_base(device_t dev)
{
	device_t parent;
	void *base;

	base = NULL;
	parent = device_get_parent(dev);
	while (base == NULL && parent != NULL) {
		base = device_get_busdma_tag(parent);
		if (base == NULL)
			parent = device_get_parent(parent);
	}
	if (base == NULL) {
		base = &busdma_root_tag;
		parent = NULL;
	}
	_busdma_tag_dump(__func__, parent, base);
	return (base);
}

static int
_busdma_tag_make(device_t dev, struct busdma_tag *base, bus_addr_t maxaddr,
    bus_addr_t align, bus_addr_t bndry, bus_size_t maxsz, u_int nsegs,
    bus_size_t maxsegsz, u_int flags, struct busdma_tag **tag_p)
{
	struct busdma_tag *tag;

	/*
	 * If nsegs is 1, ignore maxsegsz. What this means is that if we have
	 * just 1 segment, then maxsz should be equal to maxsegsz. To keep it
	 * simple for us, limit maxsegsz to maxsz in any case.
	 */
	if (maxsegsz > maxsz || nsegs == 1)
		maxsegsz = maxsz;

	tag = malloc(sizeof(*tag), M_BUSDMA_TAG, M_NOWAIT | M_ZERO);
	tag->dt_device = dev;
	tag->dt_minaddr = MAX(0, base->dt_minaddr);
	tag->dt_maxaddr = MIN(maxaddr, base->dt_maxaddr);
	tag->dt_align = MAX(align, base->dt_align);
	tag->dt_bndry = MIN(bndry, base->dt_bndry);
	tag->dt_maxsz = MIN(maxsz, base->dt_maxsz);
	tag->dt_nsegs = MIN(nsegs, base->dt_nsegs);
	tag->dt_maxsegsz = MIN(maxsegsz, base->dt_maxsegsz);
	_busdma_tag_dump(__func__, dev, tag);
	*tag_p = tag;
	return (0);
}

static struct busdma_seg *
_busdma_mem_get_seg(struct busdma_mem *mem, u_int idx)
{
	struct busdma_seg *seg;
 
	if (idx >= mem->dm_nsegs)
		return (NULL);

	seg = TAILQ_FIRST(&mem->dm_seg);
	return (seg);
}

int
busdma_tag_create(device_t dev, bus_addr_t maxaddr, bus_addr_t align,
    bus_addr_t bndry, bus_size_t maxsz, u_int nsegs, bus_size_t maxsegsz,
    u_int flags, struct busdma_tag **tag_p)
{
	struct busdma_tag *base, *first, *tag;
	int error;

	base = _busdma_tag_get_base(dev);
	error = _busdma_tag_make(dev, base, maxaddr, align, bndry, maxsz,
	    nsegs, maxsegsz, flags, &tag);
	if (error != 0)
		return (error);

	/*
	 * This is a root tag. Link it with the device.
	 */
	first = device_set_busdma_tag(dev, tag);
	tag->dt_chain = first;
	*tag_p = tag;
	return (0);
}

int
busdma_tag_derive(struct busdma_tag *base, bus_addr_t maxaddr, bus_addr_t align,
    bus_addr_t bndry, bus_size_t maxsz, u_int nsegs, bus_size_t maxsegsz,
    u_int flags, struct busdma_tag **tag_p)
{
	struct busdma_tag *tag;
	int error;

	error = _busdma_tag_make(base->dt_device, base, maxaddr, align, bndry,
	    maxsz, nsegs, maxsegsz, flags, &tag);
	if (error != 0)
		return (error);

	/*
	 * This is a derived tag. Link it with the base tag.
	 */
	tag->dt_parent = base;
	tag->dt_chain = base->dt_child;
	base->dt_child = tag;
	*tag_p = tag;
	return (0);
}

int
busdma_mem_alloc(struct busdma_tag *tag, u_int flags, struct busdma_mem **mem_p)
{
	struct busdma_mem *mem;
	struct busdma_seg *seg;
	vm_size_t maxsz;

	mem = malloc(sizeof(*mem), M_BUSDMA_MEM, M_NOWAIT | M_ZERO);
	mem->dm_tag = tag;
	TAILQ_INIT(&mem->dm_seg);

	maxsz = tag->dt_maxsz;
	while (maxsz > 0 && mem->dm_nsegs < tag->dt_nsegs) {
		seg = malloc(sizeof(*seg), M_BUSDMA_SEG, M_NOWAIT | M_ZERO);
		TAILQ_INSERT_TAIL(&mem->dm_seg, seg, ds_chain);
		seg->ds_size = MIN(maxsz, tag->dt_maxsegsz);
		seg->ds_vaddr = kmem_alloc_contig(kernel_map, seg->ds_size, 0,
		    tag->dt_minaddr, tag->dt_maxaddr, tag->dt_align,
		    tag->dt_bndry, VM_MEMATTR_DEFAULT);
		if (seg->ds_vaddr == 0) {
			/* TODO: try a smaller segment size */
			goto fail;
		}
		seg->ds_paddr = pmap_kextract(seg->ds_vaddr);
		seg->ds_baddr = seg->ds_paddr;
		maxsz -= seg->ds_size;
		mem->dm_nsegs++;
	}
	if (maxsz == 0) {
		_busdma_mem_dump(__func__, mem);
		*mem_p = mem;
		return (0);
	}

 fail:
	while (!TAILQ_EMPTY(&mem->dm_seg)) {
		seg = TAILQ_FIRST(&mem->dm_seg);
		if (seg->ds_vaddr != 0)
			kmem_free(kernel_map, seg->ds_vaddr, seg->ds_size);
		TAILQ_REMOVE(&mem->dm_seg, seg, ds_chain);
		free(seg, M_BUSDMA_SEG);
	}
	free(mem, M_BUSDMA_MEM);
	return (ENOMEM);
}

vm_offset_t
busdma_mem_get_seg_addr(struct busdma_mem *mem, u_int idx)
{
	struct busdma_seg *seg;

	seg = _busdma_mem_get_seg(mem, idx);
	return ((seg != NULL) ? seg->ds_vaddr : 0);
}

bus_addr_t
busdma_mem_get_seg_busaddr(struct busdma_mem *mem, u_int idx)
{
	struct busdma_seg *seg;

	seg = _busdma_mem_get_seg(mem, idx);
	return ((seg != NULL) ? seg->ds_baddr : 0);
}
