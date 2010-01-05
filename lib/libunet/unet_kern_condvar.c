#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_ktrace.h"

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

#include <pthread.h>

/*
 * Initialize a condition variable.  Must be called before use.
 */
void
cv_init(struct cv *cvp, const char *desc)
{
	pthread_condattr_t ca;
    
	cvp->cv_description = desc;
	pthread_cond_init(&cvp->cv_cond, &ca);
}

/*
 * Destroy a condition variable.  The condition variable must be re-initialized
 * in order to be re-used.
 */
void
cv_destroy(struct cv *cvp)
{

	pthread_cond_destroy(&cvp->cv_cond);
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

	pthread_cond_wait(&cvp->cv_cond, &lock->lo_mutex);
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

	return (pthread_cond_wait(&cvp->cv_cond, &lock->lo_mutex));
}

/*
 * Wait on a condition variable for at most timo/hz seconds.  Returns 0 if the
 * process was resumed by cv_signal or cv_broadcast, EWOULDBLOCK if the timeout
 * expires.
 */
int
_cv_timedwait(struct cv *cvp, struct lock_object *lock, int timo)
{
	struct timespec abstime;
	int secs = timo/hz;
	int nsecs = (timo%hz)*((1000*1000*1000)/hz);

	abstime.tv_sec = secs;
	abstime.tv_nsec = nsecs;

	/* XXX
	 * how do we handle getting interrupted by a signal?
	 * set the sigmask?
	 */
	return (pthread_cond_timedwait(&cvp->cv_cond, &lock->lo_mutex,
		    &abstime));
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
	struct timespec abstime;
	int secs = timo/hz;
	int nsecs = (timo%hz)*((1000*1000*1000)/hz);

	abstime.tv_sec = secs;
	abstime.tv_nsec = nsecs;

	return (pthread_cond_timedwait(&cvp->cv_cond, &lock->lo_mutex,
		    &abstime));
}

/*
 * Broadcast a signal to a condition variable.  Wakes up all waiting threads.
 * Should be called with the same mutex as was passed to cv_wait held.
 */
void
cv_broadcastpri(struct cv *cvp, int pri)
{

	pthread_cond_broadcast(&cvp->cv_cond);
}
