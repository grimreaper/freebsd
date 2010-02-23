/*-
 * Copyright (c) 2003-2010 Tim Kientzle
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "bsdtar_platform.h"
__FBSDID("$FreeBSD: user/eri/pf45/head/usr.bin/tar/err.c 203557 2010-02-06 19:44:37Z kientzle $");

#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "err.h"

const char *bsdtar_progname;

static void
bsdtar_vwarnc(int code, const char *fmt, va_list ap)
{
	fprintf(stderr, "%s: ", bsdtar_progname);
	vfprintf(stderr, fmt, ap);
	if (code != 0)
		fprintf(stderr, ": %s", strerror(code));
	fprintf(stderr, "\n");
}

void
bsdtar_warnc(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	bsdtar_vwarnc(code, fmt, ap);
	va_end(ap);
}

void
bsdtar_errc(int eval, int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	bsdtar_vwarnc(code, fmt, ap);
	va_end(ap);
	exit(eval);
}
