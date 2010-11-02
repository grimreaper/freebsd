/*
 * Copyright (c) 1995 John Birrell <jb@cimlogic.com.au>.
 * Copyright (c) 2006 David Xu <davidxu@freebsd.org>.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by John Birrell.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY JOHN BIRRELL AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
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

#include "namespace.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <pthread.h>
#include <pthread_np.h>
#include "un-namespace.h"

#include "thr_private.h"

#ifndef UMUTEX_TIDMASK
#define UMUTEX_TIDMASK	(~UMUTEX_CONTESTED)
#endif

#if defined(_PTHREADS_INVARIANTS)
#define MUTEX_INIT_LINK(m) 		do {		\
	(m)->m_qe.tqe_prev = NULL;			\
	(m)->m_qe.tqe_next = NULL;			\
} while (0)
#define MUTEX_ASSERT_IS_OWNED(m)	do {		\
	if (__predict_false((m)->m_qe.tqe_prev == NULL))\
		PANIC("mutex is not on list");		\
} while (0)
#define MUTEX_ASSERT_NOT_OWNED(m)	do {		\
	if (__predict_false((m)->m_qe.tqe_prev != NULL ||\
	    (m)->m_qe.tqe_next != NULL))		\
		PANIC("mutex is on list");		\
} while (0)
#else
#define MUTEX_INIT_LINK(m)
#define MUTEX_ASSERT_IS_OWNED(m)
#define MUTEX_ASSERT_NOT_OWNED(m)
#endif

/*
 * For adaptive mutexes, how many times to spin doing trylock2
 * before entering the kernel to block
 */
#define MUTEX_ADAPTIVE_SPINS	2000

/*
 * Prototypes
 */
int	__pthread_mutex_init(pthread_mutex_t *mutex,
		const pthread_mutexattr_t *mutex_attr);
int	__pthread_mutex_trylock(pthread_mutex_t *mutex);
int	__pthread_mutex_lock(pthread_mutex_t *mutex);
int	__pthread_mutex_timedlock(pthread_mutex_t *mutex,
		const struct timespec *abstime);
int	_pthread_mutex_init_calloc_cb(pthread_mutex_t *mutex,
    		void *(calloc_cb)(size_t, size_t));
int	_pthread_mutex_getspinloops_np(pthread_mutex_t *mutex, int *count);
int	_pthread_mutex_setspinloops_np(pthread_mutex_t *mutex, int count);
int	__pthread_mutex_setspinloops_np(pthread_mutex_t *mutex, int count);
int	_pthread_mutex_setyieldloops_np(pthread_mutex_t *mutex, int count);
int	_pthread_mutex_getyieldloops_np(pthread_mutex_t *mutex, int *count);
int	__pthread_mutex_setyieldloops_np(pthread_mutex_t *mutex, int count);

static int	mutex_self_trylock(pthread_mutex_t);
static int	mutex_self_lock(pthread_mutex_t,
				const struct timespec *abstime);
static int	mutex_unlock_common(pthread_mutex_t *);

__weak_reference(__pthread_mutex_init, pthread_mutex_init);
__strong_reference(__pthread_mutex_init, _pthread_mutex_init);
__weak_reference(__pthread_mutex_lock, pthread_mutex_lock);
__strong_reference(__pthread_mutex_lock, _pthread_mutex_lock);
__weak_reference(__pthread_mutex_timedlock, pthread_mutex_timedlock);
__strong_reference(__pthread_mutex_timedlock, _pthread_mutex_timedlock);
__weak_reference(__pthread_mutex_trylock, pthread_mutex_trylock);
__strong_reference(__pthread_mutex_trylock, _pthread_mutex_trylock);

/* Single underscore versions provided for libc internal usage: */
/* No difference between libc and application usage of these: */
__weak_reference(_pthread_mutex_destroy, pthread_mutex_destroy);
__weak_reference(_pthread_mutex_unlock, pthread_mutex_unlock);

__weak_reference(_pthread_mutex_getprioceiling, pthread_mutex_getprioceiling);
__weak_reference(_pthread_mutex_setprioceiling, pthread_mutex_setprioceiling);

