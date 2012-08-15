/*-
 * Copyright (c) 1994-1998 Mark Brinicombe.
 * Copyright (c) 1994 Brini.
 * All rights reserved.
 *
 * This code is derived from software written for Brini by Mark Brinicombe
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
 *      This product includes software developed by Brini.
 * 4. The name of the company nor the name of the author may be used to
 *    endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY BRINI ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL BRINI OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: FreeBSD: //depot/projects/arm/src/sys/arm/at91/kb920x_machdep.c, rev 45
 */

#include "opt_ddb.h"
#include "opt_platform.h"

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#define _ARM32_BUS_DMA_PRIVATE
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysproto.h>
#include <sys/signalvar.h>
#include <sys/imgact.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/cons.h>
#include <sys/bio.h>
#include <sys/bus.h>
#include <sys/buf.h>
#include <sys/exec.h>
#include <sys/kdb.h>
#include <sys/msgbuf.h>
#include <machine/reg.h>
#include <machine/cpu.h>
#include <machine/fdt.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_map.h>
#include <machine/pte.h>
#include <machine/pmap.h>
#include <machine/vmparam.h>
#include <machine/pcb.h>
#include <machine/undefined.h>
#include <machine/machdep.h>
#include <machine/metadata.h>
#include <machine/armreg.h>
#include <machine/bus.h>
#include <sys/reboot.h>

#include <arm/mv/mvreg.h>	/* XXX */
#include <arm/mv/mvvar.h>	/* XXX eventually this should be eliminated */
#include <arm/mv/mvwin.h>

#ifdef  DEBUG
#define debugf(fmt, args...) printf(fmt, ##args)
#else
#define debugf(fmt, args...)
#endif

/*
 * This is the number of L2 page tables required for covering max
 * (hypothetical) memsize of 4GB and all kernel mappings (vectors, msgbuf,
 * stacks etc.), uprounded to be divisible by 4.
 */
#define KERNEL_PT_MAX	78

/* Define various stack sizes in pages */
#define IRQ_STACK_SIZE	1
#define ABT_STACK_SIZE	1
#define UND_STACK_SIZE	1

extern unsigned char kernbase[];
extern unsigned char _etext[];
extern unsigned char _edata[];
extern unsigned char __bss_start[];
extern unsigned char _end[];

#ifdef DDB
extern vm_offset_t ksym_start, ksym_end;
#endif

extern u_int data_abort_handler_address;
extern u_int prefetch_abort_handler_address;
extern u_int undefined_handler_address;

extern vm_offset_t pmap_bootstrap_lastaddr;
extern int *end;

struct pv_addr kernel_pt_table[KERNEL_PT_MAX];

vm_offset_t physical_pages;
vm_paddr_t pmap_pa;

const struct pmap_devmap *pmap_devmap_bootstrap_table;
extern struct pv_addr systempage;
extern struct pv_addr msgbufpv;
extern struct pv_addr irqstack;
extern struct pv_addr undstack;
extern struct pv_addr abtstack;
extern struct pv_addr kernelstack;

static struct mem_region availmem_regions[FDT_MEM_REGIONS];
static int availmem_regions_sz;

static void print_kenv(void);
static void print_kernel_section_addr(void);

static int platform_devmap_init(void);
static int platform_mpp_init(void);

static char *
kenv_next(char *cp)
{

	if (cp != NULL) {
		while (*cp != 0)
			cp++;
		cp++;
		if (*cp == 0)
			cp = NULL;
	}
	return (cp);
}

static void
print_kenv(void)
{
	int len;
	char *cp;

	debugf("loader passed (static) kenv:\n");
	if (kern_envp == NULL) {
		debugf(" no env, null ptr\n");
		return;
	}
	debugf(" kern_envp = 0x%08x\n", (uint32_t)kern_envp);

	len = 0;
	for (cp = kern_envp; cp != NULL; cp = kenv_next(cp))
		debugf(" %x %s\n", (uint32_t)cp, cp);
}

