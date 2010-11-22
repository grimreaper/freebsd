/*-
 * Copyright (c) 2008-2009 Semihalf, Rafal Jaworowski
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
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/smp.h>

#include <machine/bootinfo.h>
#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/hid.h>
#include <machine/platform.h>
#include <machine/platformvar.h>
#include <machine/smp.h>
#include <machine/spr.h>
#include <machine/vmparam.h>

#include <powerpc/mpc85xx/mpc85xx.h>
#include <powerpc/mpc85xx/ocpbus.h>

#include "platform_if.h"

#ifdef SMP
extern void *ap_pcpu;
extern uint8_t __boot_page[];		/* Boot page body */
extern uint32_t kernload;		/* Kernel physical load address */
#endif

static int cpu, maxcpu;

static int bare_probe(platform_t);
static void bare_mem_regions(platform_t, struct mem_region **phys, int *physsz,
    struct mem_region **avail, int *availsz);
static u_long bare_timebase_freq(platform_t, struct cpuref *cpuref);
static int bare_smp_first_cpu(platform_t, struct cpuref *cpuref);
static int bare_smp_next_cpu(platform_t, struct cpuref *cpuref);
static int bare_smp_get_bsp(platform_t, struct cpuref *cpuref);
static int bare_smp_start_cpu(platform_t, struct pcpu *cpu);

static void e500_reset(platform_t);

static platform_method_t bare_methods[] = {
	PLATFORMMETHOD(platform_probe, 		bare_probe),
	PLATFORMMETHOD(platform_mem_regions,	bare_mem_regions),
	PLATFORMMETHOD(platform_timebase_freq,	bare_timebase_freq),

	PLATFORMMETHOD(platform_smp_first_cpu,	bare_smp_first_cpu),
	PLATFORMMETHOD(platform_smp_next_cpu,	bare_smp_next_cpu),
	PLATFORMMETHOD(platform_smp_get_bsp,	bare_smp_get_bsp),
	PLATFORMMETHOD(platform_smp_start_cpu,	bare_smp_start_cpu),

	PLATFORMMETHOD(platform_reset,		e500_reset);

	{ 0, 0 }
};

static platform_def_t bare_platform = {
	"bare metal",
	bare_methods,
	0
};

PLATFORM_DEF(bare_platform);

static int
bare_probe(platform_t plat)
{
	uint32_t ver;

	ver = SVR_VER(mfspr(SPR_SVR));
	if (ver == SVR_MPC8572E || ver == SVR_MPC8572)
		maxcpu = 2;
	else
		maxcpu = 1;

	return (BUS_PROBE_GENERIC);
}

#define MEM_REGIONS	8
static struct mem_region avail_regions[MEM_REGIONS];

void
bare_mem_regions(platform_t plat, struct mem_region **phys, int *physsz,
    struct mem_region **avail, int *availsz)
{
	struct bi_mem_region *mr;
	int i;

	/* Initialize memory regions table */
	mr = bootinfo_mr();
	for (i = 0; i < bootinfo->bi_mem_reg_no; i++, mr++) {
		if (i == MEM_REGIONS)
			break;
		if (mr->mem_base < 1048576) {
			avail_regions[i].mr_start = 1048576;
			avail_regions[i].mr_size = mr->mem_size -
			    (1048576 - mr->mem_base);
		} else {
			avail_regions[i].mr_start = mr->mem_base;
			avail_regions[i].mr_size = mr->mem_size;
		}
	}
	*availsz = i;
	*avail = avail_regions;

	/* On the bare metal platform phys == avail memory */
	*physsz = *availsz;
	*phys = *avail;
}

static u_long
bare_timebase_freq(platform_t plat, struct cpuref *cpuref)
{
	u_long ticks = -1;

	/*
	 * Time Base and Decrementer are updated every 8 CCB bus clocks.
	 * HID0[SEL_TBCLK] = 0
	 */
	ticks = bootinfo->bi_bus_clk / 8;
	if (ticks <= 0)
		panic("Unable to determine timebase frequency!");

	return (ticks);
}

static int
bare_smp_first_cpu(platform_t plat, struct cpuref *cpuref)
{

	cpu = 0;
	cpuref->cr_cpuid = cpu;
	cpuref->cr_hwref = cpuref->cr_cpuid;
	if (bootverbose)
		printf("powerpc_smp_first_cpu: cpuid %d\n", cpuref->cr_cpuid);
	cpu++;

	return (0);
}

static int
bare_smp_next_cpu(platform_t plat, struct cpuref *cpuref)
{

	if (cpu >= maxcpu)
		return (ENOENT);

	cpuref->cr_cpuid = cpu++;
	cpuref->cr_hwref = cpuref->cr_cpuid;
	if (bootverbose)
		printf("powerpc_smp_next_cpu: cpuid %d\n", cpuref->cr_cpuid);

	return (0);
}

static int
bare_smp_get_bsp(platform_t plat, struct cpuref *cpuref)
{

	cpuref->cr_cpuid = mfspr(SPR_PIR);
	cpuref->cr_hwref = cpuref->cr_cpuid;

	return (0);
}

static int
bare_smp_start_cpu(platform_t plat, struct pcpu *pc)
{
#ifdef SMP
	uint32_t bptr, eebpcr;
	int timeout;

	eebpcr = ccsr_read4(OCP85XX_EEBPCR);
	if ((eebpcr & (pc->pc_cpumask << 24)) != 0) {
		printf("%s: CPU=%d already out of hold-off state!\n",
		    __func__, pc->pc_cpuid);
		return (ENXIO);
	}

	ap_pcpu = pc;
	__asm __volatile("msync; isync");

	/*
	 * Set BPTR to the physical address of the boot page
	 */
	bptr = ((uint32_t)__boot_page - KERNBASE) + kernload;
	ccsr_write4(OCP85XX_BPTR, (bptr >> 12) | 0x80000000);

	/*
	 * Release AP from hold-off state
	 */
	eebpcr |= (pc->pc_cpumask << 24);
	ccsr_write4(OCP85XX_EEBPCR, eebpcr);
	__asm __volatile("isync; msync");

	timeout = 500;
	while (!pc->pc_awake && timeout--)
		DELAY(1000);	/* wait 1ms */

	return ((pc->pc_awake) ? 0 : EBUSY);
#else
	/* No SMP support */
	return (ENXIO);
#endif
}

static void
e500_reset(platform_t plat)
{
	uint32_t ver = SVR_VER(mfspr(SPR_SVR));

	if (ver == SVR_MPC8572E || ver == SVR_MPC8572 ||
	    ver == SVR_MPC8548E || ver == SVR_MPC8548)
		/* Systems with dedicated reset register */
		ccsr_write4(OCP85XX_RSTCR, 2);
	else {
		/* Clear DBCR0, disables debug interrupts and events. */
		mtspr(SPR_DBCR0, 0);
		__asm __volatile("isync");

		/* Enable Debug Interrupts in MSR. */
		mtmsr(mfmsr() | PSL_DE);

		/* Enable debug interrupts and issue reset. */
		mtspr(SPR_DBCR0, mfspr(SPR_DBCR0) | DBCR0_IDM |
		    DBCR0_RST_SYSTEM);
	}

	printf("Reset failed...\n");
	while (1);
}

