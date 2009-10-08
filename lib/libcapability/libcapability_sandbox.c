/*-
 * Copyright (c) 2009 Robert N. M. Watson
 * All rights reserved.
 *
 * WARNING: THIS IS EXPERIMENTAL SECURITY SOFTWARE THAT MUST NOT BE RELIED
 * ON IN PRODUCTION SYSTEMS.  IT WILL BREAK YOUR SOFTWARE IN NEW AND
 * UNEXPECTED WAYS.
 * 
 * This software was developed at the University of Cambridge Computer
 * Laboratory with support from a grant from Google, Inc. 
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

#include <sys/types.h>
#include <sys/capability.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "libcapability.h"
#include "libcapability_internal.h"
#include "libcapability_sandbox_api.h"

static int		lch_initialized;
static struct lc_host	lch_global;

int
lcs_get(struct lc_host **lchpp)
{
	char *endp, *env, *env_dup, *env_dup_free, *name, *token, *value;
	int error, fd_sock;
	long long ll;

	if (lch_initialized) {
		*lchpp = &lch_global;
		return (0);
	}

	if (!ld_insandbox()) {
		errno = EINVAL;
		return (-1);
	}

	env = getenv(LIBCAPABILITY_SANDBOX_API_ENV);
	if (env == NULL) {
		errno = EINVAL;		/* XXXRW: Better errno? */
		return (-1);
	}

	env_dup = env_dup_free = strdup(env);
	if (env_dup == NULL)
		return (-1);

	fd_sock = -1;
	while ((token = strsep(&env_dup, ",")) != NULL) {
		name = strsep(&token, ":");
		if (name == NULL)
			continue;
		value = token;
		if (strcmp(name, LIBCAPABILITY_SANDBOX_API_SOCK) == 0) {
			ll = strtoll(value, &endp, 10);
			if (*endp != '\0' || ll < 0 || ll > INT_MAX)
				continue;
			fd_sock = ll;
		}
	}
	if (fd_sock == -1) {
		error = errno;
		free(env_dup_free);
		errno = error;
		return (-1);
	}
	lch_global.lch_fd_sock = fd_sock;
	lch_initialized = 1;
	*lchpp = &lch_global;
	free(env_dup_free);
	return (0);
}

int
lcs_getsock(struct lc_host *lchp, int *fdp)
{

	*fdp = lchp->lch_fd_sock;
	return (0);
}
