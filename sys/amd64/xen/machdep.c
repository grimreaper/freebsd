/* $FreeBSD$ */

/*-
 * Copyright (c) 2012 Spectra Logic Corporation
 * All rights reserved.
 *
 * Portions of this software were developed by
 * Cherry G. Mathew <cherry.g.mathew@gmail.com> under sponsorship
 * from Spectra Logic Corporation.
 *
 * Copyright (c) 2003 Peter Wemm.
 * Copyright (c) 1992 Terrence R. Lambert.
 * Copyright (c) 1982, 1987, 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
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
 *	from: @(#)machdep.c	7.4 (Berkeley) 6/3/91
 */

#include "opt_compat.h"
#include "opt_cpu.h"
#include "opt_kstack_pages.h"
#include "opt_maxmem.h"
#include "opt_smp.h"

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/imgact.h>
#include <sys/linker.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/cpu.h>
#include <machine/md_var.h>
#include <machine/pcb.h>
#include <machine/stdarg.h>
#include <machine/tss.h>
#include <machine/vmparam.h>
#include <machine/xen/xen-os.h>
#include <machine/xen/xenpmap.h>
#include <xen/hypervisor.h>
#include <xen/interface/arch-x86/cpuid.h>
#include <xen/xen_intr.h>

#define	CS_SECURE(cs)		(0) /* XXX: TODO */
#define	EFL_SECURE(ef, oef)	(0) /* XXX: TODO */

int	_udatasel, _ucodesel, _ufssel, _ugssel;

int cold = 1;
int gdtset = 0;
long Maxmem = 0;
long realmem = 0;
unsigned long physfree;

start_info_t *xen_start_info;
shared_info_t *HYPERVISOR_shared_info;
xen_pfn_t *xen_machine_phys = machine_to_phys_mapping;
xen_pfn_t *xen_phys_machine;
vm_paddr_t phys_avail[0]; /* XXX: todo */
vm_paddr_t dump_avail[0]; /* XXX: todo */

struct pcpu __pcpu[MAXCPU];

struct user_segment_descriptor gdt[512] 
__aligned(PAGE_SIZE); /* vcpu0 global descriptor tables */

struct mtx icu_lock;
struct mtx dt_lock;	/* lock for GDT and LDT */ /* XXX : please review its use */

vm_paddr_t initxen(struct start_info *);

static void get_fpcontext(struct thread *td, mcontext_t *mcp);
static int  set_fpcontext(struct thread *td, const mcontext_t *mcp,
    char *xfpustate, size_t xfpustate_len);
/* Expects a zero-ed page aligned page */
static void
setup_gdt(struct user_segment_descriptor *thisgdt)
{
	uint32_t base, limit;
	uint8_t type, dpl, p, l, def32, gran;
	
	int i;
	for (i = 0; i < NGDT; i++) {
		base = 0;
		limit = 0;
		type = 0;
		dpl = 0;
		p = 0;
		l = 0;
		def32 = 0;
		gran = 0;

		switch (i) {
#if 0 /* xen manages user/kernel stack switches by itself (not via tss) */
		case GPROC0_SEL:	/* kernel TSS (64bit) first half */
			/* Second half is all zeroes */
			limit = sizeof(struct amd64tss) + IOPAGES * PAGE_SIZE - 1;
			type = SDT_SYSTSS;
			dpl = SEL_KPL;
			p = 1;
			break;
#endif /* 0 */
		case GUFS32_SEL:
		case GUGS32_SEL:
		case GUDATA_SEL:
			limit = 0xfffff;
			type = SDT_MEMRWA;
			dpl = SEL_UPL;
			p = 1;
			def32 = 1;
			gran = 1;
			break;

		case GCODE_SEL:
			limit = 0xfffff;
			type = SDT_MEMERA;
			dpl = SEL_KPL;
			p = 1;
			l = 1;
			gran = 1;
			break;

		case GDATA_SEL:
			limit = 0xfffff;
			type = SDT_MEMRWA;
			dpl = SEL_KPL;
			p = 1;
			l = 1;
			gran = 1;
			break;

		case GUCODE32_SEL:
			limit = 0xfffff;
			type = SDT_MEMERA;
			dpl = SEL_UPL;
			p = 1;
			def32 = 1;
			gran = 1;
			break;

		}


		USD_SETBASE(&thisgdt[i], base);
		USD_SETLIMIT(&thisgdt[i], limit);
		thisgdt[i].sd_type  = type;
		thisgdt[i].sd_dpl   = dpl;
		thisgdt[i].sd_p     = p;
		thisgdt[i].sd_long  = l;
		thisgdt[i].sd_def32 = def32;
		thisgdt[i].sd_gran  = gran;
		thisgdt[i].sd_xx = 0;
	}
}

/*
 * Tell xen about our exception handlers. Unlike page tables, this is
 * a "fire-and-forget" xen setup - we only need to pass a template of
 * the vector table which xen then makes a copy of. Each time this
 * function is called, the entire trap table is updated.
 *
 * Note: We have a page worth of boot stack, so ok to do put the
 * template on the stack.
 */
extern int Xde, Xdb, Xnmi, Xbp, Xof, Xbr, Xud, Xnm, Xdf, Xts, Xnp, Xss, Xgp, Xpf, Xmf, Xac, Xmc, Xxf;