static void
print_kernel_section_addr(void)
{

	debugf("kernel image addresses:\n");
	debugf(" kernbase       = 0x%08x\n", (uint32_t)kernbase);
	debugf(" _etext (sdata) = 0x%08x\n", (uint32_t)_etext);
	debugf(" _edata         = 0x%08x\n", (uint32_t)_edata);
	debugf(" __bss_start    = 0x%08x\n", (uint32_t)__bss_start);
	debugf(" _end           = 0x%08x\n", (uint32_t)_end);
}

void *
initarm(void *mdp, void *unused __unused)
{
	struct pv_addr kernel_l1pt;
	struct pv_addr dpcpu;
	vm_offset_t dtbp, freemempos, l2_start, lastaddr;
	uint32_t memsize, l2size;
	void *kmdp;
	u_int l1pagetable;
	int i = 0, j = 0;

	kmdp = NULL;
	lastaddr = 0;
	memsize = 0;
	dtbp = (vm_offset_t)NULL;

	set_cpufuncs();

	/*
	 * Mask metadata pointer: it is supposed to be on page boundary. If
	 * the first argument (mdp) doesn't point to a valid address the
	 * bootloader must have passed us something else than the metadata
	 * ptr... In this case we want to fall back to some built-in settings.
	 */
	mdp = (void *)((uint32_t)mdp & ~PAGE_MASK);

	/* Parse metadata and fetch parameters */
	if (mdp != NULL) {
		preload_metadata = mdp;
		kmdp = preload_search_by_type("elf kernel");
		if (kmdp != NULL) {
			boothowto = MD_FETCH(kmdp, MODINFOMD_HOWTO, int);
			kern_envp = MD_FETCH(kmdp, MODINFOMD_ENVP, char *);
			dtbp = MD_FETCH(kmdp, MODINFOMD_DTBP, vm_offset_t);
			lastaddr = MD_FETCH(kmdp, MODINFOMD_KERNEND,
			    vm_offset_t);
#ifdef DDB
			ksym_start = MD_FETCH(kmdp, MODINFOMD_SSYM, uintptr_t);
			ksym_end = MD_FETCH(kmdp, MODINFOMD_ESYM, uintptr_t);
#endif
		}

		preload_addr_relocate = KERNVIRTADDR - KERNPHYSADDR;
	} else {
		/* Fall back to hardcoded metadata. */
		lastaddr = fake_preload_metadata();
	}

#if defined(FDT_DTB_STATIC)
	/*
	 * In case the device tree blob was not retrieved (from metadata) try
	 * to use the statically embedded one.
	 */
	if (dtbp == (vm_offset_t)NULL)
		dtbp = (vm_offset_t)&fdt_static_dtb;
#endif

	if (OF_install(OFW_FDT, 0) == FALSE)
		while (1);

	if (OF_init((void *)dtbp) != 0)
		while (1);

	/* Grab physical memory regions information from device tree. */
	if (fdt_get_mem_regions(availmem_regions, &availmem_regions_sz,
	    &memsize) != 0)
		while(1);

	if (fdt_immr_addr(MV_BASE) != 0)
		while (1);

	/* Platform-specific initialisation */
	pmap_bootstrap_lastaddr = fdt_immr_va - ARM_NOCACHE_KVA_SIZE;

	pcpu0_init();

	/* Calculate number of L2 tables needed for mapping vm_page_array */
	l2size = (memsize / PAGE_SIZE) * sizeof(struct vm_page);
	l2size = (l2size >> L1_S_SHIFT) + 1;

	/*
	 * Add one table for end of kernel map, one for stacks, msgbuf and
	 * L1 and L2 tables map and one for vectors map.
	 */
	l2size += 3;

	/* Make it divisible by 4 */
	l2size = (l2size + 3) & ~3;

#define KERNEL_TEXT_BASE (KERNBASE)
	freemempos = (lastaddr + PAGE_MASK) & ~PAGE_MASK;

	/* Define a macro to simplify memory allocation */
#define valloc_pages(var, np)                   \
	alloc_pages((var).pv_va, (np));         \
	(var).pv_pa = (var).pv_va + (KERNPHYSADDR - KERNVIRTADDR);

#define alloc_pages(var, np)			\
	(var) = freemempos;		\
	freemempos += (np * PAGE_SIZE);		\
	memset((char *)(var), 0, ((np) * PAGE_SIZE));

	while (((freemempos - L1_TABLE_SIZE) & (L1_TABLE_SIZE - 1)) != 0)
		freemempos += PAGE_SIZE;
	valloc_pages(kernel_l1pt, L1_TABLE_SIZE / PAGE_SIZE);

	for (i = 0; i < l2size; ++i) {
		if (!(i % (PAGE_SIZE / L2_TABLE_SIZE_REAL))) {
			valloc_pages(kernel_pt_table[i],
			    L2_TABLE_SIZE / PAGE_SIZE);
			j = i;
		} else {
			kernel_pt_table[i].pv_va = kernel_pt_table[j].pv_va +
			    L2_TABLE_SIZE_REAL * (i - j);
			kernel_pt_table[i].pv_pa =
			    kernel_pt_table[i].pv_va - KERNVIRTADDR +
			    KERNPHYSADDR;

		}
	}
	/*
	 * Allocate a page for the system page mapped to 0x00000000
	 * or 0xffff0000. This page will just contain the system vectors
	 * and can be shared by all processes.
	 */
	valloc_pages(systempage, 1);

	/* Allocate dynamic per-cpu area. */
	valloc_pages(dpcpu, DPCPU_SIZE / PAGE_SIZE);
	dpcpu_init((void *)dpcpu.pv_va, 0);

	/* Allocate stacks for all modes */
	valloc_pages(irqstack, (IRQ_STACK_SIZE * MAXCPU));
	valloc_pages(abtstack, (ABT_STACK_SIZE * MAXCPU));
	valloc_pages(undstack, (UND_STACK_SIZE * MAXCPU));
	valloc_pages(kernelstack, (KSTACK_PAGES * MAXCPU));

	init_param1();

	valloc_pages(msgbufpv, round_page(msgbufsize) / PAGE_SIZE);

	/*
	 * Now we start construction of the L1 page table
	 * We start by mapping the L2 page tables into the L1.
	 * This means that we can replace L1 mappings later on if necessary
	 */
	l1pagetable = kernel_l1pt.pv_va;

	/*
	 * Try to map as much as possible of kernel text and data using
	 * 1MB section mapping and for the rest of initial kernel address
	 * space use L2 coarse tables.
	 *
	 * Link L2 tables for mapping remainder of kernel (modulo 1MB)
	 * and kernel structures
	 */
	l2_start = lastaddr & ~(L1_S_OFFSET);
	for (i = 0 ; i < l2size - 1; i++)
		pmap_link_l2pt(l1pagetable, l2_start + i * L1_S_SIZE,
		    &kernel_pt_table[i]);

	pmap_curmaxkvaddr = l2_start + (l2size - 1) * L1_S_SIZE;
	
	/* Map kernel code and data */
	pmap_map_chunk(l1pagetable, KERNVIRTADDR, KERNPHYSADDR,
	   (((uint32_t)(lastaddr) - KERNVIRTADDR) + PAGE_MASK) & ~PAGE_MASK,
	    VM_PROT_READ|VM_PROT_WRITE, PTE_CACHE);


	/* Map L1 directory and allocated L2 page tables */
	pmap_map_chunk(l1pagetable, kernel_l1pt.pv_va, kernel_l1pt.pv_pa,
	    L1_TABLE_SIZE, VM_PROT_READ|VM_PROT_WRITE, PTE_PAGETABLE);

	pmap_map_chunk(l1pagetable, kernel_pt_table[0].pv_va,
	    kernel_pt_table[0].pv_pa,
	    L2_TABLE_SIZE_REAL * l2size,
	    VM_PROT_READ|VM_PROT_WRITE, PTE_PAGETABLE);

	/* Map allocated DPCPU, stacks and msgbuf */
	pmap_map_chunk(l1pagetable, dpcpu.pv_va, dpcpu.pv_pa,
	    freemempos - dpcpu.pv_va,
	    VM_PROT_READ|VM_PROT_WRITE, PTE_CACHE);

	/* Link and map the vector page */
	pmap_link_l2pt(l1pagetable, ARM_VECTORS_HIGH,
	    &kernel_pt_table[l2size - 1]);
	pmap_map_entry(l1pagetable, ARM_VECTORS_HIGH, systempage.pv_pa,
	    VM_PROT_READ|VM_PROT_WRITE|VM_PROT_EXECUTE, PTE_CACHE);

	/* Map pmap_devmap[] entries */
	if (platform_devmap_init() != 0)
		while (1);
	pmap_devmap_bootstrap(l1pagetable, pmap_devmap_bootstrap_table);

	cpu_domains((DOMAIN_CLIENT << (PMAP_DOMAIN_KERNEL * 2)) |
	    DOMAIN_CLIENT);
	pmap_pa = kernel_l1pt.pv_pa;
	setttb(kernel_l1pt.pv_pa);
	cpu_tlb_flushID();
	cpu_domains(DOMAIN_CLIENT << (PMAP_DOMAIN_KERNEL * 2));

	/*
	 * Only after the SOC registers block is mapped we can perform device
	 * tree fixups, as they may attempt to read parameters from hardware.
	 */
	OF_interpret("perform-fixup", 0);

	/*
	 * Re-initialise MPP. It is important to call this prior to using
	 * console as the physical connection can be routed via MPP.
	 */
	if (platform_mpp_init() != 0)
		while (1);

	cninit();

	physmem = memsize / PAGE_SIZE;

	debugf("initarm: console initialized\n");
	debugf(" arg1 mdp = 0x%08x\n", (uint32_t)mdp);
	debugf(" boothowto = 0x%08x\n", boothowto);
	debugf(" dtbp = 0x%08x\n", (uint32_t)dtbp);
	print_kernel_section_addr();
	print_kenv();

	/*
	 * Re-initialise decode windows
	 */
#if !defined(SOC_MV_FREY)
	if (soc_decode_win() != 0)
		printf("WARNING: could not re-initialise decode windows! "
		    "Running with existing settings...\n");
#else
	/* Disable watchdog and timers */
	write_cpu_ctrl(CPU_TIMERS_BASE + CPU_TIMER_CONTROL, 0);
#endif
	/*
	 * Pages were allocated during the secondary bootstrap for the
	 * stacks for different CPU modes.
	 * We must now set the r13 registers in the different CPU modes to
	 * point to these stacks.
	 * Since the ARM stacks use STMFD etc. we must set r13 to the top end
	 * of the stack memory.
	 */
	cpu_control(CPU_CONTROL_MMU_ENABLE, CPU_CONTROL_MMU_ENABLE);

	set_stackptrs(0);

	/*
	 * We must now clean the cache again....
	 * Cleaning may be done by reading new data to displace any
	 * dirty data in the cache. This will have happened in setttb()
	 * but since we are boot strapping the addresses used for the read
	 * may have just been remapped and thus the cache could be out
	 * of sync. A re-clean after the switch will cure this.
	 * After booting there are no gross relocations of the kernel thus
	 * this problem will not occur after initarm().
	 */
	cpu_idcache_wbinv_all();

	/* Set stack for exception handlers */
	data_abort_handler_address = (u_int)data_abort_handler;
	prefetch_abort_handler_address = (u_int)prefetch_abort_handler;
	undefined_handler_address = (u_int)undefinedinstruction_bounce;
	undefined_init();

	init_proc0(kernelstack.pv_va);

	arm_vector_init(ARM_VECTORS_HIGH, ARM_VEC_ALL);

	dump_avail[0] = 0;
	dump_avail[1] = memsize;
	dump_avail[2] = 0;
	dump_avail[3] = 0;

	pmap_bootstrap(freemempos, pmap_bootstrap_lastaddr, &kernel_l1pt);
	msgbufp = (void *)msgbufpv.pv_va;
	msgbufinit(msgbufp, msgbufsize);
	mutex_init();

	/*
	 * Prepare map of physical memory regions available to vm subsystem.
	 */
	physmap_init(availmem_regions, availmem_regions_sz);

	/* Do basic tuning, hz etc */
	init_param2(physmem);
	kdb_init();
	return ((void *)(kernelstack.pv_va + USPACE_SVC_STACK_TOP -
	    sizeof(struct pcb)));
}

