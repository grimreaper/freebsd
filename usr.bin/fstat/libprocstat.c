/*-
 * Copyright (c) 2009 Stanislav Sedov <stas@FreeBSD.org>
 * Copyright (c) 1988, 1993
 *      The Regents of the University of California.  All rights reserved.
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
 *      This product includes software developed by the University of
 *      California, Berkeley and its contributors.
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/time.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <sys/sysctl.h>
#include <sys/tty.h>
#include <sys/filedesc.h>
#include <sys/queue.h>
#define	_WANT_FILE
#include <sys/file.h>
#include <sys/conf.h>
#define	_KERNEL
#include <sys/mount.h>
#include <sys/pipe.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <fs/devfs/devfs.h>
#include <fs/devfs/devfs_int.h>
#undef _KERNEL
#include <nfs/nfsproto.h>
#include <nfsclient/nfs.h>
#include <nfsclient/nfsnode.h>

#include <vm/vm.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>

#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <kvm.h>
#include <libutil.h>
#include <limits.h>
#include <paths.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#include "libprocstat.h"
#include "common_kvm.h"

int     statfs(const char *, struct statfs *);	/* XXX */

/*
 * Vnode-to-filestat types translation table.
 */
static struct {
	int	vtype; 
	int	fst_vtype;
} vt2fst[] = {
	{ VNON, PS_FST_VTYPE_VNON },
	{ VREG, PS_FST_VTYPE_VREG },
	{ VDIR, PS_FST_VTYPE_VDIR },
	{ VBLK, PS_FST_VTYPE_VBLK },
	{ VCHR, PS_FST_VTYPE_VCHR },
	{ VLNK, PS_FST_VTYPE_VLNK },
	{ VSOCK, PS_FST_VTYPE_VSOCK },
	{ VFIFO, PS_FST_VTYPE_VFIFO },
	{ VBAD, PS_FST_VTYPE_VBAD }
};
#define	NVFTYPES	(sizeof(vt2fst) / sizeof(*vt2fst))

/*
 * kinfo tof ilestat vnode types translation table.
 */
static struct {
	int	kf_vtype; 
	int	fst_vtype;
} kfvtypes2fst[] = {
	{ KF_VTYPE_VNON, PS_FST_VTYPE_VNON },
	{ KF_VTYPE_VREG, PS_FST_VTYPE_VREG },
	{ KF_VTYPE_VDIR, PS_FST_VTYPE_VDIR },
	{ KF_VTYPE_VBLK, PS_FST_VTYPE_VBLK },
	{ KF_VTYPE_VCHR, PS_FST_VTYPE_VCHR },
	{ KF_VTYPE_VLNK, PS_FST_VTYPE_VLNK },
	{ KF_VTYPE_VSOCK, PS_FST_VTYPE_VSOCK },
	{ KF_VTYPE_VFIFO, PS_FST_VTYPE_VFIFO },
	{ KF_VTYPE_VBAD, PS_FST_VTYPE_VBAD }
};
#define	NKFVTYPES	(sizeof(kfvtypes2fst) / sizeof(*kfvtypes2fst))

/*
 * Descriptor-to-filestat flags translation table.
 */
static struct {
	int flag;
	int fst_flag;
} fstflags[] = {
	{ FREAD, PS_FST_FFLAG_READ },
	{ FWRITE, PS_FST_FFLAG_WRITE },
	{ O_NONBLOCK, PS_FST_FFLAG_NONBLOCK },
	{ O_APPEND, PS_FST_FFLAG_APPEND },
	{ O_SHLOCK, PS_FST_FFLAG_SHLOCK },
	{ O_EXLOCK, PS_FST_FFLAG_EXLOCK },
	{ O_ASYNC, PS_FST_FFLAG_ASYNC },
	{ O_SYNC, PS_FST_FFLAG_SYNC },
	{ O_NOFOLLOW, PS_FST_FFLAG_NOFOLLOW },
	{ O_CREAT, PS_FST_FFLAG_CREAT },
	{ O_TRUNC, PS_FST_FFLAG_TRUNC },
	{ O_EXCL, PS_FST_FFLAG_EXCL },
	{ O_DIRECT, PS_FST_FFLAG_DIRECT },
	{ O_EXEC, PS_FST_FFLAG_EXEC }
};
#define NFSTFLAGS	(sizeof(fstflags) / sizeof(*fstflags))

/*
 * kinfo types to filestat translation table.
 */
