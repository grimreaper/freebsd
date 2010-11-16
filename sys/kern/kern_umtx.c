/*-
 * Copyright (c) 2004, David Xu <davidxu@freebsd.org>
 * Copyright (c) 2002, Jeffrey Roberson <jeff@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
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

#include "opt_compat.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/sysproto.h>
#include <sys/syscallsubr.h>
#include <sys/eventhandler.h>
#include <sys/umtx.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>

#include <machine/cpu.h>

#ifdef COMPAT_FREEBSD32
#include <compat/freebsd32/freebsd32_proto.h>
#endif

enum {
	TYPE_SIMPLE_WAIT,
	TYPE_CV,
	TYPE_SEM,
	TYPE_SIMPLE_LOCK,
	TYPE_NORMAL_UMUTEX,
	TYPE_PI_UMUTEX,
	TYPE_PP_UMUTEX,
	TYPE_RWLOCK
};

#define _UMUTEX_TRY		1
#define _UMUTEX_WAIT		2

/* Key to represent a unique userland synchronous object */
struct umtx_key {
	int	hash;
	int	type;
	int	shared;
	union {
		struct {
			vm_object_t	object;
			uintptr_t	offset;
		} shared;
		struct {
			struct vmspace	*vs;
			uintptr_t	addr;
		} private;
		struct {
			void		*a;
			uintptr_t	b;
		} both;
	} info;
	struct umtxq_chain	* volatile chain;
};

/* Priority inheritance mutex info. */
struct umtx_pi {
	/* Owner thread */
	struct thread		*pi_owner;

	/* Reference count */
	int			pi_refcount;

 	/* List entry to link umtx holding by thread */
	TAILQ_ENTRY(umtx_pi)	pi_link;

	/* List entry in hash */
	TAILQ_ENTRY(umtx_pi)	pi_hashlink;

	/* List for waiters */
	TAILQ_HEAD(,umtx_q)	pi_blocked;

	/* Identify a userland lock object */
	struct umtx_key		pi_key;
};

struct robust_info {
	struct thread			*ownertd;
	SLIST_ENTRY(robust_info)	hash_qe;
	LIST_ENTRY(robust_info)		td_qe;
	struct umutex			*umtxp;
};

SLIST_HEAD(robust_hashlist, robust_info);
LIST_HEAD(robust_list, robust_info);

/* A userland synchronous object user. */
struct umtx_q {
	/* Linked list for the hash. */
	TAILQ_ENTRY(umtx_q)	uq_link;

	/* Umtx key. */
	struct umtx_key		uq_key;

	/* Umtx flags. */
	int			uq_flags;
#define UQF_UMTXQ	0x0001

	/* The thread waits on. */
	struct thread		*uq_thread;

	/*
	 * Blocked on PI mutex. read can use chain lock
	 * or umtx_lock, write must have both chain lock and
	 * umtx_lock being hold.
	 */
	struct umtx_pi		*uq_pi_blocked;

	/* On blocked list */
	TAILQ_ENTRY(umtx_q)	uq_lockq;

	/* Thread contending with us */
	TAILQ_HEAD(,umtx_pi)	uq_pi_contested;

	/* Inherited priority from PP mutex */
	u_char			uq_inherited_pri;
	
	/* Spare queue ready to be reused */
	struct umtxq_queue	*uq_spare_queue;

	/* The queue we on */
	struct umtxq_queue	*uq_cur_queue;

	int			uq_repair_mutex;

	/* Robust mutex list */
	struct	robust_list	uq_rob_list;

	/* Thread is exiting. */
	char			uq_exiting;
};

TAILQ_HEAD(umtxq_head, umtx_q);

/* Per-key wait-queue */
struct umtxq_queue {
	struct umtxq_head	head;
	struct umtx_key		key;
	LIST_ENTRY(umtxq_queue)	link;
	int			length;

	int			binding;
	struct umutex		*bind_mutex;
	struct umtx_key		bind_mkey;
};

LIST_HEAD(umtxq_list, umtxq_queue);

/* Userland lock object's wait-queue chain */
struct umtxq_chain {
	/* Lock for this chain. */
	struct mtx		uc_lock;

	/* List of sleep queues. */
	struct umtxq_list	uc_queue[2];
#define UMTX_SHARED_QUEUE	0
#define UMTX_EXCLUSIVE_QUEUE	1

	LIST_HEAD(, umtxq_queue) uc_spare_queue;

	/* Busy flag */
	volatile char		uc_busy;

	/* Chain lock waiters */
	int			uc_waiters;

	/* All PI in the list */
	TAILQ_HEAD(,umtx_pi)	uc_pi_list;

};

struct robust_chain {
	/* Lock for this chain. */
	struct mtx		lock;
	struct robust_hashlist	rob_list;
};


#define	UMTXQ_LOCKED_ASSERT(uc)		mtx_assert(&(uc)->uc_lock, MA_OWNED)
#define	UMTXQ_BUSY_ASSERT(uc)	KASSERT(&(uc)->uc_busy, ("umtx chain is not busy"))

/*
 * Don't propagate time-sharing priority, there is a security reason,
 * a user can simply introduce PI-mutex, let thread A lock the mutex,
 * and let another thread B block on the mutex, because B is
 * sleeping, its priority will be boosted, this causes A's priority to
 * be boosted via priority propagating too and will never be lowered even
 * if it is using 100%CPU, this is unfair to other processes.
 */

#define UPRI(td)	(((td)->td_user_pri >= PRI_MIN_TIMESHARE &&\
			  (td)->td_user_pri <= PRI_MAX_TIMESHARE) ?\
			 PRI_MAX_TIMESHARE : (td)->td_user_pri)

#define	GOLDEN_RATIO_PRIME	2654404609U
#define	UMTX_CHAINS		128
#define	UMTX_SHIFTS		(__WORD_BIT - 7)

#define THREAD_SHARE		0
#define PROCESS_SHARE		1
#define AUTO_SHARE		2

#define	GET_SHARE(flags)	\
    (((flags) & USYNC_PROCESS_SHARED) == 0 ? THREAD_SHARE : PROCESS_SHARE)

#define BUSY_SPINS		200

#define	ROBUST_CHAINS		128
#define	ROBUST_SHIFTS		(__WORD_BIT - 7)

static uma_zone_t		umtx_pi_zone;
static uma_zone_t		robust_zone;
static struct umtxq_chain	umtxq_chains[2][UMTX_CHAINS];
static MALLOC_DEFINE(M_UMTX, "umtx", "UMTX queue memory");
static int			umtx_pi_allocated;
#ifdef SMP
static int			umtx_cvsig_migrate = 0;
#else
static int			umtx_cvsig_migrate = 1;
#endif

static struct robust_chain	robust_chains[ROBUST_CHAINS];
static int	set_max_robust(SYSCTL_HANDLER_ARGS);

SYSCTL_NODE(_debug, OID_AUTO, umtx, CTLFLAG_RW, 0, "umtx debug");
SYSCTL_INT(_debug_umtx, OID_AUTO, umtx_pi_allocated, CTLFLAG_RD,
    &umtx_pi_allocated, 0, "Allocated umtx_pi");

SYSCTL_INT(_debug_umtx, OID_AUTO, umtx_cvsig_migrate, CTLFLAG_RW,
    &umtx_cvsig_migrate, 0, "cvsig migrate");

SYSCTL_PROC(_debug_umtx, OID_AUTO, max_robust_per_proc,
	CTLTYPE_INT | CTLFLAG_RW, 0, sizeof(int), set_max_robust, "I",
	"Set maximum number of robust mutex");

static int		max_robust_per_proc = 1500;
static struct mtx	max_robust_lock;
static struct timeval	max_robust_lasttime;
static struct timeval	max_robust_interval;

#define UMTX_STATE
#ifdef UMTX_STATE
static int			umtx_cv_broadcast_migrate;
static int			umtx_cv_signal_migrate;
static int			umtx_cv_insert_failure;
static int			umtx_cv_unlock_failure;
static int			umtx_timedlock_count;
SYSCTL_INT(_debug_umtx, OID_AUTO, umtx_cv_broadcast_migrate, CTLFLAG_RD,
    &umtx_cv_broadcast_migrate, 0, "cv_broadcast thread migrated");
SYSCTL_INT(_debug_umtx, OID_AUTO, umtx_cv_signal_migrate, CTLFLAG_RD,
    &umtx_cv_signal_migrate, 0, "cv_signal  thread migrated");
SYSCTL_INT(_debug_umtx, OID_AUTO, umtx_cv_insert_failure, CTLFLAG_RD,
    &umtx_cv_insert_failure, 0, "cv_wait failure");
SYSCTL_INT(_debug_umtx, OID_AUTO, umtx_cv_unlock_failure, CTLFLAG_RD,
    &umtx_cv_unlock_failure, 0, "cv_wait unlock mutex failure");
SYSCTL_INT(_debug_umtx, OID_AUTO, umtx_timedlock_count, CTLFLAG_RD,
    &umtx_timedlock_count, 0, "umutex timedlock count");
#define UMTX_STATE_INC(var)		umtx_##var++
#define UMTX_STATE_ADD(var, val)	(umtx_##var += (val))
#else
#define UMTX_STATE_INC(var)
#endif

static void umtxq_sysinit(void *);
static void umtxq_hash(struct umtx_key *key);
static struct umtxq_chain *umtxq_getchain(struct umtx_key *key);
static void umtxq_lock(struct umtx_key *key);
static void umtxq_unlock(struct umtx_key *key);
static void umtxq_busy(struct umtx_key *key);
static void umtxq_unbusy(struct umtx_key *key);
static void umtxq_insert_queue(struct umtx_q *, int);
static int umtxq_insert_queue2(struct umtx_q *, int, struct umutex *,
	const struct umtx_key *);
static void umtxq_remove_queue(struct umtx_q *uq, int q);
static int umtxq_sleep(struct umtx_q *uq, const char *wmesg, int timo);
static int umtxq_count(struct umtx_key *key);
static int umtx_key_match(const struct umtx_key *k1, const struct umtx_key *k2);
static int umtx_key_get(void *addr, int type, int share,
	struct umtx_key *key);
static void umtx_key_release(struct umtx_key *key);
static struct umtx_pi *umtx_pi_alloc(int);
static void umtx_pi_free(struct umtx_pi *pi);
static void umtx_pi_adjust_locked(struct thread *td, u_char oldpri);
static int do_unlock_pp(struct thread *, struct umutex *, uint32_t, int);
static void umtx_thread_cleanup(struct thread *);
static void umtx_exec_hook(void *arg __unused, struct proc *p __unused,
	struct image_params *imgp __unused);
static void umtx_exit_hook(void *arg __unused, struct proc *p __unused);
static void umtx_fork_hook(void *arg __unused, struct proc *p1 __unused,
	struct proc *p2, int flags __unused);
static int robust_alloc(struct robust_info **);
static void robust_free(struct robust_info *);
static int robust_insert(struct thread *, struct robust_info *);
static void robust_remove(struct thread *, struct umutex *);
static int do_unlock_umutex(struct thread *, struct umutex *, int);

SYSINIT(umtx, SI_SUB_EVENTHANDLER+1, SI_ORDER_MIDDLE, umtxq_sysinit, NULL);

#define umtxq_signal(key, nwake)	umtxq_signal_queue((key), (nwake), UMTX_SHARED_QUEUE)
#define umtxq_insert(uq)	umtxq_insert_queue((uq), UMTX_SHARED_QUEUE)
#define umtxq_remove(uq)	umtxq_remove_queue((uq), UMTX_SHARED_QUEUE)

static struct mtx umtx_lock;