#define MPP_PIN_MAX		68
#define MPP_PIN_CELLS		2
#define MPP_PINS_PER_REG	8
#define MPP_SEL(pin,func)	(((func) & 0xf) <<		\
    (((pin) % MPP_PINS_PER_REG) * 4))

static int
platform_mpp_init(void)
{
	pcell_t pinmap[MPP_PIN_MAX * MPP_PIN_CELLS];
	int mpp[MPP_PIN_MAX];
	uint32_t ctrl_val, ctrl_offset;
	pcell_t reg[4];
	u_long start, size;
	phandle_t node;
	pcell_t pin_cells, *pinmap_ptr, pin_count;
	ssize_t len;
	int par_addr_cells, par_size_cells;
	int tuple_size, tuples, rv, pins, i, j;
	int mpp_pin, mpp_function;

	/*
	 * Try to access the MPP node directly i.e. through /aliases/mpp.
	 */
	if ((node = OF_finddevice("mpp")) != -1)
		if (fdt_is_compatible(node, "mrvl,mpp"))
			goto moveon;
	/*
	 * Find the node the long way.
	 */
	if ((node = OF_finddevice("/")) == -1)
		return (ENXIO);

	if ((node = fdt_find_compatible(node, "simple-bus", 0)) == 0)
		return (ENXIO);

	if ((node = fdt_find_compatible(node, "mrvl,mpp", 0)) == 0)
		/*
		 * No MPP node. Fall back to how MPP got set by the
		 * first-stage loader and try to continue booting.
		 */
		return (0);
moveon:
	/*
	 * Process 'reg' prop.
	 */
	if ((rv = fdt_addrsize_cells(OF_parent(node), &par_addr_cells,
	    &par_size_cells)) != 0)
		return(ENXIO);

	tuple_size = sizeof(pcell_t) * (par_addr_cells + par_size_cells);
	len = OF_getprop(node, "reg", reg, sizeof(reg));
	tuples = len / tuple_size;
	if (tuple_size <= 0)
		return (EINVAL);

	/*
	 * Get address/size. XXX we assume only the first 'reg' tuple is used.
	 */
	rv = fdt_data_to_res(reg, par_addr_cells, par_size_cells,
	    &start, &size);
	if (rv != 0)
		return (rv);
	start += fdt_immr_va;

	/*
	 * Process 'pin-count' and 'pin-map' props.
	 */
	if (OF_getprop(node, "pin-count", &pin_count, sizeof(pin_count)) <= 0)
		return (ENXIO);
	pin_count = fdt32_to_cpu(pin_count);
	if (pin_count > MPP_PIN_MAX)
		return (ERANGE);

	if (OF_getprop(node, "#pin-cells", &pin_cells, sizeof(pin_cells)) <= 0)
		pin_cells = MPP_PIN_CELLS;
	pin_cells = fdt32_to_cpu(pin_cells);
	if (pin_cells > MPP_PIN_CELLS)
		return (ERANGE);
	tuple_size = sizeof(pcell_t) * pin_cells;

	bzero(pinmap, sizeof(pinmap));
	len = OF_getprop(node, "pin-map", pinmap, sizeof(pinmap));
	if (len <= 0)
		return (ERANGE);
	if (len % tuple_size)
		return (ERANGE);
	pins = len / tuple_size;
	if (pins > pin_count)
		return (ERANGE);
	/*
	 * Fill out a "mpp[pin] => function" table. All pins unspecified in
	 * the 'pin-map' property are defaulted to 0 function i.e. GPIO.
	 */
	bzero(mpp, sizeof(mpp));
	pinmap_ptr = pinmap;
	for (i = 0; i < pins; i++) {
		mpp_pin = fdt32_to_cpu(*pinmap_ptr);
		mpp_function = fdt32_to_cpu(*(pinmap_ptr + 1));
		mpp[mpp_pin] = mpp_function;
		pinmap_ptr += pin_cells;
	}

	/*
	 * Prepare and program MPP control register values.
	 */
	ctrl_offset = 0;
	for (i = 0; i < pin_count;) {
		ctrl_val = 0;

		for (j = 0; j < MPP_PINS_PER_REG; j++) {
			if (i + j == pin_count - 1)
				break;
			ctrl_val |= MPP_SEL(i + j, mpp[i + j]);
		}
		i += MPP_PINS_PER_REG;
		bus_space_write_4(fdtbus_bs_tag, start, ctrl_offset,
		    ctrl_val);

#if defined(SOC_MV_ORION)
		/*
		 * Third MPP reg on Orion SoC is placed
		 * non-linearly (with different offset).
		 */
		if (i ==  (2 * MPP_PINS_PER_REG))
			ctrl_offset = 0x50;
		else
#endif
			ctrl_offset += 4;
	}

	return (0);
}

