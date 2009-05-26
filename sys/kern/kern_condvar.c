/*-
 * Copyright (c) 2000 Jake Burkholder <jake@freebsd.org>.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_ktrace.h"
#include "opt_witness.h"	

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/condvar.h>
#include <sys/sched.h>
#include <sys/signalvar.h>
#include <sys/sleepqueue.h>
#include <sys/resourcevar.h>
#ifdef KTRACE
#include <sys/uio.h>
#include <sys/ktrace.h>
#endif

/*
 * Common sanity checks for cv_wait* functions.
 */
#define	CV_ASSERT(cvp, lock, td) do {					\
	KASSERT((td) != NULL, ("%s: curthread NULL", __func__));	\
	KASSERT(TD_IS_RUNNING(td), ("%s: not TDS_RUNNING", __func__));	\
	KASSERT((cvp) != NULL, ("%s: cvp NULL", __func__));		\
	KASSERT((lock) != NULL, ("%s: lock NULL", __func__));		\
} while (0)

/*
 * Initialize a condition variable.  Must be called before use.
 */
void
cv_init(struct cv *cvp, const char *desc)
{

	cvp->cv_description = desc;
	cvp->cv_waiters = 0;
}

/*
 * Destroy a condition variable.  The condition variable must be re-initialized
 * in order to be re-used.
 */
void
cv_destroy(struct cv *cvp)
{
#ifdef INVARIANTS
	struct sleepqueue *sq;

	sleepq_lock(cvp);
	sq = sleepq_lookup(cvp);
	sleepq_release(cvp);
	KASSERT(sq == NULL, ("%s: associated sleep queue non-empty", __func__));
#endif
}

/*
 * Wait on a condition variable.  The current thread is placed on the condition
 * variable's wait queue and suspended.  A cv_signal or cv_broadcast on the same
 * condition variable will resume the thread.  The mutex is released before
 * sleeping and will be held on return.  It is recommended that the mutex be
 * held when cv_signal or cv_broadcast are called.
 */
void
_cv_wait(struct cv *cvp, struct lock_object *lock)
{
	WITNESS_SAVE_DECL(lock_witness);
	struct lock_class *class;
	struct thread *td;
	int lock_state;

	td = curthread;
	lock_state = 0;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(1, 0);
#endif
	CV_ASSERT(cvp, lock, td);
#ifndef WITNESS_SKIPCV
	WITNESS_WARN(WARN_GIANTOK, lock,
	    "Waiting on \"%s\"", cvp->cv_description);
#endif
	WITNESS_SAVE(lock, lock_witness);
	class = LOCK_CLASS(lock);

	if (cold || panicstr) {
		/*
		 * During autoconfiguration, just give interrupts
		 * a chance, then just return.  Don't run any other
		 * thread or panic below, in case this is the idle
		 * process and already asleep.
		 */
		return;
	}

	sleepq_lock(cvp);

	cvp->cv_waiters++;
	DROP_GIANT();

	sleepq_add(cvp, lock, cvp->cv_description, SLEEPQ_CONDVAR, 0);
	if (lock != &Giant.lock_object) {
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_release(cvp);
		lock_state = class->lc_unlock(lock);
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_lock(cvp);
	}
	sleepq_wait(cvp, 0);

#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(0, 0);
#endif
	PICKUP_GIANT();
	if (lock != &Giant.lock_object) {
		class->lc_lock(lock, lock_state);
		WITNESS_RESTORE(lock, lock_witness);
	}
}

/*
 * Wait on a condition variable.  This function differs from cv_wait by
 * not aquiring the mutex after condition variable was signaled.
 */
