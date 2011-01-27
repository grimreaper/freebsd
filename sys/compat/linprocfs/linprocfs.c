/*-
 * Copyright (c) 2000 Dag-Erling Coïdan Smørgrav
 * Copyright (c) 1999 Pierre Beyssac
 * Copyright (c) 1993 Jan-Simon Pendry
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jan-Simon Pendry.
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
 *	@(#)procfs_status.c	8.4 (Berkeley) 6/15/94
 */

#include "opt_compat.h"

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/blist.h>
#include <sys/conf.h>
#include <sys/exec.h>
#include <sys/fcntl.h>
#include <sys/filedesc.h>
#include <sys/jail.h>
#include <sys/kernel.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/resourcevar.h>
#include <sys/sbuf.h>
#include <sys/sem.h>
#include <sys/smp.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/tty.h>
#include <sys/user.h>
#include <sys/vmmeter.h>
#include <sys/vnode.h>
#include <sys/bus.h>

#include <net/if.h>
#include <net/vnet.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_param.h>
#include <vm/vm_object.h>
#include <vm/swap_pager.h>

#include <machine/clock.h>

#include <geom/geom.h>
#include <geom/geom_int.h>

#if defined(__i386__) || defined(__amd64__)
#include <machine/cputypes.h>
#include <machine/md_var.h>
#endif /* __i386__ || __amd64__ */

#ifdef COMPAT_FREEBSD32
#include <compat/freebsd32/freebsd32_util.h>
#endif

#ifdef COMPAT_LINUX32				/* XXX */
#include <machine/../linux32/linux.h>
#else
#include <machine/../linux/linux.h>
#endif
#include <compat/linux/linux_ioctl.h>
#include <compat/linux/linux_mib.h>
#include <compat/linux/linux_util.h>
#include <fs/pseudofs/pseudofs.h>
#include <fs/procfs/procfs.h>

/*
 * Various conversion macros
 */
#define T2J(x) ((long)(((x) * 100ULL) / (stathz ? stathz : hz)))	/* ticks to jiffies */
#define T2CS(x) ((unsigned long)(((x) * 100ULL) / (stathz ? stathz : hz)))	/* ticks to centiseconds */
#define T2S(x) ((x) / (stathz ? stathz : hz))		/* ticks to seconds */
#define B2K(x) ((x) >> 10)				/* bytes to kbytes */
#define B2P(x) ((x) >> PAGE_SHIFT)			/* bytes to pages */
#define P2B(x) ((x) << PAGE_SHIFT)			/* pages to bytes */
#define P2K(x) ((x) << (PAGE_SHIFT - 10))		/* pages to kbytes */
#define TV2J(x)	((x)->tv_sec * 100UL + (x)->tv_usec / 10000)

/**
 * @brief Mapping of ki_stat in struct kinfo_proc to the linux state
 *
 * The linux procfs state field displays one of the characters RSDZTW to
 * denote running, sleeping in an interruptible wait, waiting in an
 * uninterruptible disk sleep, a zombie process, process is being traced
 * or stopped, or process is paging respectively.
 *
 * Our struct kinfo_proc contains the variable ki_stat which contains a
 * value out of SIDL, SRUN, SSLEEP, SSTOP, SZOMB, SWAIT and SLOCK.
 *
 * This character array is used with ki_stati-1 as an index and tries to
 * map our states to suitable linux states.
 */
static char linux_state[] = "RRSTZDD";

/*
 * Filler function for proc/meminfo
 */
static int
linprocfs_domeminfo(PFS_FILL_ARGS)
{
	unsigned long memtotal;		/* total memory in bytes */
	unsigned long memused;		/* used memory in bytes */
	unsigned long memfree;		/* free memory in bytes */
	unsigned long memshared;	/* shared memory ??? */
	unsigned long buffers, cached;	/* buffer / cache memory ??? */
	unsigned long long swaptotal;	/* total swap space in bytes */
	unsigned long long swapused;	/* used swap space in bytes */
	unsigned long long swapfree;	/* free swap space in bytes */
	vm_object_t object;
	int i, j;

	memtotal = physmem * PAGE_SIZE;
	/*
	 * The correct thing here would be:
	 *
	memfree = cnt.v_free_count * PAGE_SIZE;
	memused = memtotal - memfree;
	 *
	 * but it might mislead linux binaries into thinking there
	 * is very little memory left, so we cheat and tell them that
	 * all memory that isn't wired down is free.
	 */
	memused = cnt.v_wire_count * PAGE_SIZE;
	memfree = memtotal - memused;
	swap_pager_status(&i, &j);
	swaptotal = (unsigned long long)i * PAGE_SIZE;
	swapused = (unsigned long long)j * PAGE_SIZE;
	swapfree = swaptotal - swapused;
	memshared = 0;
	mtx_lock(&vm_object_list_mtx);
	TAILQ_FOREACH(object, &vm_object_list, object_list)
		if (object->shadow_count > 1)
			memshared += object->resident_page_count;
	mtx_unlock(&vm_object_list_mtx);
	memshared *= PAGE_SIZE;
	/*
	 * We'd love to be able to write:
	 *
	buffers = bufspace;
	 *
	 * but bufspace is internal to vfs_bio.c and we don't feel
	 * like unstaticizing it just for linprocfs's sake.
	 */
	buffers = 0;
	cached = cnt.v_cache_count * PAGE_SIZE;

	sbuf_printf(sb,
	    "	     total:    used:	free:  shared: buffers:	 cached:\n"
	    "Mem:  %lu %lu %lu %lu %lu %lu\n"
	    "Swap: %llu %llu %llu\n"
	    "MemTotal: %9lu kB\n"
	    "MemFree:  %9lu kB\n"
	    "MemShared:%9lu kB\n"
	    "Buffers:  %9lu kB\n"
	    "Cached:   %9lu kB\n"
	    "SwapTotal:%9llu kB\n"
	    "SwapFree: %9llu kB\n",
	    memtotal, memused, memfree, memshared, buffers, cached,
	    swaptotal, swapused, swapfree,
	    B2K(memtotal), B2K(memfree),
	    B2K(memshared), B2K(buffers), B2K(cached),
	    B2K(swaptotal), B2K(swapfree));

	return (0);
}

#if defined(__i386__) || defined(__amd64__)
/*
 * Filler function for proc/cpuinfo (i386 & amd64 version)
 */