static struct {
	int kf_type;
	int fst_type;
} kftypes2fst[] = {
	{ KF_TYPE_NONE, PS_FST_TYPE_NONE },
	{ KF_TYPE_VNODE, PS_FST_TYPE_VNODE },
	{ KF_TYPE_SOCKET, PS_FST_TYPE_SOCKET },
	{ KF_TYPE_PIPE, PS_FST_TYPE_PIPE },
	{ KF_TYPE_FIFO, PS_FST_TYPE_FIFO },
	{ KF_TYPE_KQUEUE, PS_FST_TYPE_KQUEUE },
	{ KF_TYPE_CRYPTO, PS_FST_TYPE_CRYPTO },
	{ KF_TYPE_MQUEUE, PS_FST_TYPE_MQUEUE },
	{ KF_TYPE_SHM, PS_FST_TYPE_SHM },
	{ KF_TYPE_SEM, PS_FST_TYPE_SEM },
	{ KF_TYPE_PTS, PS_FST_TYPE_PTS },
	{ KF_TYPE_UNKNOWN, PS_FST_TYPE_UNKNOWN }
};
#define NKFTYPES	(sizeof(kftypes2fst) / sizeof(*kftypes2fst))

/*
 * kinfo flags to filestat translation table.
 */
static struct {
	int kf_flag;
	int fst_flag;
} kfflags2fst[] = {
	{ KF_FLAG_READ, PS_FST_FFLAG_READ },
	{ KF_FLAG_WRITE, PS_FST_FFLAG_WRITE },
	{ KF_FLAG_NONBLOCK, PS_FST_FFLAG_NONBLOCK },
	{ KF_FLAG_APPEND, PS_FST_FFLAG_APPEND },
	{ KF_FLAG_HASLOCK, PS_FST_FFLAG_SHLOCK },	/* XXX: which lock? */
	{ KF_FLAG_ASYNC, PS_FST_FFLAG_ASYNC },
	{ KF_FLAG_FSYNC, PS_FST_FFLAG_SYNC },
	{ KF_FLAG_DIRECT, PS_FST_FFLAG_DIRECT },
	/* XXX: other types? */
};
#define NKFFLAGS	(sizeof(kfflags2fst) / sizeof(*kfflags2fst))

/*
 * Filesystem specific handlers.
 */
#define FSTYPE(fst)     {#fst, fst##_filestat}
struct {
        const char      *tag;
        int             (*handler)(kvm_t *kd, struct vnode *vp,
            struct vnstat *vn);
} fstypes[] = {
        FSTYPE(ufs),
        FSTYPE(devfs),
        FSTYPE(nfs),
        FSTYPE(msdosfs),
        FSTYPE(isofs),
#ifdef ZFS
        FSTYPE(zfs),
#endif
/*
        FSTYPE(ntfs),
        FSTYPE(nwfs), 
        FSTYPE(smbfs),
        FSTYPE(udf), 
*/
};
#define NTYPES  (sizeof(fstypes) / sizeof(*fstypes))

#define	PROCSTAT_KVM	1
#define	PROCSTAT_SYSCTL	2

static char	*getmnton(kvm_t *kd, struct mount *m);
static struct filestat_list	*procstat_getfiles_kvm(kvm_t *kd,
    struct kinfo_proc *kp, int mmapped);
static struct filestat_list	*procstat_getfiles_sysctl(
    struct kinfo_proc *kp, int mmapped);
static int	procstat_get_pipe_info_sysctl(struct filestat *fst,
    struct pipestat *pipe, char *errbuf);
static int	procstat_get_pipe_info_kvm(kvm_t *kd, struct filestat *fst,
    struct pipestat *pipe, char *errbuf);
static int	procstat_get_pts_info_sysctl(struct filestat *fst,
    struct ptsstat *pts, char *errbuf);
static int	procstat_get_pts_info_kvm(kvm_t *kd, struct filestat *fst,
    struct ptsstat *pts, char *errbuf);
static int	procstat_get_socket_info_sysctl(struct filestat *fst,
    struct sockstat *sock, char *errbuf);
static int	procstat_get_socket_info_kvm(kvm_t *kd, struct filestat *fst,
    struct sockstat *sock, char *errbuf);
static int	to_filestat_flags(int flags);
static int	procstat_get_vnode_info_kvm(kvm_t *kd, struct filestat *fst,
    struct vnstat *vn, char *errbuf);
static int	procstat_get_vnode_info_sysctl(struct filestat *fst,
    struct vnstat *vn, char *errbuf);
static int	vntype2psfsttype(int type);