__weak_reference(__pthread_mutex_setspinloops_np, pthread_mutex_setspinloops_np);
__strong_reference(__pthread_mutex_setspinloops_np, _pthread_mutex_setspinloops_np);
__weak_reference(_pthread_mutex_getspinloops_np, pthread_mutex_getspinloops_np);

__weak_reference(__pthread_mutex_setyieldloops_np, pthread_mutex_setyieldloops_np);
__strong_reference(__pthread_mutex_setyieldloops_np, _pthread_mutex_setyieldloops_np);
__weak_reference(_pthread_mutex_getyieldloops_np, pthread_mutex_getyieldloops_np);
__weak_reference(_pthread_mutex_isowned_np, pthread_mutex_isowned_np);

static int
mutex_init(pthread_mutex_t *mutex,
    const struct pthread_mutex_attr *mutex_attr,
    void *(calloc_cb)(size_t, size_t))
{
	const struct pthread_mutex_attr *attr;
	struct pthread_mutex *pmutex;

	if (mutex_attr == NULL) {
		attr = &_pthread_mutexattr_default;
	} else {
		attr = mutex_attr;
		if (attr->m_type < PTHREAD_MUTEX_ERRORCHECK ||
		    attr->m_type >= PTHREAD_MUTEX_TYPE_MAX)
			return (EINVAL);
		if (attr->m_protocol < PTHREAD_PRIO_NONE ||
		    attr->m_protocol > PTHREAD_PRIO_PROTECT)
			return (EINVAL);
	}
	if ((pmutex = (pthread_mutex_t)
		calloc_cb(1, sizeof(struct pthread_mutex))) == NULL)
		return (ENOMEM);

	pmutex->m_type = attr->m_type;
	pmutex->m_owner = NULL;
	pmutex->m_recurse = 0;
	pmutex->m_refcount = 0;
	pmutex->m_spinloops = 0;
	pmutex->m_yieldloops = 0;
	MUTEX_INIT_LINK(pmutex);
	switch(attr->m_protocol) {
	case PTHREAD_PRIO_NONE:
		pmutex->m_lock.m_owner = 0;
		pmutex->m_lock.m_flags = 0;
		break;
	case PTHREAD_PRIO_INHERIT:
		pmutex->m_lock.m_owner = UMUTEX_UNOWNED;
		pmutex->m_lock.m_flags = UMUTEX_PRIO_INHERIT;
		break;
	case PTHREAD_PRIO_PROTECT:
		pmutex->m_lock.m_owner = UMUTEX_CONTESTED;
		pmutex->m_lock.m_flags = UMUTEX_PRIO_PROTECT;
		pmutex->m_lock.m_ceilings[0] = attr->m_ceiling;
		break;
	}

	if (pmutex->m_type == PTHREAD_MUTEX_ADAPTIVE_NP) {
		pmutex->m_spinloops =
		    _thr_spinloops ? _thr_spinloops: MUTEX_ADAPTIVE_SPINS;
		pmutex->m_yieldloops = _thr_yieldloops;
	}
	*mutex = pmutex;
	return (0);
}

static int
init_static(struct pthread *thread, pthread_mutex_t *mutex)
{
	int error;

	THR_LOCK_ACQUIRE(thread, &_mutex_static_lock);

	if (*mutex == THR_MUTEX_INITIALIZER)
		error = mutex_init(mutex, &_pthread_mutexattr_default, calloc);
	else if (*mutex == THR_ADAPTIVE_MUTEX_INITIALIZER)
		error = mutex_init(mutex, &_pthread_mutexattr_adaptive_default, calloc);
	else
		error = 0;
	THR_LOCK_RELEASE(thread, &_mutex_static_lock);

	return (error);
}

static void
set_inherited_priority(struct pthread *curthread, struct pthread_mutex *m)
{
	struct pthread_mutex *m2;

	m2 = TAILQ_LAST(&curthread->pp_mutexq, mutex_queue);
	if (m2 != NULL)
		m->m_lock.m_ceilings[1] = m2->m_lock.m_ceilings[0];
	else
		m->m_lock.m_ceilings[1] = -1;
}

