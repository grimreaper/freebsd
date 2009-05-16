/*-
 * Copyright (c) 1996 Charles D. Cranor and Washington University.
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
 *      This product includes software developed by Charles D. Cranor and
 *      Washington University.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
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
 *
 * $NetBSD: natm_proto.c,v 1.3 1996/09/18 00:56:41 chuck Exp $
 */

/*
 * protocol layer for access to native mode ATM
 */

#include "opt_netisr.h"

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/protosw.h>
#include <sys/domain.h>

#include <net/if.h>
#include <net/netisr.h>
#include <net/netisr2.h>

#include <netinet/in.h>

#include <netnatm/natm.h>

static	void natm_init(void);

static struct domain natmdomain;

static struct protosw natmsw[] = {
{
	.pr_type =		SOCK_STREAM,
	.pr_domain =		&natmdomain,
	.pr_protocol =		PROTO_NATMAAL5,
	.pr_flags =		PR_CONNREQUIRED,
	.pr_usrreqs =		&natm_usrreqs
},
{
	.pr_type =		SOCK_DGRAM,
	.pr_domain =		&natmdomain,
	.pr_protocol =		PROTO_NATMAAL5,
	.pr_flags =		PR_CONNREQUIRED|PR_ATOMIC,
	.pr_usrreqs =		&natm_usrreqs
},
{
	.pr_type =		SOCK_STREAM,
	.pr_domain =		&natmdomain,
	.pr_protocol =		PROTO_NATMAAL0,
	.pr_flags =		PR_CONNREQUIRED,
	.pr_usrreqs =		&natm_usrreqs
},
};

static struct domain natmdomain = {
	.dom_family =		AF_NATM,
	.dom_name =		"natm",
	.dom_init =		natm_init,
	.dom_protosw =		natmsw,
	.dom_protoswNPROTOSW =	&natmsw[sizeof(natmsw)/sizeof(natmsw[0])],
};

static int natmqmaxlen = 1000 /* IFQ_MAXLEN */;	/* max # of packets on queue */
static struct ifqueue natmintrq;
#ifdef NATM_STAT
u_int natm_sodropcnt;		/* # mbufs dropped due to full sb */
u_int natm_sodropbytes;		/* # of bytes dropped */
u_int natm_sookcnt;		/* # mbufs ok */
u_int natm_sookbytes;		/* # of bytes ok */
#endif

static void
natm_init(void)
{
	LIST_INIT(&natm_pcbs);
	bzero(&natmintrq, sizeof(natmintrq));
	natmintrq.ifq_maxlen = natmqmaxlen;
	NATM_LOCK_INIT();
	mtx_init(&natmintrq.ifq_mtx, "natm_inq", NULL, MTX_DEF);
#ifdef NETISR2
	netisr2_register(NETISR_NATM, "natm", natmintr, NULL, NULL,
	    natmqmaxlen);
#else
	netisr_register(NETISR_NATM, natmintr, &natmintrq, 0);
#endif
}

DOMAIN_SET(natm);