static void
umtxq_sysinit(void *arg __unused)
{
	int i, j;

	umtx_pi_zone = uma_zcreate("umtx pi", sizeof(struct umtx_pi),
		NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	robust_zone = uma_zcreate("robust umtx", sizeof(struct robust_info),
		NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	for (i = 0; i < 2; ++i) {
		for (j = 0; j < UMTX_CHAINS; ++j) {
			mtx_init(&umtxq_chains[i][j].uc_lock, "umtxql", NULL,
				 MTX_DEF | MTX_DUPOK);
			LIST_INIT(&umtxq_chains[i][j].uc_queue[0]);
			LIST_INIT(&umtxq_chains[i][j].uc_queue[1]);
			LIST_INIT(&umtxq_chains[i][j].uc_spare_queue);
			TAILQ_INIT(&umtxq_chains[i][j].uc_pi_list);
			umtxq_chains[i][j].uc_busy = 0;
			umtxq_chains[i][j].uc_waiters = 0;
		}
	}

	for (i = 0; i < ROBUST_CHAINS; ++i) {
		mtx_init(&robust_chains[i].lock, "robql", NULL,
			 MTX_DEF | MTX_DUPOK);
		SLIST_INIT(&robust_chains[i].rob_list);
	}

	mtx_init(&umtx_lock, "umtx lock", NULL, MTX_SPIN);
	mtx_init(&max_robust_lock, "max robust lock", NULL, MTX_DEF);
	EVENTHANDLER_REGISTER(process_exec, umtx_exec_hook, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(process_exit, umtx_exit_hook, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(process_fork, umtx_fork_hook, NULL,
	    EVENTHANDLER_PRI_ANY);

	max_robust_interval.tv_sec = 10;
	max_robust_interval.tv_usec = 0;
}

struct umtx_q *
umtxq_alloc(void)
{
	struct umtx_q *uq;

	uq = malloc(sizeof(struct umtx_q), M_UMTX, M_WAITOK | M_ZERO);
	uq->uq_spare_queue = malloc(sizeof(struct umtxq_queue), M_UMTX,
		M_WAITOK | M_ZERO);
	TAILQ_INIT(&uq->uq_spare_queue->head);
	TAILQ_INIT(&uq->uq_pi_contested);
	LIST_INIT(&uq->uq_rob_list);
	uq->uq_inherited_pri = PRI_MAX;
	return (uq);
}

void
umtxq_free(struct umtx_q *uq)
{
	MPASS(uq->uq_spare_queue != NULL);
	free(uq->uq_spare_queue, M_UMTX);
	free(uq, M_UMTX);
}

static inline void
umtxq_hash(struct umtx_key *key)
{
	unsigned n = (uintptr_t)key->info.both.a + key->info.both.b;
	key->hash = ((n * GOLDEN_RATIO_PRIME) >> UMTX_SHIFTS) % UMTX_CHAINS;
}

static inline int
umtx_key_match(const struct umtx_key *k1, const struct umtx_key *k2)
{
	return (k1->type == k2->type &&
		k1->info.both.a == k2->info.both.a &&
	        k1->info.both.b == k2->info.both.b);
}

static inline void
umtx_key_copy(struct umtx_key *k1, const struct umtx_key *k2)
{
	k1->hash = k2->hash;
	k1->type = k2->type;
	k1->shared = k2->shared;
	k1->info.both = k2->info.both;
	k1->chain = k2->chain;
}

static inline struct umtxq_chain *
umtxq_calcchain(struct umtx_key *key)
{
	if (key->type <= TYPE_SEM)
		return (&umtxq_chains[1][key->hash]);
	return (&umtxq_chains[0][key->hash]);
}

static inline struct umtxq_chain *
umtxq_getchain(struct umtx_key *key)
{
	return (key->chain);
}

/*
 * Lock a chain.
 */
static inline void
umtxq_lock(struct umtx_key *key)
{
	struct umtxq_chain *uc;

	for (;;) {
		uc = key->chain;
		mtx_lock(&uc->uc_lock);
		if (key->chain != uc)
			mtx_unlock(&uc->uc_lock);
		else
			break;
	}
}

/*
 * Unlock a chain.
 */
static inline void
umtxq_unlock(struct umtx_key *key)
{
	mtx_unlock(&key->chain->uc_lock);
}

/*
 * Set chain to busy state when following operation
 * may be blocked (kernel mutex can not be used).
 */
static inline void
umtxq_busy(struct umtx_key *key)
{
	struct umtxq_chain *uc;

	uc = umtxq_getchain(key);
	mtx_assert(&uc->uc_lock, MA_OWNED);
	if (uc->uc_busy) {
#ifdef SMP
		if (smp_cpus > 1) {
			int count = BUSY_SPINS;
			if (count > 0) {
				umtxq_unlock(key);
				while (uc->uc_busy && --count > 0) {
					cpu_spinwait();
					uc = key->chain;
				}
				umtxq_lock(key);
			}
		}
#endif
		while (uc->uc_busy) {
			uc->uc_waiters++;
			msleep(uc, &uc->uc_lock, 0, "umtxqb", 0);
			uc->uc_waiters--;
			mtx_unlock(&uc->uc_lock);
			umtxq_lock(key);
			uc = umtxq_getchain(key);
		}
	}
	uc->uc_busy = 1;
}

/*
 * Unbusy a chain.
 */
static inline void
umtxq_unbusy(struct umtx_key *key)
{
	struct umtxq_chain *uc;

	uc = umtxq_getchain(key);
	mtx_assert(&uc->uc_lock, MA_OWNED);
	KASSERT(uc->uc_busy != 0, ("not busy"));
	uc->uc_busy = 0;
	if (uc->uc_waiters)
		wakeup_one(uc);
}

static struct umtxq_queue *
umtxq_queue_lookup(struct umtx_key *key, int q)
{
	struct umtxq_queue *uh;
	struct umtxq_chain *uc;

	uc = umtxq_getchain(key);
	UMTXQ_LOCKED_ASSERT(uc);
	LIST_FOREACH(uh, &uc->uc_queue[q], link) {
		if (umtx_key_match(&uh->key, key))
			return (uh);
	}

	return (NULL);
}

static inline void
umtxq_insert_queue(struct umtx_q *uq, int q)
{
	int error;

	error = umtxq_insert_queue2(uq, q, NULL, NULL);
	MPASS(error == 0);
}

static inline int
umtxq_insert_queue2(struct umtx_q *uq, int q, struct umutex *m,
	const struct umtx_key *mkey)
{
	struct umtxq_queue *uh;
	struct umtxq_chain *uc;

	uc = umtxq_getchain(&uq->uq_key);
	UMTXQ_LOCKED_ASSERT(uc);
	KASSERT((uq->uq_flags & UQF_UMTXQ) == 0, 
		("umtx_q is already on queue"));
	uh = umtxq_queue_lookup(&uq->uq_key, q);
	if (uh != NULL) {
		if (uh->binding) {
			if (mkey == NULL ||
			    !umtx_key_match(&uh->bind_mkey, mkey))
				return (EEXIST);
		} else {
			if (mkey != NULL)
				return (EEXIST);
		}
		LIST_INSERT_HEAD(&uc->uc_spare_queue, uq->uq_spare_queue, link);
	} else {
		uh = uq->uq_spare_queue;
		uh->key = uq->uq_key;
		LIST_INSERT_HEAD(&uc->uc_queue[q], uh, link);
		uh->bind_mutex = m;
		uh->length = 0;
		if (mkey != NULL) {
			uh->binding = 1;
			uh->bind_mkey = *mkey;
		} else {
			uh->binding = 0;
		}
	}
	uq->uq_spare_queue = NULL;

	TAILQ_INSERT_TAIL(&uh->head, uq, uq_link);
	uh->length++;
	uq->uq_flags |= UQF_UMTXQ;
	uq->uq_cur_queue = uh;
	return (0);
}

static inline void
umtxq_remove_queue(struct umtx_q *uq, int q)
{
	struct umtxq_chain *uc;
	struct umtxq_queue *uh;

	uc = umtxq_getchain(&uq->uq_key);
	UMTXQ_LOCKED_ASSERT(uc);
	if (uq->uq_flags & UQF_UMTXQ) {
		uh = uq->uq_cur_queue;
		TAILQ_REMOVE(&uh->head, uq, uq_link);
		uh->length--;
		uq->uq_flags &= ~UQF_UMTXQ;
		if (TAILQ_EMPTY(&uh->head)) {
			KASSERT(uh->length == 0,
			    ("inconsistent umtxq_queue length"));
			LIST_REMOVE(uh, link);
		} else {
			uh = LIST_FIRST(&uc->uc_spare_queue);
			KASSERT(uh != NULL, ("uc_spare_queue is empty"));
			LIST_REMOVE(uh, link);
			uh->bind_mutex = NULL;
			uh->binding = 0;
		}
		uq->uq_spare_queue = uh;
		uq->uq_cur_queue = NULL;
	}
}

/*
 * Check if there are multiple waiters
 */
static int
umtxq_count(struct umtx_key *key)
{
	struct umtxq_chain *uc;
	struct umtxq_queue *uh;

	uc = umtxq_getchain(key);
	UMTXQ_LOCKED_ASSERT(uc);
	uh = umtxq_queue_lookup(key, UMTX_SHARED_QUEUE);
	if (uh != NULL)
		return (uh->length);
	return (0);
}

/*
 * Check if there are multiple PI waiters and returns first
 * waiter.
 */
static int
umtxq_count_pi(struct umtx_key *key, struct umtx_q **first)
{
	struct umtxq_chain *uc;
	struct umtxq_queue *uh;

	*first = NULL;
	uc = umtxq_getchain(key);
	UMTXQ_LOCKED_ASSERT(uc);
	uh = umtxq_queue_lookup(key, UMTX_SHARED_QUEUE);
	if (uh != NULL) {
		*first = TAILQ_FIRST(&uh->head);
		return (uh->length);
	}
	return (0);
}

/*
 * Wake up threads waiting on an userland object.
 */

static int
umtxq_signal_queue(struct umtx_key *key, int n_wake, int q)
{
	struct umtxq_chain *uc;
	struct umtxq_queue *uh;
	struct umtx_q *uq;
	int ret;

	ret = 0;
	uc = umtxq_getchain(key);
	UMTXQ_LOCKED_ASSERT(uc);
	uh = umtxq_queue_lookup(key, q);
	if (uh != NULL) {
		while ((uq = TAILQ_FIRST(&uh->head)) != NULL) {
			umtxq_remove_queue(uq, q);
			wakeup(uq);
			if (++ret >= n_wake)
				return (ret);
		}
	}
	return (ret);
}


/*
 * Wake up specified thread.
 */
static inline void
umtxq_signal_thread(struct umtx_q *uq)
{
	struct umtxq_chain *uc;

	uc = umtxq_getchain(&uq->uq_key);
	UMTXQ_LOCKED_ASSERT(uc);
	umtxq_remove(uq);
	wakeup(uq);
}

/*
 * Put thread into sleep state, before sleeping, check if
 * thread was removed from umtx queue.
 */
static inline int
umtxq_sleep(struct umtx_q *uq, const char *wmesg, int timo)
{
	struct umtxq_chain *uc;
	int error;

	uc = umtxq_getchain(&uq->uq_key);
	UMTXQ_LOCKED_ASSERT(uc);
	if (!(uq->uq_flags & UQF_UMTXQ))
		return (0);
	error = msleep(uq, &uc->uc_lock, PCATCH|PDROP, wmesg, timo);
	if (error == EWOULDBLOCK)
		error = ETIMEDOUT;
	umtxq_lock(&uq->uq_key);
	return (error);
}

/*
 * Convert userspace address into unique logical address.
 */
static int
umtx_key_get(void *addr, int type, int share, struct umtx_key *key)
{
	struct thread *td = curthread;
	vm_map_t map;
	vm_map_entry_t entry;
	vm_pindex_t pindex;
	vm_prot_t prot;
	boolean_t wired;

	key->type = type;
	key->chain = NULL;
	if (share == THREAD_SHARE) {
		key->shared = 0;
		key->info.private.vs = td->td_proc->p_vmspace;
		key->info.private.addr = (uintptr_t)addr;
	} else {
		MPASS(share == PROCESS_SHARE || share == AUTO_SHARE);
		map = &td->td_proc->p_vmspace->vm_map;
		if (vm_map_lookup(&map, (vm_offset_t)addr, VM_PROT_WRITE,
		    &entry, &key->info.shared.object, &pindex, &prot,
		    &wired) != KERN_SUCCESS) {
			return EFAULT;
		}

		if ((share == PROCESS_SHARE) ||
		    (share == AUTO_SHARE &&
		     VM_INHERIT_SHARE == entry->inheritance)) {
			key->shared = 1;
			key->info.shared.offset = entry->offset + entry->start -
				(vm_offset_t)addr;
			vm_object_reference(key->info.shared.object);
		} else {
			key->shared = 0;
			key->info.private.vs = td->td_proc->p_vmspace;
			key->info.private.addr = (uintptr_t)addr;
		}
		vm_map_lookup_done(map, entry);
	}

	umtxq_hash(key);
	key->chain = umtxq_calcchain(key);
	return (0);
}

/*
 * Release key.
 */
static inline void
umtx_key_release(struct umtx_key *key)
{
	if (key->shared)
		vm_object_deallocate(key->info.shared.object);
}

/*
 * Lock a umtx object.
 */
static int
_do_lock_umtx(struct thread *td, struct umtx *umtx, u_long id, int timo)
{
	struct umtx_q *uq;
	u_long owner;
	u_long old;
	int error = 0;

	uq = td->td_umtxq;

	/*
	 * Care must be exercised when dealing with umtx structure. It
	 * can fault on any access.
	 */
	for (;;) {
		/*
		 * Try the uncontested case.  This should be done in userland.
		 */
		owner = casuword(&umtx->u_owner, UMTX_UNOWNED, id);

		/* The acquire succeeded. */
		if (owner == UMTX_UNOWNED)
			return (0);

		/* The address was invalid. */
		if (owner == -1)
			return (EFAULT);

		/* If no one owns it but it is contested try to acquire it. */
		if (owner == UMTX_CONTESTED) {
			owner = casuword(&umtx->u_owner,
			    UMTX_CONTESTED, id | UMTX_CONTESTED);

			if (owner == UMTX_CONTESTED)
				return (0);

			/* The address was invalid. */
			if (owner == -1)
				return (EFAULT);

			/* If this failed the lock has changed, restart. */
			continue;
		}

		/*
		 * If we caught a signal, we have retried and now
		 * exit immediately.
		 */
		if (error != 0)
			return (error);

		if ((error = umtx_key_get(umtx, TYPE_SIMPLE_LOCK,
			AUTO_SHARE, &uq->uq_key)) != 0)
			return (error);

		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_insert(uq);
		umtxq_unbusy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

		/*
		 * Set the contested bit so that a release in user space
		 * knows to use the system call for unlock.  If this fails
		 * either some one else has acquired the lock or it has been
		 * released.
		 */
		old = casuword(&umtx->u_owner, owner, owner | UMTX_CONTESTED);

		/* The address was invalid. */
		if (old == -1) {
			umtxq_lock(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unlock(&uq->uq_key);
			umtx_key_release(&uq->uq_key);
			return (EFAULT);
		}

		/*
		 * We set the contested bit, sleep. Otherwise the lock changed
		 * and we need to retry or we lost a race to the thread
		 * unlocking the umtx.
		 */
		umtxq_lock(&uq->uq_key);
		if (old == owner)
			error = umtxq_sleep(uq, "umtx", timo);
		umtxq_remove(uq);
		umtxq_unlock(&uq->uq_key);
		umtx_key_release(&uq->uq_key);
	}

	return (0);
}

/*
 * Lock a umtx object.
 */
static int
do_lock_umtx(struct thread *td, struct umtx *umtx, u_long id,
	struct timespec *timeout)
{
	struct timespec ts, ts2, ts3;
	struct timeval tv;
	int error;

	if (timeout == NULL) {
		error = _do_lock_umtx(td, umtx, id, 0);
		/* Mutex locking is restarted if it is interrupted. */
		if (error == EINTR)
			error = ERESTART;
	} else {
		getnanouptime(&ts);
		timespecadd(&ts, timeout);
		TIMESPEC_TO_TIMEVAL(&tv, timeout);
		for (;;) {
			error = _do_lock_umtx(td, umtx, id, tvtohz(&tv));
			if (error != ETIMEDOUT)
				break;
			getnanouptime(&ts2);
			if (timespeccmp(&ts2, &ts, >=)) {
				error = ETIMEDOUT;
				break;
			}
			ts3 = ts;
			timespecsub(&ts3, &ts2);
			TIMESPEC_TO_TIMEVAL(&tv, &ts3);
		}
		/* Timed-locking is not restarted. */
		if (error == ERESTART)
			error = EINTR;
	}
	return (error);
}

/*
 * Unlock a umtx object.
 */
static int
do_unlock_umtx(struct thread *td, struct umtx *umtx, u_long id)
{
	struct umtx_key key;
	u_long owner;
	u_long old;
	int error;
	int count;

	/*
	 * Make sure we own this mtx.
	 */
	owner = fuword(__DEVOLATILE(u_long *, &umtx->u_owner));
	if (owner == -1)
		return (EFAULT);

	if ((owner & ~UMTX_CONTESTED) != id)
		return (EPERM);

	/* This should be done in userland */
	if ((owner & UMTX_CONTESTED) == 0) {
		old = casuword(&umtx->u_owner, owner, UMTX_UNOWNED);
		if (old == -1)
			return (EFAULT);
		if (old == owner)
			return (0);
		owner = old;
	}

	/* We should only ever be in here for contested locks */
	if ((error = umtx_key_get(umtx, TYPE_SIMPLE_LOCK, AUTO_SHARE,
		&key)) != 0)
		return (error);

	umtxq_lock(&key);
	umtxq_busy(&key);
	count = umtxq_count(&key);
	umtxq_unlock(&key);

	/*
	 * When unlocking the umtx, it must be marked as unowned if
	 * there is zero or one thread only waiting for it.
	 * Otherwise, it must be marked as contested.
	 */
	old = casuword(&umtx->u_owner, owner,
		count <= 1 ? UMTX_UNOWNED : UMTX_CONTESTED);
	umtxq_lock(&key);
	umtxq_signal(&key,1);
	umtxq_unbusy(&key);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	if (old == -1)
		return (EFAULT);
	if (old != owner)
		return (EINVAL);
	return (0);
}

#ifdef COMPAT_FREEBSD32

/*
 * Lock a umtx object.
 */
static int
_do_lock_umtx32(struct thread *td, uint32_t *m, uint32_t id, int timo)
{
	struct umtx_q *uq;
	uint32_t owner;
	uint32_t old;
	int error = 0;

	uq = td->td_umtxq;

	/*
	 * Care must be exercised when dealing with umtx structure. It
	 * can fault on any access.
	 */
	for (;;) {
		/*
		 * Try the uncontested case.  This should be done in userland.
		 */
		owner = casuword32(m, UMUTEX_UNOWNED, id);

		/* The acquire succeeded. */
		if (owner == UMUTEX_UNOWNED)
			return (0);

		/* The address was invalid. */
		if (owner == -1)
			return (EFAULT);

		/* If no one owns it but it is contested try to acquire it. */
		if (owner == UMUTEX_CONTESTED) {
			owner = casuword32(m,
			    UMUTEX_CONTESTED, id | UMUTEX_CONTESTED);
			if (owner == UMUTEX_CONTESTED)
				return (0);

			/* The address was invalid. */
			if (owner == -1)
				return (EFAULT);

			/* If this failed the lock has changed, restart. */
			continue;
		}

		/*
		 * If we caught a signal, we have retried and now
		 * exit immediately.
		 */
		if (error != 0)
			return (error);

		if ((error = umtx_key_get(m, TYPE_SIMPLE_LOCK,
			AUTO_SHARE, &uq->uq_key)) != 0)
			return (error);

		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_insert(uq);
		umtxq_unbusy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

		/*
		 * Set the contested bit so that a release in user space
		 * knows to use the system call for unlock.  If this fails
		 * either some one else has acquired the lock or it has been
		 * released.
		 */
		old = casuword32(m, owner, owner | UMUTEX_CONTESTED);

		/* The address was invalid. */
		if (old == -1) {
			umtxq_lock(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unlock(&uq->uq_key);
			umtx_key_release(&uq->uq_key);
			return (EFAULT);
		}

		/*
		 * We set the contested bit, sleep. Otherwise the lock changed
		 * and we need to retry or we lost a race to the thread
		 * unlocking the umtx.
		 */
		umtxq_lock(&uq->uq_key);
		if (old == owner)
			error = umtxq_sleep(uq, "umtx", timo);
		umtxq_remove(uq);
		umtxq_unlock(&uq->uq_key);
		umtx_key_release(&uq->uq_key);
	}

	return (0);
}

/*
 * Lock a umtx object.
 */
static int
do_lock_umtx32(struct thread *td, void *m, uint32_t id,
	struct timespec *timeout)
{
	struct timespec ts, ts2, ts3;
	struct timeval tv;
	int error;

	if (timeout == NULL) {
		error = _do_lock_umtx32(td, m, id, 0);
		/* Mutex locking is restarted if it is interrupted. */
		if (error == EINTR)
			error = ERESTART;
	} else {
		getnanouptime(&ts);
		timespecadd(&ts, timeout);
		TIMESPEC_TO_TIMEVAL(&tv, timeout);
		for (;;) {
			error = _do_lock_umtx32(td, m, id, tvtohz(&tv));
			if (error != ETIMEDOUT)
				break;
			getnanouptime(&ts2);
			if (timespeccmp(&ts2, &ts, >=)) {
				error = ETIMEDOUT;
				break;
			}
			ts3 = ts;
			timespecsub(&ts3, &ts2);
			TIMESPEC_TO_TIMEVAL(&tv, &ts3);
		}
		/* Timed-locking is not restarted. */
		if (error == ERESTART)
			error = EINTR;
	}
	return (error);
}

/*
 * Unlock a umtx object.
 */
static int
do_unlock_umtx32(struct thread *td, uint32_t *m, uint32_t id)
{
	struct umtx_key key;
	uint32_t owner;
	uint32_t old;
	int error;
	int count;

	/*
	 * Make sure we own this mtx.
	 */
	owner = fuword32(m);
	if (owner == -1)
		return (EFAULT);

	if ((owner & ~UMUTEX_CONTESTED) != id)
		return (EPERM);

	/* This should be done in userland */
	if ((owner & UMUTEX_CONTESTED) == 0) {
		old = casuword32(m, owner, UMUTEX_UNOWNED);
		if (old == -1)
			return (EFAULT);
		if (old == owner)
			return (0);
		owner = old;
	}

	/* We should only ever be in here for contested locks */
	if ((error = umtx_key_get(m, TYPE_SIMPLE_LOCK, AUTO_SHARE,
		&key)) != 0)
		return (error);

	umtxq_lock(&key);
	umtxq_busy(&key);
	count = umtxq_count(&key);
	umtxq_unlock(&key);

	/*
	 * When unlocking the umtx, it must be marked as unowned if
	 * there is zero or one thread only waiting for it.
	 * Otherwise, it must be marked as contested.
	 */
	old = casuword32(m, owner,
		count <= 1 ? UMUTEX_UNOWNED : UMUTEX_CONTESTED);
	umtxq_lock(&key);
	umtxq_signal(&key,1);
	umtxq_unbusy(&key);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	if (old == -1)
		return (EFAULT);
	if (old != owner)
		return (EINVAL);
	return (0);
}
#endif

/*
 * Fetch and compare value, sleep on the address if value is not changed.
 */
static int
do_wait(struct thread *td, void *addr, u_long id,
	struct timespec *timeout, int compat32, int is_private)
{
	struct umtx_q *uq;
	struct timespec ts, ts2, ts3;
	struct timeval tv;
	u_long tmp;
	int error = 0;

	uq = td->td_umtxq;
	if ((error = umtx_key_get(addr, TYPE_SIMPLE_WAIT,
		is_private ? THREAD_SHARE : AUTO_SHARE, &uq->uq_key)) != 0)
		return (error);

	umtxq_lock(&uq->uq_key);
	umtxq_insert(uq);
	umtxq_unlock(&uq->uq_key);
	if (compat32 == 0)
		tmp = fuword(addr);
        else
		tmp = (unsigned int)fuword32(addr);
	if (tmp != id) {
		umtxq_lock(&uq->uq_key);
		umtxq_remove(uq);
		umtxq_unlock(&uq->uq_key);
	} else if (timeout == NULL) {
		umtxq_lock(&uq->uq_key);
		error = umtxq_sleep(uq, "uwait", 0);
		umtxq_remove(uq);
		umtxq_unlock(&uq->uq_key);
	} else {
		getnanouptime(&ts);
		timespecadd(&ts, timeout);
		TIMESPEC_TO_TIMEVAL(&tv, timeout);
		umtxq_lock(&uq->uq_key);
		for (;;) {
			error = umtxq_sleep(uq, "uwait", tvtohz(&tv));
			if (!(uq->uq_flags & UQF_UMTXQ)) {
				error = 0;
				break;
			}
			if (error != ETIMEDOUT)
				break;
			umtxq_unlock(&uq->uq_key);
			getnanouptime(&ts2);
			if (timespeccmp(&ts2, &ts, >=)) {
				error = ETIMEDOUT;
				umtxq_lock(&uq->uq_key);
				break;
			}
			ts3 = ts;
			timespecsub(&ts3, &ts2);
			TIMESPEC_TO_TIMEVAL(&tv, &ts3);
			umtxq_lock(&uq->uq_key);
		}
		umtxq_remove(uq);
		umtxq_unlock(&uq->uq_key);
	}
	umtx_key_release(&uq->uq_key);
	if (error == ERESTART)
		error = EINTR;
	return (error);
}

/*
 * Wake up threads sleeping on the specified address.
 */
int
kern_umtx_wake(struct thread *td, void *uaddr, int n_wake, int is_private)
{
	struct umtx_key key;
	int ret;
	
	if ((ret = umtx_key_get(uaddr, TYPE_SIMPLE_WAIT,
		is_private ? THREAD_SHARE : AUTO_SHARE, &key)) != 0)
		return (ret);
	umtxq_lock(&key);
	ret = umtxq_signal(&key, n_wake);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	return (0);
}

static uint32_t
calc_lockword(uint32_t oldval, uint16_t flags, int qlen, int td_exit, int *nwake)
{
	uint32_t newval;

	if (flags & UMUTEX_ROBUST) {
		if (td_exit) {
			/*
			 * Thread is exiting, but did not unlock the mutex,
			 * mark it in OWNER_DEAD state.
			 */
			newval = (oldval & ~UMUTEX_OWNER_MASK) | UMUTEX_OWNER_DEAD;
			*nwake = 1;
		} else if ((oldval & UMUTEX_OWNER_DEAD) != 0) {
			/*
			 * if user unlocks it, and previous owner was dead,
			 * mark it in INCONSISTENT state.
			 */
			newval = (oldval & ~UMUTEX_OWNER_MASK) | UMUTEX_INCONSISTENT;
			*nwake = INT_MAX;
			return (newval);
		} else {
			newval = oldval & ~UMUTEX_OWNER_MASK;
			*nwake = 1;
		}
	} else {
		*nwake = 1;
		newval = oldval & ~UMUTEX_OWNER_MASK;
	}

	/*
	 * When unlocking the umtx, it must be marked as unowned if
	 * there is zero or one thread only waiting for it.
	 * Otherwise, it must be marked as contested.
	 */
	if (qlen <= 1)
		newval &= ~UMUTEX_CONTESTED;
	else
		newval |= UMUTEX_CONTESTED;

	return (newval);
}

/*
 * Lock PTHREAD_PRIO_NONE protocol POSIX mutex.
 */
static int
_do_lock_normal(struct thread *td, struct umutex *m, uint16_t flags, int timo,
	int mode)
{
	struct umtx_q *uq;
	uint32_t owner, old, id;
	int error = 0;

	if (flags & UMUTEX_SIMPLE)
		id = UMUTEX_SIMPLE_OWNER;
	else
		id = td->td_tid;
	uq = td->td_umtxq;

	/*
	 * Care must be exercised when dealing with umtx structure. It
	 * can fault on any access.
	 */
	for (;;) {
		owner = fuword32(__DEVOLATILE(void *, &m->m_owner));
		if ((flags & UMUTEX_ROBUST) != 0 &&
		    (owner & UMUTEX_OWNER_MASK) == UMUTEX_INCONSISTENT) {
			return (ENOTRECOVERABLE);
		}

		if ((owner & UMUTEX_OWNER_MASK) == 0) {
			if (mode == _UMUTEX_WAIT)
				return (0);
			/*
			 * Try lock it.
			 */
			old = casuword32(&m->m_owner, owner, owner|id);
			/* The acquire succeeded. */
			if (owner == old) {
				if ((flags & UMUTEX_ROBUST) != 0 &&
				    (owner & UMUTEX_OWNER_DEAD) != 0)
					return (EOWNERDEAD);
				return (0);
			}

			/* The address was invalid. */
			if (old == -1)
				return (EFAULT);

			/* If this failed the lock has changed, restart. */
			continue;
		}

		if ((flags & UMUTEX_ERROR_CHECK) != 0 &&
		    (owner & UMUTEX_OWNER_MASK) == id)
			return (EDEADLK);

		if (mode == _UMUTEX_TRY)
			return (EBUSY);

		/*
		 * If we caught a signal, we have retried and now
		 * exit immediately.
		 */
		if (error != 0)
			return (error);

		if ((error = umtx_key_get(m, TYPE_NORMAL_UMUTEX,
		    GET_SHARE(flags), &uq->uq_key)) != 0)
			return (error);

		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_insert(uq);
		umtxq_unlock(&uq->uq_key);

		/*
		 * Set the contested bit so that a release in user space
		 * knows to use the system call for unlock.  If this fails
		 * either some one else has acquired the lock or it has been
		 * released.
		 */
		old = casuword32(&m->m_owner, owner, owner | UMUTEX_CONTESTED);

		/* The address was invalid. */
		if (old == -1) {
			umtxq_lock(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unbusy(&uq->uq_key);
			umtxq_unlock(&uq->uq_key);
			umtx_key_release(&uq->uq_key);
			return (EFAULT);
		}

		/*
		 * We set the contested bit, sleep. Otherwise the lock changed
		 * and we need to retry or we lost a race to the thread
		 * unlocking the umtx.
		 */
		umtxq_lock(&uq->uq_key);
		umtxq_unbusy(&uq->uq_key);
		if (old == owner)
			error = umtxq_sleep(uq, "umtxn", timo);
		if ((uq->uq_flags & UQF_UMTXQ) != 0) {
			umtxq_busy(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unbusy(&uq->uq_key);
		}
		umtxq_unlock(&uq->uq_key);
		umtx_key_release(&uq->uq_key);
	}

	return (0);
}

/*
 * Lock PTHREAD_PRIO_NONE protocol POSIX mutex.
 */
/*
 * Unlock PTHREAD_PRIO_NONE protocol POSIX mutex.
 */
static int
do_unlock_normal(struct thread *td, struct umutex *m, uint16_t flags, 
	int td_exit)
{
	struct umtx_key key;
	uint32_t owner, old, id, newval;
	int error, count, nwake;

	if (flags & UMUTEX_SIMPLE)
		id = UMUTEX_SIMPLE_OWNER;
	else
		id = td->td_tid;
	/*
	 * Make sure we own this mtx.
	 */
	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	if (owner == -1)
		return (EFAULT);

	if ((owner & UMUTEX_OWNER_MASK) != id)
		return (EPERM);

	if ((owner & ~UMUTEX_OWNER_MASK) == 0) {
		/* No other bits set, just unlock it. */
		old = casuword32(&m->m_owner, owner, UMUTEX_UNOWNED);
		if (old == -1)
			return (EFAULT);
		if (old == owner)
			return (0);
	}

	if ((error = umtx_key_get(m, TYPE_NORMAL_UMUTEX, GET_SHARE(flags),
	    &key)) != 0)
		return (error);

	umtxq_lock(&key);
	umtxq_busy(&key);
	count = umtxq_count(&key);
	umtxq_unlock(&key);
	
	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	newval = calc_lockword(owner, flags, count, td_exit, &nwake);

	old = casuword32(&m->m_owner, owner, newval);
	umtxq_lock(&key);
	umtxq_signal(&key, nwake);
	umtxq_unbusy(&key);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	if (old == -1)
		return (EFAULT);
	if (old != owner)
		return (EINVAL);
	return (0);
}

/*
 * Check if the mutex is available and wake up a waiter,
 * only for simple mutex.
 */
static int
do_wake_umutex(struct thread *td, struct umutex *m)
{
	struct umtx_key key;
	uint32_t owner;
	uint32_t flags;
	int error;
	int count;

	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	if (owner == -1)
		return (EFAULT);

	if ((owner & UMUTEX_OWNER_MASK) != 0)
		return (0);

	flags = fuword32(&m->m_flags);

	/* We should only ever be in here for contested locks */
	if ((error = umtx_key_get(m, TYPE_NORMAL_UMUTEX, GET_SHARE(flags),
	    &key)) != 0)
		return (error);

	umtxq_lock(&key);
	umtxq_busy(&key);
	count = umtxq_count(&key);
	umtxq_unlock(&key);

	if (count <= 1)
		owner = casuword32(&m->m_owner, UMUTEX_CONTESTED, UMUTEX_UNOWNED);

	umtxq_lock(&key);
	if (count != 0 && (owner & UMUTEX_OWNER_MASK) == 0)
		umtxq_signal(&key, 1);
	umtxq_unbusy(&key);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	return (0);
}

static inline struct umtx_pi *
umtx_pi_alloc(int flags)
{
	struct umtx_pi *pi;

	pi = uma_zalloc(umtx_pi_zone, M_ZERO | flags);
	TAILQ_INIT(&pi->pi_blocked);
	atomic_add_int(&umtx_pi_allocated, 1);
	return (pi);
}

static inline void
umtx_pi_free(struct umtx_pi *pi)
{
	uma_zfree(umtx_pi_zone, pi);
	atomic_add_int(&umtx_pi_allocated, -1);
}

/*
 * Adjust the thread's position on a pi_state after its priority has been
 * changed.
 */
static int
umtx_pi_adjust_thread(struct umtx_pi *pi, struct thread *td)
{
	struct umtx_q *uq, *uq1, *uq2;
	struct thread *td1;

	mtx_assert(&umtx_lock, MA_OWNED);
	if (pi == NULL)
		return (0);

	uq = td->td_umtxq;

	/*
	 * Check if the thread needs to be moved on the blocked chain.
	 * It needs to be moved if either its priority is lower than
	 * the previous thread or higher than the next thread.
	 */
	uq1 = TAILQ_PREV(uq, umtxq_head, uq_lockq);
	uq2 = TAILQ_NEXT(uq, uq_lockq);
	if ((uq1 != NULL && UPRI(td) < UPRI(uq1->uq_thread)) ||
	    (uq2 != NULL && UPRI(td) > UPRI(uq2->uq_thread))) {
		/*
		 * Remove thread from blocked chain and determine where
		 * it should be moved to.
		 */
		TAILQ_REMOVE(&pi->pi_blocked, uq, uq_lockq);
		TAILQ_FOREACH(uq1, &pi->pi_blocked, uq_lockq) {
			td1 = uq1->uq_thread;
			MPASS(td1->td_proc->p_magic == P_MAGIC);
			if (UPRI(td1) > UPRI(td))
				break;
		}

		if (uq1 == NULL)
			TAILQ_INSERT_TAIL(&pi->pi_blocked, uq, uq_lockq);
		else
			TAILQ_INSERT_BEFORE(uq1, uq, uq_lockq);
	}
	return (1);
}

/*
 * Propagate priority when a thread is blocked on POSIX
 * PI mutex.
 */ 
static void
umtx_propagate_priority(struct thread *td)
{
	struct umtx_q *uq;
	struct umtx_pi *pi;
	int pri;

	mtx_assert(&umtx_lock, MA_OWNED);
	pri = UPRI(td);
	uq = td->td_umtxq;
	pi = uq->uq_pi_blocked;
	if (pi == NULL)
		return;

	for (;;) {
		td = pi->pi_owner;
		if (td == NULL)
			return;

		MPASS(td->td_proc != NULL);
		MPASS(td->td_proc->p_magic == P_MAGIC);

		if (UPRI(td) <= pri)
			return;

		thread_lock(td);
		sched_lend_user_prio(td, pri);
		thread_unlock(td);

		/*
		 * Pick up the lock that td is blocked on.
		 */
		uq = td->td_umtxq;
		pi = uq->uq_pi_blocked;
		/* Resort td on the list if needed. */
		if (!umtx_pi_adjust_thread(pi, td))
			break;
	}
}

/*
 * Unpropagate priority for a PI mutex when a thread blocked on
 * it is interrupted by signal or resumed by others.
 */
static void
umtx_unpropagate_priority(struct umtx_pi *pi)
{
	struct umtx_q *uq, *uq_owner;
	struct umtx_pi *pi2;
	int pri, oldpri;

	mtx_assert(&umtx_lock, MA_OWNED);

	while (pi != NULL && pi->pi_owner != NULL) {
		pri = PRI_MAX;
		uq_owner = pi->pi_owner->td_umtxq;

		TAILQ_FOREACH(pi2, &uq_owner->uq_pi_contested, pi_link) {
			uq = TAILQ_FIRST(&pi2->pi_blocked);
			if (uq != NULL) {
				if (pri > UPRI(uq->uq_thread))
					pri = UPRI(uq->uq_thread);
			}
		}

		if (pri > uq_owner->uq_inherited_pri)
			pri = uq_owner->uq_inherited_pri;
		thread_lock(pi->pi_owner);
		oldpri = pi->pi_owner->td_user_pri;
		sched_unlend_user_prio(pi->pi_owner, pri);
		thread_unlock(pi->pi_owner);
		if (uq_owner->uq_pi_blocked != NULL)
			umtx_pi_adjust_locked(pi->pi_owner, oldpri);
		pi = uq_owner->uq_pi_blocked;
	}
}

/*
 * Insert a PI mutex into owned list.
 */
static void
umtx_pi_setowner(struct umtx_pi *pi, struct thread *owner)
{
	struct umtx_q *uq_owner;

	uq_owner = owner->td_umtxq;
	mtx_assert(&umtx_lock, MA_OWNED);
	if (pi->pi_owner != NULL)
		panic("pi_ower != NULL");
	pi->pi_owner = owner;
	TAILQ_INSERT_TAIL(&uq_owner->uq_pi_contested, pi, pi_link);
}

/*
 * Claim ownership of a PI mutex.
 */
static int
umtx_pi_claim(struct umtx_pi *pi, struct thread *owner)
{
	struct umtx_q *uq, *uq_owner;

	uq_owner = owner->td_umtxq;
	mtx_lock_spin(&umtx_lock);
	if (pi->pi_owner == owner) {
		mtx_unlock_spin(&umtx_lock);
		return (0);
	}

	if (pi->pi_owner != NULL) {
		/*
		 * userland may have already messed the mutex, sigh.
		 */
		mtx_unlock_spin(&umtx_lock);
		return (EPERM);
	}
	umtx_pi_setowner(pi, owner);
	uq = TAILQ_FIRST(&pi->pi_blocked);
	if (uq != NULL) {
		int pri;

		pri = UPRI(uq->uq_thread);
		thread_lock(owner);
		if (pri < UPRI(owner))
			sched_lend_user_prio(owner, pri);
		thread_unlock(owner);
	}
	mtx_unlock_spin(&umtx_lock);
	return (0);
}

static void
umtx_pi_adjust_locked(struct thread *td, u_char oldpri)
{
	struct umtx_q *uq;
	struct umtx_pi *pi;

	uq = td->td_umtxq;
	/*
	 * Pick up the lock that td is blocked on.
	 */
	pi = uq->uq_pi_blocked;
	MPASS(pi != NULL);

	/* Resort the turnstile on the list. */
	if (!umtx_pi_adjust_thread(pi, td))
		return;

	/*
	 * If our priority was lowered and we are at the head of the
	 * turnstile, then propagate our new priority up the chain.
	 */
	if (uq == TAILQ_FIRST(&pi->pi_blocked) && UPRI(td) < oldpri)
		umtx_propagate_priority(td);
}

/*
 * Adjust a thread's order position in its blocked PI mutex,
 * this may result new priority propagating process.
 */
void
umtx_pi_adjust(struct thread *td, u_char oldpri)
{
	struct umtx_q *uq;
	struct umtx_pi *pi;

	uq = td->td_umtxq;
	mtx_lock_spin(&umtx_lock);
	/*
	 * Pick up the lock that td is blocked on.
	 */
	pi = uq->uq_pi_blocked;
	if (pi != NULL)
		umtx_pi_adjust_locked(td, oldpri);
	mtx_unlock_spin(&umtx_lock);
}

/*
 * Sleep on a PI mutex.
 */
static int
umtxq_sleep_pi(struct umtx_q *uq, struct umtx_pi *pi,
	uint32_t owner, const char *wmesg, int timo)
{
	struct umtxq_chain *uc;
	struct thread *td, *td1;
	struct umtx_q *uq1;
	int pri;
	int error = 0;

	td = uq->uq_thread;
	KASSERT(td == curthread, ("inconsistent uq_thread"));
	uc = umtxq_getchain(&uq->uq_key);
	UMTXQ_LOCKED_ASSERT(uc);
	UMTXQ_BUSY_ASSERT(uc);
	umtxq_insert(uq);
	mtx_lock_spin(&umtx_lock);
	if (pi->pi_owner == NULL) {
		mtx_unlock_spin(&umtx_lock);
		/* XXX Only look up thread in current process. */
		td1 = tdfind(owner, curproc->p_pid);
		mtx_lock_spin(&umtx_lock);
		if (td1 != NULL) {
			if((uq1 = td1->td_umtxq) != NULL &&
			    uq1->uq_exiting == 0) {
				if (pi->pi_owner == NULL)
					umtx_pi_setowner(pi, td1);
			}
			PROC_UNLOCK(td1->td_proc);
		}
	}

	TAILQ_FOREACH(uq1, &pi->pi_blocked, uq_lockq) {
		pri = UPRI(uq1->uq_thread);
		if (pri > UPRI(td))
			break;
	}

	if (uq1 != NULL)
		TAILQ_INSERT_BEFORE(uq1, uq, uq_lockq);
	else
		TAILQ_INSERT_TAIL(&pi->pi_blocked, uq, uq_lockq);

	uq->uq_pi_blocked = pi;
	thread_lock(td);
	td->td_flags |= TDF_UPIBLOCKED;
	thread_unlock(td);
	umtx_propagate_priority(td);
	mtx_unlock_spin(&umtx_lock);
	umtxq_unbusy(&uq->uq_key);

	if ((uq->uq_flags & UQF_UMTXQ) != 0) {
		error = msleep(uq, &uc->uc_lock, PCATCH, wmesg, timo);
		if (error == EWOULDBLOCK)
			error = ETIMEDOUT;
		if ((uq->uq_flags & UQF_UMTXQ) != 0) {
			umtxq_busy(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unbusy(&uq->uq_key);
		}
	}
	mtx_lock_spin(&umtx_lock);
	uq->uq_pi_blocked = NULL;
	thread_lock(td);
	td->td_flags &= ~TDF_UPIBLOCKED;
	thread_unlock(td);
	TAILQ_REMOVE(&pi->pi_blocked, uq, uq_lockq);
	umtx_unpropagate_priority(pi);
	mtx_unlock_spin(&umtx_lock);
	umtxq_unlock(&uq->uq_key);

	return (error);
}

/*
 * Add reference count for a PI mutex.
 */
static void
umtx_pi_ref(struct umtx_pi *pi)
{
	struct umtxq_chain *uc;

	uc = umtxq_getchain(&pi->pi_key);
	UMTXQ_LOCKED_ASSERT(uc);
	pi->pi_refcount++;
}

/*
 * Decrease reference count for a PI mutex, if the counter
 * is decreased to zero, its memory space is freed.
 */ 
static void
umtx_pi_unref(struct umtx_pi *pi)
{
	struct umtxq_chain *uc;

	uc = umtxq_getchain(&pi->pi_key);
	UMTXQ_LOCKED_ASSERT(uc);
	KASSERT(pi->pi_refcount > 0, ("invalid reference count"));
	if (--pi->pi_refcount == 0) {
		mtx_lock_spin(&umtx_lock);
		if (pi->pi_owner != NULL) {
			TAILQ_REMOVE(&pi->pi_owner->td_umtxq->uq_pi_contested,
				pi, pi_link);
			pi->pi_owner = NULL;
		}
		KASSERT(TAILQ_EMPTY(&pi->pi_blocked),
			("blocked queue not empty"));
		mtx_unlock_spin(&umtx_lock);
		TAILQ_REMOVE(&uc->uc_pi_list, pi, pi_hashlink);
		umtx_pi_free(pi);
	}
}

/*
 * Find a PI mutex in hash table.
 */
static struct umtx_pi *
umtx_pi_lookup(struct umtx_key *key)
{
	struct umtxq_chain *uc;
	struct umtx_pi *pi;

	uc = umtxq_getchain(key);
	UMTXQ_LOCKED_ASSERT(uc);

	TAILQ_FOREACH(pi, &uc->uc_pi_list, pi_hashlink) {
		if (umtx_key_match(&pi->pi_key, key)) {
			return (pi);
		}
	}
	return (NULL);
}

/*
 * Insert a PI mutex into hash table.
 */
static inline void
umtx_pi_insert(struct umtx_pi *pi)
{
	struct umtxq_chain *uc;

	uc = umtxq_getchain(&pi->pi_key);
	UMTXQ_LOCKED_ASSERT(uc);
	TAILQ_INSERT_TAIL(&uc->uc_pi_list, pi, pi_hashlink);
}

/*
 * Lock a PI mutex.
 */
static int
_do_lock_pi(struct thread *td, struct umutex *m, uint16_t flags, int timo,
	int try)
{
	struct umtx_q *uq;
	struct umtx_pi *pi, *new_pi;
	uint32_t id, owner, old;
	int error;

	id = td->td_tid;
	uq = td->td_umtxq;

	if ((error = umtx_key_get(m, TYPE_PI_UMUTEX, GET_SHARE(flags),
	    &uq->uq_key)) != 0)
		return (error);
	umtxq_lock(&uq->uq_key);
	pi = umtx_pi_lookup(&uq->uq_key);
	if (pi == NULL) {
		new_pi = umtx_pi_alloc(M_NOWAIT);
		if (new_pi == NULL) {
			umtxq_unlock(&uq->uq_key);
			new_pi = umtx_pi_alloc(M_WAITOK);
			umtxq_lock(&uq->uq_key);
			pi = umtx_pi_lookup(&uq->uq_key);
			if (pi != NULL) {
				umtx_pi_free(new_pi);
				new_pi = NULL;
			}
		}
		if (new_pi != NULL) {
			new_pi->pi_key = uq->uq_key;
			umtx_pi_insert(new_pi);
			pi = new_pi;
		}
	}
	umtx_pi_ref(pi);
	umtxq_unlock(&uq->uq_key);

	/*
	 * Care must be exercised when dealing with umtx structure.  It
	 * can fault on any access.
	 */
	for (;;) {
		owner = fuword32(__DEVOLATILE(void *, &m->m_owner));
		if ((flags & UMUTEX_ROBUST) != 0 &&
		    (owner & UMUTEX_OWNER_MASK) == UMUTEX_INCONSISTENT) {
			error = ENOTRECOVERABLE;
			break;
		}

		if ((owner & UMUTEX_OWNER_MASK) == 0) {
			old = casuword32(&m->m_owner, owner, id|owner);
			/* The acquire succeeded. */
			if (owner == old) {
				if ((owner & UMUTEX_CONTESTED) != 0) {
					umtxq_lock(&uq->uq_key);
					umtxq_busy(&uq->uq_key);
					umtx_pi_claim(pi, td);
					umtxq_unbusy(&uq->uq_key);
					umtxq_unlock(&uq->uq_key);
				}
				if ((flags & UMUTEX_ROBUST) != 0 &&
				    (owner & UMUTEX_OWNER_DEAD) != 0)
					error = EOWNERDEAD;
				else
					error = 0;
				break;
			}

			/* The address was invalid. */
			if (old == -1) {
				error = EFAULT;
				break;
			}

			continue;
		}

		if ((flags & UMUTEX_ERROR_CHECK) != 0 &&
		    (owner & ~UMUTEX_CONTESTED) == id) {
			error = EDEADLK;
			break;
		}

		if (try != 0) {
			error = EBUSY;
			break;
		}

		/*
		 * If we caught a signal, we have retried and now
		 * exit immediately.
		 */
		if (error != 0)
			break;
			
		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

		/*
		 * Set the contested bit so that a release in user space
		 * knows to use the system call for unlock.  If this fails
		 * either some one else has acquired the lock or it has been
		 * released.
		 */
		old = casuword32(&m->m_owner, owner, owner | UMUTEX_CONTESTED);

		/* The address was invalid. */
		if (old == -1) {
			umtxq_lock(&uq->uq_key);
			umtxq_unbusy(&uq->uq_key);
			umtxq_unlock(&uq->uq_key);
			error = EFAULT;
			break;
		}

		umtxq_lock(&uq->uq_key);
		/*
		 * We set the contested bit, sleep. Otherwise the lock changed
		 * and we need to retry or we lost a race to the thread
		 * unlocking the umtx.
		 */
		if (old == owner)
			error = umtxq_sleep_pi(uq, pi, owner & ~UMUTEX_CONTESTED,
				 "umtxpi", timo);
		else {
			umtxq_unbusy(&uq->uq_key);
			umtxq_unlock(&uq->uq_key);
		}
	}

	umtxq_lock(&uq->uq_key);
	umtx_pi_unref(pi);
	umtxq_unlock(&uq->uq_key);

	umtx_key_release(&uq->uq_key);
	return (error);
}

/*
 * Unlock a PI mutex.
 */
static int
do_unlock_pi(struct thread *td, struct umutex *m, uint32_t flags,
	int td_exit)
{
	struct umtx_key key;
	struct umtx_q *uq_first, *uq_first2, *uq_me;
	struct umtx_pi *pi, *pi2;
	uint32_t owner, old, id, newval;
	int error, count, nwake;
	int pri;

	id = td->td_tid;
	/*
	 * Make sure we own this mtx.
	 */
	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	if (owner == -1)
		return (EFAULT);

	if ((owner & UMUTEX_OWNER_MASK) != id)
		return (EPERM);

	if ((owner & ~UMUTEX_OWNER_MASK) == 0) {
		old = casuword32(&m->m_owner, owner, UMUTEX_UNOWNED);
		if (old == -1)
			return (EFAULT);
		if (old == owner)
			return (0);
		owner = old;
	}

	if ((error = umtx_key_get(m, TYPE_PI_UMUTEX, GET_SHARE(flags),
	    &key)) != 0)
		return (error);

	umtxq_lock(&key);
	umtxq_busy(&key);
	count = umtxq_count_pi(&key, &uq_first);
	if (uq_first != NULL) {
		mtx_lock_spin(&umtx_lock);
		pi = uq_first->uq_pi_blocked;
		KASSERT(pi != NULL, ("pi == NULL?"));
		if (pi->pi_owner != curthread) {
			mtx_unlock_spin(&umtx_lock);
			umtxq_unbusy(&key);
			umtxq_unlock(&key);
			umtx_key_release(&key);
			/* userland messed the mutex */
			return (EPERM);
		}
		uq_me = curthread->td_umtxq;
		pi->pi_owner = NULL;
		TAILQ_REMOVE(&uq_me->uq_pi_contested, pi, pi_link);
		/* get highest priority thread which is still sleeping. */
		uq_first = TAILQ_FIRST(&pi->pi_blocked);
		while (uq_first != NULL && 
		       (uq_first->uq_flags & UQF_UMTXQ) == 0) {
			uq_first = TAILQ_NEXT(uq_first, uq_lockq);
		}
		pri = PRI_MAX;
		TAILQ_FOREACH(pi2, &uq_me->uq_pi_contested, pi_link) {
			uq_first2 = TAILQ_FIRST(&pi2->pi_blocked);
			if (uq_first2 != NULL) {
				if (pri > UPRI(uq_first2->uq_thread))
					pri = UPRI(uq_first2->uq_thread);
			}
		}
		thread_lock(curthread);
		sched_unlend_user_prio(curthread, pri);
		thread_unlock(curthread);
		mtx_unlock_spin(&umtx_lock);
		if (uq_first)
			umtxq_signal_thread(uq_first);
	}
	umtxq_unlock(&key);

	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	newval = calc_lockword(owner, flags, count, td_exit, &nwake);

	/*
	 * When unlocking the umtx, it must be marked as unowned if
	 * there is zero or one thread only waiting for it.
	 * Otherwise, it must be marked as contested.
	 */
	old = casuword32(&m->m_owner, owner, newval);

	umtxq_lock(&key);
	umtxq_unbusy(&key);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	if (old == -1)
		return (EFAULT);
	if (old != owner)
		return (EINVAL);
	return (0);
}

struct old_pp_mutex {
	volatile __lwpid_t      m_owner;        /* Owner of the mutex */
	__uint32_t		m_flags;        /* Flags of the mutex */
	__uint32_t		m_ceilings[2];  /* Priority protect ceiling */
};

/*
 * Lock a PP mutex.
 */
static int
_do_lock_pp(struct thread *td, struct umutex *m, uint32_t flags, int timo,
	int try)
{
	struct umtx_q *uq, *uq2;
	struct umtx_pi *pi;
	uint32_t ceiling;
	uint32_t owner, id, old;
	int error, pri, old_inherited_pri, su;
	struct old_pp_mutex *oldmtx = (struct old_pp_mutex *)m;

	if (flags & UMUTEX_SIMPLE)
		id = UMUTEX_SIMPLE_OWNER;
	else
		id = td->td_tid;
	uq = td->td_umtxq;
	if ((error = umtx_key_get(m, TYPE_PP_UMUTEX, GET_SHARE(flags),
	    &uq->uq_key)) != 0)
		return (error);
	su = (priv_check(td, PRIV_SCHED_RTPRIO) == 0);
	for (;;) {
		/*
		 * We busy the lock, so no one can change the priority ceiling
		 * while we are locking it.
		 */
		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

		old_inherited_pri = uq->uq_inherited_pri;
		if (flags & UMUTEX_PRIO_PROTECT)
			ceiling = RTP_PRIO_MAX - fuword32(&oldmtx->m_ceilings[0]);
		else
			ceiling = RTP_PRIO_MAX - fubyte(&m->m_ceilings[0]);
		if (ceiling > RTP_PRIO_MAX) {
			error = EINVAL;
			goto out;
		}

		mtx_lock_spin(&umtx_lock);
		if (UPRI(td) < PRI_MIN_REALTIME + ceiling) {
			mtx_unlock_spin(&umtx_lock);
			error = EINVAL;
			goto out;
		}
		if (su && PRI_MIN_REALTIME + ceiling < uq->uq_inherited_pri) {
			uq->uq_inherited_pri = PRI_MIN_REALTIME + ceiling;
			thread_lock(td);
			if (uq->uq_inherited_pri < UPRI(td))
				sched_lend_user_prio(td, uq->uq_inherited_pri);
			thread_unlock(td);
		}
		mtx_unlock_spin(&umtx_lock);

again:
		owner = fuword32(__DEVOLATILE(void *, &m->m_owner));
		if ((flags & UMUTEX_ROBUST) != 0 &&
		    (owner & UMUTEX_OWNER_MASK) == UMUTEX_INCONSISTENT) {
			error = ENOTRECOVERABLE;
			break;
		}

		/*
		 * Try lock it.
		 */
		if ((owner & UMUTEX_OWNER_MASK) == 0) {
			old = casuword32(&m->m_owner, owner, id|owner);
			/* The acquire succeeded. */
			if (owner == old) {
				if ((flags & UMUTEX_ROBUST) != 0 &&
				    (owner & UMUTEX_OWNER_DEAD) != 0)
					error = EOWNERDEAD;
				else
					error = 0;
				break;
			}

			/* The address was invalid. */
			if (old == -1) {
				error = EFAULT;
				break;
			}
			goto again;
		}

		if ((flags & UMUTEX_ERROR_CHECK) != 0 &&
		    (owner & ~UMUTEX_CONTESTED) == id) {
			error = EDEADLK;
			break;
		}

		if (try != 0) {
			error = EBUSY;
			break;
		}

		/*
		 * If we caught a signal, we have retried and now
		 * exit immediately.
		 */
		if (error != 0)
			break;

		umtxq_lock(&uq->uq_key);
		umtxq_insert(uq);
		umtxq_unlock(&uq->uq_key);

		old = casuword32(&m->m_owner, owner, owner | UMUTEX_CONTESTED);

		umtxq_lock(&uq->uq_key);
		umtxq_unbusy(&uq->uq_key);
		if (old == owner)
			error = umtxq_sleep(uq, "umtxn", timo);
		if ((uq->uq_flags & UQF_UMTXQ) != 0) {
			umtxq_busy(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unbusy(&uq->uq_key);
		}
		umtxq_unlock(&uq->uq_key);

		mtx_lock_spin(&umtx_lock);
		uq->uq_inherited_pri = old_inherited_pri;
		pri = PRI_MAX;
		TAILQ_FOREACH(pi, &uq->uq_pi_contested, pi_link) {
			uq2 = TAILQ_FIRST(&pi->pi_blocked);
			if (uq2 != NULL) {
				if (pri > UPRI(uq2->uq_thread))
					pri = UPRI(uq2->uq_thread);
			}
		}
		if (pri > uq->uq_inherited_pri)
			pri = uq->uq_inherited_pri;
		thread_lock(td);
		sched_unlend_user_prio(td, pri);
		thread_unlock(td);
		mtx_unlock_spin(&umtx_lock);
	}

	if (error != 0) {
		mtx_lock_spin(&umtx_lock);
		uq->uq_inherited_pri = old_inherited_pri;
		pri = PRI_MAX;
		TAILQ_FOREACH(pi, &uq->uq_pi_contested, pi_link) {
			uq2 = TAILQ_FIRST(&pi->pi_blocked);
			if (uq2 != NULL) {
				if (pri > UPRI(uq2->uq_thread))
					pri = UPRI(uq2->uq_thread);
			}
		}
		if (pri > uq->uq_inherited_pri)
			pri = uq->uq_inherited_pri;
		thread_lock(td);
		sched_unlend_user_prio(td, pri);
		thread_unlock(td);
		mtx_unlock_spin(&umtx_lock);
	}

out:
	umtxq_lock(&uq->uq_key);
	umtxq_unbusy(&uq->uq_key);
	umtxq_unlock(&uq->uq_key);
	umtx_key_release(&uq->uq_key);
	return (error);
}

/*
 * Unlock a PP mutex.
 */
static int
do_unlock_pp(struct thread *td, struct umutex *m, uint32_t flags,
	int td_exit)
{
	struct old_pp_mutex *oldmtx = (struct old_pp_mutex *)m;
	struct umtx_key key;
	struct umtx_q *uq, *uq2;
	struct umtx_pi *pi;
	uint32_t owner, id, newval, old;
	uint32_t rceiling;
	int error, pri, new_inherited_pri, su;
	int count, nwake;

	if (flags & UMUTEX_SIMPLE)
		id = UMUTEX_SIMPLE_OWNER;
	else
		id = td->td_tid;
	uq = td->td_umtxq;
	su = (priv_check(td, PRIV_SCHED_RTPRIO) == 0);

	/*
	 * Make sure we own this mtx.
	 */
	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	if (owner == -1)
		return (EFAULT);

	if ((owner & UMUTEX_OWNER_MASK) != id)
		return (EPERM);

	if (flags & UMUTEX_PRIO_PROTECT) {
		/* old style */
		rceiling = fuword32(&oldmtx->m_ceilings[1]);
	} else {
		rceiling = fubyte(&m->m_ceilings[1]);
	}

	if (rceiling == -1)
		new_inherited_pri = PRI_MAX;
	else {
		rceiling = RTP_PRIO_MAX - rceiling;
		if (rceiling > RTP_PRIO_MAX)
			return (EINVAL);
		new_inherited_pri = PRI_MIN_REALTIME + rceiling;
	}

	if ((error = umtx_key_get(m, TYPE_PP_UMUTEX, GET_SHARE(flags),
	    &key)) != 0)
		return (error);
	umtxq_lock(&key);
	umtxq_busy(&key);
	count = umtxq_count(&key);
	umtxq_unlock(&key);

	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	newval = calc_lockword(owner, flags, count, td_exit, &nwake);

	if (flags & UMUTEX_PRIO_PROTECT) {
		/*
		 * For old priority protected mutex, always set unlocked state
		 * to UMUTEX_CONTESTED, so that userland always enters kernel
		 * to lock the mutex, it is necessary because thread priority
		 * has to be adjusted for such mutex.
		 */
		newval |= UMUTEX_CONTESTED;
	}
	old = casuword32(&m->m_owner, owner, newval);
	if (old == -1)
		error = EFAULT;
	if (old != owner)
		error = EINVAL;

	umtxq_lock(&key);
	umtxq_signal(&key, nwake);
	umtxq_unbusy(&key);
	umtxq_unlock(&key);

	mtx_lock_spin(&umtx_lock);
	if (su != 0)
		uq->uq_inherited_pri = new_inherited_pri;
	pri = PRI_MAX;
	TAILQ_FOREACH(pi, &uq->uq_pi_contested, pi_link) {
		uq2 = TAILQ_FIRST(&pi->pi_blocked);
		if (uq2 != NULL) {
			if (pri > UPRI(uq2->uq_thread))
				pri = UPRI(uq2->uq_thread);
		}
	}
	if (pri > uq->uq_inherited_pri)
		pri = uq->uq_inherited_pri;
	thread_lock(td);
	sched_unlend_user_prio(td, pri);
	thread_unlock(td);
	mtx_unlock_spin(&umtx_lock);
	umtx_key_release(&key);
	return (error);
}

static int
do_set_ceiling(struct thread *td, struct umutex *m, uint32_t ceiling,
	uint32_t *old_ceiling)
{
	struct old_pp_mutex *oldmtx = (struct old_pp_mutex *)m;
	struct umtx_q *uq;
	uint32_t save_ceiling;
	uint32_t owner, id, old;
	uint32_t flags;
	int error;

	flags = fuword32(&m->m_flags);
	if ((flags & (UMUTEX_PRIO_PROTECT|UMUTEX_PRIO_PROTECT2)) == 0)
		return (EINVAL);
	if (ceiling > RTP_PRIO_MAX)
		return (EINVAL);
	if (flags & UMUTEX_SIMPLE)
		id = UMUTEX_SIMPLE_OWNER;
	else
		id = td->td_tid;
	uq = td->td_umtxq;
	if ((error = umtx_key_get(m, TYPE_PP_UMUTEX, GET_SHARE(flags),
	   &uq->uq_key)) != 0)
		return (error);
	for (;;) {
		/*
		 * This is the protocol that we must busy the lock
		 * before locking.
		 */
		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

again:
		if (flags & UMUTEX_PRIO_PROTECT) {
			/* old style */
			save_ceiling = fuword32(&oldmtx->m_ceilings[0]);
		} else {
			save_ceiling = fubyte(&m->m_ceilings[0]);
		}

		owner = fuword32(__DEVOLATILE(void *, &m->m_owner));
		if ((flags & UMUTEX_ROBUST) != 0 &&
		    (owner & UMUTEX_OWNER_MASK) == UMUTEX_INCONSISTENT) {
			error = ENOTRECOVERABLE;
			break;
		}

		/*
		 * Try lock it.
		 */
		if ((owner & UMUTEX_OWNER_MASK) == 0) {
			old = casuword32(&m->m_owner, owner, id|owner);
			/* The acquire succeeded. */
			if (owner == old) {
				if ((flags & UMUTEX_ROBUST) != 0 &&
				    (owner & UMUTEX_OWNER_DEAD) != 0)
					error = EOWNERDEAD;
				else
					error = 0;
				if (flags & UMUTEX_PRIO_PROTECT)
					suword32(&oldmtx->m_ceilings[0], ceiling);
				else
					subyte(&m->m_ceilings[0], ceiling);
				/* unlock */
				suword32(__DEVOLATILE(void *, &m->m_owner), old);
				break;
			}

			/* The address was invalid. */
			if (old == -1) {
				error = EFAULT;
				break;
			}
			goto again;
		}

		/*
		 * If we caught a signal, we have retried and now
		 * exit immediately.
		 */
		if (error != 0)
			break;

		/*
		 * We set the contested bit, sleep. Otherwise the lock changed
		 * and we need to retry or we lost a race to the thread
		 * unlocking the umtx.
		 */
		umtxq_lock(&uq->uq_key);
		umtxq_insert(uq);
		umtxq_unbusy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

		old = casuword32(&m->m_owner, owner, owner | UMUTEX_CONTESTED);

		umtxq_lock(&uq->uq_key);
		umtxq_unbusy(&uq->uq_key);
		if (old == owner)
			error = umtxq_sleep(uq, "umtxpp", 0);
		if ((uq->uq_flags & UQF_UMTXQ) != 0) {
			umtxq_busy(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unbusy(&uq->uq_key);
		}
		umtxq_unlock(&uq->uq_key);
	}
	umtxq_lock(&uq->uq_key);
	umtxq_signal(&uq->uq_key, INT_MAX);
	umtxq_unbusy(&uq->uq_key);
	umtxq_unlock(&uq->uq_key);
	umtx_key_release(&uq->uq_key);
	if (error == 0 && old_ceiling != NULL)
		suword32(old_ceiling, save_ceiling);
	return (error);
}

static int
_do_lock_umutex(struct thread *td, struct umutex *m, int flags, int timo,
	int mode)
{
	switch(flags & (UMUTEX_PRIO_INHERIT | UMUTEX_PRIO_PROTECT |
	       UMUTEX_PRIO_PROTECT2)) {
	case 0:
		return (_do_lock_normal(td, m, flags, timo, mode));
	case UMUTEX_PRIO_INHERIT:
		return (_do_lock_pi(td, m, flags, timo, mode));
	case UMUTEX_PRIO_PROTECT:
	case UMUTEX_PRIO_PROTECT2:
		return (_do_lock_pp(td, m, flags, timo, mode));
	}
	return (EINVAL);
}

/*
 * Lock a userland POSIX mutex.
 */
static int
do_lock_umutex(struct thread *td, struct umutex *m,
	struct timespec *timeout, int mode, int wflags)
{
	struct timespec cts, ets, tts;
	struct robust_info *rob = NULL;
	struct timeval tv;
	uint32_t flags;
	int error;

	flags = fuword32(&m->m_flags);
	if (flags == -1)
		return (EFAULT);

	if ((flags & UMUTEX_ROBUST) != 0 && mode != _UMUTEX_WAIT) {
		error = robust_alloc(&rob);
		if (error != 0) {
			if (timeout == NULL) {
				if (error == EINTR && mode != _UMUTEX_WAIT)
					error = ERESTART;
			} else if (error == ERESTART) {
				error = EINTR;
			}
			return (error);
		}
		rob->ownertd = td;
		rob->umtxp = m;
	}

	if (timeout == NULL) {
		error = _do_lock_umutex(td, m, flags, 0, mode);
		/* Mutex locking is restarted if it is interrupted. */
		if (error == EINTR && mode != _UMUTEX_WAIT)
			error = ERESTART;
	} else {
		const clockid_t clockid = CLOCK_REALTIME;

		UMTX_STATE_INC(timedlock_count);

		if ((wflags & UMUTEX_ABSTIME) == 0) {
			kern_clock_gettime(td, clockid, &ets);
			timespecadd(&ets, timeout);
			tts = *timeout;
		} else { /* absolute time */
			ets = *timeout;
			tts = *timeout;
			kern_clock_gettime(td, clockid, &cts);
			timespecsub(&tts, &cts);
		}
		TIMESPEC_TO_TIMEVAL(&tv, &tts);
		for (;;) {
			error = _do_lock_umutex(td, m, flags, tvtohz(&tv), mode);
			if (error != ETIMEDOUT)
				break;
			kern_clock_gettime(td, clockid, &cts);
			if (timespeccmp(&cts, &ets, >=)) {
				error = ETIMEDOUT;
				break;
			}
			tts = ets;
			timespecsub(&tts, &cts);
			TIMESPEC_TO_TIMEVAL(&tv, &tts);
		}
		/* Timed-locking is not restarted. */
		if (error == ERESTART)
			error = EINTR;
	}

	if (error == 0 || error == EOWNERDEAD) {
		if (rob != NULL && robust_insert(td, rob))
			robust_free(rob);
	} else if (rob != NULL) {
		robust_free(rob);
	}
	return (error);
}

/*
 * Unlock a userland POSIX mutex.
 */
static int
do_unlock_umutex(struct thread *td, struct umutex *m, int td_exit)
{
	uint16_t flags;
	int error;

	flags = fuword16(&m->m_flags);
	if ((flags & UMUTEX_ROBUST) != 0 || td_exit)
		robust_remove(td, m);

	switch(flags & (UMUTEX_PRIO_INHERIT | UMUTEX_PRIO_PROTECT)) {
	case 0:
		error = do_unlock_normal(td, m, flags, td_exit);
		break;
	case UMUTEX_PRIO_INHERIT:
		error = do_unlock_pi(td, m, flags, td_exit);
		break;
	case UMUTEX_PRIO_PROTECT:
	case UMUTEX_PRIO_PROTECT2:
		error = do_unlock_pp(td, m, flags, td_exit);
		break;
	default:
		error = EINVAL;
	}
	return (error);
}

static int
set_contested_bit(struct umutex *m, struct umtxq_queue *uhm, int repair)
{
	int do_wake;
	int qlen = uhm->length;
	uint32_t owner;

	do_wake = 0;
	/*
	 * Set contested bit for mutex when necessary, so that userland
	 * mutex unlocker will wake up a waiter thread.
	 */
	owner = fuword32(__DEVOLATILE(uint32_t *, &m->m_owner));
	for (;;) {
		if (owner == UMUTEX_UNOWNED) {
			if (!repair && qlen == 1) {
				do_wake = 1;
				break;
			}
			if ((owner = casuword32(&m->m_owner, UMUTEX_UNOWNED,
				UMUTEX_CONTESTED)) == UMUTEX_UNOWNED) {
				do_wake = 1;
				break;
			}
		}
		if (owner == UMUTEX_CONTESTED) {
			do_wake = 1;
			break;
		}
		if ((owner & UMUTEX_CONTESTED) == 0) {
			uint32_t old;
	  		old = casuword32(&m->m_owner, owner,
				owner|UMUTEX_CONTESTED);
			if (old == owner)
				break;
			owner = old;
		} else {
			break;
		}
	}
	return (do_wake);
}

static int
do_cv_wait(struct thread *td, struct ucond *cv, struct umutex *m,
	struct timespec *timeout, u_long wflags)
{
	struct umtx_q *uq;
	struct umtx_key mkey, *mkeyp, savekey;
	struct umutex *bind_mutex;
	struct timeval tv;
	struct timespec cts, ets, tts;
	struct umtxq_chain *old_chain;
	uint32_t flags, mflags;
	uint32_t clockid;
	int error;

	uq = td->td_umtxq;
	flags = fuword32(&cv->c_flags);
	mflags = fuword32(&m->m_flags);
	error = umtx_key_get(cv, TYPE_CV, GET_SHARE(flags), &uq->uq_key);
	if (error != 0)
		return (error);

	if ((wflags & CVWAIT_CLOCKID) != 0) {
		clockid = fuword32(&cv->c_clockid);
		if (clockid < CLOCK_REALTIME ||
		    clockid >= CLOCK_THREAD_CPUTIME_ID) {
			/* hmm, only HW clock id will work. */
			return (EINVAL);
		}
	} else {
		clockid = CLOCK_REALTIME;
	}

	savekey = uq->uq_key;
	if ((flags & UCOND_BIND_MUTEX) != 0) {
		if ((mflags & UMUTEX_PRIO_INHERIT) != 0)
			goto ignore;
		error = umtx_key_get(m, TYPE_NORMAL_UMUTEX,
				GET_SHARE(mflags), &mkey);
		if (error != 0) {
			umtx_key_release(&uq->uq_key);
			return (error);
		}
		if (mkey.shared == 0)
			bind_mutex = m;
		else
			bind_mutex = NULL;
		mkeyp = &mkey;
	} else {
ignore:
		bind_mutex = NULL;
		mkeyp = NULL;
	}

	old_chain = uq->uq_key.chain;
	umtxq_lock(&uq->uq_key);
	umtxq_busy(&uq->uq_key);
	error = umtxq_insert_queue2(uq, UMTX_SHARED_QUEUE, bind_mutex, mkeyp);
	if (error != 0) {
		UMTX_STATE_INC(cv_insert_failure);
		umtxq_unbusy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);
		return (error);
	}
	umtxq_unlock(&uq->uq_key);

	/*
	 * Set c_has_waiters to 1 before releasing user mutex, also
	 * don't modify cache line when unnecessary.
	 */
	if (fuword32(__DEVOLATILE(uint32_t *, &cv->c_has_waiters)) == 0)
		suword32(__DEVOLATILE(uint32_t *, &cv->c_has_waiters), 1);

	umtxq_lock(&uq->uq_key);
	umtxq_unbusy(&uq->uq_key);
	umtxq_unlock(&uq->uq_key);

	error = do_unlock_umutex(td, m, 0);
	if (error) {
		UMTX_STATE_INC(cv_unlock_failure);
		error = 0; /* ignore the error */
	}
	
	umtxq_lock(&uq->uq_key);
	if (error == 0) {
		if (timeout == NULL) {
			error = umtxq_sleep(uq, "ucond", 0);
		} else {
			if ((wflags & CVWAIT_ABSTIME) == 0) {
				kern_clock_gettime(td, clockid, &ets);
				timespecadd(&ets, timeout);
				tts = *timeout;
			} else { /* absolute time */
				ets = *timeout;
				tts = *timeout;
				kern_clock_gettime(td, clockid, &cts);
				timespecsub(&tts, &cts);
			}
			TIMESPEC_TO_TIMEVAL(&tv, &tts);
			for (;;) {
				error = umtxq_sleep(uq, "ucond", tvtohz(&tv));
				if (error != ETIMEDOUT)
					break;
				kern_clock_gettime(td, clockid, &cts);
				if (timespeccmp(&cts, &ets, >=)) {
					error = ETIMEDOUT;
					break;
				}
				tts = ets;
				timespecsub(&tts, &cts);
				TIMESPEC_TO_TIMEVAL(&tv, &tts);
			}
		}
	}
	if ((uq->uq_flags & UQF_UMTXQ) == 0)
		error = 0;
	else {
		/*
		 * This must be timeout or interrupted by signal or
		 * surprious wakeup.
		 */
		umtxq_busy(&uq->uq_key);
		if ((uq->uq_flags & UQF_UMTXQ) != 0) {
			int oldlen = uq->uq_cur_queue->length;
			umtxq_remove(uq);
			if (oldlen == 1 && old_chain == uq->uq_key.chain) {
				umtxq_unlock(&uq->uq_key);
				suword32(
				    __DEVOLATILE(uint32_t *,
					 &cv->c_has_waiters), 0);
				umtxq_lock(&uq->uq_key);
			}
		}
		umtxq_unbusy(&uq->uq_key);
		if (error == ERESTART)
			error = EINTR;
	}
	umtxq_unlock(&uq->uq_key);

	/* We were moved to mutex queue. */
	if (mkeyp != NULL &&
	    old_chain != uq->uq_key.chain) {
		/*
		 * cv_broadcast can not access the mutex if we are pshared,
		 * but it still migrate threads to mutex queue,
		 * we should repair contested bit here. 
		 */
		if ((mflags & USYNC_PROCESS_SHARED) != 0 && uq->uq_repair_mutex) {
			uint32_t owner = fuword32(
				__DEVOLATILE(void *, &m->m_owner));
			if ((owner & UMUTEX_CONTESTED) == 0) {
				struct umtxq_queue *uhm;
				umtxq_lock(mkeyp);
				umtxq_busy(mkeyp);
				uhm = umtxq_queue_lookup(mkeyp,
					UMTX_SHARED_QUEUE);
				if (uhm != NULL)
					set_contested_bit(m, uhm, 1);
				umtxq_unbusy(mkeyp);
				umtxq_unlock(mkeyp);
			}
		}

		error = 0;
	}
	/*
	 * Note that we should release a saved key, because if we
	 * were migrated, the vmobject reference is no longer the original,
	 * however, we should release the original.
	 */
	umtx_key_release(&savekey);
	if (mkeyp != NULL)
		umtx_key_release(mkeyp);
	uq->uq_spare_queue->bind_mutex = NULL;
	uq->uq_spare_queue->binding = 0;
	uq->uq_repair_mutex = 0;
	return (error);
}

/*
 * Entered with queue busied but not locked, exits with queue locked.
 */
static void
cv_after_migration(int oldlen, struct umutex *bind_mutex,
	struct umtxq_queue *uhm)
{
	struct umtx_q *uq;
	int do_wake = 0;
	int shared = uhm->key.shared;

	/*
	 * Wake one thread when necessary. if before the queue
	 * migration, there is thread on mutex queue, we don't
	 * need to wake up a thread, because the mutex contention
	 * bit should have already been set by other mutex locking
	 * code.
	 * For pshared mutex, because different process has different
	 * address even for same process-shared mutex!
	 * we don't know where the mutex is in our address space.
	 * In this situation, we let a thread resumed from cv_wait
	 * to repair the mutex contention bit.
	 * XXX Fixme! we should make the repairing thread runs as
	 * soon as possible, boost its priority.
	 */

	if (oldlen == 0) {
		if (!shared) {
			do_wake = set_contested_bit(bind_mutex, uhm, 0);
		} else {
			do_wake = 1;
		}
	} else {
		do_wake = 0;
	}

	umtxq_lock(&uhm->key);
	if (do_wake) {
		uq = TAILQ_FIRST(&uhm->head);
		if (uq != NULL) {
			if (shared)
				uq->uq_repair_mutex = 1;
			umtxq_signal_thread(uq);
		}
	}
}

/*
 * Signal a userland condition variable.
 */
static int
do_cv_signal(struct thread *td, struct ucond *cv)
{
	struct umtxq_queue *uh, *uhm;
	struct umtxq_chain *uc, *ucm;
	struct umtx_q *uq;
	struct umtx_key key;
	int error, len, migrate;
	uint32_t flags, owner;

	flags = fuword32(&cv->c_flags);
	if ((error = umtx_key_get(cv, TYPE_CV, GET_SHARE(flags), &key)) != 0)
		return (error);	

	umtxq_lock(&key);
	umtxq_busy(&key);
	uh = umtxq_queue_lookup(&key, UMTX_SHARED_QUEUE);
	if (uh == NULL) {
		int has_waiters = fuword32(__DEVOLATILE(uint32_t *,
		 	&cv->c_has_waiters));
		if (has_waiters) {
			suword32(__DEVOLATILE(uint32_t *,
			 	&cv->c_has_waiters), 0);
		}
		umtxq_unbusy(&key);
		umtxq_unlock(&key);
		umtx_key_release(&key);
		return (0);
	}

	len = uh->length;
	switch(umtx_cvsig_migrate) {
	case 1: /* auto */
		migrate = (mp_ncpus == 1);
		break;
	case 0: /* disable */
		migrate = 0;
		break;
	default: /* always */
		migrate = 1;
	}
	if (migrate && uh->binding) {
		struct umutex *bind_mutex = uh->bind_mutex;
		struct umtx_key mkey;
		int oldlen;

		mkey = uh->bind_mkey;
		umtxq_unlock(&key);

		if (!mkey.shared) {
			owner = fuword32(__DEVOLATILE(void *,
				&bind_mutex->m_owner));
			 /* If mutex is not locked, wake up one. */
			if ((owner & ~UMUTEX_CONTESTED) == 0) {
				goto wake_one;
			}
		}

		/* Try to move thread between mutex and cv queues. */
		uc = umtxq_getchain(&key);
		ucm = umtxq_getchain(&mkey);

		umtxq_lock(&mkey);
		umtxq_busy(&mkey);
		umtxq_unlock(&mkey);
		umtxq_lock(&key);
		umtxq_lock(&mkey);
		uhm = umtxq_queue_lookup(&mkey, UMTX_SHARED_QUEUE);
		if (uhm == NULL)
			oldlen = 0;
		else
			oldlen = uhm->length;
		uq = TAILQ_FIRST(&uh->head);
		umtxq_remove_queue(uq, UMTX_SHARED_QUEUE);
		umtx_key_copy(&uq->uq_key, &mkey);
		umtxq_insert(uq);
		if (uhm == NULL)
			uhm = uq->uq_cur_queue;
		umtxq_unlock(&mkey);
		umtxq_unlock(&key);
		UMTX_STATE_INC(cv_signal_migrate);
		if (len == 1)
			suword32(__DEVOLATILE(uint32_t *,
				&cv->c_has_waiters), 0);

		umtxq_lock(&key);
		umtxq_unbusy(&key);
		umtxq_unlock(&key);
		umtx_key_release(&key);

		cv_after_migration(oldlen, bind_mutex, uhm);

		umtxq_unbusy(&mkey);
		umtxq_unlock(&mkey);
		return (0);
	} else {
		umtxq_unlock(&key);
	}

wake_one:
	if (len == 1)
		suword32(__DEVOLATILE(uint32_t *, &cv->c_has_waiters), 0);
	umtxq_lock(&key);
	uq = TAILQ_FIRST(&uh->head);
	umtxq_signal_thread(uq);
	umtxq_unbusy(&key);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	return (0);
}

static int
do_cv_broadcast(struct thread *td, struct ucond *cv)
{
	struct umtxq_queue *uh, *uhm, *uh_temp;
	struct umtxq_chain *uc, *ucm;
	struct umtx_key key;
	int error;
	uint32_t flags;

	flags = fuword32(&cv->c_flags);
	if ((error = umtx_key_get(cv, TYPE_CV, GET_SHARE(flags), &key)) != 0)
		return (error);	

	umtxq_lock(&key);
	umtxq_busy(&key);
	uh = umtxq_queue_lookup(&key, UMTX_SHARED_QUEUE);
	if (uh != NULL && uh->binding) {
		/*
		 * To avoid thundering herd problem, if there are waiters,
		 * try to move them to mutex queue.
		 */
		struct umutex *bind_mutex = uh->bind_mutex;
		struct umtx_key mkey;
		struct umtx_q *uq;
		int len, oldlen;

		len = uh->length;
		mkey = uh->bind_mkey;
		uc = umtxq_getchain(&key);
		ucm = umtxq_getchain(&mkey);
		LIST_REMOVE(uh, link);

		/*
		 * Before busying mutex sleep-queue, we must unlock cv's
		 * sleep-queue mutex, because the mutex is unsleepable.
		 */
		umtxq_unlock(&key);

		umtxq_lock(&mkey);
		umtxq_busy(&mkey);
		umtxq_unlock(&mkey);
		umtxq_lock(&key);
		umtxq_lock(&mkey);
		uhm = umtxq_queue_lookup(&mkey, UMTX_SHARED_QUEUE);

		/* Change waiter's key (include chain address). */
		TAILQ_FOREACH(uq, &uh->head, uq_link) {
			umtx_key_copy(&uq->uq_key, &mkey);
			if (uhm != NULL)
				uq->uq_cur_queue = uhm;
		}
		if (uhm == NULL) {
			/*
			 * Mutex has no waiters, just move the queue head to
			 * new chain.
			 */
			oldlen = 0;
			uh->key = mkey;
			uh->bind_mutex = NULL;
			uh->binding = 0;
			LIST_INSERT_HEAD(&ucm->uc_queue[UMTX_SHARED_QUEUE],
				uh, link);
			uhm = uh;
		} else {
			/*
			 * Otherwise, move cv waiters.
			 */
			oldlen = uhm->length;
			TAILQ_CONCAT(&uhm->head, &uh->head, uq_link);
			uhm->length += uh->length;
			uh->length = 0;
			uh->bind_mutex = NULL;
			uh->binding = 0;
			LIST_INSERT_HEAD(&ucm->uc_spare_queue, uh, link);
		}

		UMTX_STATE_ADD(cv_broadcast_migrate, len);

		/*
		 * At this point, cv's queue no longer needs to be accessed,
		 * NULL it.
		 */
		uh = NULL;

		/*
		 * One queue head has already been moved, we need to
		 * move (n - 1) free queue head to new chain.
		 */
		while (--len > 0) {
			uh_temp = LIST_FIRST(&uc->uc_spare_queue);
			LIST_REMOVE(uh_temp, link);
			LIST_INSERT_HEAD(&ucm->uc_spare_queue, uh_temp, link);
		}

		umtxq_unlock(&mkey);
		umtxq_unlock(&key);

		/* Now, the cv does not have any waiter. */
		suword32(__DEVOLATILE(uint32_t *, &cv->c_has_waiters), 0);

		umtxq_lock(&key);
		umtxq_unbusy(&key);
		umtxq_unlock(&key);
		umtx_key_release(&key);

		cv_after_migration(oldlen, bind_mutex, uhm);

		umtxq_unbusy(&mkey);
		umtxq_unlock(&mkey);
		return (0);
	} else {
		umtxq_signal(&key, INT_MAX);
		umtxq_unlock(&key);
		suword32(__DEVOLATILE(uint32_t *, &cv->c_has_waiters), 0);
		umtxq_lock(&key);
		umtxq_unbusy(&key);
		umtxq_unlock(&key);
		umtx_key_release(&key);
	}
	return (0);
}

static int
do_rw_rdlock(struct thread *td, struct urwlock *rwlock, long fflag, int timo)
{
	struct umtx_q *uq;
	uint32_t flags, wrflags;
	int32_t state, oldstate;
	int32_t blocked_readers;
	int error;

	uq = td->td_umtxq;
	flags = fuword32(&rwlock->rw_flags);
	error = umtx_key_get(rwlock, TYPE_RWLOCK, GET_SHARE(flags), &uq->uq_key);
	if (error != 0)
		return (error);

	wrflags = URWLOCK_WRITE_OWNER;
	if (!(fflag & URWLOCK_PREFER_READER) && !(flags & URWLOCK_PREFER_READER))
		wrflags |= URWLOCK_WRITE_WAITERS;

	for (;;) {
		state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));
		/* try to lock it */
		while (!(state & wrflags)) {
			if (__predict_false(URWLOCK_READER_COUNT(state) == URWLOCK_MAX_READERS)) {
				umtx_key_release(&uq->uq_key);
				return (EAGAIN);
			}
			oldstate = casuword32(&rwlock->rw_state, state, state + 1);
			if (oldstate == state) {
				umtx_key_release(&uq->uq_key);
				return (0);
			}
			state = oldstate;
		}

		if (error)
			break;

		/* grab monitor lock */
		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

		/*
		 * re-read the state, in case it changed between the try-lock above
		 * and the check below
		 */
		state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));

		/* set read contention bit */
		while ((state & wrflags) && !(state & URWLOCK_READ_WAITERS)) {
			oldstate = casuword32(&rwlock->rw_state, state, state | URWLOCK_READ_WAITERS);
			if (oldstate == state)
				goto sleep;
			state = oldstate;
		}

		/* state is changed while setting flags, restart */
		if (!(state & wrflags)) {
			umtxq_lock(&uq->uq_key);
			umtxq_unbusy(&uq->uq_key);
			umtxq_unlock(&uq->uq_key);
			continue;
		}

sleep:
		/* contention bit is set, before sleeping, increase read waiter count */
		blocked_readers = fuword32(&rwlock->rw_blocked_readers);
		suword32(&rwlock->rw_blocked_readers, blocked_readers+1);

		while (state & wrflags) {
			umtxq_lock(&uq->uq_key);
			umtxq_insert(uq);
			umtxq_unbusy(&uq->uq_key);

			error = umtxq_sleep(uq, "urdlck", timo);

			umtxq_busy(&uq->uq_key);
			umtxq_remove(uq);
			umtxq_unlock(&uq->uq_key);
			if (error)
				break;
			state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));
		}

		/* decrease read waiter count, and may clear read contention bit */
		blocked_readers = fuword32(&rwlock->rw_blocked_readers);
		suword32(&rwlock->rw_blocked_readers, blocked_readers-1);
		if (blocked_readers == 1) {
			state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));
			for (;;) {
				oldstate = casuword32(&rwlock->rw_state, state,
					 state & ~URWLOCK_READ_WAITERS);
				if (oldstate == state)
					break;
				state = oldstate;
			}
		}

		umtxq_lock(&uq->uq_key);
		umtxq_unbusy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);
	}
	umtx_key_release(&uq->uq_key);
	return (error);
}

static int
do_rw_rdlock2(struct thread *td, void *obj, long val, struct timespec *timeout)
{
	struct timespec ts, ts2, ts3;
	struct timeval tv;
	int error;

	getnanouptime(&ts);
	timespecadd(&ts, timeout);
	TIMESPEC_TO_TIMEVAL(&tv, timeout);
	for (;;) {
		error = do_rw_rdlock(td, obj, val, tvtohz(&tv));
		if (error != ETIMEDOUT)
			break;
		getnanouptime(&ts2);
		if (timespeccmp(&ts2, &ts, >=)) {
			error = ETIMEDOUT;
			break;
		}
		ts3 = ts;
		timespecsub(&ts3, &ts2);
		TIMESPEC_TO_TIMEVAL(&tv, &ts3);
	}
	if (error == ERESTART)
		error = EINTR;
	return (error);
}

static int
do_rw_wrlock(struct thread *td, struct urwlock *rwlock, int timo)
{
	struct umtx_q *uq;
	uint32_t flags;
	int32_t state, oldstate;
	int32_t blocked_writers;
	int32_t blocked_readers;
	int error;

	uq = td->td_umtxq;
	flags = fuword32(&rwlock->rw_flags);
	error = umtx_key_get(rwlock, TYPE_RWLOCK, GET_SHARE(flags), &uq->uq_key);
	if (error != 0)
		return (error);

	blocked_readers = 0;
	for (;;) {
		state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));
		while (!(state & URWLOCK_WRITE_OWNER) && URWLOCK_READER_COUNT(state) == 0) {
			oldstate = casuword32(&rwlock->rw_state, state, state | URWLOCK_WRITE_OWNER);
			if (oldstate == state) {
				umtx_key_release(&uq->uq_key);
				return (0);
			}
			state = oldstate;
		}

		if (error) {
			if (!(state & (URWLOCK_WRITE_OWNER|URWLOCK_WRITE_WAITERS)) &&
			    blocked_readers != 0) {
				umtxq_lock(&uq->uq_key);
				umtxq_busy(&uq->uq_key);
				umtxq_signal_queue(&uq->uq_key, INT_MAX, UMTX_SHARED_QUEUE);
				umtxq_unbusy(&uq->uq_key);
				umtxq_unlock(&uq->uq_key);
			}

			break;
		}

		/* grab monitor lock */
		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);

		/*
		 * re-read the state, in case it changed between the try-lock above
		 * and the check below
		 */
		state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));

		while (((state & URWLOCK_WRITE_OWNER) || URWLOCK_READER_COUNT(state) != 0) &&
		       (state & URWLOCK_WRITE_WAITERS) == 0) {
			oldstate = casuword32(&rwlock->rw_state, state, state | URWLOCK_WRITE_WAITERS);
			if (oldstate == state)
				goto sleep;
			state = oldstate;
		}

		if (!(state & URWLOCK_WRITE_OWNER) && URWLOCK_READER_COUNT(state) == 0) {
			umtxq_lock(&uq->uq_key);
			umtxq_unbusy(&uq->uq_key);
			umtxq_unlock(&uq->uq_key);
			continue;
		}
