/*-
 * Copyright (c) 2007-2009 Robert N. M. Watson
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
 *
 * $FreeBSD$
 */

#ifndef _NET_NETISR2_H_
#define	_NET_NETISR2_H_

#ifndef _KERNEL
#error "no user-serviceable parts inside"
#endif

/*-
 * Protocols express ordering constraints and affinity preferences by
 * implementing one or neither of nh_m2flow and nh_m2cpu, which are used by
 * netisr2 to determine which per-CPU workstream to assign mbufs to.
 *
 * The following policies may be used by protocols:
 *
 * NETISR_POLICY_SOURCE - netisr2 should maintain source ordering without
 *                        advice from the protocol.  netisr2 will ignore any
 *                        flow IDs present on the mbuf for the purposes of
 *                        work placement.
 *
 * NETISR_POLICY_FLOW - netisr2 should maintain flow ordering as defined by
 *                      the mbuf header flow ID field.  If the protocol
 *                      implements nh_m2flow, then netisr2 will query the
 *                      protocol in the event that the mbuf doesn't have a
 *                      flow ID, falling back on source ordering.
 *
 * NETISR_POLICY_CPU - netisr2 will delegate all work placement decisions to
 *                     the protocol, querying nh_m2cpu for each packet.
 *
 * Protocols might make decisions about work placement based on an existing
 * calculated flow ID on the mbuf, such as one provided in hardware, the
 * receive interface pointed to by the mbuf (if any), the optional source
 * identifier passed at some dispatch points, or even parse packet headers to
 * calculate a flow.  Both protocol handlers may return a new mbuf pointer
 * for the chain, or NULL if the packet proves invalid or m_pullup() fails.
 *
 * XXXRW: If we eventually support dynamic reconfiguration, there should be
 * protocol handlers to notify them of CPU configuration changes so that they
 * can rebalance work.
 */
typedef struct mbuf	*netisr_m2cpu_t(struct mbuf *m, uintptr_t source,
			 u_int *cpuid);
typedef	struct mbuf	*netisr_m2flow_t(struct mbuf *m, uintptr_t source);

#define	NETISR_POLICY_SOURCE	1	/* Maintain source ordering. */
#define	NETISR_POLICY_FLOW	2	/* Maintain flow ordering. */
#define	NETISR_POLICY_CPU	3	/* Protocol determines CPU placement. */

/*
 * Data structure describing a protocol handler.
 */
struct netisr_handler {
	const char	*nh_name;	/* Character string protocol name. */
	netisr_t	*nh_handler;	/* Protocol handler. */
	netisr_m2flow_t	*nh_m2flow;	/* Query flow for untagged packet. */
	netisr_m2cpu_t	*nh_m2cpu;	/* Query CPU to process packet on. */
	u_int		 nh_proto;	/* Integer protocol ID. */
	u_int		 nh_qlimit;	/* Maximum per-CPU queue depth. */
	u_int		 nh_policy;	/* Work placement policy. */
	u_int		 nh_ispare[5];	/* For future use. */
	void		*nh_pspare[4];	/* For future use. */
};

/*
 * Register, unregister, and other netisr2 handler management functions.
 */
void	netisr2_clearqdrops(const struct netisr_handler *nhp);
void	netisr2_getqdrops(const struct netisr_handler *nhp,
	    u_int64_t *qdropsp);
void	netisr2_getqlimit(const struct netisr_handler *nhp, u_int *qlimitp);
void	netisr2_register(const struct netisr_handler *nhp);
int	netisr2_setqlimit(const struct netisr_handler *nhp, u_int qlimit);
void	netisr2_unregister(const struct netisr_handler *nhp);

/*
 * Process a packet destined for a protocol, and attempt direct dispatch.
 * Supplemental source ordering information can be passed using the _src
 * variant.
 */
//int	netisr_dispatch(u_int proto, struct mbuf *m);
//int	netisr_queue(u_int proto, struct mbuf *m);
int	netisr2_dispatch(u_int proto, struct mbuf *m);
int	netisr2_dispatch_src(u_int proto, uintptr_t source, struct mbuf *m);
int	netisr2_queue(u_int proto, struct mbuf *m);
int	netisr2_queue_src(u_int proto, uintptr_t source, struct mbuf *m);

/*
 * Provide a default implementation of "map a ID to a cpu ID".
 */
u_int	netisr2_default_flow2cpu(u_int flowid);

/*
 * Utility routines to return the number of CPUs participting in netisr2, and
 * to return a mapping from a number to a CPU ID that can be used with the
 * scheduler.
 */
u_int	netisr2_get_cpucount(void);
u_int	netisr2_get_cpuid(u_int cpunumber);

#endif /* !_NET_NETISR2_H_ */
