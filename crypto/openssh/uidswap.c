/* $OpenBSD: uidswap.c,v 1.35 2006/08/03 03:34:42 deraadt Exp $ */
/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * Code for uid-swapping.
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */

#include "includes.h"

#include <sys/param.h>
#include <errno.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include <grp.h>

#include "log.h"
#include "uidswap.h"
#include "xmalloc.h"

/*
 * Note: all these functions must work in all of the following cases:
 *    1. euid=0, ruid=0
 *    2. euid=0, ruid!=0
 *    3. euid!=0, ruid!=0
 * Additionally, they must work regardless of whether the system has
 * POSIX saved uids or not.
 */

#if defined(_POSIX_SAVED_IDS) && !defined(BROKEN_SAVED_UIDS)
/* Lets assume that posix saved ids also work with seteuid, even though that
   is not part of the posix specification. */
#define SAVED_IDS_WORK_WITH_SETEUID
/* Saved effective uid. */
static uid_t 	saved_euid = 0;
static gid_t	saved_egid = 0;
#endif

/* Saved effective uid. */
static int	privileged = 0;
static int	temporarily_use_uid_effective = 0;
static gid_t	*saved_egroups = NULL, *user_groups = NULL;
static int	saved_egroupslen = -1, user_groupslen = -1;

/*
 * Temporarily changes to the given uid.  If the effective user
 * id is not root, this does nothing.  This call cannot be nested.
 */
void
temporarily_use_uid(struct passwd *pw)
{
	/* Save the current euid, and egroups. */
#ifdef SAVED_IDS_WORK_WITH_SETEUID
	saved_euid = geteuid();
	saved_egid = getegid();
	debug("temporarily_use_uid: %u/%u (e=%u/%u)",
	    (u_int)pw->pw_uid, (u_int)pw->pw_gid,
	    (u_int)saved_euid, (u_int)saved_egid);
#ifndef HAVE_CYGWIN
	if (saved_euid != 0) {
		privileged = 0;
		return;
	}
#endif
#else
	if (geteuid() != 0) {
		privileged = 0;
		return;
	}
#endif /* SAVED_IDS_WORK_WITH_SETEUID */

	privileged = 1;
	temporarily_use_uid_effective = 1;

	saved_egroupslen = getgroups(0, NULL);
	if (saved_egroupslen < 0)
		fatal("getgroups: %.100s", strerror(errno));
	if (saved_egroupslen > 0) {
		saved_egroups = xrealloc(saved_egroups,
		    saved_egroupslen, sizeof(gid_t));
		if (getgroups(saved_egroupslen, saved_egroups) < 0)
			fatal("getgroups: %.100s", strerror(errno));
	} else { /* saved_egroupslen == 0 */
		if (saved_egroups != NULL)
			xfree(saved_egroups);
	}

	/* set and save the user's groups */
	if (user_groupslen == -1) {
		if (initgroups(pw->pw_name, pw->pw_gid) < 0)
			fatal("initgroups: %s: %.100s", pw->pw_name,
			    strerror(errno));

		user_groupslen = getgroups(0, NULL);
		if (user_groupslen < 0)
			fatal("getgroups: %.100s", strerror(errno));
		if (user_groupslen > 0) {
			user_groups = xrealloc(user_groups,
			    user_groupslen, sizeof(gid_t));
			if (getgroups(user_groupslen, user_groups) < 0)
				fatal("getgroups: %.100s", strerror(errno));
		} else { /* user_groupslen == 0 */
			if (user_groups)
				xfree(user_groups);
		}
	}
	/* Set the effective uid to the given (unprivileged) uid. */
	if (setgroups(user_groupslen, user_groups) < 0)
		fatal("setgroups: %.100s", strerror(errno));
#ifndef SAVED_IDS_WORK_WITH_SETEUID
	/* Propagate the privileged gid to all of our gids. */
	if (setgid(getegid()) < 0)
		debug("setgid %u: %.100s", (u_int) getegid(), strerror(errno));
	/* Propagate the privileged uid to all of our uids. */
	if (setuid(geteuid()) < 0)
		debug("setuid %u: %.100s", (u_int) geteuid(), strerror(errno));
#endif /* SAVED_IDS_WORK_WITH_SETEUID */
	if (setegid(pw->pw_gid) < 0)
		fatal("setegid %u: %.100s", (u_int)pw->pw_gid,
		    strerror(errno));
	if (seteuid(pw->pw_uid) == -1)
		fatal("seteuid %u: %.100s", (u_int)pw->pw_uid,
		    strerror(errno));
}

void
permanently_drop_suid(uid_t uid)
{
	uid_t old_uid = getuid();

	debug("permanently_drop_suid: %u", (u_int)uid);
#if defined(HAVE_SETRESUID) && !defined(BROKEN_SETRESUID)
	if (setresuid(uid, uid, uid) < 0)
		fatal("setresuid %u: %.100s", (u_int)uid, strerror(errno));
#elif defined(HAVE_SETREUID) && !defined(BROKEN_SETREUID)
	if (setreuid(uid, uid) < 0)
		fatal("setreuid %u: %.100s", (u_int)uid, strerror(errno));
#else
# ifndef SETEUID_BREAKS_SETUID
	if (seteuid(uid) < 0)
		fatal("seteuid %u: %.100s", (u_int)uid, strerror(errno));
# endif
	if (setuid(uid) < 0)
		fatal("setuid %u: %.100s", (u_int)uid, strerror(errno));
#endif

#ifndef HAVE_CYGWIN
	/* Try restoration of UID if changed (test clearing of saved uid) */
	if (old_uid != uid &&
	    (setuid(old_uid) != -1 || seteuid(old_uid) != -1))
		fatal("%s: was able to restore old [e]uid", __func__);
#endif

	/* Verify UID drop was successful */
	if (getuid() != uid || geteuid() != uid) {
		fatal("%s: euid incorrect uid:%u euid:%u (should be %u)",
		    __func__, (u_int)getuid(), (u_int)geteuid(), (u_int)uid);
	}
}