void
procstat_close(struct procstat *procstat)
{

	assert(procstat);
	if (procstat->type == PROCSTAT_KVM)
		kvm_close(procstat->kd);
}

struct procstat *
procstat_open(const char *nlistf, const char *memf)
{
	kvm_t *kd;
	char buf[_POSIX2_LINE_MAX];
	struct procstat *procstat;

	procstat = calloc(1, sizeof(*procstat));
	if (procstat == NULL) {
		warn("malloc()");
		return (NULL);
	}
	if (memf != NULL) {
		kd = kvm_openfiles(nlistf, memf, NULL, O_RDONLY, buf);
		if (kd == NULL) {
			warnx("kvm_openfiles(): %s", buf);
			free(procstat);
			return (NULL);
		}
		procstat->type = PROCSTAT_KVM;
		procstat->kd = kd;
	} else {
		procstat->type = PROCSTAT_SYSCTL;
	}
	return (procstat);
}

struct kinfo_proc *
procstat_getprocs(struct procstat *procstat, int what, int arg,
    unsigned int *count)
{
	struct kinfo_proc *p0, *p;
	size_t len;
	int name[4];
	int error;

	assert(procstat);
	assert(count);
	p = NULL;
	if (procstat->type == PROCSTAT_KVM) {
		p0 = kvm_getprocs(procstat->kd, what, arg, count);
		if (p0 == NULL || count == 0)
			return (NULL);
		len = *count * sizeof(*p);
		p = malloc(len);
		if (p == NULL) {
			warnx("malloc(%zd)", len);
			goto fail;
		}
		bcopy(p0, p, len);
		return (p);
	} else if (procstat->type == PROCSTAT_SYSCTL) {
		len = 0;
		name[0] = CTL_KERN;
		name[1] = KERN_PROC;
		name[2] = what;
		name[3] = arg;
		error = sysctl(name, 4, NULL, &len, NULL, 0);
		if (error < 0) {
			warn("sysctl(kern.proc)");
			goto fail;
		}
		if (len == 0) {
			warnx("no processes?");
			goto fail;
		}
		p = malloc(len);
		if (p == NULL) {
			warnx("malloc(%zd)", len);
			goto fail;
		}
		error = sysctl(name, 4, p, &len, NULL, 0);
		if (error < 0) {
			warn("sysctl(kern.proc)");
			goto fail;
		}
		/* Perform simple consistency checks. */
		if ((len % sizeof(*p)) != 0 || p->ki_structsize != sizeof(*p)) {
			warnx("kinfo_proc structure size mismatch");
			goto fail;
		}
		*count = len / sizeof(*p);
		return (p);
	} else {
		warnx("unknown access method");
		return (NULL);
	}
fail:
	if (p)
		free(p);
	return (NULL);
}

struct filestat_list *
procstat_getfiles(struct procstat *procstat, struct kinfo_proc *kp, int mmapped)
{
	
	if (procstat->type == PROCSTAT_SYSCTL)
		return (procstat_getfiles_sysctl(kp, mmapped));
	else if (procstat->type == PROCSTAT_KVM)
		 return (procstat_getfiles_kvm(procstat->kd, kp, mmapped));
	else
		return (NULL);
}

static struct filestat *
filestat_new_entry(void *typedep, int type, int fd, int fflags, int uflags)
{
	struct filestat *entry;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		warn("malloc()");
		return (NULL);
	}
	entry->fs_typedep = typedep;
	entry->fs_fflags = fflags;
	entry->fs_uflags = uflags;
	entry->fs_fd = fd;
	entry->fs_type = type;
	return (entry);
}