static void
init_exception_table(void)
{
	/* 
	 * The vector mapping is dictated by the Intel 64 and
	 * IA-32 Architectures Software Developer's Manual, Volume 3,
	 * System Programming Guide.... Table 6.1 "Exceptions and
	 * Interrupts".
	 *
	 * Note: Xen only re-routes exceptions via this
	 * mechanism. Hardware Interrupts are managed via an "event"
	 * mechanism, elsewhere.
	 */

	struct trap_info exception_table[] = {
		/* .vector, .flags, .cs, .address */
		{ 0, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xde },
		{ 1, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xdb },
		{ 2, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xnmi }, /* XXX: masking */
		{ 3, SEL_UPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xbp },
		{ 4, SEL_UPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xof },
		{ 5, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xbr },
		{ 6, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xud },
		{ 7, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xnm },
		{ 8, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xdf },
		{ 10, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xts },
		{ 11, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xnp },
		{ 12, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xss },
		{ 13, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xgp },
		{ 14, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xpf },
		{ 15, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xmf },
		{ 16, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xac },
		{ 17, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xmc }, /* XXX: investigate MCA on XEN */
		{ 18, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL), (unsigned long) &Xxf },
		{  0, 0,           0, 0 } /* End of table marker
					   * .address == 0 */
	};

	PANIC_IF(HYPERVISOR_set_trap_table(exception_table));

}

#define XEN_CPUID_LEAF_HYPERCALL XEN_CPUID_LEAF(3 - 1)

void xen_set_hypercall_page(vm_paddr_t);
extern char hypercall_page[]; /* locore.s */
extern uint64_t xenstack; /* start of Xen provided stack */

/* 
 * Setup early kernel environment, based on start_info passed to us by
 * xen
 */
vm_paddr_t
initxen(struct start_info *si)
{

	caddr_t kmdp;
	size_t kstack0_sz;
	struct pcpu *pc;

	KASSERT(si != NULL, ("start_info invalid"));

	xen_start_info = si;

	/* xen variables */
	xen_phys_machine = (xen_pfn_t *)si->mfn_list;

	physmem = si->nr_pages;
	Maxmem = si->nr_pages + 1;

	/* setup kernel tls registers. pcpu needs them */
	pc = &__pcpu[0];
	HYPERVISOR_set_segment_base (SEGBASE_FS, 0);
	HYPERVISOR_set_segment_base (SEGBASE_GS_KERNEL, (uint64_t) pc);
	HYPERVISOR_set_segment_base (SEGBASE_GS_USER, 0);

	/* Setup paging */
	/* 
	 * We'll reclaim the space taken by bootstrap PT and bootstrap
	 * stack by marking them later as an available chunk via
	 * phys_avail[] to the vm subsystem.
	 */

	/* Address of lowest unused page */
	physfree = VTOP(si->pt_base + si->nr_pt_frames * PAGE_SIZE);

	pmap_bootstrap(&physfree);

	thread0.td_kstack = PTOV(physfree);
	thread0.td_kstack_pages = KSTACK_PAGES;
	kstack0_sz = thread0.td_kstack_pages * PAGE_SIZE;
	bzero((void *)thread0.td_kstack, kstack0_sz);
	physfree += kstack0_sz;
	thread0.td_pcb = (struct pcb *)(thread0.td_kstack + kstack0_sz) - 1;

	/*
 	 * This may be done better later if it gets more high level
 	 * components in it. If so just link td->td_proc here.
	 */
	proc_linkup0(&proc0, &thread0);

	KASSERT(si->mod_start == 0, ("MISMATCH"));

	if (si->mod_start != 0) { /* we have a ramdisk or kernel module */
		preload_metadata = (caddr_t)(si->mod_start);
		// preload_bootstrap_relocate(KERNBASE); XXX: not sure this is needed.
	}

	kmdp = preload_search_by_type("elf kernel");
	if (kmdp == NULL)
		kmdp = preload_search_by_type("elf64 kernel");

#ifdef notyet
	boothowto = MD_FETCH(kmdp, MODINFOMD_HOWTO, int);
	kern_envp = MD_FETCH(kmdp, MODINFOMD_ENVP, char *); // XXX: +
						    // KERNBASE;
#endif /* notyet */

#ifdef DDB
	ksym_start = MD_FETCH(kmdp, MODINFOMD_SSYM, uintptr_t);
	ksym_end = MD_FETCH(kmdp, MODINFOMD_ESYM, uintptr_t);
#endif
	/* Init basic tunables, hz etc */
	init_param1();

	/* gdt */
	vm_paddr_t gdt0_frame = phystomach(VTOP(gdt));
	vm_paddr_t gdt0_frame_mfn = PFNTOMFN(VTOPFN(gdt));

	memset(gdt, 0, sizeof gdt);
	setup_gdt(gdt);

	/* gdt resides in R/O memory. Update mappings */
	if (HYPERVISOR_update_va_mapping((vm_offset_t)gdt, 
		gdt0_frame | PG_U | PG_V, UVMF_INVLPG)) {
		printk("HYPERVISOR_update_va_mapping() failed\n");
		cpu_halt();
		/* NOTREACHED */
	}

	if (HYPERVISOR_set_gdt((unsigned long *)&gdt0_frame_mfn, NGDT) != 0) {
		printk("HYPERVISOR_set_gdt() failed\n");
		cpu_halt();
		/* NOTREACHED */
	}

	lgdt(NULL); /* See: support.S */

	/* exception handling */
	init_exception_table();

	/* page tables */
	/* console */
	printk("Hello world!\n");

	while(1);
	KASSERT(0, ("TODO"));
	return thread0.td_kstack;
}

/*
 * Flush the D-cache for non-DMA I/O so that the I-cache can
 * be made coherent later.
 */
