/*-
 * Copyright (c) 2009 Stanislav Sedov <stas@FreeBSD.org>
 * Copyright (c) 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
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

#include "common_kvm.h"
#include "functions.h"
#include "libprocstat.h"

int 	fsflg,	/* show files on same filesystem as file(s) argument */
	pflg,	/* show files open by a particular pid */
	uflg;	/* show files open by a particular (effective) user */
int 	checkfile; /* true if restricting to particular files or filesystems */
int	nflg;	/* (numerical) display f.s. and rdev as dev_t */
int	mflg;	/* include memory-mapped files */
int	vflg;	/* be verbose */

typedef struct devs {
	struct devs	*next;
	long		fsid;
	long		ino;
	const char	*name;
} DEVS;

DEVS *devs;
char *memf, *nlistf;

static void fstat1(int what, int arg);
static void dofiles(struct procstat *procstat, struct kinfo_proc *p);
void dofiles_kinfo(struct kinfo_proc *kp);
void dommap(struct kinfo_proc *kp);
void vtrans(struct vnode *vp, int i, int flag, const char *uname, const char *cmd, int pid);
char *getmnton(struct mount *m);
void pipetrans(struct pipe *pi, int i, int flag, const char *uname, const char *cmd, int pid);
void socktrans(struct socket *sock, int i, const char *uname, const char *cmd, int pid);
void ptstrans(struct tty *tp, int i, int flag, const char *uname, const char *cmd, int pid);
void getinetproto(int number);
int  getfname(const char *filename);
void usage(void);
void vtrans_kinfo(struct kinfo_file *, int i, int flag, const char *uname, const char *cmd, int pid);
static void print_file_info(struct procstat *procstat, struct filestat *fst, const char *uname, const char *cmd, int pid);

static void
print_socket_info(struct procstat *procstat, struct filestat *fst);
static void
print_pipe_info(struct procstat *procstat, struct filestat *fst);
static void
print_pts_info(struct procstat *procstat, struct filestat *fst);
static void
print_vnode_info(struct procstat *procstat, struct filestat *fst);
static void
print_access_flags(int flags);

int
do_fstat(int argc, char **argv)
{
	struct passwd *passwd;
	int arg, ch, what;

	arg = 0;
	what = KERN_PROC_PROC;
	nlistf = memf = NULL;
	while ((ch = getopt(argc, argv, "fmnp:u:vN:M:")) != -1)
		switch((char)ch) {
		case 'f':
			fsflg = 1;
			break;
		case 'M':
			memf = optarg;
			break;
		case 'N':
			nlistf = optarg;
			break;
		case 'm':
			mflg = 1;
			break;
		case 'n':
			nflg = 1;
			break;
		case 'p':
			if (pflg++)
				usage();
			if (!isdigit(*optarg)) {
				warnx("-p requires a process id");
				usage();
			}
			what = KERN_PROC_PID;
			arg = atoi(optarg);
			break;
		case 'u':
			if (uflg++)
				usage();
			if (!(passwd = getpwnam(optarg)))
				errx(1, "%s: unknown uid", optarg);
			what = KERN_PROC_UID;
			arg = passwd->pw_uid;
			break;
		case 'v':
			vflg = 1;
			break;
		case '?':
		default:
			usage();
		}

	if (*(argv += optind)) {
		for (; *argv; ++argv) {
			if (getfname(*argv))
				checkfile = 1;
		}
		if (!checkfile)	/* file(s) specified, but none accessable */
			exit(1);
	}

	if (fsflg && !checkfile) {
		/* -f with no files means use wd */
		if (getfname(".") == 0)
			exit(1);
		checkfile = 1;
	}

		fstat1(what, arg);
	exit(0);
}

static void
fstat1(int what, int arg)
{
	struct kinfo_proc *p;
	struct procstat *procstat;
	int cnt;
	int i;

	procstat = procstat_open(nlistf, memf);
	if (procstat == NULL)
		errx(1, "procstat_open()");
	p = procstat_getprocs(procstat, what, arg, &cnt);
	if (p == NULL)
		errx(1, "procstat_getprocs()");

	/*
	 * Print header.
	 */
	if (nflg)
		printf("%s",
"USER     CMD          PID   FD  DEV    INUM       MODE SZ|DV R/W");
	else
		printf("%s",
"USER     CMD          PID   FD MOUNT      INUM MODE         SZ|DV R/W");
	if (checkfile && fsflg == 0)
		printf(" NAME\n");
	else
		putchar('\n');

	/*
	 * Go through the process list.
	 */
	for (i = 0; i < cnt; i++) {
		if (p[i].ki_stat == SZOMB)
			continue;
		dofiles(procstat, &p[i]);
/*
		if (mflg)
			dommap(procstat, &p[i]);
*/
	}
	free(p);
	procstat_close(procstat);
}