static struct filestat_list *
procstat_getfiles_kvm(kvm_t *kd, struct kinfo_proc *kp, int mmapped)
{
	int i;
	struct file file;
	struct filedesc filed;
	unsigned int nfiles;
	struct file **ofiles;
	struct filestat *entry;
	struct filestat_list *head;
	int type;
	void *data;
	vm_map_t map;
	struct vmspace vmspace;
	struct vm_map_entry vmentry;
	vm_map_entry_t entryp;
	struct vm_object object;
	vm_object_t objp;
	int prot, fflags;

	assert(kd);

	if (kp->ki_fd == NULL)
		return (NULL);
	if (!kvm_read_all(kd, (unsigned long)kp->ki_fd, &filed,
	    sizeof(filed))) {
		warnx("can't read filedesc at %p", (void *)kp->ki_fd);
		return (NULL);
	}

	/*
	 * Allocate list head.
	 */
	head = malloc(sizeof(*head));
	if (head == NULL)
		return (NULL);
	STAILQ_INIT(head);

	/* root directory vnode, if one. */
	if (filed.fd_rdir) {
		entry = filestat_new_entry(filed.fd_rdir, PS_FST_TYPE_VNODE, -1,
		    PS_FST_FFLAG_READ, PS_FST_UFLAG_RDIR);
		if (entry != NULL)
			STAILQ_INSERT_TAIL(head, entry, next);
	}
	/* current working directory vnode. */
	if (filed.fd_cdir) {
		entry = filestat_new_entry(filed.fd_cdir, PS_FST_TYPE_VNODE, -1,
		    PS_FST_FFLAG_READ, PS_FST_UFLAG_CDIR);
		if (entry != NULL)
			STAILQ_INSERT_TAIL(head, entry, next);
	}
	/* jail root, if any. */
	if (filed.fd_jdir) {
		entry = filestat_new_entry(filed.fd_jdir, PS_FST_TYPE_VNODE, -1,
		    PS_FST_FFLAG_READ, PS_FST_UFLAG_JAIL);
		if (entry != NULL)
			STAILQ_INSERT_TAIL(head, entry, next);
	}
	/* ktrace vnode, if one */
	if (kp->ki_tracep) {
		entry = filestat_new_entry(kp->ki_tracep, PS_FST_TYPE_VNODE, -1,
		    PS_FST_FFLAG_READ | PS_FST_FFLAG_WRITE, PS_FST_UFLAG_TRACE);
		if (entry != NULL)
			STAILQ_INSERT_TAIL(head, entry, next);
	}
	/* text vnode, if one */
	if (kp->ki_textvp) {
		entry = filestat_new_entry(kp->ki_textvp, PS_FST_TYPE_VNODE, -1,
		    PS_FST_FFLAG_READ, PS_FST_UFLAG_TEXT);
		if (entry != NULL)
			STAILQ_INSERT_TAIL(head, entry, next);
	}

	nfiles = filed.fd_lastfile + 1;
	ofiles = malloc(nfiles * sizeof(struct file *));
	if (ofiles == NULL) {
		warn("malloc(%zd)", nfiles * sizeof(struct file *));
		goto do_mmapped;
	}
	if (!kvm_read_all(kd, (unsigned long)filed.fd_ofiles, ofiles,
	    nfiles * sizeof(struct file *))) {
		warnx("cannot read file structures at %p",
		    (void *)filed.fd_ofiles);
		free(ofiles);
		goto do_mmapped;
	}
	for (i = 0; i <= filed.fd_lastfile; i++) {
		if (ofiles[i] == NULL)
			continue;
		if (!kvm_read_all(kd, (unsigned long)ofiles[i], &file,
		    sizeof(struct file))) {
			warnx("can't read file %d at %p", i,
			    (void *)ofiles[i]);
			continue;
		}
		switch (file.f_type) {
		case DTYPE_VNODE:
			type = PS_FST_TYPE_VNODE;
			data = file.f_vnode;
			break;
		case DTYPE_SOCKET:
			type = PS_FST_TYPE_SOCKET;
			data = file.f_data;
			break;
		case DTYPE_PIPE:
			type = PS_FST_TYPE_PIPE;
			data = file.f_data;
			break;
		case DTYPE_FIFO:
			type = PS_FST_TYPE_FIFO;
			data = file.f_vnode;
			break;
#ifdef DTYPE_PTS
		case DTYPE_PTS:
			type = PS_FST_TYPE_PTS;
			data = file.f_data;
			break;
#endif
		default:
			continue;
		}
		entry = filestat_new_entry(data, type, i,
		    to_filestat_flags(file.f_flag), 0);
		if (entry != NULL)
			STAILQ_INSERT_TAIL(head, entry, next);
	}
	free(ofiles);

do_mmapped:

	/*
	 * Process mmapped files if requested.
	 */
	if (mmapped) {
		if (!kvm_read_all(kd, (unsigned long)kp->ki_vmspace, &vmspace,
		    sizeof(vmspace))) {
			warnx("can't read vmspace at %p",
			    (void *)kp->ki_vmspace);
			goto exit;
		}
		map = &vmspace.vm_map;

		for (entryp = map->header.next;
		    entryp != &kp->ki_vmspace->vm_map.header;
		    entryp = vmentry.next) {
			if (!kvm_read_all(kd, (unsigned long)entryp, &vmentry,
			    sizeof(vmentry))) {
				warnx("can't read vm_map_entry at %p",
				    (void *)entryp);
				continue;
			}
			if (vmentry.eflags & MAP_ENTRY_IS_SUB_MAP)
				continue;
			if ((objp = vmentry.object.vm_object) == NULL)
				continue;
			for (; objp; objp = object.backing_object) {
				if (!kvm_read_all(kd, (unsigned long)objp,
				    &object, sizeof(object))) {
					warnx("can't read vm_object at %p",
					    (void *)objp);
					break;
				}
			}

			/* We want only vnode objects. */
			if (object.type != OBJT_VNODE)
				continue;

			prot = vmentry.protection;
			fflags = 0;
			if (prot & VM_PROT_READ)
				fflags = PS_FST_FFLAG_READ;
			if (prot & VM_PROT_WRITE)
				fflags |= PS_FST_FFLAG_WRITE;

			/*
			 * Create filestat entry.
			 */
			entry = filestat_new_entry(object.handle,
			    PS_FST_TYPE_VNODE, -1, fflags, PS_FST_UFLAG_MMAP);
			if (entry != NULL)
				STAILQ_INSERT_TAIL(head, entry, next);
		}
	}
exit:
	return (head);
}