void
cpu_flush_dcache(void *ptr, size_t len)
{
	/* Not applicable */
}

/* Get current clock frequency for the given cpu id. */
int
cpu_est_clockrate(int cpu_id, uint64_t *rate)
{
	uint64_t tsc1, tsc2;
	register_t reg;

	if (pcpu_find(cpu_id) == NULL || rate == NULL)
		return (EINVAL);

#ifdef SMP
	if (smp_cpus > 1) {
		/* Schedule ourselves on the indicated cpu. */
		thread_lock(curthread);
		sched_bind(curthread, cpu_id);
		thread_unlock(curthread);
	}
#endif

	/* Calibrate by measuring a short delay. */
	reg = intr_disable();
	tsc1 = rdtsc();
	DELAY(1000);
	tsc2 = rdtsc();
	intr_restore(reg);
	*rate = (tsc2 - tsc1) * 1000;

#ifdef SMP
	if (smp_cpus > 1) {
		thread_lock(curthread);
		sched_unbind(curthread);
		thread_unlock(curthread);
	}
#endif

	return (0);
}

void
cpu_halt(void)
{
	HYPERVISOR_shutdown(SHUTDOWN_poweroff);
}

#define	STATE_RUNNING	0x0
#define	STATE_MWAIT	0x1
#define	STATE_SLEEPING	0x2

int scheduler_running;

void
cpu_idle(int busy)
{

	CTR2(KTR_SPARE2, "cpu_idle(%d) at %d",
	    busy, curcpu);

	/* If we have time - switch timers into idle mode. */
	if (!busy) {
		critical_enter();
		cpu_idleclock();
	}

	/* Call main idle method. */
	scheduler_running = 1;
	enable_intr();
	idle_block();

	/* Switch timers mack into active mode. */
	if (!busy) {
		cpu_activeclock();
		critical_exit();
	}

	CTR2(KTR_SPARE2, "cpu_idle(%d) at %d done",
	    busy, curcpu);
}

int
cpu_idle_wakeup(int cpu)
{
	struct pcpu *pcpu;
	int *state;

	pcpu = pcpu_find(cpu);
	state = (int *)pcpu->pc_monitorbuf;
	/*
	 * This doesn't need to be atomic since missing the race will
	 * simply result in unnecessary IPIs.
	 */
	if (*state == STATE_SLEEPING)
		return (0);
	if (*state == STATE_MWAIT)
		*state = STATE_RUNNING;
	return (1);
}

/* XXX: Unify with "native" machdep.c */
/*
 * Reset registers to default values on exec.
 */
void
exec_setregs(struct thread *td, struct image_params *imgp, u_long stack)
{
	KASSERT(0, ("TODO"));	
}

void
cpu_pcpu_init(struct pcpu *pcpu, int cpuid, size_t size)
{

	pcpu->pc_acpi_id = 0xffffffff;
}

void
spinlock_enter(void)
{
	struct thread *td;
	register_t flags;

	td = curthread;
	if (td->td_md.md_spinlock_count == 0) {
		flags = intr_disable();
		td->td_md.md_spinlock_count = 1;
		td->td_md.md_saved_flags = flags;
	} else
		td->td_md.md_spinlock_count++;
	critical_enter();
}

void
spinlock_exit(void)
{
	struct thread *td;
	register_t flags;

	td = curthread;
	critical_exit();
	flags = td->td_md.md_saved_flags;
	td->td_md.md_spinlock_count--;
	if (td->td_md.md_spinlock_count == 0)
		intr_restore(flags);
}

/*
 * Construct a PCB from a trapframe. This is called from kdb_trap() where
 * we want to start a backtrace from the function that caused us to enter
 * the debugger. We have the context in the trapframe, but base the trace
 * on the PCB. The PCB doesn't have to be perfect, as long as it contains
 * enough for a backtrace.
 */
void
makectx(struct trapframe *tf, struct pcb *pcb)
{

	pcb->pcb_r12 = tf->tf_r12;
	pcb->pcb_r13 = tf->tf_r13;
	pcb->pcb_r14 = tf->tf_r14;
	pcb->pcb_r15 = tf->tf_r15;
	pcb->pcb_rbp = tf->tf_rbp;
	pcb->pcb_rbx = tf->tf_rbx;
	pcb->pcb_rip = tf->tf_rip;
	pcb->pcb_rsp = tf->tf_rsp;
}

int
ptrace_set_pc(struct thread *td, unsigned long addr)
{
	td->td_frame->tf_rip = addr;
	return (0);
}

int
ptrace_single_step(struct thread *td)
{
	td->td_frame->tf_rflags |= PSL_T;
	return (0);
}

int
ptrace_clear_single_step(struct thread *td)
{
	td->td_frame->tf_rflags &= ~PSL_T;
	return (0);
}

int
fill_regs(struct thread *td, struct reg *regs)
{
	struct trapframe *tp;

	tp = td->td_frame;
	return (fill_frame_regs(tp, regs));
}