int
__pthread_mutex_init(pthread_mutex_t *mutex,
    const pthread_mutexattr_t *mutex_attr)
{
	return mutex_init(mutex, mutex_attr ? *mutex_attr : NULL, calloc);
}

/* This function is used internally by malloc. */
int
_pthread_mutex_init_calloc_cb(pthread_mutex_t *mutex,
    void *(calloc_cb)(size_t, size_t))
{
	static const struct pthread_mutex_attr attr = {
		.m_type = PTHREAD_MUTEX_NORMAL,
		.m_protocol = PTHREAD_PRIO_NONE,
		.m_ceiling = 0
	};
	int error;

	error = mutex_init(mutex, &attr, calloc_cb);
	if (error == 0)
		(*mutex)->m_private = 1;
	return (error);
}

void
_mutex_fork(struct pthread *curthread)
{
	struct pthread_mutex *m;

	/*
	 * Fix mutex ownership for child process.
	 * note that process shared mutex should not
	 * be inherited because owner is forking thread
	 * which is in parent process, they should be
	 * removed from the owned mutex list, current,
	 * process shared mutex is not supported, so I
	 * am not worried.
	 */
	TAILQ_FOREACH(m, &curthread->mutexq, m_qe) {
		m->m_lock.m_owner = TID(curthread);
	}
	TAILQ_FOREACH(m, &curthread->pp_mutexq, m_qe)
		m->m_lock.m_owner = TID(curthread) | UMUTEX_CONTESTED;
}

int
_pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	pthread_mutex_t m;
	int ret;

	m = *mutex;
	if (m < THR_MUTEX_DESTROYED) {
		ret = 0;
	} else if (m == THR_MUTEX_DESTROYED) {
		ret = EINVAL;
	} else {
		if (m->m_owner != NULL || m->m_refcount != 0) {
			ret = EBUSY;
		} else {
			*mutex = THR_MUTEX_DESTROYED;
			MUTEX_ASSERT_NOT_OWNED(m);
			free(m);
			ret = 0;
		}
	}

	return (ret);
}

#define ENQUEUE_MUTEX(curthread, m)  					\
	do {								\
		(m)->m_owner = curthread;				\
		/* Add to the list of owned mutexes: */			\
		MUTEX_ASSERT_NOT_OWNED((m));				\
		if (__predict_true(((m)->m_lock.m_flags & UMUTEX_PRIO_PROTECT) == 0))	\
			TAILQ_INSERT_TAIL(&curthread->mutexq, (m), m_qe);\
		else							\
			TAILQ_INSERT_TAIL(&curthread->pp_mutexq, (m), m_qe);\
	} while (0)

#define DEQUEUE_MUTEX(curthread, m)					\
	(m)->m_owner = NULL;						\
	/* Remove the mutex from the threads queue. */			\
	MUTEX_ASSERT_IS_OWNED(m);					\
	if (__predict_true(((m)->m_lock.m_flags & UMUTEX_PRIO_PROTECT) == 0))	\
		TAILQ_REMOVE(&curthread->mutexq, (m), m_qe);		\
	else {								\
		TAILQ_REMOVE(&curthread->pp_mutexq, (m), m_qe);		\
		set_inherited_priority(curthread, (m));			\
	}								\
	MUTEX_INIT_LINK(m);

#define CHECK_AND_INIT_MUTEX						\
	if (__predict_false((m = *mutex) <= THR_MUTEX_DESTROYED)) {	\
		if (m == THR_MUTEX_DESTROYED)				\
			return (EINVAL);				\
		int error;						\
		error = init_static(_get_curthread(), mutex);		\
		if (error)						\
			return (error);					\
		m = *mutex;						\
	}

static inline int
set_lockword(struct pthread_mutex *m)
{
	uint32_t old;

	if (atomic_cmpset_acq_32(&m->m_lockword, 0, MTX_LOCKED))
		return (0);
	old = m->m_lockword;
	while ((old & MTX_LOCKED) == 0) {
		if (atomic_cmpset_acq_32(&m->m_lockword, old, old|MTX_LOCKED))
			return (0);
	}
	return (EBUSY);
}