static int
kinfo_type2fst(int kftype)
{
	unsigned int i;

	for (i = 0; i < NKFTYPES; i++)
		if (kftypes2fst[i].kf_type == kftype)
			break;
	if (i == NKFTYPES)
		return (PS_FST_TYPE_UNKNOWN);
	return (kftypes2fst[i].fst_type);
}

static int
kinfo_fflags2fst(int kfflags)
{
	unsigned int i;
	int flags;

	flags = 0;
	for (i = 0; i < NKFFLAGS; i++)
		if ((kfflags & kfflags2fst[i].kf_flag) != 0)
			flags |= kfflags2fst[i].fst_flag;
	return (flags);
}

static int
kinfo_uflags2fst(int fd)
{
	switch (fd) {
	case KF_FD_TYPE_CWD:
		return (PS_FST_UFLAG_CDIR);
	case KF_FD_TYPE_ROOT:
		return (PS_FST_UFLAG_RDIR);
	case KF_FD_TYPE_JAIL:
		return (PS_FST_UFLAG_JAIL);
#if 0
	case KF_FD_TYPE_TRACE:
		return (PS_FST_UFLAG_TRACE);
	case KF_FD_TYPE_TEXT:
		return (PS_FST_UFLAG_TEXT);
#endif
	}
	return (0);
}

static struct filestat_list *
procstat_getfiles_sysctl(struct kinfo_proc *kp, int mmapped __unused)
{
	struct kinfo_file *kif, *files;
	struct filestat_list *head;
	int fd, fflags, uflags, type;
	struct filestat *entry;
	int cnt, i;

	assert(kp);

	if (kp->ki_fd == NULL)
		return (NULL);

	files = kinfo_getfile(kp->ki_pid, &cnt);
	if (files == NULL) {
		warn("kinfo_getfile()");
		return (NULL);
	}

	/*
	 * Allocate list head.
	 */
	head = malloc(sizeof(*head));
	if (head == NULL)
		return (NULL);
	STAILQ_INIT(head);
	for (i = 0; i < cnt; i++) {
		kif = &files[i];

		type = kinfo_type2fst(kif->kf_type);
		fd = kif->kf_fd >= 0 ? kif->kf_fd : -1;
		fflags = kinfo_fflags2fst(kif->kf_flags);
		uflags = kinfo_uflags2fst(kif->kf_fd);

		/*
		 * Create filestat entry.
		 */
		entry = filestat_new_entry(kif, type, fd, fflags, uflags);
		if (entry != NULL)
			STAILQ_INSERT_TAIL(head, entry, next);
	}
	return (head);
}

int
procstat_get_pipe_info(struct procstat *procstat, struct filestat *fst,
    struct pipestat *ps, char *errbuf)
{

	assert(ps);
	if (procstat->type == PROCSTAT_KVM) {
		return (procstat_get_pipe_info_kvm(procstat->kd, fst, ps,
		    errbuf));
	} else if (procstat->type == PROCSTAT_SYSCTL) {
		return (procstat_get_pipe_info_sysctl(fst, ps, errbuf));
	} else {
		warnx("unknow access method: %d", procstat->type);
		snprintf(errbuf, _POSIX2_LINE_MAX, "error");
		return (1);
	}
}

static int
procstat_get_pipe_info_kvm(kvm_t *kd, struct filestat *fst,
    struct pipestat *ps, char *errbuf)
{
	struct pipe pi;
	void *pipep;

