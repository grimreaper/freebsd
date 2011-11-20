# FreeBSD system call names.
# DO NOT EDIT-- this file is automatically generated.
# $FreeBSD$
# created from FreeBSD: head/sys/kern/syscalls.master 227691 2011-11-19 06:35:15Z ed 
MIASM =  \
	syscall.o \
	exit.o \
	fork.o \
	read.o \
	write.o \
	open.o \
	close.o \
	wait4.o \
	link.o \
	unlink.o \
	chdir.o \
	fchdir.o \
	mknod.o \
	chmod.o \
	chown.o \
	break.o \
	freebsd4_getfsstat.o \
	getpid.o \
	mount.o \
	unmount.o \
	setuid.o \
	getuid.o \
	geteuid.o \
	ptrace.o \
	recvmsg.o \
	sendmsg.o \
	recvfrom.o \
	accept.o \
	getpeername.o \
	getsockname.o \
	access.o \
	chflags.o \
	fchflags.o \
	sync.o \
	kill.o \
	getppid.o \
	dup.o \
	pipe.o \
	getegid.o \
	profil.o \
	ktrace.o \
	getgid.o \
	getlogin.o \
	setlogin.o \
	acct.o \
	sigaltstack.o \
	ioctl.o \
	reboot.o \
	revoke.o \
	symlink.o \
	readlink.o \
	execve.o \
	umask.o \
	chroot.o \
	msync.o \
	vfork.o \
	sbrk.o \
	sstk.o \
	vadvise.o \
	munmap.o \
	mprotect.o \
	madvise.o \
	mincore.o \
	getgroups.o \
	setgroups.o \
	getpgrp.o \
	setpgid.o \
	setitimer.o \
	swapon.o \
	getitimer.o \
	getdtablesize.o \
	dup2.o \
	fcntl.o \
	select.o \
	fsync.o \
	setpriority.o \
	socket.o \
	connect.o \
	getpriority.o \
	bind.o \
	setsockopt.o \
	listen.o \
	gettimeofday.o \
	getrusage.o \
	getsockopt.o \
	readv.o \
	writev.o \
	settimeofday.o \
	fchown.o \
	fchmod.o \
	setreuid.o \
	setregid.o \
	rename.o \
	flock.o \
	mkfifo.o \
	sendto.o \
	shutdown.o \
	socketpair.o \
	mkdir.o \
	rmdir.o \
	utimes.o \
	adjtime.o \
	setsid.o \
	quotactl.o \
	nlm_syscall.o \
	nfssvc.o \
	freebsd4_statfs.o \
	freebsd4_fstatfs.o \
	lgetfh.o \
	getfh.o \
	freebsd4_getdomainname.o \
	freebsd4_setdomainname.o \
	freebsd4_uname.o \
	sysarch.o \
	rtprio.o \
	semsys.o \
	msgsys.o \
	shmsys.o \
	freebsd6_pread.o \
	freebsd6_pwrite.o \
	setfib.o \
	ntp_adjtime.o \
	setgid.o \
	setegid.o \
	seteuid.o \
	stat.o \
	fstat.o \
	lstat.o \
	pathconf.o \
	fpathconf.o \
	getrlimit.o \
	setrlimit.o \
	getdirentries.o \
	freebsd6_mmap.o \
	__syscall.o \
	freebsd6_lseek.o \
	freebsd6_truncate.o \
	freebsd6_ftruncate.o \
	__sysctl.o \
	mlock.o \
	munlock.o \
	undelete.o \
	futimes.o \
	getpgid.o \
	poll.o \
	freebsd7___semctl.o \
	semget.o \
	semop.o \
	freebsd7_msgctl.o \
	msgget.o \
	msgsnd.o \
	msgrcv.o \
	shmat.o \
	freebsd7_shmctl.o \
	shmdt.o \
	shmget.o \
	clock_gettime.o \
	clock_settime.o \
	clock_getres.o \
	ktimer_create.o \
	ktimer_delete.o \
	ktimer_settime.o \
	ktimer_gettime.o \
	ktimer_getoverrun.o \
	nanosleep.o \
	ntp_gettime.o \
	minherit.o \
	rfork.o \
	openbsd_poll.o \
	issetugid.o \
	lchown.o \
	aio_read.o \
	aio_write.o \
	lio_listio.o \
	getdents.o \
	lchmod.o \
	netbsd_lchown.o \
	lutimes.o \
	netbsd_msync.o \
	nstat.o \
	nfstat.o \
	nlstat.o \
	preadv.o \
	pwritev.o \
	freebsd4_fhstatfs.o \
	fhopen.o \
	fhstat.o \
	modnext.o \
	modstat.o \
	modfnext.o \
	modfind.o \
	kldload.o \
	kldunload.o \
	kldfind.o \
	kldnext.o \
	kldstat.o \
	kldfirstmod.o \
	getsid.o \
	setresuid.o \
	setresgid.o \
	aio_return.o \
	aio_suspend.o \
	aio_cancel.o \
	aio_error.o \
	oaio_read.o \
	oaio_write.o \
	olio_listio.o \
	yield.o \
	mlockall.o \
	munlockall.o \
	__getcwd.o \
	sched_setparam.o \
	sched_getparam.o \
	sched_setscheduler.o \
	sched_getscheduler.o \
	sched_yield.o \
	sched_get_priority_max.o \
	sched_get_priority_min.o \
	sched_rr_get_interval.o \
	utrace.o \
	freebsd4_sendfile.o \
	kldsym.o \
	jail.o \
	nnpfs_syscall.o \
	sigprocmask.o \
	sigsuspend.o \
	freebsd4_sigaction.o \
	sigpending.o \
	freebsd4_sigreturn.o \
	sigtimedwait.o \
	sigwaitinfo.o \
	__acl_get_file.o \
	__acl_set_file.o \
	__acl_get_fd.o \
	__acl_set_fd.o \
	__acl_delete_file.o \
	__acl_delete_fd.o \
	__acl_aclcheck_file.o \
	__acl_aclcheck_fd.o \
	extattrctl.o \
	extattr_set_file.o \
	extattr_get_file.o \
	extattr_delete_file.o \
	aio_waitcomplete.o \
	getresuid.o \
	getresgid.o \
	kqueue.o \
	kevent.o \
	extattr_set_fd.o \
	extattr_get_fd.o \
	extattr_delete_fd.o \
	__setugid.o \
	eaccess.o \
	afs3_syscall.o \
	nmount.o \
	__mac_get_proc.o \
	__mac_set_proc.o \
	__mac_get_fd.o \
	__mac_get_file.o \
	__mac_set_fd.o \
	__mac_set_file.o \
	kenv.o \
	lchflags.o \
	uuidgen.o \
	sendfile.o \
	mac_syscall.o \
	getfsstat.o \
	statfs.o \
	fstatfs.o \
	fhstatfs.o \
	ksem_close.o \
	ksem_post.o \
	ksem_wait.o \
	ksem_trywait.o \
	ksem_init.o \
	ksem_open.o \
	ksem_unlink.o \
	ksem_getvalue.o \
	ksem_destroy.o \
	__mac_get_pid.o \
	__mac_get_link.o \
	__mac_set_link.o \
	extattr_set_link.o \
	extattr_get_link.o \
	extattr_delete_link.o \
	__mac_execve.o \
	sigaction.o \
	sigreturn.o \
	getcontext.o \
	setcontext.o \
	swapcontext.o \
	swapoff.o \
	__acl_get_link.o \
	__acl_set_link.o \
	__acl_delete_link.o \
	__acl_aclcheck_link.o \
	sigwait.o \
	thr_create.o \
	thr_exit.o \
	thr_self.o \
	thr_kill.o \
	_umtx_lock.o \
	_umtx_unlock.o \
	jail_attach.o \
	extattr_list_fd.o \
	extattr_list_file.o \
	extattr_list_link.o \
	ksem_timedwait.o \
	thr_suspend.o \
	thr_wake.o \
	kldunloadf.o \
	audit.o \
	auditon.o \
	getauid.o \
	setauid.o \
	getaudit.o \
	setaudit.o \
	getaudit_addr.o \
	setaudit_addr.o \
	auditctl.o \
	_umtx_op.o \
	thr_new.o \
	sigqueue.o \
	kmq_open.o \
	kmq_setattr.o \
	kmq_timedreceive.o \
	kmq_timedsend.o \
	kmq_notify.o \
	kmq_unlink.o \
	abort2.o \
	thr_set_name.o \
	aio_fsync.o \
	rtprio_thread.o \
	sctp_peeloff.o \
	sctp_generic_sendmsg.o \
	sctp_generic_sendmsg_iov.o \
	sctp_generic_recvmsg.o \
	pread.o \
	pwrite.o \
	mmap.o \
	lseek.o \
	truncate.o \
	ftruncate.o \
	thr_kill2.o \
	shm_open.o \
	shm_unlink.o \
	cpuset.o \
	cpuset_setid.o \
	cpuset_getid.o \
	cpuset_getaffinity.o \
	cpuset_setaffinity.o \
	faccessat.o \
	fchmodat.o \
	fchownat.o \
	fexecve.o \
	fstatat.o \
	futimesat.o \
	linkat.o \
	mkdirat.o \
	mkfifoat.o \
	mknodat.o \
	openat.o \
	readlinkat.o \
	renameat.o \
	symlinkat.o \
	unlinkat.o \
	posix_openpt.o \
	gssd_syscall.o \
	jail_get.o \
	jail_set.o \
	jail_remove.o \
	closefrom.o \
	__semctl.o \
	msgctl.o \
	shmctl.o \
	lpathconf.o \
	cap_new.o \
	cap_getrights.o \
	cap_enter.o \
	cap_getmode.o \
	pdfork.o \
	pdkill.o \
	pdgetpid.o \
	pselect.o \
	getloginclass.o \
	setloginclass.o \
	rctl_get_racct.o \
	rctl_get_rules.o \
	rctl_get_limits.o \
	rctl_add_rule.o \
	rctl_remove_rule.o \
	posix_fallocate.o \
	posix_fadvise.o
