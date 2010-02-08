/*-
 * Copyright (c) 2009-2010
 * 	Swinburne University of Technology, Melbourne, Australia
 * Copyright (c) 2010 Lawrence Stewart <lstewart@freebsd.org>
 * All rights reserved.
 *
 * This software was developed at the Centre for Advanced Internet
 * Architectures, Swinburne University, by David Hayes and Lawrence Stewart,
 * made possible in part by a grant from the FreeBSD Foundation and
 * Cisco University Research Program Fund at Community Foundation
 * Silicon Valley.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
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

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/pfil.h>

#include <netinet/helper.h>
#include <netinet/helper_module.h>
#include <netinet/hhooks.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/tcp_var.h>

void ertt_tcpest_hook(void *udata, void *ctx_data, void *dblock);
int ertt_mod_init(void);
int ertt_mod_destroy(void);
int ertt_uma_ctor(void *mem, int size, void *arg, int flags);
void ertt_uma_dtor(void *mem, int size, void *arg);

struct ertt {
	int test;
};

struct helper ertt_helper = {
	.mod_init = ertt_mod_init,
	.mod_destroy = ertt_mod_destroy,
	.flags = HELPER_NEEDS_DBLOCK,
	.class = HELPER_CLASS_TCP
};


void
ertt_tcpest_hook(void *udata, void *ctx_data, void *dblock)
{
	//struct ertt *e = (struct ertt *)(((struct tcpcb *)inp->inp_ppcb)->helper_data[0]);
	struct ertt *e = (struct ertt *)dblock;
	printf("In the hook with errt->test: %d, ctx_data: %p, curack = %u\n",
	e->test, ctx_data, ((struct tcp_hhook_data *)ctx_data)->curack);
}


int
ertt_mod_init(void)
{
	return register_hhook(HHOOK_TYPE_TCP, HHOOK_TCP_ESTABLISHED,
	    &ertt_helper, &ertt_tcpest_hook, NULL, HHOOK_WAITOK);
}

int
ertt_mod_destroy(void)
{
	return deregister_hhook(HHOOK_TYPE_TCP, HHOOK_TCP_ESTABLISHED,
	    &ertt_tcpest_hook, NULL, 0);
}

int
ertt_uma_ctor(void *mem, int size, void *arg, int flags)
{
	printf("Creating ertt block %p\n", mem);

	((struct ertt *)mem)->test = 5;

	return (0);
}

void
ertt_uma_dtor(void *mem, int size, void *arg)
{
	printf("Destroying ertt block %p\n", mem);
}

DECLARE_HELPER_UMA(ertt, &ertt_helper, sizeof(struct ertt), ertt_uma_ctor, ertt_uma_dtor);