sleep:
		blocked_writers = fuword32(&rwlock->rw_blocked_writers);
		suword32(&rwlock->rw_blocked_writers, blocked_writers+1);

		while ((state & URWLOCK_WRITE_OWNER) || URWLOCK_READER_COUNT(state) != 0) {
			umtxq_lock(&uq->uq_key);
			umtxq_insert_queue(uq, UMTX_EXCLUSIVE_QUEUE);
			umtxq_unbusy(&uq->uq_key);

			error = umtxq_sleep(uq, "uwrlck", timo);

			umtxq_busy(&uq->uq_key);
			umtxq_remove_queue(uq, UMTX_EXCLUSIVE_QUEUE);
			umtxq_unlock(&uq->uq_key);
			if (error)
				break;
			state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));
		}

		blocked_writers = fuword32(&rwlock->rw_blocked_writers);
		suword32(&rwlock->rw_blocked_writers, blocked_writers-1);
		if (blocked_writers == 1) {
			state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));
			for (;;) {
				oldstate = casuword32(&rwlock->rw_state, state,
					 state & ~URWLOCK_WRITE_WAITERS);
				if (oldstate == state)
					break;
				state = oldstate;
			}
			blocked_readers = fuword32(&rwlock->rw_blocked_readers);
		} else
			blocked_readers = 0;

		umtxq_lock(&uq->uq_key);
		umtxq_unbusy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);
	}

	umtx_key_release(&uq->uq_key);
	return (error);
}