int
fill_frame_regs(struct trapframe *tp, struct reg *regs)
{
	regs->r_r15 = tp->tf_r15;
	regs->r_r14 = tp->tf_r14;
	regs->r_r13 = tp->tf_r13;
	regs->r_r12 = tp->tf_r12;
	regs->r_r11 = tp->tf_r11;
	regs->r_r10 = tp->tf_r10;
	regs->r_r9  = tp->tf_r9;
	regs->r_r8  = tp->tf_r8;
	regs->r_rdi = tp->tf_rdi;
	regs->r_rsi = tp->tf_rsi;
	regs->r_rbp = tp->tf_rbp;
	regs->r_rbx = tp->tf_rbx;
	regs->r_rdx = tp->tf_rdx;
	regs->r_rcx = tp->tf_rcx;
	regs->r_rax = tp->tf_rax;
	regs->r_rip = tp->tf_rip;
	regs->r_cs = tp->tf_cs;
	regs->r_rflags = tp->tf_rflags;
	regs->r_rsp = tp->tf_rsp;
	regs->r_ss = tp->tf_ss;
	if (tp->tf_flags & TF_HASSEGS) {
		regs->r_ds = tp->tf_ds;
		regs->r_es = tp->tf_es;
		regs->r_fs = tp->tf_fs;
		regs->r_gs = tp->tf_gs;
	} else {
		regs->r_ds = 0;
		regs->r_es = 0;
		regs->r_fs = 0;
		regs->r_gs = 0;
	}
	return (0);
}

int
set_regs(struct thread *td, struct reg *regs)
{
	struct trapframe *tp;
	register_t rflags;

	tp = td->td_frame;
	rflags = regs->r_rflags & 0xffffffff;
	if (!EFL_SECURE(rflags, tp->tf_rflags) || !CS_SECURE(regs->r_cs))
		return (EINVAL);
	tp->tf_r15 = regs->r_r15;
	tp->tf_r14 = regs->r_r14;
	tp->tf_r13 = regs->r_r13;
	tp->tf_r12 = regs->r_r12;
	tp->tf_r11 = regs->r_r11;
	tp->tf_r10 = regs->r_r10;
	tp->tf_r9  = regs->r_r9;
	tp->tf_r8  = regs->r_r8;
	tp->tf_rdi = regs->r_rdi;
	tp->tf_rsi = regs->r_rsi;
	tp->tf_rbp = regs->r_rbp;
	tp->tf_rbx = regs->r_rbx;
	tp->tf_rdx = regs->r_rdx;
	tp->tf_rcx = regs->r_rcx;
	tp->tf_rax = regs->r_rax;
	tp->tf_rip = regs->r_rip;
	tp->tf_cs = regs->r_cs;
	tp->tf_rflags = rflags;
	tp->tf_rsp = regs->r_rsp;
	tp->tf_ss = regs->r_ss;
	if (0) {	/* XXXKIB */
		tp->tf_ds = regs->r_ds;
		tp->tf_es = regs->r_es;
		tp->tf_fs = regs->r_fs;
		tp->tf_gs = regs->r_gs;
		tp->tf_flags = TF_HASSEGS;
		set_pcb_flags(td->td_pcb, PCB_FULL_IRET);
	}
	return (0);
}

/* XXX check all this stuff! */
/* externalize from sv_xmm */
static void
fill_fpregs_xmm(struct savefpu *sv_xmm, struct fpreg *fpregs)
{
	struct envxmm *penv_fpreg = (struct envxmm *)&fpregs->fpr_env;
	struct envxmm *penv_xmm = &sv_xmm->sv_env;
	int i;

	/* pcb -> fpregs */
	bzero(fpregs, sizeof(*fpregs));

	/* FPU control/status */
	penv_fpreg->en_cw = penv_xmm->en_cw;
	penv_fpreg->en_sw = penv_xmm->en_sw;
	penv_fpreg->en_tw = penv_xmm->en_tw;
	penv_fpreg->en_opcode = penv_xmm->en_opcode;
	penv_fpreg->en_rip = penv_xmm->en_rip;
	penv_fpreg->en_rdp = penv_xmm->en_rdp;
	penv_fpreg->en_mxcsr = penv_xmm->en_mxcsr;
	penv_fpreg->en_mxcsr_mask = penv_xmm->en_mxcsr_mask;

	/* FPU registers */
	for (i = 0; i < 8; ++i)
		bcopy(sv_xmm->sv_fp[i].fp_acc.fp_bytes, fpregs->fpr_acc[i], 10);

	/* SSE registers */
	for (i = 0; i < 16; ++i)
		bcopy(sv_xmm->sv_xmm[i].xmm_bytes, fpregs->fpr_xacc[i], 16);
}

/* internalize from fpregs into sv_xmm */
static void
set_fpregs_xmm(struct fpreg *fpregs, struct savefpu *sv_xmm)
{
	struct envxmm *penv_xmm = &sv_xmm->sv_env;
	struct envxmm *penv_fpreg = (struct envxmm *)&fpregs->fpr_env;
	int i;

	/* fpregs -> pcb */
	/* FPU control/status */
	penv_xmm->en_cw = penv_fpreg->en_cw;
	penv_xmm->en_sw = penv_fpreg->en_sw;
	penv_xmm->en_tw = penv_fpreg->en_tw;
	penv_xmm->en_opcode = penv_fpreg->en_opcode;
	penv_xmm->en_rip = penv_fpreg->en_rip;
	penv_xmm->en_rdp = penv_fpreg->en_rdp;
	penv_xmm->en_mxcsr = penv_fpreg->en_mxcsr;
	penv_xmm->en_mxcsr_mask = penv_fpreg->en_mxcsr_mask & cpu_mxcsr_mask;

	/* FPU registers */
	for (i = 0; i < 8; ++i)
		bcopy(fpregs->fpr_acc[i], sv_xmm->sv_fp[i].fp_acc.fp_bytes, 10);

	/* SSE registers */
	for (i = 0; i < 16; ++i)
		bcopy(fpregs->fpr_xacc[i], sv_xmm->sv_xmm[i].xmm_bytes, 16);
}