	assert(kd);
	assert(ps);
	assert(fst);
	bzero(ps, sizeof(*ps));
	pipep = fst->fs_typedep;
	if (pipep == NULL)
		goto fail;
	if (!kvm_read_all(kd, (unsigned long)pipep, &pi, sizeof(struct pipe))) {
		warnx("can't read pipe at %p", (void *)pipep);
		goto fail;
	}
	ps->addr = (caddr_t)pipep;
	ps->peer = (caddr_t)pi.pipe_peer;
	ps->buffer_cnt = pi.pipe_buffer.cnt;
	return (0);

fail:
	snprintf(errbuf, _POSIX2_LINE_MAX, "error");
	return (1);
}

static int
procstat_get_pipe_info_sysctl(struct filestat *fst __unused, struct pipestat *ps __unused,
    char *errbuf __unused)
{

	warnx("not implemented: %s:%d", __FUNCTION__, __LINE__);
	snprintf(errbuf, _POSIX2_LINE_MAX, "error");
	return (1);
}

int
procstat_get_pts_info(struct procstat *procstat, struct filestat *fst,
    struct ptsstat *pts, char *errbuf)
{

	assert(pts);
	if (procstat->type == PROCSTAT_KVM) {
		return (procstat_get_pts_info_kvm(procstat->kd, fst, pts,
		    errbuf));
	} else if (procstat->type == PROCSTAT_SYSCTL) {
		return (procstat_get_pts_info_sysctl(fst, pts, errbuf));
	} else {
		warnx("unknow access method: %d", procstat->type);
		snprintf(errbuf, _POSIX2_LINE_MAX, "error");
		return (1);
	}
}

static int
procstat_get_pts_info_kvm(kvm_t *kd, struct filestat *fst,
    struct ptsstat *pts, char *errbuf)
{
	struct tty tty;
	void *ttyp;

	assert(kd);
	assert(pts);
	assert(fst);
	bzero(pts, sizeof(*pts));
	ttyp = fst->fs_typedep;
	if (ttyp == NULL)
		goto fail;
	if (!kvm_read_all(kd, (unsigned long)ttyp, &tty, sizeof(struct tty))) {
		warnx("can't read tty at %p", (void *)ttyp);
		goto fail;
	}
	pts->dev = dev2udev(kd, tty.t_dev);
	(void)kdevtoname(kd, tty.t_dev, pts->devname);
	return (0);

fail:
	snprintf(errbuf, _POSIX2_LINE_MAX, "error");
	return (1);
}

static int
procstat_get_pts_info_sysctl(struct filestat *fst __unused, struct ptsstat *pts __unused,
    char *errbuf __unused)
{

	warnx("not implemented: %s:%d", __FUNCTION__, __LINE__);
	snprintf(errbuf, _POSIX2_LINE_MAX, "error");
	return (1);
}

int
procstat_get_vnode_info(struct procstat *procstat, struct filestat *fst,
    struct vnstat *vn, char *errbuf)
{

	assert(vn);
	if (procstat->type == PROCSTAT_KVM) {
		return (procstat_get_vnode_info_kvm(procstat->kd, fst, vn,
		    errbuf));
	} else if (procstat->type == PROCSTAT_SYSCTL) {
		return (procstat_get_vnode_info_sysctl(fst, vn, errbuf));
	} else {
		warnx("unknow access method: %d", procstat->type);
		snprintf(errbuf, _POSIX2_LINE_MAX, "error");
		return (1);
	}
}

static int
procstat_get_vnode_info_kvm(kvm_t *kd, struct filestat *fst,
    struct vnstat *vn, char *errbuf)
{
	char tagstr[12];
	int error;
	int found;
	struct vnode vnode;
	void *vp;
	unsigned int i;

	assert(kd);
	assert(vn);
	assert(fst);
	vp = fst->fs_typedep;
	if (vp == NULL)
		goto fail;
	error = kvm_read_all(kd, (unsigned long)vp, &vnode, sizeof(vnode));
	if (error == 0) {
		warnx("can't read vnode at %p", (void *)vp);
		goto fail;
	}
	bzero(vn, sizeof(*vn));
	vn->vn_type = vntype2psfsttype(vnode.v_type);
	if (vnode.v_type == VNON || vnode.v_type == VBAD)
		return (0);
	error = kvm_read_all(kd, (unsigned long)vnode.v_tag, tagstr,
	    sizeof(tagstr));
	if (error == 0) {
		warnx("can't read v_tag at %p", (void *)vp);
		goto fail;
	}
	tagstr[sizeof(tagstr) - 1] = '\0';