static int
linprocfs_docpuinfo(PFS_FILL_ARGS)
{
	int hw_model[2];
	char model[128];
	size_t size;
	int class, fqmhz, fqkhz;
	int i;

	/*
	 * We default the flags to include all non-conflicting flags,
	 * and the Intel versions of conflicting flags.
	 */
	static char *flags[] = {
		"fpu",	    "vme",     "de",	   "pse",      "tsc",
		"msr",	    "pae",     "mce",	   "cx8",      "apic",
		"sep",	    "sep",     "mtrr",	   "pge",      "mca",
		"cmov",	    "pat",     "pse36",	   "pn",       "b19",
		"b20",	    "b21",     "mmxext",   "mmx",      "fxsr",
		"xmm",	    "sse2",    "b27",	   "b28",      "b29",
		"3dnowext", "3dnow"
	};

	switch (cpu_class) {
#ifdef __i386__
	case CPUCLASS_286:
		class = 2;
		break;
	case CPUCLASS_386:
		class = 3;
		break;
	case CPUCLASS_486:
		class = 4;
		break;
	case CPUCLASS_586:
		class = 5;
		break;
	case CPUCLASS_686:
		class = 6;
		break;
	default:
		class = 0;
		break;
#else /* __amd64__ */
	default:
		class = 15;
		break;
#endif
	}

	hw_model[0] = CTL_HW;
	hw_model[1] = HW_MODEL;
	model[0] = '\0';
	size = sizeof(model);
	if (kernel_sysctl(td, hw_model, 2, &model, &size, 0, 0, 0, 0) != 0)
		strcpy(model, "unknown");
	for (i = 0; i < mp_ncpus; ++i) {
		sbuf_printf(sb,
		    "processor\t: %d\n"
		    "vendor_id\t: %.20s\n"
		    "cpu family\t: %d\n"
		    "model\t\t: %d\n"
		    "model name\t: %s\n"
		    "stepping\t: %d\n",
		    i, cpu_vendor, class, cpu, model, cpu_id & 0xf);
		/* XXX per-cpu vendor / class / model / id? */
	}

	sbuf_cat(sb, "flags\t\t:");

#ifdef __i386__
	switch (cpu_vendor_id) {
	case CPU_VENDOR_AMD:
		if (class < 6)
			flags[16] = "fcmov";
		break;
	case CPU_VENDOR_CYRIX:
		flags[24] = "cxmmx";
		break;
	}
#endif

	for (i = 0; i < 32; i++)
		if (cpu_feature & (1 << i))
			sbuf_printf(sb, " %s", flags[i]);
	sbuf_cat(sb, "\n");
	if (class >= 5) {
		fqmhz = (tsc_freq + 4999) / 1000000;
		fqkhz = ((tsc_freq + 4999) / 10000) % 100;
		sbuf_printf(sb,
		    "cpu MHz\t\t: %d.%02d\n"
		    "bogomips\t: %d.%02d\n",
		    fqmhz, fqkhz, fqmhz, fqkhz);
	}

	return (0);
}
#endif /* __i386__ || __amd64__ */

/*
 * Filler function for proc/mtab
 *
 * This file doesn't exist in Linux' procfs, but is included here so
 * users can symlink /compat/linux/etc/mtab to /proc/mtab
 */
static int
linprocfs_domtab(PFS_FILL_ARGS)
{
	struct nameidata nd;
	struct mount *mp;
	const char *lep;
	char *dlep, *flep, *mntto, *mntfrom, *fstype;
	size_t lep_len;
	int error;

	/* resolve symlinks etc. in the emulation tree prefix */
	NDINIT(&nd, LOOKUP, FOLLOW | MPSAFE, UIO_SYSSPACE, linux_emul_path, td);
	flep = NULL;
	error = namei(&nd);
	lep = linux_emul_path;
	if (error == 0) {
		if (vn_fullpath(td, nd.ni_vp, &dlep, &flep) == 0)
			lep = dlep;
		vrele(nd.ni_vp);
		VFS_UNLOCK_GIANT(NDHASGIANT(&nd));
	}
	lep_len = strlen(lep);

	mtx_lock(&mountlist_mtx);
	error = 0;
	TAILQ_FOREACH(mp, &mountlist, mnt_list) {
		/* determine device name */
		mntfrom = mp->mnt_stat.f_mntfromname;

		/* determine mount point */
		mntto = mp->mnt_stat.f_mntonname;
		if (strncmp(mntto, lep, lep_len) == 0 &&
		    mntto[lep_len] == '/')
			mntto += lep_len;

		/* determine fs type */
		fstype = mp->mnt_stat.f_fstypename;
		if (strcmp(fstype, pn->pn_info->pi_name) == 0)
			mntfrom = fstype = "proc";
		else if (strcmp(fstype, "procfs") == 0)
			continue;

		if (strcmp(fstype, "linsysfs") == 0) {
			sbuf_printf(sb, "/sys %s sysfs %s", mntto,
			    mp->mnt_stat.f_flags & MNT_RDONLY ? "ro" : "rw");
		} else {
			/* For Linux msdosfs is called vfat */
			if (strcmp(fstype, "msdosfs") == 0)
				fstype = "vfat";
			sbuf_printf(sb, "%s %s %s %s", mntfrom, mntto, fstype,
			    mp->mnt_stat.f_flags & MNT_RDONLY ? "ro" : "rw");
		}
#define ADD_OPTION(opt, name) \
	if (mp->mnt_stat.f_flags & (opt)) sbuf_printf(sb, "," name);
		ADD_OPTION(MNT_SYNCHRONOUS,	"sync");
		ADD_OPTION(MNT_NOEXEC,		"noexec");
		ADD_OPTION(MNT_NOSUID,		"nosuid");
		ADD_OPTION(MNT_UNION,		"union");
		ADD_OPTION(MNT_ASYNC,		"async");
		ADD_OPTION(MNT_SUIDDIR,		"suiddir");
		ADD_OPTION(MNT_NOSYMFOLLOW,	"nosymfollow");
		ADD_OPTION(MNT_NOATIME,		"noatime");
#undef ADD_OPTION
		/* a real Linux mtab will also show NFS options */
		sbuf_printf(sb, " 0 0\n");
	}
	mtx_unlock(&mountlist_mtx);
	if (flep != NULL)
		free(flep, M_TEMP);
	return (error);
}