#define FDT_DEVMAP_MAX	(MV_WIN_CPU_MAX + 2)
static struct pmap_devmap fdt_devmap[FDT_DEVMAP_MAX] = {
	{ 0, 0, 0, 0, 0, }
};

#if 0
static int
platform_sram_devmap(struct pmap_devmap *map)
{
#if !defined(SOC_MV_ARMADAXP)
	phandle_t child, root;
	u_long base, size;
	/*
	 * SRAM range.
	 */
	if ((child = OF_finddevice("/sram")) != 0)
		if (fdt_is_compatible(child, "mrvl,cesa-sram") ||
		    fdt_is_compatible(child, "mrvl,scratchpad"))
			goto moveon;

	if ((root = OF_finddevice("/")) == 0)
		return (ENXIO);

	if ((child = fdt_find_compatible(root, "mrvl,cesa-sram", 0)) == 0 &&
	    (child = fdt_find_compatible(root, "mrvl,scratchpad", 0)) == 0)
			goto out;

moveon:
	if (fdt_regsize(child, &base, &size) != 0)
		return (EINVAL);

	map->pd_va = MV_CESA_SRAM_BASE; /* XXX */
	map->pd_pa = base;
	map->pd_size = size;
	map->pd_prot = VM_PROT_READ | VM_PROT_WRITE;
	map->pd_cache = PTE_NOCACHE;

	return (0);
out:
#endif
	return (ENOENT);

}
#endif