static void
dofiles(struct procstat *procstat, struct kinfo_proc *kp)
{
	struct filestat_list *head;
	const char *cmd;
	const char *uname;
	int pid;
	struct filestat *fst;

	uname = user_from_uid(kp->ki_uid, 0);
	pid = kp->ki_pid;
	cmd = kp->ki_comm;

	head = procstat_getfiles(procstat, kp);
	if (head == NULL)
		return;

	STAILQ_FOREACH(fst, head, next)
		print_file_info(procstat, fst, uname, cmd, pid);
}


static void
print_file_info(struct procstat *procstat, struct filestat *fst,
    const char *uname, const char *cmd, int pid)
{
	const char *filename;
	struct vnstat vn;
	int error;
	int fsmatch = 0;
	DEVS *d;

	filename = NULL;
	if (checkfile != 0) {
		if (fst->fs_type != PS_FST_TYPE_VNODE &&
		    fst->fs_type == PS_FST_TYPE_FIFO)
			return;
		error = procstat_get_vnode_info(procstat, fst, &vn, NULL);
		if (error != 0)
			return;

		for (d = devs; d != NULL; d = d->next)
			if (d->fsid == vn.vn_fsid) {
				fsmatch = 1;
				if (d->ino == vn.vn_fileid) {
					filename = d->name;
					break;
				}
			}
		if (fsmatch == 0 || (filename == NULL && fsflg == 0))
			return;
	}

	/*
	 * Print entry prefix.
	 */
	printf("%-8.8s %-10s %5d", uname, cmd, pid);
	switch(fst->fs_fd) {
	case PS_FST_FD_TEXT:
		printf(" text");
		break;
	case PS_FST_FD_CDIR:
		printf("   wd");
		break;
	case PS_FST_FD_RDIR:
		printf(" root");
		break;
	case PS_FST_FD_TRACE:
		printf("   tr");
		break;
	case PS_FST_FD_MMAP:
		printf(" mmap");
		break;
	case PS_FST_FD_JAIL:
		printf(" jail");
		break;
	default:
		printf(" %4d", fst->fs_fd);
		break;
	}

	/*
	 * Print type-specific data.
	 */
	switch (fst->fs_type) {
	case PS_FST_TYPE_FIFO:
	case PS_FST_TYPE_VNODE:
		print_vnode_info(procstat, fst);
		break;
	case PS_FST_TYPE_SOCKET:
		print_socket_info(procstat, fst);
		break;
	case PS_FST_TYPE_PIPE:
		print_pipe_info(procstat, fst);
		break;
	case PS_FST_TYPE_PTS:
		print_pts_info(procstat, fst);
		break;
	default:	
		if (vflg)
			fprintf(stderr,
			    "unknown file type %d for file %d of pid %d\n",
			    fst->fs_type, fst->fs_fd, pid);
	}
	if (filename && !fsflg)
		printf("  %s", filename);
	putchar('\n');
}

static void
print_socket_info(struct procstat *procstat, struct filestat *fst)
{
	static const char *stypename[] = {
		"unused",	/* 0 */
		"stream",	/* 1 */
		"dgram",	/* 2 */
		"raw",		/* 3 */
		"rdm",		/* 4 */
		"seqpak"	/* 5 */
	};
#define STYPEMAX 5
	struct sockstat sock;
	char errbuf[_POSIX2_LINE_MAX];
	static int isopen;
	struct protoent *pe;
	int error;

	error = procstat_get_socket_info(procstat, fst, &sock, errbuf);
	if (error != 0) {
		printf("* error");
		return;
	}
	if (sock.type > STYPEMAX)
		printf("* %s ?%d", sock.dname, sock.type);
	else
		printf("* %s %s", sock.dname, stypename[sock.type]);

	/*
	 * protocol specific formatting
	 *
	 * Try to find interesting things to print.  For tcp, the interesting
	 * thing is the address of the tcpcb, for udp and others, just the
	 * inpcb (socket pcb).  For unix domain, its the address of the socket
	 * pcb and the address of the connected pcb (if connected).  Otherwise
	 * just print the protocol number and address of the socket itself.
	 * The idea is not to duplicate netstat, but to make available enough
	 * information for further analysis.
	 */
	switch (sock.dom_family) {
	case AF_INET:
	case AF_INET6:
		if (!isopen)
			setprotoent(++isopen);
		if ((pe = getprotobynumber(sock.proto)) != NULL)
			printf(" %s", pe->p_name);
		else
			printf(" %d", sock.proto);
		if (sock.proto == IPPROTO_TCP ) {
			if (sock.inp_ppcb != 0)
				printf(" %lx", (u_long)sock.inp_ppcb);
		}
		else if (sock.so_pcb != 0)
			printf(" %lx", (u_long)sock.so_pcb);
		break;
	case AF_UNIX:
		/* print address of pcb and connected pcb */
		if (sock.so_pcb != 0) {
			printf(" %lx", (u_long)sock.so_pcb);
			if (sock.unp_conn) {
				char shoconn[4], *cp;

				cp = shoconn;
				if (!(sock.so_rcv_sb_state & SBS_CANTRCVMORE))
					*cp++ = '<';
				*cp++ = '-';
				if (!(sock.so_snd_sb_state & SBS_CANTSENDMORE))
					*cp++ = '>';
				*cp = '\0';
				printf(" %s %lx", shoconn,
				    (u_long)sock.unp_conn);
                        }
		}
		break;
	default:
		/* print protocol number and socket address */
		printf(" %d %lx", sock.proto, (u_long)sock.so_addr);
	}
}