/*
 * Filler function for proc/partitions
 *
 */
static int
linprocfs_dopartitions(PFS_FILL_ARGS)
{
	struct g_class *cp;
	struct g_geom *gp;
	struct g_provider *pp;
	struct nameidata nd;
	const char *lep;
	char  *dlep, *flep;
	size_t lep_len;
	int error;
	int major, minor;

	/* resolve symlinks etc. in the emulation tree prefix */
	NDINIT(&nd, LOOKUP, FOLLOW | MPSAFE, UIO_SYSSPACE, linux_emul_path, td);
	flep = NULL;
	error = namei(&nd);
	lep = linux_emul_path;
	if (error == 0) {
		if (vn_fullpath(td, nd.ni_vp, &dlep, &flep) == 0)
			lep = dlep;
		vrele(nd.ni_vp);
		VFS_UNLOCK_GIANT(NDHASGIANT(&nd));
	}
	lep_len = strlen(lep);

	g_topology_lock();
	error = 0;
	sbuf_printf(sb, "major minor  #blocks  name rio rmerge rsect "
	    "ruse wio wmerge wsect wuse running use aveq\n");

	LIST_FOREACH(cp, &g_classes, class) {
		if (strcmp(cp->name, "DISK") == 0 ||
		    strcmp(cp->name, "PART") == 0)
			LIST_FOREACH(gp, &cp->geom, geom) {
				LIST_FOREACH(pp, &gp->provider, provider) {
					if (linux_driver_get_major_minor(
					    pp->name, &major, &minor) != 0) {
						major = 0;
						minor = 0;
					}
					sbuf_printf(sb, "%d %d %lld %s "
					    "%d %d %d %d %d "
					     "%d %d %d %d %d %d\n",
					     major, minor,
					     (long long)pp->mediasize, pp->name,
					     0, 0, 0, 0, 0,
					     0, 0, 0, 0, 0, 0);
				}
			}
	}
	g_topology_unlock();

	if (flep != NULL)
		free(flep, M_TEMP);
	return (error);
}


/*
 * Filler function for proc/stat
 */
static int
linprocfs_dostat(PFS_FILL_ARGS)
{
	struct pcpu *pcpu;
	long cp_time[CPUSTATES];
	long *cp;
	int i;

	read_cpu_time(cp_time);
	sbuf_printf(sb, "cpu %ld %ld %ld %ld\n",
	    T2J(cp_time[CP_USER]),
	    T2J(cp_time[CP_NICE]),
	    T2J(cp_time[CP_SYS] /*+ cp_time[CP_INTR]*/),
	    T2J(cp_time[CP_IDLE]));
	CPU_FOREACH(i) {
		pcpu = pcpu_find(i);
		cp = pcpu->pc_cp_time;
		sbuf_printf(sb, "cpu%d %ld %ld %ld %ld\n", i,
		    T2J(cp[CP_USER]),
		    T2J(cp[CP_NICE]),
		    T2J(cp[CP_SYS] /*+ cp[CP_INTR]*/),
		    T2J(cp[CP_IDLE]));
	}
	sbuf_printf(sb,
	    "disk 0 0 0 0\n"
	    "page %u %u\n"
	    "swap %u %u\n"
	    "intr %u\n"
	    "ctxt %u\n"
	    "btime %lld\n",
	    cnt.v_vnodepgsin,
	    cnt.v_vnodepgsout,
	    cnt.v_swappgsin,
	    cnt.v_swappgsout,
	    cnt.v_intr,
	    cnt.v_swtch,
	    (long long)boottime.tv_sec);
	return (0);
}

/*
 * Filler function for proc/uptime
 */
static int
linprocfs_douptime(PFS_FILL_ARGS)
{
	long cp_time[CPUSTATES];
	struct timeval tv;

	getmicrouptime(&tv);
	read_cpu_time(cp_time);
	sbuf_printf(sb, "%lld.%02ld %ld.%02lu\n",
	    (long long)tv.tv_sec, tv.tv_usec / 10000,
	    T2S(cp_time[CP_IDLE] / mp_ncpus),
	    T2CS(cp_time[CP_IDLE] / mp_ncpus) % 100);
	return (0);
}

/*
 * Get OS build date
 */
static void
linprocfs_osbuild(struct thread *td, struct sbuf *sb)
{
#if 0
	char osbuild[256];
	char *cp1, *cp2;

	strncpy(osbuild, version, 256);
	osbuild[255] = '\0';
	cp1 = strstr(osbuild, "\n");
	cp2 = strstr(osbuild, ":");
	if (cp1 && cp2) {
		*cp1 = *cp2 = '\0';
		cp1 = strstr(osbuild, "#");
	} else
		cp1 = NULL;
	if (cp1)
		sbuf_printf(sb, "%s%s", cp1, cp2 + 1);
	else
#endif
		sbuf_cat(sb, "#4 Sun Dec 18 04:30:00 CET 1977");
}

/*
 * Get OS builder
 */
static void
linprocfs_osbuilder(struct thread *td, struct sbuf *sb)
{
#if 0
	char builder[256];
	char *cp;

	cp = strstr(version, "\n    ");
	if (cp) {
		strncpy(builder, cp + 5, 256);
		builder[255] = '\0';
		cp = strstr(builder, ":");
		if (cp)
			*cp = '\0';
	}
	if (cp)
		sbuf_cat(sb, builder);
	else
#endif
		sbuf_cat(sb, "des@freebsd.org");
}

/*
 * Filler function for proc/version
 */
static int
linprocfs_doversion(PFS_FILL_ARGS)
{
	char osname[LINUX_MAX_UTSNAME];
	char osrelease[LINUX_MAX_UTSNAME];

	linux_get_osname(td, osname);
	linux_get_osrelease(td, osrelease);
	sbuf_printf(sb, "%s version %s (", osname, osrelease);
	linprocfs_osbuilder(td, sb);
	sbuf_cat(sb, ") (gcc version " __VERSION__ ") ");
	linprocfs_osbuild(td, sb);
	sbuf_cat(sb, "\n");

	return (0);
}

/*
 * Filler function for proc/loadavg
 */