void
_cv_wait_unlock(struct cv *cvp, struct lock_object *lock)
{
	struct lock_class *class;
	struct thread *td;

	td = curthread;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(1, 0);
#endif
	CV_ASSERT(cvp, lock, td);
	WITNESS_WARN(WARN_GIANTOK | WARN_SLEEPOK, lock,
	    "Waiting on \"%s\"", cvp->cv_description);
	KASSERT(lock != &Giant.lock_object,
	    ("cv_wait_unlock cannot be used with Giant"));
	class = LOCK_CLASS(lock);

	if (cold || panicstr) {
		/*
		 * During autoconfiguration, just give interrupts
		 * a chance, then just return.  Don't run any other
		 * thread or panic below, in case this is the idle
		 * process and already asleep.
		 */
		class->lc_unlock(lock);
		return;
	}

	sleepq_lock(cvp);

	cvp->cv_waiters++;
	DROP_GIANT();

	sleepq_add(cvp, lock, cvp->cv_description, SLEEPQ_CONDVAR, 0);
	if (class->lc_flags & LC_SLEEPABLE)
		sleepq_release(cvp);
	class->lc_unlock(lock);
	if (class->lc_flags & LC_SLEEPABLE)
		sleepq_lock(cvp);
	sleepq_wait(cvp, 0);

#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(0, 0);
#endif
	PICKUP_GIANT();
}

/*
 * Wait on a condition variable, allowing interruption by signals.  Return 0 if
 * the thread was resumed with cv_signal or cv_broadcast, EINTR or ERESTART if
 * a signal was caught.  If ERESTART is returned the system call should be
 * restarted if possible.
 */
int
_cv_wait_sig(struct cv *cvp, struct lock_object *lock)
{
	WITNESS_SAVE_DECL(lock_witness);
	struct lock_class *class;
	struct thread *td;
	struct proc *p;
	int lock_state, rval;

	td = curthread;
	p = td->td_proc;
	lock_state = 0;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(1, 0);
#endif
	CV_ASSERT(cvp, lock, td);
	WITNESS_WARN(WARN_GIANTOK | WARN_SLEEPOK, lock,
	    "Waiting on \"%s\"", cvp->cv_description);
	WITNESS_SAVE(lock, lock_witness);
	class = LOCK_CLASS(lock);

	if (cold || panicstr) {
		/*
		 * After a panic, or during autoconfiguration, just give
		 * interrupts a chance, then just return; don't run any other
		 * procs or panic below, in case this is the idle process and
		 * already asleep.
		 */
		return (0);
	}

	sleepq_lock(cvp);

	cvp->cv_waiters++;
	DROP_GIANT();

	sleepq_add(cvp, lock, cvp->cv_description, SLEEPQ_CONDVAR |
	    SLEEPQ_INTERRUPTIBLE, 0);
	if (lock != &Giant.lock_object) {
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_release(cvp);
		lock_state = class->lc_unlock(lock);
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_lock(cvp);
	}
	rval = sleepq_wait_sig(cvp, 0);

#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(0, 0);
#endif
	PICKUP_GIANT();
	if (lock != &Giant.lock_object) {
		class->lc_lock(lock, lock_state);
		WITNESS_RESTORE(lock, lock_witness);
	}

	return (rval);
}

/*
 * Wait on a condition variable for at most timo/hz seconds.  Returns 0 if the
 * process was resumed by cv_signal or cv_broadcast, EWOULDBLOCK if the timeout
 * expires.
 */
int
_cv_timedwait(struct cv *cvp, struct lock_object *lock, int timo)
{
	WITNESS_SAVE_DECL(lock_witness);
	struct lock_class *class;
	struct thread *td;
	int lock_state, rval;

	td = curthread;
	rval = 0;
	lock_state = 0;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(1, 0);
#endif
	CV_ASSERT(cvp, lock, td);
	WITNESS_WARN(WARN_GIANTOK | WARN_SLEEPOK, lock,
	    "Waiting on \"%s\"", cvp->cv_description);
	WITNESS_SAVE(lock, lock_witness);
	class = LOCK_CLASS(lock);

	if (cold || panicstr) {
		/*
		 * After a panic, or during autoconfiguration, just give
		 * interrupts a chance, then just return; don't run any other
		 * thread or panic below, in case this is the idle process and
		 * already asleep.
		 */
		return 0;
	}

	sleepq_lock(cvp);

	cvp->cv_waiters++;
	DROP_GIANT();

	sleepq_add(cvp, lock, cvp->cv_description, SLEEPQ_CONDVAR, 0);
	sleepq_set_timeout(cvp, timo);
	if (lock != &Giant.lock_object) {
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_release(cvp);
		lock_state = class->lc_unlock(lock);
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_lock(cvp);
	}
	rval = sleepq_timedwait(cvp, 0);

#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(0, 0);
#endif
	PICKUP_GIANT();
	if (lock != &Giant.lock_object) {
		class->lc_lock(lock, lock_state);
		WITNESS_RESTORE(lock, lock_witness);
	}

	return (rval);
}