static int
do_rw_wrlock2(struct thread *td, void *obj, struct timespec *timeout)
{
	struct timespec ts, ts2, ts3;
	struct timeval tv;
	int error;

	getnanouptime(&ts);
	timespecadd(&ts, timeout);
	TIMESPEC_TO_TIMEVAL(&tv, timeout);
	for (;;) {
		error = do_rw_wrlock(td, obj, tvtohz(&tv));
		if (error != ETIMEDOUT)
			break;
		getnanouptime(&ts2);
		if (timespeccmp(&ts2, &ts, >=)) {
			error = ETIMEDOUT;
			break;
		}
		ts3 = ts;
		timespecsub(&ts3, &ts2);
		TIMESPEC_TO_TIMEVAL(&tv, &ts3);
	}
	if (error == ERESTART)
		error = EINTR;
	return (error);
}

static int
do_rw_unlock(struct thread *td, struct urwlock *rwlock)
{
	struct umtx_q *uq;
	uint32_t flags;
	int32_t state, oldstate;
	int error, q, count;

	uq = td->td_umtxq;
	flags = fuword32(&rwlock->rw_flags);
	error = umtx_key_get(rwlock, TYPE_RWLOCK, GET_SHARE(flags), &uq->uq_key);
	if (error != 0)
		return (error);

	state = fuword32(__DEVOLATILE(int32_t *, &rwlock->rw_state));
	if (state & URWLOCK_WRITE_OWNER) {
		for (;;) {
			oldstate = casuword32(&rwlock->rw_state, state, 
				state & ~URWLOCK_WRITE_OWNER);
			if (oldstate != state) {
				state = oldstate;
				if (!(oldstate & URWLOCK_WRITE_OWNER)) {
					error = EPERM;
					goto out;
				}
			} else
				break;
		}
	} else if (URWLOCK_READER_COUNT(state) != 0) {
		for (;;) {
			oldstate = casuword32(&rwlock->rw_state, state,
				state - 1);
			if (oldstate != state) {
				state = oldstate;
				if (URWLOCK_READER_COUNT(oldstate) == 0) {
					error = EPERM;
					goto out;
				}
			}
			else
				break;
		}
	} else {
		error = EPERM;
		goto out;
	}

	count = 0;

	if (!(flags & URWLOCK_PREFER_READER)) {
		if (state & URWLOCK_WRITE_WAITERS) {
			count = 1;
			q = UMTX_EXCLUSIVE_QUEUE;
		} else if (state & URWLOCK_READ_WAITERS) {
			count = INT_MAX;
			q = UMTX_SHARED_QUEUE;
		}
	} else {
		if (state & URWLOCK_READ_WAITERS) {
			count = INT_MAX;
			q = UMTX_SHARED_QUEUE;
		} else if (state & URWLOCK_WRITE_WAITERS) {
			count = 1;
			q = UMTX_EXCLUSIVE_QUEUE;
		}
	}

	if (count) {
		umtxq_lock(&uq->uq_key);
		umtxq_busy(&uq->uq_key);
		umtxq_signal_queue(&uq->uq_key, count, q);
		umtxq_unbusy(&uq->uq_key);
		umtxq_unlock(&uq->uq_key);
	}
out:
	umtx_key_release(&uq->uq_key);
	return (error);
}