static int
linprocfs_doloadavg(PFS_FILL_ARGS)
{

	sbuf_printf(sb,
	    "%d.%02d %d.%02d %d.%02d %d/%d %d\n",
	    (int)(averunnable.ldavg[0] / averunnable.fscale),
	    (int)(averunnable.ldavg[0] * 100 / averunnable.fscale % 100),
	    (int)(averunnable.ldavg[1] / averunnable.fscale),
	    (int)(averunnable.ldavg[1] * 100 / averunnable.fscale % 100),
	    (int)(averunnable.ldavg[2] / averunnable.fscale),
	    (int)(averunnable.ldavg[2] * 100 / averunnable.fscale % 100),
	    1,				/* number of running tasks */
	    nprocs,			/* number of tasks */
	    lastpid			/* the last pid */
	);
	return (0);
}

/*
 * Filler function for proc/pid/stat
 */
static int
linprocfs_doprocstat(PFS_FILL_ARGS)
{
	struct kinfo_proc kp;
	char state;
	static int ratelimit = 0;
	vm_offset_t startcode, startdata;

	PROC_LOCK(p);
	fill_kinfo_proc(p, &kp);
	if (p->p_vmspace) {
	   startcode = (vm_offset_t)p->p_vmspace->vm_taddr;
	   startdata = (vm_offset_t)p->p_vmspace->vm_daddr;
	} else {
	   startcode = 0;
	   startdata = 0;
	};
	sbuf_printf(sb, "%d", p->p_pid);
#define PS_ADD(name, fmt, arg) sbuf_printf(sb, " " fmt, arg)
	PS_ADD("comm",		"(%s)",	p->p_comm);
	if (kp.ki_stat > sizeof(linux_state)) {
		state = 'R';

		if (ratelimit == 0) {
			printf("linprocfs: don't know how to handle unknown FreeBSD state %d/%zd, mapping to R\n",
			    kp.ki_stat, sizeof(linux_state));
			++ratelimit;
		}
	} else
		state = linux_state[kp.ki_stat - 1];
	PS_ADD("state",		"%c",	state);
	PS_ADD("ppid",		"%d",	p->p_pptr ? p->p_pptr->p_pid : 0);
	PS_ADD("pgrp",		"%d",	p->p_pgid);
	PS_ADD("session",	"%d",	p->p_session->s_sid);
	PROC_UNLOCK(p);
	PS_ADD("tty",		"%d",	kp.ki_tdev);
	PS_ADD("tpgid",		"%d",	kp.ki_tpgid);
	PS_ADD("flags",		"%u",	0); /* XXX */
	PS_ADD("minflt",	"%lu",	kp.ki_rusage.ru_minflt);
	PS_ADD("cminflt",	"%lu",	kp.ki_rusage_ch.ru_minflt);
	PS_ADD("majflt",	"%lu",	kp.ki_rusage.ru_majflt);
	PS_ADD("cmajflt",	"%lu",	kp.ki_rusage_ch.ru_majflt);
	PS_ADD("utime",		"%ld",	TV2J(&kp.ki_rusage.ru_utime));
	PS_ADD("stime",		"%ld",	TV2J(&kp.ki_rusage.ru_stime));
	PS_ADD("cutime",	"%ld",	TV2J(&kp.ki_rusage_ch.ru_utime));
	PS_ADD("cstime",	"%ld",	TV2J(&kp.ki_rusage_ch.ru_stime));
	PS_ADD("priority",	"%d",	kp.ki_pri.pri_user);
	PS_ADD("nice",		"%d",	kp.ki_nice); /* 19 (nicest) to -19 */
	PS_ADD("0",		"%d",	0); /* removed field */
	PS_ADD("itrealvalue",	"%d",	0); /* XXX */
	PS_ADD("starttime",	"%lu",	TV2J(&kp.ki_start) - TV2J(&boottime));
	PS_ADD("vsize",		"%ju",	P2K((uintmax_t)kp.ki_size));
	PS_ADD("rss",		"%ju",	(uintmax_t)kp.ki_rssize);
	PS_ADD("rlim",		"%lu",	kp.ki_rusage.ru_maxrss);
	PS_ADD("startcode",	"%ju",	(uintmax_t)startcode);
	PS_ADD("endcode",	"%ju",	(uintmax_t)startdata);
	PS_ADD("startstack",	"%u",	0); /* XXX */
	PS_ADD("kstkesp",	"%u",	0); /* XXX */
	PS_ADD("kstkeip",	"%u",	0); /* XXX */
	PS_ADD("signal",	"%u",	0); /* XXX */
	PS_ADD("blocked",	"%u",	0); /* XXX */
	PS_ADD("sigignore",	"%u",	0); /* XXX */
	PS_ADD("sigcatch",	"%u",	0); /* XXX */
	PS_ADD("wchan",		"%u",	0); /* XXX */
	PS_ADD("nswap",		"%lu",	kp.ki_rusage.ru_nswap);
	PS_ADD("cnswap",	"%lu",	kp.ki_rusage_ch.ru_nswap);
	PS_ADD("exitsignal",	"%d",	0); /* XXX */
	PS_ADD("processor",	"%u",	kp.ki_lastcpu);
	PS_ADD("rt_priority",	"%u",	0); /* XXX */ /* >= 2.5.19 */
	PS_ADD("policy",	"%u",	kp.ki_pri.pri_class); /* >= 2.5.19 */
#undef PS_ADD
	sbuf_putc(sb, '\n');

	return (0);
}

/*
 * Filler function for proc/pid/statm
 */
static int
linprocfs_doprocstatm(PFS_FILL_ARGS)
{
	struct kinfo_proc kp;
	segsz_t lsize;

	PROC_LOCK(p);
	fill_kinfo_proc(p, &kp);
	PROC_UNLOCK(p);

	/*
	 * See comments in linprocfs_doprocstatus() regarding the
	 * computation of lsize.
	 */
	/* size resident share trs drs lrs dt */
	sbuf_printf(sb, "%ju ", B2P((uintmax_t)kp.ki_size));
	sbuf_printf(sb, "%ju ", (uintmax_t)kp.ki_rssize);
	sbuf_printf(sb, "%ju ", (uintmax_t)0); /* XXX */
	sbuf_printf(sb, "%ju ",	(uintmax_t)kp.ki_tsize);
	sbuf_printf(sb, "%ju ", (uintmax_t)(kp.ki_dsize + kp.ki_ssize));
	lsize = B2P(kp.ki_size) - kp.ki_dsize -
	    kp.ki_ssize - kp.ki_tsize - 1;
	sbuf_printf(sb, "%ju ", (uintmax_t)lsize);
	sbuf_printf(sb, "%ju\n", (uintmax_t)0); /* XXX */

	return (0);
}