static int
mutex_trylock_common(pthread_mutex_t *mutex)
{
	struct pthread *curthread = _get_curthread();
	struct pthread_mutex *m = *mutex;
	int error;
                 
	if (m->m_private)
		THR_CRITICAL_ENTER(curthread);
	if (IS_SIMPLE_MUTEX(m)) {
		if (set_lockword(m) == 0) {
			m->m_owner = curthread;
			return (0);
		}

		if (m->m_owner == curthread)
			error = mutex_self_trylock(m);
		else
			error = EBUSY;	
	} else {
		uint32_t id;

		id = TID(curthread);
		error = _thr_umutex_trylock(&m->m_lock, id);
		if (__predict_true(error == 0)) {
			ENQUEUE_MUTEX(curthread, m);
		} else if (m->m_owner == curthread) {
			error = mutex_self_trylock(m);
		} /* else {} */
	}
	if (error != 0 && m->m_private)
		THR_CRITICAL_LEAVE(curthread);
        return (error);
}

int
__pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	struct pthread_mutex *m;

	CHECK_AND_INIT_MUTEX

	return (mutex_trylock_common(mutex));
}

/* Lock user-mode queue based mutex. */
static int
mutex_lock_queued(struct pthread_mutex *m,
	const struct timespec *abstime)
{
	struct pthread *curthread  = _get_curthread();
	struct sleepqueue *sq;
	uint32_t old;
	int error = 0;
	int spin;

	if (m->m_owner == curthread)
		return mutex_self_lock(m, abstime);

	if (__predict_false(abstime != NULL &&
	    (abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
	     abstime->tv_nsec >= 1000000000)))
		return (EINVAL);

	spin = m->m_spinloops;
	for (;;) {
		if (!_thr_is_smp)
			goto sleep;
		while (spin-- > 0) {
			/*
			 * For adaptive mutexes, spin for a bit in the expectation
			 * that if the application requests this mutex type then
			 * the lock is likely to be released quickly and it is
			 * faster than entering the kernel
			 */
			old = m->m_lockword;
			if ((old & MTX_LOCKED) == 0) {
				if (set_lockword(m) == 0) {
					m->m_owner = curthread;
					error = 0;
					goto out;
				}
			}
			CPU_SPINWAIT;
		}
sleep:
		_thr_clear_wake(curthread);

		sq = _sleepq_lock(m, MX);
		if (curthread->wchan == NULL)
			_sleepq_add(sq, curthread);
		_sleepq_unlock(sq);
		old = m->m_lockword;
		/* Set contested bit. */
		while ((old & MTX_LOCKED) != 0 && (old & MTX_CONTESTED) == 0) {
			if (atomic_cmpset_acq_32(&m->m_lockword,
			    old, old|MTX_CONTESTED))
				break;
			old = m->m_lockword;
		}
		if ((old & MTX_LOCKED) != 0) {
			error = _thr_sleep(curthread, abstime, CLOCK_REALTIME);
			if (error != EINTR) {
				if (curthread->wchan != NULL) {
					sq = _sleepq_lock(m, MX);
					if (curthread->wchan != NULL)
						_sleepq_remove(sq, curthread);
					_sleepq_unlock(sq);
				}
			} else
				error = 0;
			old = m->m_lockword;
		}
		if (set_lockword(m) == 0) {
			m->m_owner = curthread;
			error = 0;
			break;
		}
		if (error != 0)
			break;
		spin = m->m_spinloops;
	}
out:
	if (curthread->wchan != NULL) {
		sq = _sleepq_lock(m, MX);
		if (curthread->wchan != NULL)
			_sleepq_remove(sq, curthread);
		_sleepq_unlock(sq);
	}
	return (error);
}