static int
do_sem_wait(struct thread *td, struct _usem *sem, struct timespec *timeout)
{
	struct umtx_q *uq;
	struct timeval tv;
	struct timespec cts, ets, tts;
	uint32_t flags, count;
	int error;

	uq = td->td_umtxq;
	flags = fuword32(&sem->_flags);
	error = umtx_key_get(sem, TYPE_SEM, GET_SHARE(flags), &uq->uq_key);
	if (error != 0)
		return (error);
	umtxq_lock(&uq->uq_key);
	umtxq_busy(&uq->uq_key);
	umtxq_insert(uq);
	umtxq_unlock(&uq->uq_key);

	/* Don't modify cacheline when unnecessary. */
	if (fuword32(__DEVOLATILE(uint32_t *, &sem->_has_waiters)) == 0)
		suword32(__DEVOLATILE(uint32_t *, &sem->_has_waiters), 1);

	count = fuword32(__DEVOLATILE(uint32_t *, &sem->_count));
	if (count != 0) {
		umtxq_lock(&uq->uq_key);
		umtxq_unbusy(&uq->uq_key);
		umtxq_remove(uq);
		umtxq_unlock(&uq->uq_key);
		umtx_key_release(&uq->uq_key);
		return (0);
	}

	umtxq_lock(&uq->uq_key);
	umtxq_unbusy(&uq->uq_key);
	umtxq_unlock(&uq->uq_key);

	umtxq_lock(&uq->uq_key);
	if (timeout == NULL) {
		error = umtxq_sleep(uq, "usem", 0);
	} else {
		getnanouptime(&ets);
		timespecadd(&ets, timeout);
		TIMESPEC_TO_TIMEVAL(&tv, timeout);
		for (;;) {
			error = umtxq_sleep(uq, "usem", tvtohz(&tv));
			if (error != ETIMEDOUT)
				break;
			getnanouptime(&cts);
			if (timespeccmp(&cts, &ets, >=)) {
				error = ETIMEDOUT;
				break;
			}
			tts = ets;
			timespecsub(&tts, &cts);
			TIMESPEC_TO_TIMEVAL(&tv, &tts);
		}
	}

	if ((uq->uq_flags & UQF_UMTXQ) == 0)
		error = 0;
	else {
		umtxq_remove(uq);
		if (error == ERESTART)
			error = EINTR;
	}
	umtxq_unlock(&uq->uq_key);
	umtx_key_release(&uq->uq_key);
	return (error);
}