/* externalize from td->pcb */
int
fill_fpregs(struct thread *td, struct fpreg *fpregs)
{

	KASSERT(td == curthread || TD_IS_SUSPENDED(td),
	    ("not suspended thread %p", td));
	fpugetregs(td);
	fill_fpregs_xmm(get_pcb_user_save_td(td), fpregs);
	return (0);
}

/* internalize to td->pcb */
int
set_fpregs(struct thread *td, struct fpreg *fpregs)
{

	set_fpregs_xmm(fpregs, get_pcb_user_save_td(td));
	fpuuserinited(td);
	return (0);
}

/*
 * Get machine context.
 */
int
get_mcontext(struct thread *td, mcontext_t *mcp, int flags)
{
	struct pcb *pcb;
	struct trapframe *tp;

	pcb = td->td_pcb;
	tp = td->td_frame;
	PROC_LOCK(curthread->td_proc);
	mcp->mc_onstack = sigonstack(tp->tf_rsp);
	PROC_UNLOCK(curthread->td_proc);
	mcp->mc_r15 = tp->tf_r15;
	mcp->mc_r14 = tp->tf_r14;
	mcp->mc_r13 = tp->tf_r13;
	mcp->mc_r12 = tp->tf_r12;
	mcp->mc_r11 = tp->tf_r11;
	mcp->mc_r10 = tp->tf_r10;
	mcp->mc_r9  = tp->tf_r9;
	mcp->mc_r8  = tp->tf_r8;
	mcp->mc_rdi = tp->tf_rdi;
	mcp->mc_rsi = tp->tf_rsi;
	mcp->mc_rbp = tp->tf_rbp;
	mcp->mc_rbx = tp->tf_rbx;
	mcp->mc_rcx = tp->tf_rcx;
	mcp->mc_rflags = tp->tf_rflags;
	if (flags & GET_MC_CLEAR_RET) {
		mcp->mc_rax = 0;
		mcp->mc_rdx = 0;
		mcp->mc_rflags &= ~PSL_C;
	} else {
		mcp->mc_rax = tp->tf_rax;
		mcp->mc_rdx = tp->tf_rdx;
	}
	mcp->mc_rip = tp->tf_rip;
	mcp->mc_cs = tp->tf_cs;
	mcp->mc_rsp = tp->tf_rsp;
	mcp->mc_ss = tp->tf_ss;
	mcp->mc_ds = tp->tf_ds;
	mcp->mc_es = tp->tf_es;
	mcp->mc_fs = tp->tf_fs;
	mcp->mc_gs = tp->tf_gs;
	mcp->mc_flags = tp->tf_flags;
	mcp->mc_len = sizeof(*mcp);
	get_fpcontext(td, mcp);
	mcp->mc_fsbase = pcb->pcb_fsbase;
	mcp->mc_gsbase = pcb->pcb_gsbase;
	bzero(mcp->mc_spare, sizeof(mcp->mc_spare));
	return (0);
}

/*
 * Set machine context.
 *
 * However, we don't set any but the user modifiable flags, and we won't
 * touch the cs selector.
 */
int
set_mcontext(struct thread *td, const mcontext_t *mcp)
{
	struct pcb *pcb;
	struct trapframe *tp;
	char *xfpustate;
	long rflags;
	int ret;

	pcb = td->td_pcb;
	tp = td->td_frame;
	if (mcp->mc_len != sizeof(*mcp) ||
	    (mcp->mc_flags & ~_MC_FLAG_MASK) != 0)
		return (EINVAL);
	rflags = (mcp->mc_rflags & PSL_USERCHANGE) |
	    (tp->tf_rflags & ~PSL_USERCHANGE);
	if (mcp->mc_flags & _MC_HASFPXSTATE) {
		if (mcp->mc_xfpustate_len > cpu_max_ext_state_size -
		    sizeof(struct savefpu))
			return (EINVAL);
		xfpustate = __builtin_alloca(mcp->mc_xfpustate_len);
		ret = copyin((void *)mcp->mc_xfpustate, xfpustate,
		    mcp->mc_xfpustate_len);
		if (ret != 0)
			return (ret);
	} else
		xfpustate = NULL;
	ret = set_fpcontext(td, mcp, xfpustate, mcp->mc_xfpustate_len);
	if (ret != 0)
		return (ret);
	tp->tf_r15 = mcp->mc_r15;
	tp->tf_r14 = mcp->mc_r14;
	tp->tf_r13 = mcp->mc_r13;
	tp->tf_r12 = mcp->mc_r12;
	tp->tf_r11 = mcp->mc_r11;
	tp->tf_r10 = mcp->mc_r10;
	tp->tf_r9  = mcp->mc_r9;
	tp->tf_r8  = mcp->mc_r8;
	tp->tf_rdi = mcp->mc_rdi;
	tp->tf_rsi = mcp->mc_rsi;
	tp->tf_rbp = mcp->mc_rbp;
	tp->tf_rbx = mcp->mc_rbx;
	tp->tf_rdx = mcp->mc_rdx;
	tp->tf_rcx = mcp->mc_rcx;
	tp->tf_rax = mcp->mc_rax;
	tp->tf_rip = mcp->mc_rip;
	tp->tf_rflags = rflags;
	tp->tf_rsp = mcp->mc_rsp;
	tp->tf_ss = mcp->mc_ss;
	tp->tf_flags = mcp->mc_flags;
	if (tp->tf_flags & TF_HASSEGS) {
		tp->tf_ds = mcp->mc_ds;
		tp->tf_es = mcp->mc_es;
		tp->tf_fs = mcp->mc_fs;
		tp->tf_gs = mcp->mc_gs;
	}
	if (mcp->mc_flags & _MC_HASBASES) {
		pcb->pcb_fsbase = mcp->mc_fsbase;
		pcb->pcb_gsbase = mcp->mc_gsbase;
	}
	set_pcb_flags(pcb, PCB_FULL_IRET);
	return (0);
}

