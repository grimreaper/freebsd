/*
 * Copyright (c) 2005-2006 Sandvine Incorporated. All rights reserved.
 *   - Modified by Adrian Dewhurst to work with FreeBSD 5.2, send a dump header,
 *     and improve performance.
 *
 * Copyright (c) 2000 Darrell Anderson <anderson@cs.duke.edu>
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

#ifndef __NETDUMP_H
#define __NETDUMP_H

#ifdef _KERNEL

struct mtx;

struct netdump_methods {
	void (*test_get_lock)(struct ifnet *);
	int (*break_lock)(struct ifnet *, int *, uint8_t *, u_int);
	void (*release_lock)(struct ifnet *);
	int (*poll_locked)(struct ifnet *, enum poll_cmd, int);
};

int	 netdump_break_lock(struct mtx *lock, const char *name,
	    int *broke_lock, uint8_t *broken_state, u_int index,
	    u_int bstatesz);

#endif

#endif /* __NETDUMP_H */