/*
 * Signal a userland condition variable.
 */
static int
do_sem_wake(struct thread *td, struct _usem *sem)
{
	struct umtx_key key;
	int error, cnt, nwake;
	uint32_t flags;

	flags = fuword32(&sem->_flags);
	if ((error = umtx_key_get(sem, TYPE_SEM, GET_SHARE(flags), &key)) != 0)
		return (error);	
	umtxq_lock(&key);
	umtxq_busy(&key);
	cnt = umtxq_count(&key);
	nwake = umtxq_signal(&key, 1);
	if (cnt <= nwake) {
		umtxq_unlock(&key);
		error = suword32(
		    __DEVOLATILE(uint32_t *, &sem->_has_waiters), 0);
		umtxq_lock(&key);
	}
	umtxq_unbusy(&key);
	umtxq_unlock(&key);
	umtx_key_release(&key);
	return (error);
}

int
_umtx_lock(struct thread *td, struct _umtx_lock_args *uap)
    /* struct umtx *umtx */
{
	return _do_lock_umtx(td, uap->umtx, td->td_tid, 0);
}

int
_umtx_unlock(struct thread *td, struct _umtx_unlock_args *uap)
    /* struct umtx *umtx */
{
	return do_unlock_umtx(td, uap->umtx, td->td_tid);
}