/* Enter kernel and lock mutex */
static int
mutex_lock_kernel(struct pthread_mutex *m,
	const struct timespec *abstime)
{
	struct pthread	*curthread = _get_curthread();
	uint32_t	id;
	int		error;

	if (m->m_owner == curthread)
		return mutex_self_lock(m, abstime);

	if (__predict_false(abstime != NULL &&
	   (abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
	    abstime->tv_nsec >= 1000000000)))
		return (EINVAL);

	id = TID(curthread);
	if (abstime == NULL) {
		error = __thr_umutex_lock(&m->m_lock, id);
	} else {
		error = __thr_umutex_timedlock(&m->m_lock, id, abstime);
	}
	if (error == 0)
		ENQUEUE_MUTEX(curthread, m);
	return (error);
}

static inline int
_mutex_lock_common(struct pthread_mutex *m, const struct timespec *abstime)
{
	struct pthread	*curthread  = _get_curthread();

	if (IS_SIMPLE_MUTEX(m)) {
		if (set_lockword(m) == 0) {
			m->m_owner = curthread;
			return (0);
		}
		return mutex_lock_queued(m, abstime);
	} else {
		if (_thr_umutex_trylock2(&m->m_lock, TID(curthread)) == 0) {
			ENQUEUE_MUTEX(curthread, m);
			return (0);
		}
		return mutex_lock_kernel(m, abstime);
	}
}

static inline int
mutex_lock_common(struct pthread_mutex *m, const struct timespec *abstime)
{
	int	error;

	if (!m->m_private) {
		error = _mutex_lock_common(m, abstime);
	} else {
		struct pthread *curthread = _get_curthread();

		THR_CRITICAL_ENTER(curthread);
		error = _mutex_lock_common(m, abstime);
		if (error != 0)
			THR_CRITICAL_LEAVE(curthread);
	}
	return (error);
}

int
__pthread_mutex_lock(pthread_mutex_t *mutex)
{
	struct pthread_mutex	*m;

	_thr_check_init();

	CHECK_AND_INIT_MUTEX

	return (mutex_lock_common(m, NULL));
}

int
__pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime)
{
	struct pthread_mutex	*m;

	_thr_check_init();

	CHECK_AND_INIT_MUTEX

	return (mutex_lock_common(m, abstime));
}

int
_pthread_mutex_unlock(pthread_mutex_t *m)
{
	return (mutex_unlock_common(m));
}

static int
mutex_self_trylock(struct pthread_mutex *m)
{
	int	error;

	switch (m->m_type) {
	case PTHREAD_MUTEX_ERRORCHECK:
	case PTHREAD_MUTEX_NORMAL:
		error = EBUSY; 
		break;

	case PTHREAD_MUTEX_RECURSIVE:
		/* Increment the lock count: */
		if (m->m_recurse + 1 > 0) {
			m->m_recurse++;
			error = 0;
		} else
			error = EAGAIN;
		break;

	default:
		/* Trap invalid mutex types; */
		error = EINVAL;
	}

	return (error);
}

static int
mutex_self_lock(struct pthread_mutex *m, const struct timespec *abstime)
{
	struct timespec	ts1, ts2;
	int	error;

	switch (m->m_type) {
	case PTHREAD_MUTEX_ERRORCHECK:
	case PTHREAD_MUTEX_ADAPTIVE_NP:
		if (abstime) {
			if (abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
			    abstime->tv_nsec >= 1000000000) {
				error = EINVAL;
			} else {
				clock_gettime(CLOCK_REALTIME, &ts1);
				TIMESPEC_SUB(&ts2, abstime, &ts1);
				__sys_nanosleep(&ts2, NULL);
				error = ETIMEDOUT;
			}
		} else {
			/*
			 * POSIX specifies that mutexes should return
			 * EDEADLK if a recursive lock is detected.
			 */
			error = EDEADLK; 
		}
		break;

	case PTHREAD_MUTEX_NORMAL:
		/*
		 * What SS2 define as a 'normal' mutex.  Intentionally
		 * deadlock on attempts to get a lock you already own.
		 */
		error = 0;
		if (abstime) {
			if (abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
			    abstime->tv_nsec >= 1000000000) {
				error = EINVAL;
			} else {
				clock_gettime(CLOCK_REALTIME, &ts1);
				TIMESPEC_SUB(&ts2, abstime, &ts1);
				__sys_nanosleep(&ts2, NULL);
				error = ETIMEDOUT;
			}
		} else {
			for (;;)
				thr_suspend(NULL);
		}
		break;

	case PTHREAD_MUTEX_RECURSIVE:
		/* Increment the lock count: */
		if (m->m_recurse + 1 > 0) {
			m->m_recurse++;
			error = 0;
		} else
			error = EAGAIN;
		break;

	default:
		/* Trap invalid mutex types; */
		error = EINVAL;
	}

	return (error);
}