/*
 * Construct pmap_devmap[] with DT-derived config data.
 */
static int
platform_devmap_init(void)
{
	phandle_t root, child;
	pcell_t bank_count;
	u_long base, size;
	int i, num_mapped;

	i = 0;
	pmap_devmap_bootstrap_table = &fdt_devmap[0];

	/*
	 * IMMR range.
	 */
	fdt_devmap[i].pd_va = fdt_immr_va;
	fdt_devmap[i].pd_pa = fdt_immr_pa;
	fdt_devmap[i].pd_size = fdt_immr_size;
	fdt_devmap[i].pd_prot = VM_PROT_READ | VM_PROT_WRITE;
	fdt_devmap[i].pd_cache = PTE_NOCACHE;
	i++;

	/*
	 * PCI range(s) and localbus.
	 */
	if ((root = OF_finddevice("/")) == -1)
		return (ENXIO);

	for (child = OF_child(root); child != 0; child = OF_peer(child)) {
		if (fdt_is_type(child, "pci")) {
			/*
			 * Check space: each PCI node will consume 2 devmap
			 * entries.
			 */
			if (i + 1 >= FDT_DEVMAP_MAX) {
				return (ENOMEM);
			}

			/*
			 * XXX this should account for PCI and multiple ranges
			 * of a given kind.
			 */
			if (fdt_pci_devmap(child, &fdt_devmap[i],
			    MV_PCIE_IO_BASE, MV_PCIE_MEM_BASE) != 0)
				return (ENXIO);
			i += 2;
		}

		if (fdt_is_compatible(child, "mrvl,lbc")) {
			/* Check available space */
			if (OF_getprop(child, "bank-count", (void *)&bank_count,
			    sizeof(bank_count)) <= 0)
				/* If no property, use default value */
				bank_count = 1;
			else
				bank_count = fdt32_to_cpu(bank_count);

			if ((i + bank_count) >= FDT_DEVMAP_MAX)
				return (ENOMEM);

			/* Add all localbus ranges to device map */
			num_mapped = 0;

			if (fdt_localbus_devmap(child, &fdt_devmap[i],
			    (int)bank_count, &num_mapped) != 0)
				return (ENXIO);

			i += num_mapped;
		}
	}

	/*
	 * CESA SRAM range.
	 */
	if ((child = OF_finddevice("sram")) != -1)
		if (fdt_is_compatible(child, "mrvl,cesa-sram"))
			goto moveon;

	if ((child = fdt_find_compatible(root, "mrvl,cesa-sram", 0)) == 0)
		/* No CESA SRAM node. */
		return (0);
moveon:
	if (i >= FDT_DEVMAP_MAX)
		return (ENOMEM);

	if (fdt_regsize(child, &base, &size) != 0)
		return (EINVAL);

	fdt_devmap[i].pd_va = MV_CESA_SRAM_BASE; /* XXX */
	fdt_devmap[i].pd_pa = base;
	fdt_devmap[i].pd_size = size;
	fdt_devmap[i].pd_prot = VM_PROT_READ | VM_PROT_WRITE;
	fdt_devmap[i].pd_cache = PTE_NOCACHE;

	return (0);
}