static int
__umtx_op_lock_umtx(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout, sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		ts = &timeout;
	}
	return (do_lock_umtx(td, uap->obj, uap->val, ts));
}

static int
__umtx_op_unlock_umtx(struct thread *td, struct _umtx_op_args *uap)
{
	return (do_unlock_umtx(td, uap->obj, uap->val));
}

static int
__umtx_op_wait(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout, sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return do_wait(td, uap->obj, uap->val, ts, 0, 0);
}

static int
__umtx_op_wait_uint(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout, sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return do_wait(td, uap->obj, uap->val, ts, 1, 0);
}

static int
__umtx_op_wait_uint_private(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout, sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return do_wait(td, uap->obj, uap->val, ts, 1, 1);
}

static int
__umtx_op_wake(struct thread *td, struct _umtx_op_args *uap)
{
	return (kern_umtx_wake(td, uap->obj, uap->val, 0));
}

static int
__umtx_op_wake_private(struct thread *td, struct _umtx_op_args *uap)
{
	return (kern_umtx_wake(td, uap->obj, uap->val, 1));
}

static int
__umtx_op_lock_umutex(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout,
		    sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		ts = &timeout;
	}
	return do_lock_umutex(td, uap->obj, ts, 0, uap->val);
}

static int
__umtx_op_trylock_umutex(struct thread *td, struct _umtx_op_args *uap)
{
	return do_lock_umutex(td, uap->obj, NULL, _UMUTEX_TRY, 0);
}

static int
__umtx_op_wait_umutex(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout,
		    sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		ts = &timeout;
	}
	return do_lock_umutex(td, uap->obj, ts, _UMUTEX_WAIT, uap->val);
}