/*
 * Filler function for proc/pid/status
 */
static int
linprocfs_doprocstatus(PFS_FILL_ARGS)
{
	struct kinfo_proc kp;
	char *state;
	segsz_t lsize;
	struct thread *td2;
	struct sigacts *ps;
	int i;

	PROC_LOCK(p);
	td2 = FIRST_THREAD_IN_PROC(p); /* XXXKSE pretend only one thread */

	if (P_SHOULDSTOP(p)) {
		state = "T (stopped)";
	} else {
		PROC_SLOCK(p);
		switch(p->p_state) {
		case PRS_NEW:
			state = "I (idle)";
			break;
		case PRS_NORMAL:
			if (p->p_flag & P_WEXIT) {
				state = "X (exiting)";
				break;
			}
			switch(td2->td_state) {
			case TDS_INHIBITED:
				state = "S (sleeping)";
				break;
			case TDS_RUNQ:
			case TDS_RUNNING:
				state = "R (running)";
				break;
			default:
				state = "? (unknown)";
				break;
			}
			break;
		case PRS_ZOMBIE:
			state = "Z (zombie)";
			break;
		default:
			state = "? (unknown)";
			break;
		}
		PROC_SUNLOCK(p);
	}

	fill_kinfo_proc(p, &kp);
	sbuf_printf(sb, "Name:\t%s\n",		p->p_comm); /* XXX escape */
	sbuf_printf(sb, "State:\t%s\n",		state);

	/*
	 * Credentials
	 */
	sbuf_printf(sb, "Pid:\t%d\n",		p->p_pid);
	sbuf_printf(sb, "PPid:\t%d\n",		p->p_pptr ?
						p->p_pptr->p_pid : 0);
	sbuf_printf(sb, "Uid:\t%d %d %d %d\n",	p->p_ucred->cr_ruid,
						p->p_ucred->cr_uid,
						p->p_ucred->cr_svuid,
						/* FreeBSD doesn't have fsuid */
						p->p_ucred->cr_uid);
	sbuf_printf(sb, "Gid:\t%d %d %d %d\n",	p->p_ucred->cr_rgid,
						p->p_ucred->cr_gid,
						p->p_ucred->cr_svgid,
						/* FreeBSD doesn't have fsgid */
						p->p_ucred->cr_gid);
	sbuf_cat(sb, "Groups:\t");
	for (i = 0; i < p->p_ucred->cr_ngroups; i++)
		sbuf_printf(sb, "%d ",		p->p_ucred->cr_groups[i]);
	PROC_UNLOCK(p);
	sbuf_putc(sb, '\n');

	/*
	 * Memory
	 *
	 * While our approximation of VmLib may not be accurate (I
	 * don't know of a simple way to verify it, and I'm not sure
	 * it has much meaning anyway), I believe it's good enough.
	 *
	 * The same code that could (I think) accurately compute VmLib
	 * could also compute VmLck, but I don't really care enough to
	 * implement it. Submissions are welcome.
	 */
	sbuf_printf(sb, "VmSize:\t%8ju kB\n",	B2K((uintmax_t)kp.ki_size));
	sbuf_printf(sb, "VmLck:\t%8u kB\n",	P2K(0)); /* XXX */
	sbuf_printf(sb, "VmRSS:\t%8ju kB\n",	P2K((uintmax_t)kp.ki_rssize));
	sbuf_printf(sb, "VmData:\t%8ju kB\n",	P2K((uintmax_t)kp.ki_dsize));
	sbuf_printf(sb, "VmStk:\t%8ju kB\n",	P2K((uintmax_t)kp.ki_ssize));
	sbuf_printf(sb, "VmExe:\t%8ju kB\n",	P2K((uintmax_t)kp.ki_tsize));
	lsize = B2P(kp.ki_size) - kp.ki_dsize -
	    kp.ki_ssize - kp.ki_tsize - 1;
	sbuf_printf(sb, "VmLib:\t%8ju kB\n",	P2K((uintmax_t)lsize));

	/*
	 * Signal masks
	 *
	 * We support up to 128 signals, while Linux supports 32,
	 * but we only define 32 (the same 32 as Linux, to boot), so
	 * just show the lower 32 bits of each mask. XXX hack.
	 *
	 * NB: on certain platforms (Sparc at least) Linux actually
	 * supports 64 signals, but this code is a long way from
	 * running on anything but i386, so ignore that for now.
	 */
	PROC_LOCK(p);
	sbuf_printf(sb, "SigPnd:\t%08x\n",	p->p_siglist.__bits[0]);
	/*
	 * I can't seem to find out where the signal mask is in
	 * relation to struct proc, so SigBlk is left unimplemented.
	 */
	sbuf_printf(sb, "SigBlk:\t%08x\n",	0); /* XXX */
	ps = p->p_sigacts;
	mtx_lock(&ps->ps_mtx);
	sbuf_printf(sb, "SigIgn:\t%08x\n",	ps->ps_sigignore.__bits[0]);
	sbuf_printf(sb, "SigCgt:\t%08x\n",	ps->ps_sigcatch.__bits[0]);
	mtx_unlock(&ps->ps_mtx);
	PROC_UNLOCK(p);

	/*
	 * Linux also prints the capability masks, but we don't have
	 * capabilities yet, and when we do get them they're likely to
	 * be meaningless to Linux programs, so we lie. XXX
	 */
	sbuf_printf(sb, "CapInh:\t%016x\n",	0);
	sbuf_printf(sb, "CapPrm:\t%016x\n",	0);
	sbuf_printf(sb, "CapEff:\t%016x\n",	0);

	return (0);
}


/*
 * Filler function for proc/pid/cwd
 */
static int
linprocfs_doproccwd(PFS_FILL_ARGS)
{
	char *fullpath = "unknown";
	char *freepath = NULL;

	vn_fullpath(td, p->p_fd->fd_cdir, &fullpath, &freepath);
	sbuf_printf(sb, "%s", fullpath);
	if (freepath)
		free(freepath, M_TEMP);
	return (0);
}

/*
 * Filler function for proc/pid/root
 */