static int
mutex_unlock_queued(struct pthread *curthread, struct pthread_mutex *m)
{
	struct pthread *td;
	unsigned int *wake_addr = NULL;
	struct sleepqueue *sq;
	int32_t newval;

	m->m_owner = NULL;
	if (!atomic_cmpset_rel_32(&m->m_lock.m_owner, MTX_LOCKED, 0)) {
		sq = _sleepq_lock(m, MX);
		if ((td = _sleepq_first(sq)) != NULL) {
			_sleepq_remove(sq, td);
			if (_sleepq_empty(sq))
				newval = 0;
			else
				newval = MTX_CONTESTED;
			wake_addr = WAKE_ADDR(td);
		} else {
			newval = 0;
			wake_addr = NULL;
		}
		atomic_store_rel_32(&m->m_lockword, newval);
		_sleepq_unlock(sq);
		if (wake_addr != NULL) {
			_thr_set_wake(wake_addr);
			_thr_umtx_wake(wake_addr, INT_MAX, 0);
		}
	}
	return (0);
}

inline int
_mutex_owned(struct pthread *curthread, const pthread_mutex_t *mutex)
{
	struct pthread_mutex *m;

	m = *mutex;
	if (__predict_false(m <= THR_MUTEX_DESTROYED)) {
		if (m == THR_MUTEX_DESTROYED)
			return (EINVAL);
		return (EPERM);
	}
	/*
	 * Check if the running thread is not the owner of the mutex.
	 */
	if (__predict_false(m->m_owner != curthread))
		return (EPERM);
	return (0);
}

static inline int
_mutex_unlock_common(struct pthread *curthread, struct pthread_mutex *m)
{
	uint32_t tid;

	tid = TID(curthread);
	if (__predict_false(
		m->m_type == PTHREAD_MUTEX_RECURSIVE &&
		m->m_recurse > 0)) {
		m->m_recurse--;
		return (0);
	}
	if (__predict_true(IS_SIMPLE_MUTEX(m)))
		mutex_unlock_queued(curthread, m);
	else {
		DEQUEUE_MUTEX(curthread, m);
		_thr_umutex_unlock(&m->m_lock, tid);
	}
	return (0);
}

static int
mutex_unlock_common(pthread_mutex_t *mutex)
{
	struct pthread *curthread = _get_curthread();
	struct pthread_mutex *m;
	int	error;

	if ((error = _mutex_owned(curthread, mutex)) != 0)
		return (error);
	m = *mutex;
	_mutex_unlock_common(curthread, m);
	if (m->m_private)
		THR_CRITICAL_LEAVE(curthread);
	return (0);
}

int
_mutex_cv_lock(pthread_mutex_t *mutex, int recurse, int detach)
{
	struct pthread *curthread = _get_curthread();
	struct pthread_mutex	*m;
	int	error;

	m = *mutex;
	if (detach == 0 && m->m_private)
		THR_CRITICAL_ENTER(curthread);
	error = _mutex_lock_common(m, NULL);
	if (error == 0) {
		m->m_refcount--;
		m->m_recurse += recurse;
	} else {
		if (detach == 0 && m->m_private)
			THR_CRITICAL_LEAVE(curthread);
	}
	return (error);
}

int
_mutex_cv_unlock(pthread_mutex_t *mutex, int *recurse)
{
	struct pthread *curthread = _get_curthread();
	struct pthread_mutex	*m;
#if 0
	/* already checked in pthread_cond_wait */
	int	error;

	if ((error = _mutex_owned(curthread, mutex)) != 0)
		return (error);
#endif
	m = *mutex;
	*recurse = m->m_recurse;
	m->m_recurse = 0;
	m->m_refcount++;
	_mutex_unlock_common(curthread, m);
	if (m->m_private)
		THR_CRITICAL_LEAVE(curthread);
	return (0);
}