static void
get_fpcontext(struct thread *td, mcontext_t *mcp)
{

	mcp->mc_ownedfp = fpugetregs(td);
	bcopy(get_pcb_user_save_td(td), &mcp->mc_fpstate,
	    sizeof(mcp->mc_fpstate));
	mcp->mc_fpformat = fpuformat();
}

static int
set_fpcontext(struct thread *td, const mcontext_t *mcp, char *xfpustate,
    size_t xfpustate_len)
{
	struct savefpu *fpstate;
	int error;
	
	if (mcp->mc_fpformat == _MC_FPFMT_NODEV)
		return (0);
	else if (mcp->mc_fpformat != _MC_FPFMT_XMM)
		return (EINVAL);
	else if (mcp->mc_ownedfp == _MC_FPOWNED_NONE)
		/* We don't care what state is left in the FPU or PCB. */
		fpstate_drop(td);
	else if (mcp->mc_ownedfp == _MC_FPOWNED_FPU ||
	    mcp->mc_ownedfp == _MC_FPOWNED_PCB) {
		fpstate = (struct savefpu *)&mcp->mc_fpstate;
		fpstate->sv_env.en_mxcsr &= cpu_mxcsr_mask;
		error = fpusetregs(td, fpstate, xfpustate, xfpustate_len);
	} else
		return (EINVAL);
	return (0);
}

void
fpstate_drop(struct thread *td)
{

	KASSERT(PCB_USER_FPU(td->td_pcb), ("fpstate_drop: kernel-owned fpu"));
	critical_enter();
	if (PCPU_GET(fpcurthread) == td)
		fpudrop();
	/*
	 * XXX force a full drop of the fpu.  The above only drops it if we
	 * owned it.
	 *
	 * XXX I don't much like fpugetuserregs()'s semantics of doing a full
	 * drop.  Dropping only to the pcb matches fnsave's behaviour.
	 * We only need to drop to !PCB_INITDONE in sendsig().  But
	 * sendsig() is the only caller of fpugetuserregs()... perhaps we just
	 * have too many layers.
	 */
	clear_pcb_flags(curthread->td_pcb,
	    PCB_FPUINITDONE | PCB_USERFPUINITDONE);
	critical_exit();
}

int
fill_dbregs(struct thread *td, struct dbreg *dbregs)
{
	KASSERT(0, ("XXX: TODO"));
	return -1;
}

int
set_dbregs(struct thread *td, struct dbreg *dbregs)
{
	KASSERT(0, ("XXX: TODO"));
	return -1;
}

void
reset_dbregs(void)
{
	KASSERT(0, ("XXX: TODO"));
}

#define PRINTK_BUFSIZE 1024
void
printk(const char *fmt, ...)
{
        __va_list ap;
        int retval;
        static char buf[PRINTK_BUFSIZE];

        va_start(ap, fmt);
        retval = vsnprintf(buf, PRINTK_BUFSIZE - 1, fmt, ap);
        va_end(ap);
        buf[retval] = 0;
        (void)HYPERVISOR_console_write(buf, retval);
}


static __inline void
cpu_write_rflags(u_int ef)
{
	__asm __volatile("pushl %0; popfl" : : "r" (ef));
}

static __inline u_int
cpu_read_rflags(void)
{
	u_int	ef;

	__asm __volatile("pushfl; popl %0" : "=r" (ef));
	return (ef);
}

u_long
read_rflags(void)
{
        vcpu_info_t *_vcpu;
	u_long rflags;

	rflags = _read_rflags();
        _vcpu = &HYPERVISOR_shared_info->vcpu_info[smp_processor_id()]; 
	if (_vcpu->evtchn_upcall_mask)
		rflags &= ~PSL_I;

	return (rflags);
}

char *console_page;
#include <machine/tss.h>
struct amd64tss common_tss[MAXCPU];

void
sdtossd(sd, ssd)
	struct user_segment_descriptor *sd;
	struct soft_segment_descriptor *ssd;
{

	ssd->ssd_base  = (sd->sd_hibase << 24) | sd->sd_lobase;
	ssd->ssd_limit = (sd->sd_hilimit << 16) | sd->sd_lolimit;
	ssd->ssd_type  = sd->sd_type;
	ssd->ssd_dpl   = sd->sd_dpl;
	ssd->ssd_p     = sd->sd_p;
	ssd->ssd_long  = sd->sd_long;
	ssd->ssd_def32 = sd->sd_def32;
	ssd->ssd_gran  = sd->sd_gran;
}

void
ssdtosyssd(ssd, sd)
	struct soft_segment_descriptor *ssd;
	struct system_segment_descriptor *sd;
{

	sd->sd_lobase = (ssd->ssd_base) & 0xffffff;
	sd->sd_hibase = (ssd->ssd_base >> 24) & 0xfffffffffful;
	sd->sd_lolimit = (ssd->ssd_limit) & 0xffff;
	sd->sd_hilimit = (ssd->ssd_limit >> 16) & 0xf;
	sd->sd_type  = ssd->ssd_type;
	sd->sd_dpl   = ssd->ssd_dpl;
	sd->sd_p     = ssd->ssd_p;
	sd->sd_gran  = ssd->ssd_gran;
}