static int
linprocfs_doprocroot(PFS_FILL_ARGS)
{
	struct vnode *rvp;
	char *fullpath = "unknown";
	char *freepath = NULL;

	rvp = jailed(p->p_ucred) ? p->p_fd->fd_jdir : p->p_fd->fd_rdir;
	vn_fullpath(td, rvp, &fullpath, &freepath);
	sbuf_printf(sb, "%s", fullpath);
	if (freepath)
		free(freepath, M_TEMP);
	return (0);
}

#define MAX_ARGV_STR	512	/* Max number of argv-like strings */
#define UIO_CHUNK_SZ	256	/* Max chunk size (bytes) for uiomove */

static int
linprocfs_doargv(struct thread *td, struct proc *p, struct sbuf *sb,
    void (*resolver)(const struct ps_strings, u_long *, int *))
{
	struct iovec iov;
	struct uio tmp_uio;
	struct ps_strings pss;
	int ret, i, n_elements, elm_len;
	u_long addr, pbegin;
	char **env_vector, *envp;
	char env_string[UIO_CHUNK_SZ];
#ifdef COMPAT_FREEBSD32
	struct freebsd32_ps_strings pss32;
	uint32_t *env_vector32;
#endif

#define	UIO_HELPER(uio, iov, base, len, cnt, offset, sz, flg, rw, td)	\
do {									\
	iov.iov_base = (caddr_t)(base);					\
	iov.iov_len = (len); 						\
	uio.uio_iov = &(iov); 						\
	uio.uio_iovcnt = (cnt);	 					\
	uio.uio_offset = (off_t)(offset);				\
	uio.uio_resid = (sz); 						\
	uio.uio_segflg = (flg);						\
	uio.uio_rw = (rw); 						\
	uio.uio_td = (td);						\
} while (0)

	env_vector = malloc(sizeof(char *) * MAX_ARGV_STR, M_TEMP, M_WAITOK);

#ifdef COMPAT_FREEBSD32
	env_vector32 = NULL;
	if ((p->p_sysent->sv_flags & SV_ILP32) != 0) {
		env_vector32 = malloc(sizeof(*env_vector32) * MAX_ARGV_STR,
		    M_TEMP, M_WAITOK);
		elm_len = sizeof(int32_t);
		envp = (char *)env_vector32;

		UIO_HELPER(tmp_uio, iov, &pss32, sizeof(pss32), 1,
		    (off_t)(p->p_sysent->sv_psstrings),
		    sizeof(pss32), UIO_SYSSPACE, UIO_READ, td);
		ret = proc_rwmem(p, &tmp_uio);
		if (ret != 0)
			goto done;
		pss.ps_argvstr = PTRIN(pss32.ps_argvstr);
		pss.ps_nargvstr = pss32.ps_nargvstr;
		pss.ps_envstr = PTRIN(pss32.ps_envstr);
		pss.ps_nenvstr = pss32.ps_nenvstr;
	} else {
#endif
		elm_len = sizeof(char *);
		envp = (char *)env_vector;

		UIO_HELPER(tmp_uio, iov, &pss, sizeof(pss), 1,
		    (off_t)(p->p_sysent->sv_psstrings),
		    sizeof(pss), UIO_SYSSPACE, UIO_READ, td);
		ret = proc_rwmem(p, &tmp_uio);
		if (ret != 0)
			goto done;
#ifdef COMPAT_FREEBSD32
	}
#endif

	/* Get the array address and the number of elements */
	resolver(pss, &addr, &n_elements);

	/* Consistent with lib/libkvm/kvm_proc.c */
	if (n_elements > MAX_ARGV_STR) {
		ret = E2BIG;
		goto done;
	}

	UIO_HELPER(tmp_uio, iov, envp, n_elements * elm_len, 1,
	    (vm_offset_t)(addr), iov.iov_len, UIO_SYSSPACE, UIO_READ, td);
	ret = proc_rwmem(p, &tmp_uio);
	if (ret != 0)
		goto done;
#ifdef COMPAT_FREEBSD32
	if (env_vector32 != NULL) {
		for (i = 0; i < n_elements; i++)
			env_vector[i] = PTRIN(env_vector32[i]);
	}
#endif

	/* Now we can iterate through the list of strings */
	for (i = 0; i < n_elements; i++) {
		pbegin = (vm_offset_t)env_vector[i];
		for (;;) {
			UIO_HELPER(tmp_uio, iov, env_string, sizeof(env_string),
			    1, pbegin, iov.iov_len, UIO_SYSSPACE, UIO_READ, td);
			ret = proc_rwmem(p, &tmp_uio);
			if (ret != 0)
				goto done;

			if (!strvalid(env_string, UIO_CHUNK_SZ)) {
				/*
				 * We didn't find the end of the string.
				 * Add the string to the buffer and move
				 * the pointer.  But do not allow strings
				 * of unlimited length.
				 */
				sbuf_bcat(sb, env_string, UIO_CHUNK_SZ);
				if (sbuf_len(sb) >= ARG_MAX) {
					ret = E2BIG;
					goto done;
				}
				pbegin += UIO_CHUNK_SZ;
			} else {
				sbuf_cat(sb, env_string);
				break;
			}
		}
		sbuf_bcat(sb, "", 1);
	}
#undef UIO_HELPER

done:
	free(env_vector, M_TEMP);
#ifdef COMPAT_FREEBSD32
	free(env_vector32, M_TEMP);
#endif
	return (ret);
}

static void
ps_string_argv(const struct ps_strings ps, u_long *addr, int *n)
{

	*addr = (u_long) ps.ps_argvstr;
	*n = ps.ps_nargvstr;
}

static void
ps_string_env(const struct ps_strings ps, u_long *addr, int *n)
{

	*addr = (u_long) ps.ps_envstr;
	*n = ps.ps_nenvstr;
}

/*
 * Filler function for proc/pid/cmdline
 */
static int
linprocfs_doproccmdline(PFS_FILL_ARGS)
{
	int ret;

	PROC_LOCK(p);
	if ((ret = p_cansee(td, p)) != 0) {
		PROC_UNLOCK(p);
		return (ret);
	}
	if (p->p_args != NULL) {
		sbuf_bcpy(sb, p->p_args->ar_args, p->p_args->ar_length);
		PROC_UNLOCK(p);
		return (0);
	}
	PROC_UNLOCK(p);

	ret = linprocfs_doargv(td, p, sb, ps_string_argv);
	return (ret);
}

