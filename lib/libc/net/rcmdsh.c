/*	$OpenBSD: rcmdsh.c,v 1.5 1998/04/25 16:23:58 millert Exp $	*/

/*
 * Copyright (c) 2001, MagniComp
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in 
 *    the documentation and/or other materials provided with the distribution. 
 * 3. Neither the name of the MagniComp nor the names of its contributors may
 *    be used to endorse or promote products derived from this software
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This is an rcmd() replacement originally by
 * Chris Siebenmann <cks@utcc.utoronto.ca>.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <errno.h>
#include <netdb.h>
#include <paths.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef _PATH_RSH
#define	_PATH_RSH	"/usr/bin/rsh"
#endif

/*
 * This is a replacement rcmd() function that uses the rsh(1)
 * program in place of a direct rcmd(3) function call so as to
 * avoid having to be root.  Note that rport is ignored.
 */
/* ARGSUSED */
int
rcmdsh(ahost, rport, locuser, remuser, cmd, rshprog)
	char **ahost;
	int rport __unused;
	const char *locuser, *remuser, *cmd, *rshprog;
{
	struct hostent *hp;
	int cpid, sp[2];
	char *p;
	struct passwd *pw;

	/* What rsh/shell to use. */
	if (rshprog == NULL)
		rshprog = _PATH_RSH;

	/* locuser must exist on this host. */
	if ((pw = getpwnam(locuser)) == NULL) {
		(void)fprintf(stderr, "rcmdsh: unknown user: %s\n", locuser);
		return (-1);
	}

	/* Validate remote hostname. */
	if (strcmp(*ahost, "localhost") != 0) {
		if ((hp = gethostbyname(*ahost)) == NULL) {
			herror(*ahost);
			return (-1);
		}
		*ahost = hp->h_name;
	}

	/* Get a socketpair we'll use for stdin and stdout. */
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, sp) == -1) {
		perror("rcmdsh: socketpair");
		return (-1);
	}

	cpid = fork();
	if (cpid == -1) {
		perror("rcmdsh: fork failed");
		return (-1);
	} else if (cpid == 0) {
		/*
		 * Child.  We use sp[1] to be stdin/stdout, and close sp[0].
		 */
		(void)close(sp[0]);
		if (dup2(sp[1], 0) == -1 || dup2(0, 1) == -1) {
			perror("rcmdsh: dup2 failed");
			_exit(255);
		}
		/* Fork again to lose parent. */
		cpid = fork();
		if (cpid == -1) {
			perror("rcmdsh: fork to lose parent failed");
			_exit(255);
		}
		if (cpid > 0)
			_exit(0);

		/* In grandchild here.  Become local user for rshprog. */
		if (setuid(pw->pw_uid) == -1) {
			(void)fprintf(stderr, "rcmdsh: setuid(%u): %s\n",
			    pw->pw_uid, strerror(errno));
			_exit(255);
		}

		/*
		 * If remote host is "localhost" and local and remote users
		 * are the same, avoid running remote shell for efficiency.
		 */
		if (strcmp(*ahost, "localhost") == 0 &&
		    strcmp(locuser, remuser) == 0) {
			if (pw->pw_shell[0] == '\0')
				rshprog = _PATH_BSHELL;
			else
				rshprog = pw->pw_shell;
			p = strrchr(rshprog, '/');
			execlp(rshprog, p ? p + 1 : rshprog, "-c", cmd,
			    (char *)NULL);
		} else {
			p = strrchr(rshprog, '/');
			execlp(rshprog, p ? p + 1 : rshprog, *ahost, "-l",
			    remuser, cmd, (char *)NULL);
		}
		(void)fprintf(stderr, "rcmdsh: execlp %s failed: %s\n",
		    rshprog, strerror(errno));
		_exit(255);
	} else {
		/* Parent. close sp[1], return sp[0]. */
		(void)close(sp[1]);
		/* Reap child. */
		(void)wait(NULL);
		return (sp[0]);
	}
	/* NOTREACHED */
}