	/*
	 * Find appropriate handler.
	 */
	for (i = 0, found = 0; i < NTYPES; i++)
		if (!strcmp(fstypes[i].tag, tagstr)) {
			if (fstypes[i].handler(kd, &vnode, vn) != 0) {
				goto fail;
			}
			break;
		}
	if (i == NTYPES) {
		snprintf(errbuf, _POSIX2_LINE_MAX, "?(%s)", tagstr);
		return (1);
	}
	vn->mntdir = getmnton(kd, vnode.v_mount);
	if ((vnode.v_type == VBLK || vnode.v_type == VCHR) &&
	    vnode.v_rdev != NULL){
		vn->vn_dev = dev2udev(kd, vnode.v_rdev);
		(void)kdevtoname(kd, vnode.v_rdev, vn->vn_devname);
	} else {
		vn->vn_dev = -1;
	}
	return (0);

fail:
	snprintf(errbuf, _POSIX2_LINE_MAX, "error");
	return (1);
}

static int
kinfo_vtype2fst(int kfvtype)
{
	unsigned int i;

	for (i = 0; i < NKFVTYPES; i++)
		if (kfvtypes2fst[i].kf_vtype == kfvtype)
			break;
	if (i == NKFVTYPES)
		return (PS_FST_VTYPE_UNKNOWN);
	return (kfvtypes2fst[i].fst_vtype);
}

static int
procstat_get_vnode_info_sysctl(struct filestat *fst, struct vnstat *vn,
    char *errbuf __unused)
{
	struct kinfo_file *kif;
	struct statfs stbuf;
	char *name;

	assert(fst);
	assert(vn);
	kif = fst->fs_typedep;
	if (kif == NULL)
		return (1);
	bzero(vn, sizeof(*vn));
	vn->vn_type = kinfo_vtype2fst(kif->kf_vnode_type);
	if (vn->vn_type == PS_FST_VTYPE_VNON ||
	    vn->vn_type == PS_FST_VTYPE_VBAD ||
	    (kif->kf_status & KF_ATTR_VALID) == 0)
		return (0);
	if (kif->kf_path && *kif->kf_path) {
		statfs(kif->kf_path, &stbuf);
		vn->mntdir = strdup(stbuf.f_mntonname);
	}
	vn->vn_dev = kif->kf_file_rdev;
	if (kif->kf_vnode_type == KF_VTYPE_VBLK) {
		name = devname(vn->vn_dev, S_IFBLK);
		if (name != NULL)
			strlcpy(vn->vn_devname, name, sizeof(vn->vn_devname));
	} else if (kif->kf_vnode_type == KF_VTYPE_VCHR) {
		name = devname(vn->vn_dev, S_IFCHR);
		if (name != NULL)
			strlcpy(vn->vn_devname, name, sizeof(vn->vn_devname));
	}
	vn->vn_fsid = kif->kf_file_fsid;
	vn->vn_fileid = kif->kf_file_fileid;
	vn->vn_size = kif->kf_file_size;
	vn->vn_mode = kif->kf_file_mode;
	return (0);
}

int
procstat_get_socket_info(struct procstat *procstat, struct filestat *fst,
    struct sockstat *sock, char *errbuf)
{

	assert(sock);
	if (procstat->type == PROCSTAT_KVM) {
		return (procstat_get_socket_info_kvm(procstat->kd, fst, sock,
		    errbuf));
	} else if (procstat->type == PROCSTAT_SYSCTL) {
		return (procstat_get_socket_info_sysctl(fst, sock, errbuf));
	} else {
		warnx("unknow access method: %d", procstat->type);
		snprintf(errbuf, _POSIX2_LINE_MAX, "error");
		return (1);
	}
}

static int
procstat_get_socket_info_kvm(kvm_t *kd, struct filestat *fst,
    struct sockstat *sock, char *errbuf)
{
	struct socket s;
	struct protosw proto;
	struct domain dom;
	struct inpcb inpcb;
	struct unpcb unpcb;
	ssize_t len;
	void *so;

