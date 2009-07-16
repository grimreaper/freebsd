/*-
 * Copyright (c) 2009 Stanislav Sedov <stas@FreeBSD.org>
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
 * $FreeBSD$
 */

#ifndef	__COMMON_KVM_H__
#define	__COMMON_KVM_H__

struct filestat {
	int	fs_type;	/* Descriptor type. */
	int	fs_flags;	/* filestat specific flags. */
	int	fs_fflags;	/* Descriptor access flags. */
	int	fs_fd;		/* File descriptor number. */
	void	*fs_typedep;	/* Type dependent data. */
	STAILQ_ENTRY(filestat)	next;
};

struct vnstat {
	dev_t	vn_dev;
	char	vn_devname[SPECNAMELEN + 1];
	int	vn_type;
	long	vn_fsid;
	long	vn_fileid;
	mode_t	vn_mode;
	u_long	vn_size;
	char	*mntdir;
};

struct ptsstat {
	dev_t	dev;
	char	devname[SPECNAMELEN + 1];
};

struct pipestat {
	caddr_t	addr;
	caddr_t	peer;
	size_t	buffer_cnt;
};

struct sockstat {
	int	type;
	int	proto;
	int	dom_family;
	caddr_t	so_addr;
	caddr_t	so_pcb;
	caddr_t	inp_ppcb;
	caddr_t	unp_conn;
	int	so_snd_sb_state;
	int	so_rcv_sb_state;
	char	dname[32];
};

STAILQ_HEAD(filestat_list, filestat);

dev_t	dev2udev(kvm_t *kd, struct cdev *dev);
int	kdevtoname(kvm_t *kd, struct cdev *dev, char *);
int	kvm_read_all(kvm_t *kd, unsigned long addr, void *buf,
    size_t nbytes);

/*
 * Filesystems specific access routines.
 */
int	devfs_filestat(kvm_t *kd, struct vnode *vp, struct vnstat *vn);
int	isofs_filestat(kvm_t *kd, struct vnode *vp, struct vnstat *vn);
int	msdosfs_filestat(kvm_t *kd, struct vnode *vp, struct vnstat *vn);
int	nfs_filestat(kvm_t *kd, struct vnode *vp, struct vnstat *vn);
int	ufs_filestat(kvm_t *kd, struct vnode *vp, struct vnstat *vn);
#ifdef ZFS
int	zfs_filestat(kvm_t *kd, struct vnode *vp, struct vnstat *vn);
void	*getvnodedata(struct vnode *vp);
struct mount	*getvnodemount(struct vnode *vp);
#endif

#endif	/* __COMMON_KVM_H__ */