/*
 * Return > 0 if a hardware breakpoint has been hit, and the
 * breakpoint was in user space.  Return 0, otherwise.
 */
int
user_dbreg_trap(void)
{
	KASSERT(0, ("XXX: TODO\n"));
	return -1;
}

#include <sys/sysent.h>
#include <machine/sigframe.h>

/*
 * Send an interrupt to process.
 *
 * Stack is set up to allow sigcode stored
 * at top to call routine, followed by call
 * to sigreturn routine below.  After sigreturn
 * resets the signal mask, the stack, and the
 * frame pointer, it returns to the user
 * specified pc, psl.
 */
void
sendsig(sig_t catcher, ksiginfo_t *ksi, sigset_t *mask)
{
	struct sigframe sf, *sfp;
	struct pcb *pcb;
	struct proc *p;
	struct thread *td;
	struct sigacts *psp;
	char *sp;
	struct trapframe *regs;
	int sig;
	int oonstack;

	td = curthread;
	pcb = td->td_pcb;
	p = td->td_proc;
	PROC_LOCK_ASSERT(p, MA_OWNED);
	sig = ksi->ksi_signo;
	psp = p->p_sigacts;
	mtx_assert(&psp->ps_mtx, MA_OWNED);
	regs = td->td_frame;
	oonstack = sigonstack(regs->tf_rsp);

	/* Save user context. */
	bzero(&sf, sizeof(sf));
	sf.sf_uc.uc_sigmask = *mask;
	sf.sf_uc.uc_stack = td->td_sigstk;
	sf.sf_uc.uc_stack.ss_flags = (td->td_pflags & TDP_ALTSTACK)
	    ? ((oonstack) ? SS_ONSTACK : 0) : SS_DISABLE;
	sf.sf_uc.uc_mcontext.mc_onstack = (oonstack) ? 1 : 0;
	bcopy(regs, &sf.sf_uc.uc_mcontext.mc_rdi, sizeof(*regs));
	sf.sf_uc.uc_mcontext.mc_len = sizeof(sf.sf_uc.uc_mcontext); /* magic */
	get_fpcontext(td, &sf.sf_uc.uc_mcontext);
	fpstate_drop(td);
	sf.sf_uc.uc_mcontext.mc_fsbase = pcb->pcb_fsbase;
	sf.sf_uc.uc_mcontext.mc_gsbase = pcb->pcb_gsbase;
	bzero(sf.sf_uc.uc_mcontext.mc_spare,
	    sizeof(sf.sf_uc.uc_mcontext.mc_spare));
	bzero(sf.sf_uc.__spare__, sizeof(sf.sf_uc.__spare__));

	/* Allocate space for the signal handler context. */
	if ((td->td_pflags & TDP_ALTSTACK) != 0 && !oonstack &&
	    SIGISMEMBER(psp->ps_sigonstack, sig)) {
		sp = td->td_sigstk.ss_sp +
		    td->td_sigstk.ss_size - sizeof(struct sigframe);
#if defined(COMPAT_43)
		td->td_sigstk.ss_flags |= SS_ONSTACK;
#endif
	} else
		sp = (char *)regs->tf_rsp - sizeof(struct sigframe) - 128;
	/* Align to 16 bytes. */
	sfp = (struct sigframe *)((unsigned long)sp & ~0xFul);

	/* Translate the signal if appropriate. */
	if (p->p_sysent->sv_sigtbl && sig <= p->p_sysent->sv_sigsize)
		sig = p->p_sysent->sv_sigtbl[_SIG_IDX(sig)];

	/* Build the argument list for the signal handler. */
	regs->tf_rdi = sig;			/* arg 1 in %rdi */
	regs->tf_rdx = (register_t)&sfp->sf_uc;	/* arg 3 in %rdx */
	bzero(&sf.sf_si, sizeof(sf.sf_si));
	if (SIGISMEMBER(psp->ps_siginfo, sig)) {
		/* Signal handler installed with SA_SIGINFO. */
		regs->tf_rsi = (register_t)&sfp->sf_si;	/* arg 2 in %rsi */
		sf.sf_ahu.sf_action = (__siginfohandler_t *)catcher;

		/* Fill in POSIX parts */
		sf.sf_si = ksi->ksi_info;
		sf.sf_si.si_signo = sig; /* maybe a translated signal */
		regs->tf_rcx = (register_t)ksi->ksi_addr; /* arg 4 in %rcx */
	} else {
		/* Old FreeBSD-style arguments. */
		regs->tf_rsi = ksi->ksi_code;	/* arg 2 in %rsi */
		regs->tf_rcx = (register_t)ksi->ksi_addr; /* arg 4 in %rcx */
		sf.sf_ahu.sf_handler = catcher;
	}
	mtx_unlock(&psp->ps_mtx);
	PROC_UNLOCK(p);

	/*
	 * Copy the sigframe out to the user's stack.
	 */
	if (copyout(&sf, sfp, sizeof(*sfp)) != 0) {
#ifdef DEBUG
		printf("process %ld has trashed its stack\n", (long)p->p_pid);
#endif
		PROC_LOCK(p);
		sigexit(td, SIGILL);
	}

	regs->tf_rsp = (long)sfp;
	regs->tf_rip = p->p_sysent->sv_sigcode_base;
	regs->tf_rflags &= ~(PSL_T | PSL_D);
	regs->tf_cs = _ucodesel;
	regs->tf_ds = _udatasel;
	regs->tf_es = _udatasel;
	regs->tf_fs = _ufssel;
	regs->tf_gs = _ugssel;
	regs->tf_flags = TF_HASSEGS;
	set_pcb_flags(pcb, PCB_FULL_IRET);
	PROC_LOCK(p);
	mtx_lock(&psp->ps_mtx);
}