int
_mutex_cv_detach(pthread_mutex_t *mutex, int *recurse)
{
	struct pthread *curthread = _get_curthread();
	struct pthread_mutex *m;
	int    error;

	if ((error = _mutex_owned(curthread, mutex)) != 0)
		return (error);

	m = *mutex;
	/*
	 * Clear the count in case this is a recursive mutex.
	 */
	*recurse = m->m_recurse;
	m->m_recurse = 0;
	m->m_refcount++;
	DEQUEUE_MUTEX(curthread, m);
	return (0);
}

int
_pthread_mutex_getprioceiling(pthread_mutex_t *mutex,
			      int *prioceiling)
{
	struct pthread_mutex *m;
	int ret;

	m = *mutex;
	if ((m <= THR_MUTEX_DESTROYED) ||
	    (m->m_lock.m_flags & UMUTEX_PRIO_PROTECT) == 0)
		ret = EINVAL;
	else {
		*prioceiling = m->m_lock.m_ceilings[0];
		ret = 0;
	}

	return (ret);
}

int
_pthread_mutex_setprioceiling(pthread_mutex_t *mutex,
			      int ceiling, int *old_ceiling)
{
	struct pthread *curthread = _get_curthread();
	struct pthread_mutex *m, *m1, *m2;
	int ret;

	m = *mutex;
	if ((m <= THR_MUTEX_DESTROYED) ||
	    (m->m_lock.m_flags & UMUTEX_PRIO_PROTECT) == 0)
		return (EINVAL);

	ret = __thr_umutex_set_ceiling(&m->m_lock, ceiling, old_ceiling);
	if (ret != 0)
		return (ret);

	if (m->m_owner == curthread) {
		MUTEX_ASSERT_IS_OWNED(m);
		m1 = TAILQ_PREV(m, mutex_queue, m_qe);
		m2 = TAILQ_NEXT(m, m_qe);
		if ((m1 != NULL && m1->m_lock.m_ceilings[0] > (u_int)ceiling) ||
		    (m2 != NULL && m2->m_lock.m_ceilings[0] < (u_int)ceiling)) {
			TAILQ_REMOVE(&curthread->pp_mutexq, m, m_qe);
			TAILQ_FOREACH(m2, &curthread->pp_mutexq, m_qe) {
				if (m2->m_lock.m_ceilings[0] > (u_int)ceiling) {
					TAILQ_INSERT_BEFORE(m2, m, m_qe);
					return (0);
				}
			}
			TAILQ_INSERT_TAIL(&curthread->pp_mutexq, m, m_qe);
		}
	}
	return (0);
}

int
_pthread_mutex_getspinloops_np(pthread_mutex_t *mutex, int *count)
{
	struct pthread_mutex	*m;

	CHECK_AND_INIT_MUTEX

	*count = m->m_spinloops;
	return (0);
}

int
__pthread_mutex_setspinloops_np(pthread_mutex_t *mutex, int count)
{
	struct pthread_mutex	*m;

	CHECK_AND_INIT_MUTEX

	m->m_spinloops = count;
	return (0);
}

int
_pthread_mutex_getyieldloops_np(pthread_mutex_t *mutex, int *count)
{
	struct pthread_mutex	*m;

	CHECK_AND_INIT_MUTEX

	*count = m->m_yieldloops;
	return (0);
}

int
__pthread_mutex_setyieldloops_np(pthread_mutex_t *mutex, int count)
{
	struct pthread_mutex	*m;

	CHECK_AND_INIT_MUTEX

	m->m_yieldloops = count;
	return (0);
}

int
_pthread_mutex_isowned_np(pthread_mutex_t *mutex)
{
	struct pthread_mutex	*m;

	m = *mutex;
	if (m <= THR_MUTEX_DESTROYED)
		return (0);
	return (m->m_owner == _get_curthread());
}