	assert(kd);
	assert(sock);
	assert(fst);
	bzero(sock, sizeof(*sock));
	so = fst->fs_typedep;
	if (so == NULL)
		goto fail;
	sock->so_addr = (caddr_t)so;
	/* fill in socket */
	if (!kvm_read_all(kd, (unsigned long)so, &s,
	    sizeof(struct socket))) {
		warnx("can't read sock at %p", (void *)so);
		goto fail;
	}
	/* fill in protosw entry */
	if (!kvm_read_all(kd, (unsigned long)s.so_proto, &proto,
	    sizeof(struct protosw))) {
		warnx("can't read protosw at %p", (void *)s.so_proto);
		goto fail;
	}
	/* fill in domain */
	if (!kvm_read_all(kd, (unsigned long)proto.pr_domain, &dom,
	    sizeof(struct domain))) {
		warnx("can't read domain at %p",
		    (void *)proto.pr_domain);
		goto fail;
	}
	if ((len = kvm_read(kd, (unsigned long)dom.dom_name, sock->dname,
	    sizeof(sock->dname) - 1)) < 0) {
		warnx("can't read domain name at %p", (void *)dom.dom_name);
		sock->dname[0] = '\0';
	}
	else
		sock->dname[len] = '\0';
	
	/*
	 * Fill in known data.
	 */
	sock->type = s.so_type;
	sock->proto = proto.pr_protocol;
	sock->dom_family = dom.dom_family;
	sock->so_pcb = s.so_pcb;

	/*
	 * Protocol specific data.
	 */
	switch(dom.dom_family) {
	case AF_INET:
	case AF_INET6:
		if (proto.pr_protocol == IPPROTO_TCP) {
			if (s.so_pcb) {
				if (kvm_read(kd, (u_long)s.so_pcb,
				    (char *)&inpcb, sizeof(struct inpcb))
				    != sizeof(struct inpcb)) {
					warnx("can't read inpcb at %p",
					    (void *)s.so_pcb);
				} else
					sock->inp_ppcb =
					    (caddr_t)inpcb.inp_ppcb;
			}
		}
		break;
	case AF_UNIX:
		if (s.so_pcb) {
			if (kvm_read(kd, (u_long)s.so_pcb, (char *)&unpcb,
			    sizeof(struct unpcb)) != sizeof(struct unpcb)){
				warnx("can't read unpcb at %p",
				    (void *)s.so_pcb);
			} else if (unpcb.unp_conn) {
				sock->so_rcv_sb_state = s.so_rcv.sb_state;
				sock->so_snd_sb_state = s.so_snd.sb_state;
				sock->unp_conn = (caddr_t)unpcb.unp_conn;
			}
		}
		break;
	default:
		break;
	}
	return (0);

fail:
	snprintf(errbuf, _POSIX2_LINE_MAX, "error");
	return (1);
}

static int
procstat_get_socket_info_sysctl(struct filestat *fst __unused, struct sockstat *sock __unused,
    char *errbuf __unused)
{

	warnx("not implemented: %s:%d", __FUNCTION__, __LINE__);
	snprintf(errbuf, _POSIX2_LINE_MAX, "error");
	return (1);
}

static int
to_filestat_flags(int flags)
{
	int fst_flags;
	unsigned int i;

	fst_flags = 0;
	for (i = 0; i < NFSTFLAGS; i++)
		if (flags & fstflags[i].flag)
			fst_flags |= fstflags[i].fst_flag;
	return (fst_flags);
}

static int
vntype2psfsttype(int type)
{
	unsigned int i, fst_type;

	fst_type = PS_FST_VTYPE_UNKNOWN;
	for (i = 0; i < NVFTYPES; i++) {
		if (type == vt2fst[i].vtype) {
			fst_type = vt2fst[i].fst_vtype;
			break;
		}
	}
	return (fst_type);
}

static char *
getmnton(kvm_t *kd, struct mount *m)
{
	static struct mount mnt;
	static struct mtab {
		struct mtab *next;
		struct mount *m;
		char mntonname[MNAMELEN + 1];
	} *mhead = NULL;
	struct mtab *mt;

	for (mt = mhead; mt != NULL; mt = mt->next)
		if (m == mt->m)
			return (mt->mntonname);
	if (!kvm_read_all(kd, (unsigned long)m, &mnt, sizeof(struct mount))) {
		warnx("can't read mount table at %p", (void *)m);
		return (NULL);
	}
	if ((mt = malloc(sizeof (struct mtab))) == NULL)
		err(1, NULL);
	mt->m = m;
	bcopy(&mnt.mnt_stat.f_mntonname[0], &mt->mntonname[0], MNAMELEN);
	mnt.mnt_stat.f_mntonname[MNAMELEN] = '\0';
	mt->next = mhead;
	mhead = mt;
	return (mt->mntonname);
}