/*
 * Restores to the original (privileged) uid.
 */
void
restore_uid(void)
{
	/* it's a no-op unless privileged */
	if (!privileged) {
		debug("restore_uid: (unprivileged)");
		return;
	}
	if (!temporarily_use_uid_effective)
		fatal("restore_uid: temporarily_use_uid not effective");

#ifdef SAVED_IDS_WORK_WITH_SETEUID
	debug("restore_uid: %u/%u", (u_int)saved_euid, (u_int)saved_egid);
	/* Set the effective uid back to the saved privileged uid. */
	if (seteuid(saved_euid) < 0)
		fatal("seteuid %u: %.100s", (u_int)saved_euid, strerror(errno));
	if (setegid(saved_egid) < 0)
		fatal("setegid %u: %.100s", (u_int)saved_egid, strerror(errno));
#else /* SAVED_IDS_WORK_WITH_SETEUID */
	/*
	 * We are unable to restore the real uid to its unprivileged value.
	 * Propagate the real uid (usually more privileged) to effective uid
	 * as well.
	 */
	setuid(getuid());
	setgid(getgid());
#endif /* SAVED_IDS_WORK_WITH_SETEUID */

	if (setgroups(saved_egroupslen, saved_egroups) < 0)
		fatal("setgroups: %.100s", strerror(errno));
	temporarily_use_uid_effective = 0;
}

/*
 * Permanently sets all uids to the given uid.  This cannot be
 * called while temporarily_use_uid is effective.
 */
void
permanently_set_uid(struct passwd *pw)
{
	uid_t old_uid = getuid();
	gid_t old_gid = getgid();

	if (pw == NULL)
		fatal("permanently_set_uid: no user given");
	if (temporarily_use_uid_effective)
		fatal("permanently_set_uid: temporarily_use_uid effective");
	debug("permanently_set_uid: %u/%u", (u_int)pw->pw_uid,
	    (u_int)pw->pw_gid);

#if defined(HAVE_SETRESGID) && !defined(BROKEN_SETRESGID)
	if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) < 0)
		fatal("setresgid %u: %.100s", (u_int)pw->pw_gid, strerror(errno));
#elif defined(HAVE_SETREGID) && !defined(BROKEN_SETREGID)
	if (setregid(pw->pw_gid, pw->pw_gid) < 0)
		fatal("setregid %u: %.100s", (u_int)pw->pw_gid, strerror(errno));
#else
	if (setegid(pw->pw_gid) < 0)
		fatal("setegid %u: %.100s", (u_int)pw->pw_gid, strerror(errno));
	if (setgid(pw->pw_gid) < 0)
		fatal("setgid %u: %.100s", (u_int)pw->pw_gid, strerror(errno));
#endif

#if defined(HAVE_SETRESUID) && !defined(BROKEN_SETRESUID)
	if (setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) < 0)
		fatal("setresuid %u: %.100s", (u_int)pw->pw_uid, strerror(errno));
#elif defined(HAVE_SETREUID) && !defined(BROKEN_SETREUID)
	if (setreuid(pw->pw_uid, pw->pw_uid) < 0)
		fatal("setreuid %u: %.100s", (u_int)pw->pw_uid, strerror(errno));
#else
# ifndef SETEUID_BREAKS_SETUID
	if (seteuid(pw->pw_uid) < 0)
		fatal("seteuid %u: %.100s", (u_int)pw->pw_uid, strerror(errno));
# endif
	if (setuid(pw->pw_uid) < 0)
		fatal("setuid %u: %.100s", (u_int)pw->pw_uid, strerror(errno));
#endif

#ifndef HAVE_CYGWIN
	/* Try restoration of GID if changed (test clearing of saved gid) */
	if (old_gid != pw->pw_gid && pw->pw_uid != 0 &&
	    (setgid(old_gid) != -1 || setegid(old_gid) != -1))
		fatal("%s: was able to restore old [e]gid", __func__);
#endif

	/* Verify GID drop was successful */
	if (getgid() != pw->pw_gid || getegid() != pw->pw_gid) {
		fatal("%s: egid incorrect gid:%u egid:%u (should be %u)",
		    __func__, (u_int)getgid(), (u_int)getegid(),
		    (u_int)pw->pw_gid);
	}

#ifndef HAVE_CYGWIN
	/* Try restoration of UID if changed (test clearing of saved uid) */
	if (old_uid != pw->pw_uid &&
	    (setuid(old_uid) != -1 || seteuid(old_uid) != -1))
		fatal("%s: was able to restore old [e]uid", __func__);
#endif

	/* Verify UID drop was successful */
	if (getuid() != pw->pw_uid || geteuid() != pw->pw_uid) {
		fatal("%s: euid incorrect uid:%u euid:%u (should be %u)",
		    __func__, (u_int)getuid(), (u_int)geteuid(),
		    (u_int)pw->pw_uid);
	}
}