/*
 * Filler function for proc/pid/environ
 */
static int
linprocfs_doprocenviron(PFS_FILL_ARGS)
{
	int ret;

	PROC_LOCK(p);
	if ((ret = p_cansee(td, p)) != 0) {
		PROC_UNLOCK(p);
		return (ret);
	}
	PROC_UNLOCK(p);

	ret = linprocfs_doargv(td, p, sb, ps_string_env);
	return (ret);
}

/*
 * Filler function for proc/pid/maps
 */
static int
linprocfs_doprocmaps(PFS_FILL_ARGS)
{
	struct vmspace *vm;
	vm_map_t map;
	vm_map_entry_t entry, tmp_entry;
	vm_object_t obj, tobj, lobj;
	vm_offset_t e_start, e_end;
	vm_ooffset_t off = 0;
	vm_prot_t e_prot;
	unsigned int last_timestamp;
	char *name = "", *freename = NULL;
	ino_t ino;
	int ref_count, shadow_count, flags;
	int error;
	struct vnode *vp;
	struct vattr vat;
	int locked;

	PROC_LOCK(p);
	error = p_candebug(td, p);
	PROC_UNLOCK(p);
	if (error)
		return (error);

	if (uio->uio_rw != UIO_READ)
		return (EOPNOTSUPP);

	error = 0;
	vm = vmspace_acquire_ref(p);
	if (vm == NULL)
		return (ESRCH);
	map = &vm->vm_map;
	vm_map_lock_read(map);
	for (entry = map->header.next; entry != &map->header;
	    entry = entry->next) {
		name = "";
		freename = NULL;
		if (entry->eflags & MAP_ENTRY_IS_SUB_MAP)
			continue;
		e_prot = entry->protection;
		e_start = entry->start;
		e_end = entry->end;
		obj = entry->object.vm_object;
		for (lobj = tobj = obj; tobj; tobj = tobj->backing_object) {
			VM_OBJECT_LOCK(tobj);
			if (lobj != obj)
				VM_OBJECT_UNLOCK(lobj);
			lobj = tobj;
		}
		last_timestamp = map->timestamp;
		vm_map_unlock_read(map);
		ino = 0;
		if (lobj) {
			off = IDX_TO_OFF(lobj->size);
			if (lobj->type == OBJT_VNODE) {
				vp = lobj->handle;
				if (vp)
					vref(vp);
			}
			else
				vp = NULL;
			if (lobj != obj)
				VM_OBJECT_UNLOCK(lobj);
			flags = obj->flags;
			ref_count = obj->ref_count;
			shadow_count = obj->shadow_count;
			VM_OBJECT_UNLOCK(obj);
			if (vp) {
				vn_fullpath(td, vp, &name, &freename);
				locked = VFS_LOCK_GIANT(vp->v_mount);
				vn_lock(vp, LK_SHARED | LK_RETRY);
				VOP_GETATTR(vp, &vat, td->td_ucred);
				ino = vat.va_fileid;
				vput(vp);
				VFS_UNLOCK_GIANT(locked);
			}
		} else {
			flags = 0;
			ref_count = 0;
			shadow_count = 0;
		}

		/*
		 * format:
		 *  start, end, access, offset, major, minor, inode, name.
		 */
		error = sbuf_printf(sb,
		    "%08lx-%08lx %s%s%s%s %08lx %02x:%02x %lu%s%s\n",
		    (u_long)e_start, (u_long)e_end,
		    (e_prot & VM_PROT_READ)?"r":"-",
		    (e_prot & VM_PROT_WRITE)?"w":"-",
		    (e_prot & VM_PROT_EXECUTE)?"x":"-",
		    "p",
		    (u_long)off,
		    0,
		    0,
		    (u_long)ino,
		    *name ? "     " : "",
		    name
		    );
		if (freename)
			free(freename, M_TEMP);
		vm_map_lock_read(map);
		if (error == -1) {
			error = 0;
			break;
		}
		if (last_timestamp != map->timestamp) {
			/*
			 * Look again for the entry because the map was
			 * modified while it was unlocked.  Specifically,
			 * the entry may have been clipped, merged, or deleted.
			 */
			vm_map_lookup_entry(map, e_end - 1, &tmp_entry);
			entry = tmp_entry;
		}
	}
	vm_map_unlock_read(map);
	vmspace_free(vm);

	return (error);
}

/*
 * Filler function for proc/net/dev
 */