/*
 * System call to cleanup state after a signal
 * has been taken.  Reset signal mask and
 * stack state from context left by sendsig (above).
 * Return to previous pc and psl as specified by
 * context left by sendsig. Check carefully to
 * make sure that the user has not modified the
 * state to gain improper privileges.
 *
 * MPSAFE
 */
int
sys_sigreturn(td, uap)
	struct thread *td;
	struct sigreturn_args /* {
		const struct __ucontext *sigcntxp;
	} */ *uap;
{
	ucontext_t uc;
	struct pcb *pcb;
	struct proc *p;
	struct trapframe *regs;
	ucontext_t *ucp;
	char *xfpustate;
	size_t xfpustate_len;
	long rflags;
	int cs, error, ret;
	ksiginfo_t ksi;

	pcb = td->td_pcb;
	p = td->td_proc;

	error = copyin(uap->sigcntxp, &uc, sizeof(uc));
	if (error != 0) {
		uprintf("pid %d (%s): sigreturn copyin failed\n",
		    p->p_pid, td->td_name);
		return (error);
	}
	ucp = &uc;
	if ((ucp->uc_mcontext.mc_flags & ~_MC_FLAG_MASK) != 0) {
		uprintf("pid %d (%s): sigreturn mc_flags %x\n", p->p_pid,
		    td->td_name, ucp->uc_mcontext.mc_flags);
		return (EINVAL);
	}
	regs = td->td_frame;
	rflags = ucp->uc_mcontext.mc_rflags;
	/*
	 * Don't allow users to change privileged or reserved flags.
	 */
	/*
	 * XXX do allow users to change the privileged flag PSL_RF.
	 * The cpu sets PSL_RF in tf_rflags for faults.  Debuggers
	 * should sometimes set it there too.  tf_rflags is kept in
	 * the signal context during signal handling and there is no
	 * other place to remember it, so the PSL_RF bit may be
	 * corrupted by the signal handler without us knowing.
	 * Corruption of the PSL_RF bit at worst causes one more or
	 * one less debugger trap, so allowing it is fairly harmless.
	 */
	if (!EFL_SECURE(rflags & ~PSL_RF, regs->tf_rflags & ~PSL_RF)) {
		uprintf("pid %d (%s): sigreturn rflags = 0x%lx\n", p->p_pid,
		    td->td_name, rflags);
		return (EINVAL);
	}

	/*
	 * Don't allow users to load a valid privileged %cs.  Let the
	 * hardware check for invalid selectors, excess privilege in
	 * other selectors, invalid %eip's and invalid %esp's.
	 */
	cs = ucp->uc_mcontext.mc_cs;
	if (!CS_SECURE(cs)) {
		uprintf("pid %d (%s): sigreturn cs = 0x%x\n", p->p_pid,
		    td->td_name, cs);
		ksiginfo_init_trap(&ksi);
		ksi.ksi_signo = SIGBUS;
		ksi.ksi_code = BUS_OBJERR;
		ksi.ksi_trapno = T_PROTFLT;
		ksi.ksi_addr = (void *)regs->tf_rip;
		trapsignal(td, &ksi);
		return (EINVAL);
	}

	if ((uc.uc_mcontext.mc_flags & _MC_HASFPXSTATE) != 0) {
		xfpustate_len = uc.uc_mcontext.mc_xfpustate_len;
		if (xfpustate_len > cpu_max_ext_state_size -
		    sizeof(struct savefpu)) {
			uprintf("pid %d (%s): sigreturn xfpusave_len = 0x%zx\n",
			    p->p_pid, td->td_name, xfpustate_len);
			return (EINVAL);
		}
		xfpustate = __builtin_alloca(xfpustate_len);
		error = copyin((const void *)uc.uc_mcontext.mc_xfpustate,
		    xfpustate, xfpustate_len);
		if (error != 0) {
			uprintf(
	"pid %d (%s): sigreturn copying xfpustate failed\n",
			    p->p_pid, td->td_name);
			return (error);
		}
	} else {
		xfpustate = NULL;
		xfpustate_len = 0;
	}
	ret = set_fpcontext(td, &ucp->uc_mcontext, xfpustate, xfpustate_len);
	if (ret != 0) {
		uprintf("pid %d (%s): sigreturn set_fpcontext err %d\n",
		    p->p_pid, td->td_name, ret);
		return (ret);
	}
	bcopy(&ucp->uc_mcontext.mc_rdi, regs, sizeof(*regs));
	pcb->pcb_fsbase = ucp->uc_mcontext.mc_fsbase;
	pcb->pcb_gsbase = ucp->uc_mcontext.mc_gsbase;

#if defined(COMPAT_43)
	if (ucp->uc_mcontext.mc_onstack & 1)
		td->td_sigstk.ss_flags |= SS_ONSTACK;
	else
		td->td_sigstk.ss_flags &= ~SS_ONSTACK;
#endif

	kern_sigprocmask(td, SIG_SETMASK, &ucp->uc_sigmask, NULL, 0);
	set_pcb_flags(pcb, PCB_FULL_IRET);
	return (EJUSTRETURN);
}

#ifdef COMPAT_FREEBSD4
int
freebsd4_sigreturn(struct thread *td, struct freebsd4_sigreturn_args *uap)
{
 
	return sys_sigreturn(td, (struct sigreturn_args *)uap);
}
#endif
