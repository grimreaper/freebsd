/*-
 * Copyright (C) 2001 Benno Rice.
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
 * THIS SOFTWARE IS PROVIDED BY Benno Rice ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: head/sys/dev/mambo/mambo_console.c 193018 2009-05-29 06:41:23Z ed $");

#include "opt_comconsole.h"
#include "opt_ofw.h"

#include <sys/param.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/priv.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/cons.h>
#include <sys/consio.h>
#include <sys/tty.h>

#include <dev/ofw/openfirm.h>

#include <ddb/ddb.h>

#include "mambocall.h"

#ifndef	MAMBOCONS_POLL_HZ
#define	MAMBOCONS_POLL_HZ	4
#endif
#define MAMBOBURSTLEN	128	/* max number of bytes to write in one chunk */

#define MAMBO_CONSOLE_WRITE	0
#define MAMBO_CONSOLE_READ	0

static tsw_open_t mambotty_open;
static tsw_close_t mambotty_close;
static tsw_outwakeup_t mambotty_outwakeup;

static struct ttydevsw mambo_ttydevsw = {
	.tsw_flags	= TF_NOPREFIX,
	.tsw_open	= mambotty_open,
	.tsw_close	= mambotty_close,
	.tsw_outwakeup	= mambotty_outwakeup,
};

static int			polltime;
static struct callout_handle	mambo_timeouthandle
    = CALLOUT_HANDLE_INITIALIZER(&mambo_timeouthandle);

#if defined(KDB) && defined(ALT_BREAK_TO_DEBUGGER)
static int			alt_break_state;
#endif

static void	mambo_timeout(void *);

static cn_probe_t	mambo_cnprobe;
static cn_init_t	mambo_cninit;
static cn_term_t	mambo_cnterm;
static cn_getc_t	mambo_cngetc;
static cn_putc_t	mambo_cnputc;

CONSOLE_DRIVER(mambo);

static void
cn_drvinit(void *unused)
{
	char output[32];
	struct tty *tp;

	if (mambo_consdev.cn_pri != CN_DEAD &&
	    mambo_consdev.cn_name[0] != '\0') {
		if (OF_finddevice("/mambo") == -1)
			return;

		tp = tty_alloc(&mambo_ttydevsw, NULL);
		tty_makedev(tp, NULL, "%s", output);
		tty_makealias(tp, "mambocons");
	}
}

SYSINIT(cndev, SI_SUB_CONFIGURE, SI_ORDER_MIDDLE, cn_drvinit, NULL);

static int
mambotty_open(struct tty *tp)
{
	polltime = hz / MAMBOCONS_POLL_HZ;
	if (polltime < 1)
		polltime = 1;

	mambo_timeouthandle = timeout(mambo_timeout, tp, polltime);

	return (0);
}

static void
mambotty_close(struct tty *tp)
{

	/* XXX Should be replaced with callout_stop(9) */
	untimeout(mambo_timeout, tp, mambo_timeouthandle);
}

static void
mambotty_outwakeup(struct tty *tp)
{
	int len;
	u_char buf[MAMBOBURSTLEN];

	for (;;) {
		len = ttydisc_getc(tp, buf, sizeof buf);
		if (len == 0)
			break;
		mambocall(MAMBO_CONSOLE_WRITE, buf, (register_t)len, 1UL);
	}
}

static void
mambo_timeout(void *v)
{
	struct	tty *tp;
	int 	c;

	tp = (struct tty *)v;

	tty_lock(tp);
	while ((c = mambo_cngetc(NULL)) != -1)
		ttydisc_rint(tp, c, 0);
	ttydisc_rint_done(tp);
	tty_unlock(tp);

	mambo_timeouthandle = timeout(mambo_timeout, tp, polltime);
}

static void
mambo_cnprobe(struct consdev *cp)
{
	if (OF_finddevice("/mambo") == -1) {
		cp->cn_pri = CN_DEAD;
		return;
	}

	cp->cn_pri = CN_NORMAL;
}

static void
mambo_cninit(struct consdev *cp)
{

	/* XXX: This is the alias, but that should be good enough */
	strcpy(cp->cn_name, "mambocons");
}

static void
mambo_cnterm(struct consdev *cp)
{
}

static int
mambo_cngetc(struct consdev *cp)
{
	unsigned char ch;

	ch = mambocall(MAMBO_CONSOLE_READ);

	if (ch > 0) {
#if defined(KDB) && defined(ALT_BREAK_TO_DEBUGGER)
		int kdb_brk;

		if ((kdb_brk = kdb_alt_break(ch, &alt_break_state)) != 0) {
			switch (kdb_brk) {
			case KDB_REQ_DEBUGGER:
				kdb_enter(KDB_WHY_BREAK,
				    "Break sequence on console");
				break;
			case KDB_REQ_PANIC:
				kdb_panic("Panic sequence on console");
				break;
			case KDB_REQ_REBOOT:
				kdb_reboot();
				break;

			}
		}
#endif
		return (ch);
	}

	return (-1);
}

static void
mambo_cnputc(struct consdev *cp, int c)
{
	char cbuf;

	cbuf = c;
	mambocall(MAMBO_CONSOLE_WRITE, &cbuf, 1UL, 1UL);
}
