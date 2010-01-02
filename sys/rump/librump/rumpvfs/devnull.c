/*	$NetBSD: devnull.c,v 1.1 2009/12/17 00:29:46 pooka Exp $	*/

/*
 * Copyright (c) 2009 Antti Kantee.  All Rights Reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * /dev/null, the infamous bytesink.
 *
 * I can't imagine it being very different in the rump kernel than in
 * the host kernel.  But nonetheless it serves as a simple example.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: devnull.c,v 1.1 2009/12/17 00:29:46 pooka Exp $");

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/stat.h>

#include "rump_vfs_private.h"

static devmajor_t null_bmaj, null_cmaj;

static dev_type_open(rump_devnullopen);
static dev_type_read(rump_devnullrw);

static struct cdevsw null_cdevsw = {
	rump_devnullopen, nullclose, rump_devnullrw, rump_devnullrw, noioctl,
	nostop, notty, nopoll, nommap, nokqfilter, D_OTHER | D_MPSAFE,
};

int
rump_devnull_init()
{
	int error;

	null_bmaj = null_cmaj = NODEVMAJOR;
	error = devsw_attach("null", NULL, &null_bmaj, &null_cdevsw,&null_cmaj);
	if (error != 0)
		return error;

	return rump_vfs_makeonedevnode(S_IFCHR, "/dev/null", null_cmaj, 0);
}

static int
rump_devnullopen(dev_t dev, int flag, int mode, struct lwp *l)
{

	return 0;
}

static int
rump_devnullrw(dev_t dev, struct uio *uio, int flags)
{

	if (uio->uio_rw == UIO_WRITE)
		uio->uio_resid = 0;
	return 0;
}