struct arm32_dma_range *
bus_dma_get_range(void)
{

	return (NULL);
}

int
bus_dma_get_range_nb(void)
{

	return (0);
}

#if defined(CPU_MV_PJ4B)
#ifdef DDB
#include <ddb/ddb.h>

DB_SHOW_COMMAND(cp15, db_show_cp15)
{
	u_int reg;

	__asm __volatile("mrc p15, 0, %0, c0, c0, 0" : "=r" (reg));
	db_printf("Cpu ID: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c0, 1" : "=r" (reg));
	db_printf("Current Cache Lvl ID: 0x%08x\n",reg);

	__asm __volatile("mrc p15, 0, %0, c1, c0, 0" : "=r" (reg));
	db_printf("Ctrl: 0x%08x\n",reg);
	__asm __volatile("mrc p15, 0, %0, c1, c0, 1" : "=r" (reg));
	db_printf("Aux Ctrl: 0x%08x\n",reg);

	__asm __volatile("mrc p15, 0, %0, c0, c1, 0" : "=r" (reg));
	db_printf("Processor Feat 0: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c1, 1" : "=r" (reg));
	db_printf("Processor Feat 1: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c1, 2" : "=r" (reg));
	db_printf("Debug Feat 0: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c1, 3" : "=r" (reg));
	db_printf("Auxiliary Feat 0: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c1, 4" : "=r" (reg));
	db_printf("Memory Model Feat 0: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c1, 5" : "=r" (reg));
	db_printf("Memory Model Feat 1: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c1, 6" : "=r" (reg));
	db_printf("Memory Model Feat 2: 0x%08x\n", reg);
	__asm __volatile("mrc p15, 0, %0, c0, c1, 7" : "=r" (reg));
	db_printf("Memory Model Feat 3: 0x%08x\n", reg);

	__asm __volatile("mrc p15, 1, %0, c15, c2, 0" : "=r" (reg));
	db_printf("Aux Func Modes Ctrl 0: 0x%08x\n",reg);
	__asm __volatile("mrc p15, 1, %0, c15, c2, 1" : "=r" (reg));
	db_printf("Aux Func Modes Ctrl 1: 0x%08x\n",reg);

	__asm __volatile("mrc p15, 1, %0, c15, c12, 0" : "=r" (reg));
	db_printf("CPU ID code extension: 0x%08x\n",reg);
}

DB_SHOW_COMMAND(vtop, db_show_vtop)
{
	u_int reg;

	if (have_addr) {
		__asm __volatile("mcr p15, 0, %0, c7, c8, 0" : : "r" (addr));
		__asm __volatile("mrc p15, 0, %0, c7, c4, 0" : "=r" (reg));
		db_printf("Physical address reg: 0x%08x\n",reg);
	} else
		db_printf("show vtop <virt_addr>\n");
}
#endif /* DDB */
#endif /* CPU_MV_PJ4B */