static void
print_pipe_info(struct procstat *procstat, struct filestat *fst)
{
	struct pipestat pipe;
	char errbuf[_POSIX2_LINE_MAX];
	int error;

	error = procstat_get_pipe_info(procstat, fst, &pipe, errbuf);
	if (error != 0) {
		printf("* error");
		return;
	}
	printf("* pipe %8lx <-> %8lx", (u_long)pipe.addr, (u_long)pipe.peer);
	printf(" %6zd", pipe.buffer_cnt);
	print_access_flags(fst->fs_fflags);
}

static void
print_pts_info(struct procstat *procstat, struct filestat *fst)
{
	struct ptsstat pts;
	char errbuf[_POSIX2_LINE_MAX];
	int error;

	error = procstat_get_pts_info(procstat, fst, &pts, errbuf);
	if (error != 0) {
		printf("* error");
		return;
	}
	printf("* pseudo-terminal master ");
	if (nflg || !*pts.devname) {
		printf("%10d,%-2d", major(pts.dev), minor(pts.dev));
	} else {
		printf("%10s", pts.devname);
	}
	print_access_flags(fst->fs_fflags);
}

static void
print_vnode_info(struct procstat *procstat, struct filestat *fst)
{
	struct vnstat vn;
	const char *badtype;
	char errbuf[_POSIX2_LINE_MAX];
	char mode[15];
	int error;

	badtype = NULL;
	error = procstat_get_vnode_info(procstat, fst, &vn, errbuf);
	if (error != 0)
		badtype = errbuf;
	else if (vn.vn_type == PS_FST_VTYPE_VBAD)
		badtype = "bad";
	else if (vn.vn_type == PS_FST_VTYPE_VNON)
		badtype = "none";
	if (badtype != NULL) {
		printf(" -         -  %10s    -", badtype);
		return;
	}

	if (nflg)
		printf(" %2d,%-2d", major(vn.vn_fsid), minor(vn.vn_fsid));
	else if (vn.mntdir != NULL)
		(void)printf(" %-8s", vn.mntdir);

	/*
	 * Print access mode.
	 */
	if (nflg)
		(void)snprintf(mode, sizeof(mode), "%o", vn.vn_mode);
	else {
		strmode(vn.vn_mode, mode);
	}
	(void)printf(" %6ld %10s", vn.vn_fileid, mode);

	if (vn.vn_type == PS_FST_VTYPE_VBLK || vn.vn_type == PS_FST_VTYPE_VCHR) {
		if (nflg || !*vn.vn_devname)
			printf("  %2d,%-2d", major(vn.vn_dev), minor(vn.vn_dev));
		else {
			printf(" %6s", vn.vn_devname);
		}
	} else
		printf(" %6lu", vn.vn_size);
	print_access_flags(fst->fs_fflags);
}

static void
print_access_flags(int flags)
{
	char rw[3];

	rw[0] = '\0';
	if (flags & PS_FST_FFLAG_READ)
		strcat(rw, "r");
	if (flags & PS_FST_FFLAG_WRITE)
		strcat(rw, "w");
	printf(" %2s", rw);
}

int
getfname(const char *filename)
{
	struct stat statbuf;
	DEVS *cur;

	if (stat(filename, &statbuf)) {
		warn("%s", filename);
		return (0);
	}
	if ((cur = malloc(sizeof(DEVS))) == NULL)
		err(1, NULL);
	cur->next = devs;
	devs = cur;

	cur->ino = statbuf.st_ino;
	cur->fsid = statbuf.st_dev;
	cur->name = filename;
	return (1);
}

void
usage(void)
{
	(void)fprintf(stderr,
 "usage: fstat [-fmnv] [-M core] [-N system] [-p pid] [-u user] [file ...]\n");
	exit(1);
}