static int
linprocfs_donetdev(PFS_FILL_ARGS)
{
	char ifname[16]; /* XXX LINUX_IFNAMSIZ */
	struct ifnet *ifp;

	sbuf_printf(sb, "%6s|%58s|%s\n%6s|%58s|%58s\n",
	    "Inter-", "   Receive", "  Transmit", " face",
	    "bytes    packets errs drop fifo frame compressed",
	    "bytes    packets errs drop fifo frame compressed");

	CURVNET_SET(TD_TO_VNET(curthread));
	IFNET_RLOCK();
	TAILQ_FOREACH(ifp, &V_ifnet, if_link) {
		linux_ifname(ifp, ifname, sizeof ifname);
			sbuf_printf(sb, "%6.6s:", ifname);
		sbuf_printf(sb, "%8lu %7lu %4lu %4lu %4lu %5lu %10lu %9lu ",
		    0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
		sbuf_printf(sb, "%8lu %7lu %4lu %4lu %4lu %5lu %7lu %10lu\n",
		    0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
	}
	IFNET_RUNLOCK();
	CURVNET_RESTORE();

	return (0);
}

/*
 * Filler function for proc/sys/kernel/osrelease
 */
static int
linprocfs_doosrelease(PFS_FILL_ARGS)
{
	char osrelease[LINUX_MAX_UTSNAME];

	linux_get_osrelease(td, osrelease);
	sbuf_printf(sb, "%s\n", osrelease);

	return (0);
}

/*
 * Filler function for proc/sys/kernel/ostype
 */
static int
linprocfs_doostype(PFS_FILL_ARGS)
{
	char osname[LINUX_MAX_UTSNAME];

	linux_get_osname(td, osname);
	sbuf_printf(sb, "%s\n", osname);

	return (0);
}

/*
 * Filler function for proc/sys/kernel/version
 */
static int
linprocfs_doosbuild(PFS_FILL_ARGS)
{

	linprocfs_osbuild(td, sb);
	sbuf_cat(sb, "\n");
	return (0);
}

/*
 * Filler function for proc/sys/kernel/msgmni
 */
static int
linprocfs_domsgmni(PFS_FILL_ARGS)
{

	sbuf_printf(sb, "%d\n", msginfo.msgmni);
	return (0);
}

/*
 * Filler function for proc/sys/kernel/pid_max
 */
static int
linprocfs_dopid_max(PFS_FILL_ARGS)
{

	sbuf_printf(sb, "%i\n", PID_MAX);
	return (0);
}

/*
 * Filler function for proc/sys/kernel/sem
 */
static int
linprocfs_dosem(PFS_FILL_ARGS)
{

	sbuf_printf(sb, "%d %d %d %d\n", seminfo.semmsl, seminfo.semmns,
	    seminfo.semopm, seminfo.semmni);
	return (0);
}

/*
 * Filler function for proc/scsi/device_info
 */
static int
linprocfs_doscsidevinfo(PFS_FILL_ARGS)
{

	return (0);
}

/*
 * Filler function for proc/scsi/scsi
 */
static int
linprocfs_doscsiscsi(PFS_FILL_ARGS)
{

	return (0);
}

extern struct cdevsw *cdevsw[];

/*
 * Filler function for proc/devices
 */
static int
linprocfs_dodevices(PFS_FILL_ARGS)
{
	char *char_devices;
	sbuf_printf(sb, "Character devices:\n");

	char_devices = linux_get_char_devices();
	sbuf_printf(sb, "%s", char_devices);
	linux_free_get_char_devices(char_devices);

	sbuf_printf(sb, "\nBlock devices:\n");

	return (0);
}

/*
 * Filler function for proc/cmdline
 */
static int
linprocfs_docmdline(PFS_FILL_ARGS)
{

	sbuf_printf(sb, "BOOT_IMAGE=%s", kernelname);
	sbuf_printf(sb, " ro root=302\n");
	return (0);
}

#if 0
/*
 * Filler function for proc/modules
 */
static int
linprocfs_domodules(PFS_FILL_ARGS)
{
	struct linker_file *lf;

	TAILQ_FOREACH(lf, &linker_files, link) {
		sbuf_printf(sb, "%-20s%8lu%4d\n", lf->filename,
		    (unsigned long)lf->size, lf->refs);
	}
	return (0);
}
#endif

/*
 * Constructor
 */
static int
linprocfs_init(PFS_INIT_ARGS)
{
	struct pfs_node *root;
	struct pfs_node *dir;

	root = pi->pi_root;

	/* /proc/... */
	pfs_create_file(root, "cmdline", &linprocfs_docmdline,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "cpuinfo", &linprocfs_docpuinfo,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "devices", &linprocfs_dodevices,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "loadavg", &linprocfs_doloadavg,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "meminfo", &linprocfs_domeminfo,
	    NULL, NULL, NULL, PFS_RD);
#if 0
	pfs_create_file(root, "modules", &linprocfs_domodules,
	    NULL, NULL, NULL, PFS_RD);
#endif
	pfs_create_file(root, "mounts", &linprocfs_domtab,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "mtab", &linprocfs_domtab,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "partitions", &linprocfs_dopartitions,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_link(root, "self", &procfs_docurproc,
	    NULL, NULL, NULL, 0);
	pfs_create_file(root, "stat", &linprocfs_dostat,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "uptime", &linprocfs_douptime,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(root, "version", &linprocfs_doversion,
	    NULL, NULL, NULL, PFS_RD);

	/* /proc/net/... */
	dir = pfs_create_dir(root, "net", NULL, NULL, NULL, 0);
	pfs_create_file(dir, "dev", &linprocfs_donetdev,
	    NULL, NULL, NULL, PFS_RD);

	/* /proc/<pid>/... */
	dir = pfs_create_dir(root, "pid", NULL, NULL, NULL, PFS_PROCDEP);
	pfs_create_file(dir, "cmdline", &linprocfs_doproccmdline,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_link(dir, "cwd", &linprocfs_doproccwd,
	    NULL, NULL, NULL, 0);
	pfs_create_file(dir, "environ", &linprocfs_doprocenviron,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_link(dir, "exe", &procfs_doprocfile,
	    NULL, &procfs_notsystem, NULL, 0);
	pfs_create_file(dir, "maps", &linprocfs_doprocmaps,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "mem", &procfs_doprocmem,
	    &procfs_attr, &procfs_candebug, NULL, PFS_RDWR|PFS_RAW);
	pfs_create_link(dir, "root", &linprocfs_doprocroot,
	    NULL, NULL, NULL, 0);
	pfs_create_file(dir, "stat", &linprocfs_doprocstat,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "statm", &linprocfs_doprocstatm,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "status", &linprocfs_doprocstatus,
	    NULL, NULL, NULL, PFS_RD);

	/* /proc/scsi/... */
	dir = pfs_create_dir(root, "scsi", NULL, NULL, NULL, 0);
	pfs_create_file(dir, "device_info", &linprocfs_doscsidevinfo,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "scsi", &linprocfs_doscsiscsi,
	    NULL, NULL, NULL, PFS_RD);

	/* /proc/sys/... */
	dir = pfs_create_dir(root, "sys", NULL, NULL, NULL, 0);
	/* /proc/sys/kernel/... */
	dir = pfs_create_dir(dir, "kernel", NULL, NULL, NULL, 0);
	pfs_create_file(dir, "osrelease", &linprocfs_doosrelease,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "ostype", &linprocfs_doostype,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "version", &linprocfs_doosbuild,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "msgmni", &linprocfs_domsgmni,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "pid_max", &linprocfs_dopid_max,
	    NULL, NULL, NULL, PFS_RD);
	pfs_create_file(dir, "sem", &linprocfs_dosem,
	    NULL, NULL, NULL, PFS_RD);

	return (0);
}

/*
 * Destructor
 */
static int
linprocfs_uninit(PFS_INIT_ARGS)
{

	/* nothing to do, pseudofs will GC */
	return (0);
}

PSEUDOFS(linprocfs, 1);
MODULE_DEPEND(linprocfs, linux, 1, 1, 1);
MODULE_DEPEND(linprocfs, procfs, 1, 1, 1);
MODULE_DEPEND(linprocfs, sysvmsg, 1, 1, 1);
MODULE_DEPEND(linprocfs, sysvsem, 1, 1, 1);