static int
__umtx_op_wake_umutex(struct thread *td, struct _umtx_op_args *uap)
{
	return do_wake_umutex(td, uap->obj);
}

static int
__umtx_op_unlock_umutex(struct thread *td, struct _umtx_op_args *uap)
{
	return do_unlock_umutex(td, uap->obj, 0);
}

static int
__umtx_op_set_ceiling(struct thread *td, struct _umtx_op_args *uap)
{
	return do_set_ceiling(td, uap->obj, uap->val, uap->uaddr1);
}

static int
__umtx_op_cv_wait(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout,
		    sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		ts = &timeout;
	}
	return (do_cv_wait(td, uap->obj, uap->uaddr1, ts, uap->val));
}

static int
__umtx_op_cv_signal(struct thread *td, struct _umtx_op_args *uap)
{
	return do_cv_signal(td, uap->obj);
}

static int
__umtx_op_cv_broadcast(struct thread *td, struct _umtx_op_args *uap)
{
	return do_cv_broadcast(td, uap->obj);
}

static int
__umtx_op_rw_rdlock(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL) {
		error = do_rw_rdlock(td, uap->obj, uap->val, 0);
	} else {
		error = copyin(uap->uaddr2, &timeout,
		    sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		error = do_rw_rdlock2(td, uap->obj, uap->val, &timeout);
	}
	return (error);
}

static int
__umtx_op_rw_wrlock(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL) {
		error = do_rw_wrlock(td, uap->obj, 0);
	} else {
		error = copyin(uap->uaddr2, &timeout,
		    sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}

		error = do_rw_wrlock2(td, uap->obj, &timeout);
	}
	return (error);
}

static int
__umtx_op_rw_unlock(struct thread *td, struct _umtx_op_args *uap)
{
	return do_rw_unlock(td, uap->obj);
}

static int
__umtx_op_sem_wait(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin(uap->uaddr2, &timeout,
		    sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		ts = &timeout;
	}
	return (do_sem_wait(td, uap->obj, ts));
}

static int
__umtx_op_sem_wake(struct thread *td, struct _umtx_op_args *uap)
{
	return do_sem_wake(td, uap->obj);
}

typedef int (*_umtx_op_func)(struct thread *td, struct _umtx_op_args *uap);

static _umtx_op_func op_table[] = {
	__umtx_op_lock_umtx,		/* UMTX_OP_LOCK */
	__umtx_op_unlock_umtx,		/* UMTX_OP_UNLOCK */
	__umtx_op_wait,			/* UMTX_OP_WAIT */
	__umtx_op_wake,			/* UMTX_OP_WAKE */
	__umtx_op_trylock_umutex,	/* UMTX_OP_MUTEX_TRYLOCK */
	__umtx_op_lock_umutex,		/* UMTX_OP_MUTEX_LOCK */
	__umtx_op_unlock_umutex,	/* UMTX_OP_MUTEX_UNLOCK */
	__umtx_op_set_ceiling,		/* UMTX_OP_SET_CEILING */
	__umtx_op_cv_wait,		/* UMTX_OP_CV_WAIT*/
	__umtx_op_cv_signal,		/* UMTX_OP_CV_SIGNAL */
	__umtx_op_cv_broadcast,		/* UMTX_OP_CV_BROADCAST */
	__umtx_op_wait_uint,		/* UMTX_OP_WAIT_UINT */
	__umtx_op_rw_rdlock,		/* UMTX_OP_RW_RDLOCK */
	__umtx_op_rw_wrlock,		/* UMTX_OP_RW_WRLOCK */
	__umtx_op_rw_unlock,		/* UMTX_OP_RW_UNLOCK */
	__umtx_op_wait_uint_private,	/* UMTX_OP_WAIT_UINT_PRIVATE */
	__umtx_op_wake_private,		/* UMTX_OP_WAKE_PRIVATE */
	__umtx_op_wait_umutex,		/* UMTX_OP_UMUTEX_WAIT */
	__umtx_op_wake_umutex,		/* UMTX_OP_UMUTEX_WAKE */
	__umtx_op_sem_wait,		/* UMTX_OP_SEM_WAIT */
	__umtx_op_sem_wake		/* UMTX_OP_SEM_WAKE */
};

int
_umtx_op(struct thread *td, struct _umtx_op_args *uap)
{
	if ((unsigned)uap->op < UMTX_OP_MAX)
		return (*op_table[uap->op])(td, uap);
	return (EINVAL);
}

#ifdef COMPAT_FREEBSD32
int
freebsd32_umtx_lock(struct thread *td, struct freebsd32_umtx_lock_args *uap)
    /* struct umtx *umtx */
{
	return (do_lock_umtx32(td, (uint32_t *)uap->umtx, td->td_tid, NULL));
}

int
freebsd32_umtx_unlock(struct thread *td, struct freebsd32_umtx_unlock_args *uap)
    /* struct umtx *umtx */
{
	return (do_unlock_umtx32(td, (uint32_t *)uap->umtx, td->td_tid));
}

struct timespec32 {
	uint32_t tv_sec;
	uint32_t tv_nsec;
};

static inline int
copyin_timeout32(void *addr, struct timespec *tsp)
{
	struct timespec32 ts32;
	int error;

	error = copyin(addr, &ts32, sizeof(struct timespec32));
	if (error == 0) {
		tsp->tv_sec = ts32.tv_sec;
		tsp->tv_nsec = ts32.tv_nsec;
	}
	return (error);
}

static int
__umtx_op_lock_umtx_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		ts = &timeout;
	}
	return (do_lock_umtx32(td, uap->obj, uap->val, ts));
}

static int
__umtx_op_unlock_umtx_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	return (do_unlock_umtx32(td, uap->obj, (uint32_t)uap->val));
}

static int
__umtx_op_wait_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return do_wait(td, uap->obj, uap->val, ts, 1, 0);
}

static int
__umtx_op_lock_umutex_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return do_lock_umutex(td, uap->obj, ts, 0);
}

static int
__umtx_op_wait_umutex_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return do_lock_umutex(td, uap->obj, ts, _UMUTEX_WAIT);
}

static int
__umtx_op_cv_wait_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return (do_cv_wait(td, uap->obj, uap->uaddr1, ts, uap->val));
}

static int
__umtx_op_rw_rdlock_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL) {
		error = do_rw_rdlock(td, uap->obj, uap->val, 0);
	} else {
		error = copyin(uap->uaddr2, &timeout,
		    sizeof(timeout));
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}
		error = do_rw_rdlock2(td, uap->obj, uap->val, &timeout);
	}
	return (error);
}

static int
__umtx_op_rw_wrlock_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL) {
		error = do_rw_wrlock(td, uap->obj, 0);
	} else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0) {
			return (EINVAL);
		}

		error = do_rw_wrlock2(td, uap->obj, &timeout);
	}
	return (error);
}

static int
__umtx_op_wait_uint_private_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return do_wait(td, uap->obj, uap->val, ts, 1, 1);
}

static int
__umtx_op_sem_wait_compat32(struct thread *td, struct _umtx_op_args *uap)
{
	struct timespec *ts, timeout;
	int error;

	/* Allow a null timespec (wait forever). */
	if (uap->uaddr2 == NULL)
		ts = NULL;
	else {
		error = copyin_timeout32(uap->uaddr2, &timeout);
		if (error != 0)
			return (error);
		if (timeout.tv_nsec >= 1000000000 ||
		    timeout.tv_nsec < 0)
			return (EINVAL);
		ts = &timeout;
	}
	return (do_sem_wait(td, uap->obj, ts));
}

static _umtx_op_func op_table_compat32[] = {
	__umtx_op_lock_umtx_compat32,	/* UMTX_OP_LOCK */
	__umtx_op_unlock_umtx_compat32,	/* UMTX_OP_UNLOCK */
	__umtx_op_wait_compat32,	/* UMTX_OP_WAIT */
	__umtx_op_wake,			/* UMTX_OP_WAKE */
	__umtx_op_trylock_umutex,	/* UMTX_OP_MUTEX_LOCK */
	__umtx_op_lock_umutex_compat32,	/* UMTX_OP_MUTEX_TRYLOCK */
	__umtx_op_unlock_umutex,	/* UMTX_OP_MUTEX_UNLOCK	*/
	__umtx_op_set_ceiling,		/* UMTX_OP_SET_CEILING */
	__umtx_op_cv_wait_compat32,	/* UMTX_OP_CV_WAIT*/
	__umtx_op_cv_signal,		/* UMTX_OP_CV_SIGNAL */
	__umtx_op_cv_broadcast,		/* UMTX_OP_CV_BROADCAST */
	__umtx_op_wait_compat32,	/* UMTX_OP_WAIT_UINT */
	__umtx_op_rw_rdlock_compat32,	/* UMTX_OP_RW_RDLOCK */
	__umtx_op_rw_wrlock_compat32,	/* UMTX_OP_RW_WRLOCK */
	__umtx_op_rw_unlock,		/* UMTX_OP_RW_UNLOCK */
	__umtx_op_wait_uint_private_compat32,	/* UMTX_OP_WAIT_UINT_PRIVATE */
	__umtx_op_wake_private,		/* UMTX_OP_WAKE_PRIVATE */
	__umtx_op_wait_umutex_compat32, /* UMTX_OP_UMUTEX_WAIT */
	__umtx_op_wake_umutex,		/* UMTX_OP_UMUTEX_WAKE */
	__umtx_op_sem_wait_compat32,	/* UMTX_OP_SEM_WAIT */
	__umtx_op_sem_wake		/* UMTX_OP_SEM_WAKE */
};

int
freebsd32_umtx_op(struct thread *td, struct freebsd32_umtx_op_args *uap)
{
	if ((unsigned)uap->op < UMTX_OP_MAX)
		return (*op_table_compat32[uap->op])(td,
			(struct _umtx_op_args *)uap);
	return (EINVAL);
}
#endif

int
robust_alloc(struct robust_info **robpp)
{
	struct proc *p = curproc;
	int error;

	atomic_fetchadd_int(&p->p_robustcount, 1);
	if (p->p_robustcount > max_robust_per_proc) {
		mtx_lock(&max_robust_lock);
		while (p->p_robustcount >= max_robust_per_proc) {
			if (ratecheck(&max_robust_lasttime,
				 &max_robust_interval)) {
				printf("Process %lu (%s) exceeded maximum"
				  "number of robust mutexes\n",
				   (u_long)p->p_pid, p->p_comm);
			}
			error = msleep(&max_robust_per_proc,
				&max_robust_lock, 0, "maxrob", 0);
			if (error != 0) {
				mtx_unlock(&max_robust_lock);
				atomic_fetchadd_int(&p->p_robustcount, -1);
				return (error);
			}
		}
		mtx_unlock(&max_robust_lock);
	}
	*robpp = uma_zalloc(robust_zone, M_ZERO|M_WAITOK);
	return (0);
}

static void
robust_free(struct robust_info *robp)
{
	struct proc *p = curproc;

	atomic_fetchadd_int(&p->p_robustcount, -1);
	uma_zfree(robust_zone, robp);
}

static unsigned int
robust_hash(struct umutex *m)
{
	unsigned n = (uintptr_t)m;
	return ((n * GOLDEN_RATIO_PRIME) >> ROBUST_SHIFTS) % ROBUST_CHAINS;
}

static int
robust_insert(struct thread *td, struct robust_info *rob)
{
	struct umtx_q *uq = td->td_umtxq;
	struct robust_info *rob2;
	int hash = robust_hash(rob->umtxp);
	struct robust_chain *robc = &robust_chains[hash];

	mtx_lock(&robc->lock);
	SLIST_FOREACH(rob2, &robc->rob_list, hash_qe) {
		if (rob2->ownertd == td &&
		    rob2->umtxp == rob->umtxp) {
			mtx_unlock(&robc->lock);
			return (EEXIST);
		}
	}
	rob->ownertd = td;
	SLIST_INSERT_HEAD(&robc->rob_list, rob, hash_qe);
	mtx_unlock(&robc->lock);
	LIST_INSERT_HEAD(&uq->uq_rob_list, rob, td_qe);
	return (0);
}

static void
robust_remove(struct thread *td, struct umutex *umtxp)
{
	struct robust_info *rob, *rob2;
	int hash = robust_hash(umtxp);
	struct robust_chain *robc = &robust_chains[hash];

	rob2 = NULL;
	mtx_lock(&robc->lock);
	SLIST_FOREACH(rob, &robc->rob_list, hash_qe) {
		if (rob->ownertd == td &&
		    rob->umtxp == umtxp) {
			if (rob2 == NULL) {
				SLIST_REMOVE_HEAD(&robc->rob_list, hash_qe);
			} else {
				SLIST_REMOVE_AFTER(rob2, hash_qe); 
			}
			break;
		}
		rob2 = rob;
	}
	mtx_unlock(&robc->lock);
	if (rob != NULL) {
		LIST_REMOVE(rob, td_qe);
		robust_free(rob);
	}
}

void
umtx_thread_init(struct thread *td)
{
	td->td_umtxq = umtxq_alloc();
	td->td_umtxq->uq_thread = td;
}

void
umtx_thread_fini(struct thread *td)
{
	umtxq_free(td->td_umtxq);
}

/*
 * It will be called when new thread is created, e.g fork().
 */
void
umtx_thread_alloc(struct thread *td)
{
	struct umtx_q *uq;

	uq = td->td_umtxq;
	uq->uq_inherited_pri = PRI_MAX;
	uq->uq_exiting = 0;

	KASSERT(uq->uq_flags == 0, ("uq_flags != 0"));
	KASSERT(uq->uq_thread == td, ("uq_thread != td"));
	KASSERT(uq->uq_pi_blocked == NULL, ("uq_pi_blocked != NULL"));
	KASSERT(TAILQ_EMPTY(&uq->uq_pi_contested), ("uq_pi_contested is not empty"));
}

/*
 * exec() hook, clean up lastest thread's umtx info.
 */
static void
umtx_exec_hook(void *arg __unused, struct proc *p __unused,
	struct image_params *imgp __unused)
{
	umtx_thread_cleanup(curthread);
}

/*
 * exit1() hook, clean up lastest thread's umtx info.
 */
static void
umtx_exit_hook(void *arg __unused, struct proc *p __unused)
{
	struct umtx_q *uq = curthread->td_umtxq;

	if (uq != NULL) {
		uq->uq_exiting = 1;
		umtx_thread_cleanup(curthread);
	}
}

/*
 * fork() hook. First thread of process never call umtx_thread_alloc()
 * again, we should clear uq_exiting here.
 */
void
umtx_fork_hook(void *arg __unused, struct proc *p1 __unused,
	struct proc *p2, int flags __unused)
{
	struct thread *td = FIRST_THREAD_IN_PROC(p2);
	struct umtx_q *uq = td->td_umtxq;

	if (uq != NULL)
		uq->uq_exiting = 0;
}

/*
 * thread_exit() hook.
 */
void
umtx_thread_exit(struct thread *td)
{
	umtx_thread_cleanup(td);
}

/*
 * clean up umtx data.
 */
static void
umtx_thread_cleanup(struct thread *td)
{
	struct umtx_q *uq;
	struct umtx_pi *pi;
	struct robust_info *rob;

	if ((uq = td->td_umtxq) == NULL)
		return;

	while ((rob = LIST_FIRST(&uq->uq_rob_list)) != NULL)
		do_unlock_umutex(td, rob->umtxp, 1);

	mtx_lock_spin(&umtx_lock);
	uq->uq_inherited_pri = PRI_MAX;
	while ((pi = TAILQ_FIRST(&uq->uq_pi_contested)) != NULL) {
		pi->pi_owner = NULL;
		TAILQ_REMOVE(&uq->uq_pi_contested, pi, pi_link);
	}
	thread_lock(td);
	td->td_flags &= ~TDF_UBORROWING;
	thread_unlock(td);
	mtx_unlock_spin(&umtx_lock);
}

static int
set_max_robust(SYSCTL_HANDLER_ARGS)
{
	int error, v;

	v = max_robust_per_proc;
	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error)
		return (error);
	if (req->newptr == NULL)
		return (error);
	if (v <= 0)
		return (EINVAL);
	mtx_lock(&max_robust_lock);
	max_robust_per_proc = v;
	wakeup(&max_robust_per_proc);
	mtx_unlock(&max_robust_lock);
	return (0);
}