/*
 * Wait on a condition variable for at most timo/hz seconds, allowing
 * interruption by signals.  Returns 0 if the thread was resumed by cv_signal
 * or cv_broadcast, EWOULDBLOCK if the timeout expires, and EINTR or ERESTART if
 * a signal was caught.
 */
int
_cv_timedwait_sig(struct cv *cvp, struct lock_object *lock, int timo)
{
	WITNESS_SAVE_DECL(lock_witness);
	struct lock_class *class;
	struct thread *td;
	struct proc *p;
	int lock_state, rval;

	td = curthread;
	p = td->td_proc;
	rval = 0;
	lock_state = 0;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(1, 0);
#endif
	CV_ASSERT(cvp, lock, td);
	WITNESS_WARN(WARN_GIANTOK | WARN_SLEEPOK, lock,
	    "Waiting on \"%s\"", cvp->cv_description);
	WITNESS_SAVE(lock, lock_witness);
	class = LOCK_CLASS(lock);

	if (cold || panicstr) {
		/*
		 * After a panic, or during autoconfiguration, just give
		 * interrupts a chance, then just return; don't run any other
		 * thread or panic below, in case this is the idle process and
		 * already asleep.
		 */
		return 0;
	}

	sleepq_lock(cvp);

	cvp->cv_waiters++;
	DROP_GIANT();

	sleepq_add(cvp, lock, cvp->cv_description, SLEEPQ_CONDVAR |
	    SLEEPQ_INTERRUPTIBLE, 0);
	sleepq_set_timeout(cvp, timo);
	if (lock != &Giant.lock_object) {
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_release(cvp);
		lock_state = class->lc_unlock(lock);
		if (class->lc_flags & LC_SLEEPABLE)
			sleepq_lock(cvp);
	}
	rval = sleepq_timedwait_sig(cvp, 0);

#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(0, 0);
#endif
	PICKUP_GIANT();
	if (lock != &Giant.lock_object) {
		class->lc_lock(lock, lock_state);
		WITNESS_RESTORE(lock, lock_witness);
	}

	return (rval);
}

/*
 * Signal a condition variable, wakes up one waiting thread.  Will also wakeup
 * the swapper if the process is not in memory, so that it can bring the
 * sleeping process in.  Note that this may also result in additional threads
 * being made runnable.  Should be called with the same mutex as was passed to
 * cv_wait held.
 */
void
cv_signal(struct cv *cvp)
{
	int wakeup_swapper;

	wakeup_swapper = 0;
	sleepq_lock(cvp);
	if (cvp->cv_waiters > 0) {
		cvp->cv_waiters--;
		wakeup_swapper = sleepq_signal(cvp, SLEEPQ_CONDVAR, 0, 0);
	}
	sleepq_release(cvp);
	if (wakeup_swapper)
		kick_proc0();
}

/*
 * Broadcast a signal to a condition variable.  Wakes up all waiting threads.
 * Should be called with the same mutex as was passed to cv_wait held.
 */
void
cv_broadcastpri(struct cv *cvp, int pri)
{
	int wakeup_swapper;

	wakeup_swapper = 0;
	sleepq_lock(cvp);
	if (cvp->cv_waiters > 0) {
		cvp->cv_waiters = 0;
		wakeup_swapper = sleepq_broadcast(cvp, SLEEPQ_CONDVAR, pri, 0);
	}
	sleepq_release(cvp);
	if (wakeup_swapper)
		kick_proc0();
}
