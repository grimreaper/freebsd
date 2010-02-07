/**************************************************************************

Copyright (c) 2008-2009, BitGravity Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

 2. Neither the name of the BitGravity Corporation nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

***************************************************************************/

#include "opt_route.h"
#include "opt_mpath.h"
#include "opt_ddb.h"
#include "opt_inet.h"
#include "opt_inet6.h"

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>  
#include <sys/types.h>
#include <sys/bitstring.h>
#include <sys/condvar.h>
#include <sys/callout.h>
#include <sys/kernel.h>  
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_llatbl.h>
#include <net/if_var.h>
#include <net/route.h> 
#include <net/flowtable.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#ifdef INET6
#include <netinet/ip6.h>
#endif
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/sctp.h>

#include <libkern/jenkins.h>
#include <ddb/ddb.h>

struct ipv4_tuple {
	uint16_t 	ip_sport;	/* source port */
	uint16_t 	ip_dport;	/* destination port */
	in_addr_t 	ip_saddr;	/* source address */
	in_addr_t 	ip_daddr;	/* destination address */
};

union ipv4_flow {
	struct ipv4_tuple ipf_ipt;
	uint32_t 	ipf_key[3];
};

struct ipv6_tuple {
	uint16_t 	ip_sport;	/* source port */
	uint16_t 	ip_dport;	/* destination port */
	struct in6_addr	ip_saddr;	/* source address */
	struct in6_addr	ip_daddr;	/* destination address */
};

union ipv6_flow {
	struct ipv6_tuple ipf_ipt;
	uint32_t 	ipf_key[9];
};

struct flentry {
	volatile uint32_t	f_fhash;	/* hash flowing forward */
	uint16_t		f_flags;	/* flow flags */
	uint8_t			f_pad;		
	uint8_t			f_proto;	/* protocol */
	uint32_t		f_fibnum;	/* fib index */
	uint32_t		f_uptime;	/* uptime at last access */
	struct flentry		*f_next;	/* pointer to collision entry */
	volatile struct rtentry *f_rt;		/* rtentry for flow */
	volatile struct llentry *f_lle;		/* llentry for flow */
};

struct flentry_v4 {
	struct flentry	fl_entry;
	union ipv4_flow	fl_flow;
};

struct flentry_v6 {
	struct flentry	fl_entry;
	union ipv6_flow	fl_flow;
};

#define	fl_fhash	fl_entry.fl_fhash
#define	fl_flags	fl_entry.fl_flags
#define	fl_proto	fl_entry.fl_proto
#define	fl_uptime	fl_entry.fl_uptime
#define	fl_rt		fl_entry.fl_rt
#define	fl_lle		fl_entry.fl_lle

#define	SECS_PER_HOUR		3600
#define	SECS_PER_DAY		(24*SECS_PER_HOUR)

#define	SYN_IDLE		300
#define	UDP_IDLE		300
#define	FIN_WAIT_IDLE		600
#define	TCP_IDLE		SECS_PER_DAY


typedef	void fl_lock_t(struct flowtable *, uint32_t);
typedef void fl_rtalloc_t(struct route *, uint32_t, u_int);

union flentryp {
	struct flentry		**global;
	struct flentry		**pcpu[MAXCPU];
};

struct flowtable {
	int 		ft_size;
	int 		ft_lock_count;
	uint32_t	ft_flags;
	uint32_t	ft_collisions;
	uint32_t	ft_allocated;
	uint32_t	ft_misses;
	uint64_t	ft_hits;

	uint32_t	ft_udp_idle;
	uint32_t	ft_fin_wait_idle;
	uint32_t	ft_syn_idle;
	uint32_t	ft_tcp_idle;

	char		*ft_name;
	fl_lock_t	*ft_lock;
	fl_lock_t 	*ft_unlock;
	fl_rtalloc_t	*ft_rtalloc;
	struct mtx	*ft_locks;

	union flentryp	ft_table;
	bitstr_t 	*ft_masks[MAXCPU];
	bitstr_t	*ft_tmpmask;
	struct flowtable *ft_next;
};

static struct proc *flowcleanerproc;
static VNET_DEFINE(struct flowtable *, flow_list_head);
static VNET_DEFINE(uint32_t, flow_hashjitter);
static VNET_DEFINE(uma_zone_t, flow_ipv4_zone);
static VNET_DEFINE(uma_zone_t, flow_ipv6_zone);

#define	V_flow_list_head	VNET(flow_list_head)
#define	V_flow_hashjitter	VNET(flow_hashjitter)
#define	V_flow_ipv4_zone	VNET(flow_ipv4_zone)
#define	V_flow_ipv6_zone	VNET(flow_ipv6_zone)

static struct cv 	flowclean_cv;
static struct mtx	flowclean_lock;
static uint32_t		flowclean_cycles;

#define FLOWTABLE_DEBUG
#ifdef FLOWTABLE_DEBUG
#define FLDPRINTF(ft, flags, fmt, ...) 		\
do {		  				\
	if ((ft)->ft_flags & (flags))		\
		printf((fmt), __VA_ARGS__);	\
} while (0);					\

#else
#define FLDPRINTF(ft, flags, fmt, ...)

#endif


/*
 * TODO:
 * - Make flowtable stats per-cpu, aggregated at sysctl call time,
 *   to avoid extra cache evictions caused by incrementing a shared
 *   counter
 * - add sysctls to resize && flush flow tables 
 * - Add per flowtable sysctls for statistics and configuring timeouts
 * - add saturation counter to rtentry to support per-packet load-balancing
 *   add flag to indicate round-robin flow, add list lookup from head
     for flows
 * - add sysctl / device node / syscall to support exporting and importing
 *   of flows with flag to indicate that a flow was imported so should
 *   not be considered for auto-cleaning
 * - support explicit connection state (currently only ad-hoc for DSR)
 * - idetach() cleanup for options VIMAGE builds.
 */
VNET_DEFINE(int, flowtable_enable) = 1;
static VNET_DEFINE(int, flowtable_debug);
static VNET_DEFINE(int, flowtable_hits);
static VNET_DEFINE(int, flowtable_lookups);
static VNET_DEFINE(int, flowtable_misses);
static VNET_DEFINE(int, flowtable_frees);
static VNET_DEFINE(int, flowtable_free_checks);
static VNET_DEFINE(int, flowtable_max_depth);
static VNET_DEFINE(int, flowtable_collisions);
static VNET_DEFINE(int, flowtable_syn_expire) = SYN_IDLE;
static VNET_DEFINE(int, flowtable_udp_expire) = UDP_IDLE;
static VNET_DEFINE(int, flowtable_fin_wait_expire) = FIN_WAIT_IDLE;
static VNET_DEFINE(int, flowtable_tcp_expire) = TCP_IDLE;
static VNET_DEFINE(int, flowtable_nmbflows) = 4096;
static VNET_DEFINE(int, flowtable_ready) = 0;

#define	V_flowtable_enable		VNET(flowtable_enable)
#define	V_flowtable_debug		VNET(flowtable_debug)
#define	V_flowtable_hits		VNET(flowtable_hits)
#define	V_flowtable_lookups		VNET(flowtable_lookups)
#define	V_flowtable_misses		VNET(flowtable_misses)
#define	V_flowtable_frees		VNET(flowtable_frees)
#define	V_flowtable_free_checks		VNET(flowtable_free_checks)
#define	V_flowtable_max_depth		VNET(flowtable_max_depth)
#define	V_flowtable_collisions		VNET(flowtable_collisions)
#define	V_flowtable_syn_expire		VNET(flowtable_syn_expire)
#define	V_flowtable_udp_expire		VNET(flowtable_udp_expire)
#define	V_flowtable_fin_wait_expire	VNET(flowtable_fin_wait_expire)
#define	V_flowtable_tcp_expire		VNET(flowtable_tcp_expire)
#define	V_flowtable_nmbflows		VNET(flowtable_nmbflows)
#define	V_flowtable_ready		VNET(flowtable_ready)

SYSCTL_NODE(_net_inet, OID_AUTO, flowtable, CTLFLAG_RD, NULL, "flowtable");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, debug, CTLFLAG_RW,
    &VNET_NAME(flowtable_debug), 0, "print debug info.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, enable, CTLFLAG_RW,
    &VNET_NAME(flowtable_enable), 0, "enable flowtable caching.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, hits, CTLFLAG_RD,
    &VNET_NAME(flowtable_hits), 0, "# flowtable hits.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, lookups, CTLFLAG_RD,
    &VNET_NAME(flowtable_lookups), 0, "# flowtable lookups.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, misses, CTLFLAG_RD,
    &VNET_NAME(flowtable_misses), 0, "#flowtable misses.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, frees, CTLFLAG_RD,
    &VNET_NAME(flowtable_frees), 0, "#flows freed.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, free_checks, CTLFLAG_RD,
    &VNET_NAME(flowtable_free_checks), 0, "#flows free checks.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, max_depth, CTLFLAG_RD,
    &VNET_NAME(flowtable_max_depth), 0, "max collision list length.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, collisions, CTLFLAG_RD,
    &VNET_NAME(flowtable_collisions), 0, "#flowtable collisions.");

/*
 * XXX This does not end up updating timeouts at runtime
 * and only reflects the value for the last table added :-/
 */
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, syn_expire, CTLFLAG_RW,
    &VNET_NAME(flowtable_syn_expire), 0,
    "seconds after which to remove syn allocated flow.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, udp_expire, CTLFLAG_RW,
    &VNET_NAME(flowtable_udp_expire), 0,
    "seconds after which to remove flow allocated to UDP.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, fin_wait_expire, CTLFLAG_RW,
    &VNET_NAME(flowtable_fin_wait_expire), 0,
    "seconds after which to remove a flow in FIN_WAIT.");
SYSCTL_VNET_INT(_net_inet_flowtable, OID_AUTO, tcp_expire, CTLFLAG_RW,
    &VNET_NAME(flowtable_tcp_expire), 0,
    "seconds after which to remove flow allocated to a TCP connection.");


/*
 * Maximum number of flows that can be allocated of a given type.
 *
 * The table is allocated at boot time (for the pure caching case
 * there is no reason why this could not be changed at runtime)
 * and thus (currently) needs to be set with a tunable.
 */
static int
sysctl_nmbflows(SYSCTL_HANDLER_ARGS)
{
	int error, newnmbflows;

	newnmbflows = V_flowtable_nmbflows;
	error = sysctl_handle_int(oidp, &newnmbflows, 0, req); 
	if (error == 0 && req->newptr) {
		if (newnmbflows > V_flowtable_nmbflows) {
			V_flowtable_nmbflows = newnmbflows;
			uma_zone_set_max(V_flow_ipv4_zone,
			    V_flowtable_nmbflows);
			uma_zone_set_max(V_flow_ipv6_zone,
			    V_flowtable_nmbflows);
		} else
			error = EINVAL;
	}
	return (error);
}
SYSCTL_VNET_PROC(_net_inet_flowtable, OID_AUTO, nmbflows,
    CTLTYPE_INT|CTLFLAG_RW, 0, 0, sysctl_nmbflows, "IU",
    "Maximum number of flows allowed");

#ifndef RADIX_MPATH
static void
in_rtalloc_ign_wrapper(struct route *ro, uint32_t hash, u_int fibnum)
{

	rtalloc_ign_fib(ro, 0, fibnum);
}
#endif

static void
flowtable_global_lock(struct flowtable *table, uint32_t hash)
{	
	int lock_index = (hash)&(table->ft_lock_count - 1);

	mtx_lock(&table->ft_locks[lock_index]);
}

static void
flowtable_global_unlock(struct flowtable *table, uint32_t hash)
{	
	int lock_index = (hash)&(table->ft_lock_count - 1);

	mtx_unlock(&table->ft_locks[lock_index]);
}

static void
flowtable_pcpu_lock(struct flowtable *table, uint32_t hash)
{

	critical_enter();
}

static void
flowtable_pcpu_unlock(struct flowtable *table, uint32_t hash)
{

	critical_exit();
}

#define FL_ENTRY_INDEX(table, hash)((hash) % (table)->ft_size)
#define FL_ENTRY(table, hash) *flowtable_entry((table), (hash))
#define FL_ENTRY_LOCK(table, hash)  (table)->ft_lock((table), (hash))
#define FL_ENTRY_UNLOCK(table, hash) (table)->ft_unlock((table), (hash))

#define FL_STALE 	(1<<8)
#define FL_IPV6  	(1<<9)
#define FL_OVERWRITE	(1<<10)

void
flow_invalidate(struct flentry *fle)
{

	fle->f_flags |= FL_STALE;
}

static __inline int
proto_to_flags(uint8_t proto)
{
	int flag;

	switch (proto) {
	case IPPROTO_TCP:
		flag = FL_TCP;
		break;
	case IPPROTO_SCTP:
		flag = FL_SCTP;
		break;		
	case IPPROTO_UDP:
		flag = FL_UDP;
		break;
	default:
		flag = 0;
		break;
	}

	return (flag);
}

static __inline int
flags_to_proto(int flags)
{
	int proto, protoflags;

	protoflags = flags & (FL_TCP|FL_SCTP|FL_UDP);
	switch (protoflags) {
	case FL_TCP:
		proto = IPPROTO_TCP;
		break;
	case FL_SCTP:
		proto = IPPROTO_SCTP;
		break;
	case FL_UDP:
		proto = IPPROTO_UDP;
		break;
	default:
		proto = 0;
		break;
	}
	return (proto);
}

void
flow_to_route(struct flentry *fle, struct route *ro)
{
	uint32_t *hashkey = NULL;
	
#ifdef INET6
	if (fle->f_flags & FL_IPV6) {
		struct sockaddr_in6 *sin6;

		sin6 = (struct sockaddr_in6 *)&ro->ro_dst;

		sin6->sin6_family = AF_INET6;
		sin6->sin6_len = sizeof(*sin6);
		hashkey = ((struct flentry_v6 *)fle)->fl_flow.ipf_key;
		memcpy(&sin6->sin6_addr, &hashkey[2], sizeof (struct in6_addr));
	} else
#endif
#ifdef INET		
	{
		struct sockaddr_in *sin;

		sin = (struct sockaddr_in *)&ro->ro_dst;

		sin->sin_family = AF_INET;
		sin->sin_len = sizeof(*sin);
		hashkey = ((struct flentry_v4 *)fle)->fl_flow.ipf_key;
		sin->sin_addr.s_addr = hashkey[2];
	}
#endif	
	; /* terminate INET6 else if no INET4 */
	ro->ro_rt = __DEVOLATILE(struct rtentry *, fle->f_rt);
	ro->ro_lle = __DEVOLATILE(struct llentry *, fle->f_lle);
}

#ifdef INET
#ifdef FLOWTABLE_DEBUG
static void
ipv4_flow_print_tuple(int flags, int proto, struct sockaddr_in *ssin,
    struct sockaddr_in *dsin)
{
	char saddr[4*sizeof "123"], daddr[4*sizeof "123"];

	if (flags & FL_HASH_ALL) {
		inet_ntoa_r(ssin->sin_addr, saddr);
		inet_ntoa_r(dsin->sin_addr, daddr);
		printf("proto=%d %s:%d->%s:%d\n",
		    proto, saddr, ntohs(ssin->sin_port), daddr,
		    ntohs(dsin->sin_port));
	} else {
		inet_ntoa_r(*(struct in_addr *) &dsin->sin_addr, daddr);
		printf("proto=%d %s\n", proto, daddr);
	}

}
#endif

static int
ipv4_mbuf_demarshal(struct flowtable *ft, struct mbuf *m,
    struct sockaddr_in *ssin, struct sockaddr_in *dsin, uint16_t *flags)
{
	struct ip *ip;
	uint8_t proto;
	int iphlen;
	struct tcphdr *th;
	struct udphdr *uh;
	struct sctphdr *sh;
	uint16_t sport, dport;

	proto = sport = dport = 0;
	ip = mtod(m, struct ip *);
	dsin->sin_family = AF_INET;
	dsin->sin_len = sizeof(*dsin);
	dsin->sin_addr = ip->ip_dst;
	ssin->sin_family = AF_INET;
	ssin->sin_len = sizeof(*dsin);
	ssin->sin_addr = ip->ip_dst;	

	proto = ip->ip_p;
	if ((*flags & FL_HASH_ALL) == 0) {
		FLDPRINTF(ft, FL_DEBUG_ALL, "skip port check flags=0x%x ",
		    *flags);
		goto skipports;
	}

	iphlen = ip->ip_hl << 2; /* XXX options? */

	switch (proto) {
	case IPPROTO_TCP:
		th = (struct tcphdr *)((caddr_t)ip + iphlen);
		sport = th->th_sport;
		dport = th->th_dport;
		if ((*flags & FL_HASH_ALL) &&
		    (th->th_flags & (TH_RST|TH_FIN)))
			*flags |= FL_STALE;
	break;
	case IPPROTO_UDP:
		uh = (struct udphdr *)((caddr_t)ip + iphlen);
		sport = uh->uh_sport;
		dport = uh->uh_dport;
	break;
	case IPPROTO_SCTP:
		sh = (struct sctphdr *)((caddr_t)ip + iphlen);
		sport = sh->src_port;
		dport = sh->dest_port;
	break;
	default:
		FLDPRINTF(ft, FL_DEBUG_ALL, "proto=0x%x not supported\n", proto);
		return (ENOTSUP);
		/* no port - hence not a protocol we care about */
		break;
	
	}

skipports:
	*flags |= proto_to_flags(proto);
	ssin->sin_port = sport;
	dsin->sin_port = dport;
	return (0);
}

static uint32_t
ipv4_flow_lookup_hash_internal(
	struct sockaddr_in *ssin, struct sockaddr_in *dsin, 
	    uint32_t *key, uint16_t flags)
{
	uint16_t sport, dport;
	uint8_t proto;
	int offset = 0;

	if ((V_flowtable_enable == 0) || (V_flowtable_ready == 0))
		return (0);

	proto = flags_to_proto(flags);
	sport = dport = key[2] = key[1] = key[0] = 0;
	if ((ssin != NULL) && (flags & FL_HASH_ALL)) {
		key[1] = ssin->sin_addr.s_addr;
		sport = ssin->sin_port;
	}
	if (dsin != NULL) {
		key[2] = dsin->sin_addr.s_addr;
		dport = dsin->sin_port;
	}
	if (flags & FL_HASH_ALL) {
		((uint16_t *)key)[0] = sport;
		((uint16_t *)key)[1] = dport; 
	} else
		offset = V_flow_hashjitter + proto;

	return (jenkins_hashword(key, 3, offset));
}

static struct flentry *
flowtable_lookup_mbuf4(struct flowtable *ft, struct mbuf *m)
{
	struct sockaddr ssa, dsa;
	uint16_t flags;
	struct sockaddr_in *dsin, *ssin;

	dsin = (struct sockaddr_in *)&dsa;
	ssin = (struct sockaddr_in *)&ssa;
	flags = ft->ft_flags;
	if (ipv4_mbuf_demarshal(ft, m, ssin, dsin, &flags) != 0)
		return (NULL);

	return (flowtable_lookup(ft, &ssa, &dsa, M_GETFIB(m), flags));
}
#endif /* INET */

#ifdef INET6
/*
 * PULLUP_TO(len, p, T) makes sure that len + sizeof(T) is contiguous,
 * then it sets p to point at the offset "len" in the mbuf. WARNING: the
 * pointer might become stale after other pullups (but we never use it
 * this way).
 */
#define PULLUP_TO(_len, p, T)						\
do {									\
	int x = (_len) + sizeof(T);					\
	if ((m)->m_len < x) {						\
		goto receive_failed;					\
	}								\
	p = (mtod(m, char *) + (_len));					\
} while (0)

#define	TCP(p)		((struct tcphdr *)(p))
#define	SCTP(p)		((struct sctphdr *)(p))
#define	UDP(p)		((struct udphdr *)(p))

static int
ipv6_mbuf_demarshal(struct flowtable *ft, struct mbuf *m,
    struct sockaddr_in6 *ssin6, struct sockaddr_in6 *dsin6, uint16_t *flags)
{
	struct ip6_hdr *ip6;
	uint8_t proto;
	int hlen;
	uint16_t src_port, dst_port;
	u_short offset;
	void *ulp;

	offset = hlen = src_port = dst_port = 0;
	ulp = NULL;
	ip6 = mtod(m, struct ip6_hdr *);
	hlen = sizeof(struct ip6_hdr);
	proto = ip6->ip6_nxt;

	if ((*flags & FL_HASH_ALL) == 0)
		goto skipports;

	while (ulp == NULL) {
		switch (proto) {
		case IPPROTO_ICMPV6:
		case IPPROTO_OSPFIGP:
		case IPPROTO_PIM:
		case IPPROTO_CARP:
		case IPPROTO_ESP:
		case IPPROTO_NONE:
			ulp = ip6;
			break;
		case IPPROTO_TCP:
			PULLUP_TO(hlen, ulp, struct tcphdr);
			dst_port = TCP(ulp)->th_dport;
			src_port = TCP(ulp)->th_sport;
			if ((*flags & FL_HASH_ALL) &&
			    (TCP(ulp)->th_flags & (TH_RST|TH_FIN)))
				*flags |= FL_STALE;
			break;
		case IPPROTO_SCTP:
			PULLUP_TO(hlen, ulp, struct sctphdr);
			src_port = SCTP(ulp)->src_port;
			dst_port = SCTP(ulp)->dest_port;
			break;
		case IPPROTO_UDP:
			PULLUP_TO(hlen, ulp, struct udphdr);
			dst_port = UDP(ulp)->uh_dport;
			src_port = UDP(ulp)->uh_sport;
			break;
		case IPPROTO_HOPOPTS:	/* RFC 2460 */
			PULLUP_TO(hlen, ulp, struct ip6_hbh);
			hlen += (((struct ip6_hbh *)ulp)->ip6h_len + 1) << 3;
			proto = ((struct ip6_hbh *)ulp)->ip6h_nxt;
			ulp = NULL;
			break;
		case IPPROTO_ROUTING:	/* RFC 2460 */
			PULLUP_TO(hlen, ulp, struct ip6_rthdr);	
			hlen += (((struct ip6_rthdr *)ulp)->ip6r_len + 1) << 3;
			proto = ((struct ip6_rthdr *)ulp)->ip6r_nxt;
			ulp = NULL;
			break;
		case IPPROTO_FRAGMENT:	/* RFC 2460 */
			PULLUP_TO(hlen, ulp, struct ip6_frag);
			hlen += sizeof (struct ip6_frag);
			proto = ((struct ip6_frag *)ulp)->ip6f_nxt;
			offset = ((struct ip6_frag *)ulp)->ip6f_offlg &
			    IP6F_OFF_MASK;
			ulp = NULL;
			break;
		case IPPROTO_DSTOPTS:	/* RFC 2460 */
			PULLUP_TO(hlen, ulp, struct ip6_hbh);
			hlen += (((struct ip6_hbh *)ulp)->ip6h_len + 1) << 3;
			proto = ((struct ip6_hbh *)ulp)->ip6h_nxt;
			ulp = NULL;
			break;
		case IPPROTO_AH:	/* RFC 2402 */
			PULLUP_TO(hlen, ulp, struct ip6_ext);
			hlen += (((struct ip6_ext *)ulp)->ip6e_len + 2) << 2;
			proto = ((struct ip6_ext *)ulp)->ip6e_nxt;
			ulp = NULL;
			break;
		default:
			PULLUP_TO(hlen, ulp, struct ip6_ext);
			break;
		}
	}

	if (src_port == 0) {
	receive_failed:
		return (ENOTSUP);
	}

skipports:
	dsin6->sin6_family = AF_INET6;
	dsin6->sin6_len = sizeof(*dsin6);
	dsin6->sin6_port = dst_port;
	memcpy(&dsin6->sin6_addr, &ip6->ip6_dst, sizeof(struct in6_addr));

	ssin6->sin6_family = AF_INET6;
	ssin6->sin6_len = sizeof(*ssin6);
	ssin6->sin6_port = src_port;
	memcpy(&ssin6->sin6_addr, &ip6->ip6_src, sizeof(struct in6_addr));
	*flags |= proto_to_flags(proto);

	return (0);
}

#define zero_key(key) 		\
do {				\
	key[0] = 0;		\
	key[1] = 0;		\
	key[2] = 0;		\
	key[3] = 0;		\
	key[4] = 0;		\
	key[5] = 0;		\
	key[6] = 0;		\
	key[7] = 0;		\
	key[8] = 0;		\
} while (0)
	
static uint32_t
ipv6_flow_lookup_hash_internal(
	struct sockaddr_in6 *ssin6, struct sockaddr_in6 *dsin6, 
	    uint32_t *key, uint16_t flags)
{
	uint16_t sport, dport;
	uint8_t proto;
	int offset = 0;

	if ((V_flowtable_enable == 0) || (V_flowtable_ready == 0))
		return (0);

	proto = flags_to_proto(flags);
	zero_key(key);
	sport = dport = 0;
	if (dsin6 != NULL) {
		memcpy(&key[1], &dsin6->sin6_addr, sizeof(struct in6_addr));
		dport = dsin6->sin6_port;
	}
	if ((ssin6 != NULL) && (flags & FL_HASH_ALL)) {
		memcpy(&key[5], &ssin6->sin6_addr, sizeof(struct in6_addr));
		sport = ssin6->sin6_port;
	}
	if (flags & FL_HASH_ALL) {
		((uint16_t *)key)[0] = sport;
		((uint16_t *)key)[1] = dport; 
	} else
		offset = V_flow_hashjitter + proto;

	return (jenkins_hashword(key, 9, offset));
}

static struct flentry *
flowtable_lookup_mbuf6(struct flowtable *ft, struct mbuf *m)
{
	struct sockaddr ssa, dsa;
	struct sockaddr_in6 *dsin6, *ssin6;	
	uint16_t flags;

	dsin6 = (struct sockaddr_in6 *)&dsa;
	ssin6 = (struct sockaddr_in6 *)&ssa;
	flags = ft->ft_flags;
	
	if (ipv6_mbuf_demarshal(ft, m, ssin6, dsin6, &flags) != 0)
		return (NULL);

	return (flowtable_lookup(ft, &ssa, &dsa, M_GETFIB(m), flags));
}
#endif /* INET6 */

static bitstr_t *
flowtable_mask(struct flowtable *ft)
{
	bitstr_t *mask;

	if (ft->ft_flags & FL_PCPU)
		mask = ft->ft_masks[curcpu];
	else
		mask = ft->ft_masks[0];

	return (mask);
}

static struct flentry **
flowtable_entry(struct flowtable *ft, uint32_t hash)
{
	struct flentry **fle;
	int index = (hash % ft->ft_size);

	if (ft->ft_flags & FL_PCPU) {
		KASSERT(&ft->ft_table.pcpu[curcpu][0] != NULL, ("pcpu not set"));
		fle = &ft->ft_table.pcpu[curcpu][index];
	} else {
		KASSERT(&ft->ft_table.global[0] != NULL, ("global not set"));
		fle = &ft->ft_table.global[index];
	}
	
	return (fle);
}

static int
flow_stale(struct flowtable *ft, struct flentry *fle)
{
	time_t idle_time;

	if ((fle->f_fhash == 0)
	    || ((fle->f_rt->rt_flags & RTF_HOST) &&
		((fle->f_rt->rt_flags & (RTF_UP))
		    != (RTF_UP)))
	    || (fle->f_rt->rt_ifp == NULL))
		return (1);

	idle_time = time_uptime - fle->f_uptime;

	if ((fle->f_flags & FL_STALE) ||
	    ((fle->f_flags & (TH_SYN|TH_ACK|TH_FIN)) == 0
		&& (idle_time > ft->ft_udp_idle)) ||
	    ((fle->f_flags & TH_FIN)
		&& (idle_time > ft->ft_fin_wait_idle)) ||
	    ((fle->f_flags & (TH_SYN|TH_ACK)) == TH_SYN
		&& (idle_time > ft->ft_syn_idle)) ||
	    ((fle->f_flags & (TH_SYN|TH_ACK)) == (TH_SYN|TH_ACK)
		&& (idle_time > ft->ft_tcp_idle)) ||
	    ((fle->f_rt->rt_flags & RTF_UP) == 0 || 
		(fle->f_rt->rt_ifp == NULL)))
		return (1);

	return (0);
}

static void
flowtable_set_hashkey(struct flentry *fle, uint32_t *key)
{
	uint32_t *hashkey;
	int i, nwords;

	if (fle->f_flags & FL_IPV6) {
		nwords = 9;
		hashkey = ((struct flentry_v4 *)fle)->fl_flow.ipf_key;
	} else {
		nwords = 3;
		hashkey = ((struct flentry_v6 *)fle)->fl_flow.ipf_key;
	}
	
	for (i = 0; i < nwords; i++) 
		hashkey[i] = key[i];
}


static uint32_t *
flowtable_get_hashkey(struct flentry *fle)
{
	uint32_t *hashkey;

	if (fle->f_flags & FL_IPV6)
		hashkey = ((struct flentry_v4 *)fle)->fl_flow.ipf_key;
	else
		hashkey = ((struct flentry_v6 *)fle)->fl_flow.ipf_key;

	return (hashkey);
}

static int
flowtable_insert(struct flowtable *ft, uint32_t hash, uint32_t *key,
    uint32_t fibnum, struct route *ro, uint16_t flags)
{
	struct flentry *fle, *fletail, *newfle, **flep;
	int depth;
	uma_zone_t flezone;
	bitstr_t *mask;
	uint8_t proto;

	flezone = (flags & FL_IPV6) ? V_flow_ipv6_zone : V_flow_ipv4_zone;
	newfle = uma_zalloc(flezone, M_NOWAIT | M_ZERO);
	if (newfle == NULL)
		return (ENOMEM);

	newfle->f_flags |= (flags & FL_IPV6);
	proto = flags_to_proto(flags);

	FL_ENTRY_LOCK(ft, hash);
	mask = flowtable_mask(ft);
	flep = flowtable_entry(ft, hash);
	fletail = fle = *flep;

	if (fle == NULL) {
		bit_set(mask, FL_ENTRY_INDEX(ft, hash));
		*flep = fle = newfle;
		goto skip;
	} 
	
	depth = 0;
	V_flowtable_collisions++;
	/*
	 * find end of list and make sure that we were not
	 * preempted by another thread handling this flow
	 */
	while (fle != NULL) {
		if (fle->f_fhash == hash && !flow_stale(ft, fle)) {
			/*
			 * there was either a hash collision
			 * or we lost a race to insert
			 */
			FL_ENTRY_UNLOCK(ft, hash);
			uma_zfree((newfle->f_flags & FL_IPV6) ?
			    V_flow_ipv6_zone : V_flow_ipv4_zone, newfle);

			if (flags & FL_OVERWRITE) 
				goto skip;
			return (EEXIST);
		}
		/*
		 * re-visit this double condition XXX
		 */
		if (fletail->f_next != NULL)
			fletail = fle->f_next;

		depth++;
		fle = fle->f_next;
	} 

	if (depth > V_flowtable_max_depth)
		V_flowtable_max_depth = depth;
	fletail->f_next = newfle;
	fle = newfle;
skip:
	flowtable_set_hashkey(fle, key);

	fle->f_proto = proto;
	fle->f_rt = ro->ro_rt;
	fle->f_lle = ro->ro_lle;
	fle->f_fhash = hash;
	fle->f_fibnum = fibnum;
	fle->f_uptime = time_uptime;
	FL_ENTRY_UNLOCK(ft, hash);
	return (0);
}

int
kern_flowtable_insert(struct flowtable *ft, struct sockaddr *ssa,
    struct sockaddr *dsa, struct route *ro, uint32_t fibnum, int flags)
{
	uint32_t key[9], hash;

	flags = (ft->ft_flags | flags | FL_OVERWRITE);
	hash = 0;

#ifdef INET
	if (ssa->sa_family == AF_INET) 
		hash = ipv4_flow_lookup_hash_internal((struct sockaddr_in *)ssa,
		    (struct sockaddr_in *)dsa, key, flags);
#endif
#ifdef INET6
	if (ssa->sa_family == AF_INET6) 
		hash = ipv6_flow_lookup_hash_internal((struct sockaddr_in6 *)ssa,
		    (struct sockaddr_in6 *)dsa, key, flags);
#endif	
	if (ro->ro_rt == NULL || ro->ro_lle == NULL)
		return (EINVAL);

	FLDPRINTF(ft, FL_DEBUG,
	    "kern_flowtable_insert: key=%x:%x:%x hash=%x fibnum=%d flags=%x\n",
	    key[0], key[1], key[2], hash, fibnum, flags);
#ifdef FLOWTABLE_DEBUG
	if (flags & FL_DEBUG)
		ipv4_flow_print_tuple(flags, flags_to_proto(flags),
		    (struct sockaddr_in *)ssa, (struct sockaddr_in *)dsa);
#endif	
	return (flowtable_insert(ft, hash, key, fibnum, ro, flags));
}

static int
flowtable_key_equal(struct flentry *fle, uint32_t *key)
{
	uint32_t *hashkey;
	int i, nwords;

	if (fle->f_flags & FL_IPV6) {
		nwords = 9;
		hashkey = ((struct flentry_v4 *)fle)->fl_flow.ipf_key;
	} else {
		nwords = 3;
		hashkey = ((struct flentry_v6 *)fle)->fl_flow.ipf_key;
	}

	for (i = 0; i < nwords; i++) 
		if (hashkey[i] != key[i])
			return (0);

	return (1);
}

struct flentry *
flowtable_lookup_mbuf(struct flowtable *ft, struct mbuf *m, int af)
{
	struct flentry *fle = NULL;

#ifdef INET
	if (af == AF_INET)
		fle = flowtable_lookup_mbuf4(ft, m);
#endif
#ifdef INET6
	if (af == AF_INET6)
		fle = flowtable_lookup_mbuf6(ft, m);
#endif	
	if (fle != NULL && m != NULL && (m->m_flags & M_FLOWID) == 0) {
		m->m_flags |= M_FLOWID;
		m->m_pkthdr.flowid = fle->f_fhash;
	}
	return (fle);
}
	
struct flentry *
flowtable_lookup(struct flowtable *ft, struct sockaddr *ssa,
    struct sockaddr *dsa, uint32_t fibnum, int flags)
{
	uint32_t key[9], hash;
	struct flentry *fle;
	uint8_t proto = 0;
	int error = 0;
	struct rtentry *rt;
	struct llentry *lle;
	struct route sro, *ro;

	ro = &sro;
	ro->ro_rt = NULL;
	ro->ro_lle = NULL;
	ro->ro_dst = *dsa;
	hash = 0;
	flags |= ft->ft_flags;
	proto = flags_to_proto(flags);
#ifdef INET
	if (ssa->sa_family == AF_INET) {
		struct sockaddr_in *ssin, *dsin;

		dsin = (struct sockaddr_in *)dsa;
		ssin = (struct sockaddr_in *)ssa;
		
		hash = ipv4_flow_lookup_hash_internal(ssin, dsin, key, flags);
#ifdef FLOWTABLE_DEBUG
		if (flags & FL_DEBUG_ALL){
			printf("lookup: key=%x:%x:%x hash=%x ",
			    key[0], key[1], key[2], hash);
			ipv4_flow_print_tuple(flags, proto, ssin, dsin);
		}
#endif		
	}
#endif	
#ifdef INET6		
	if (ssa->sa_family == AF_INET6) {
		struct sockaddr_in6 *ssin6, *dsin6;

		dsin6 = (struct sockaddr_in6 *)dsa;
		ssin6 = (struct sockaddr_in6 *)ssa;

		hash = ipv6_flow_lookup_hash_internal(ssin6, dsin6, key, flags);
	}
#endif
	/*
	 * Ports are zero and this isn't a transmit cache
	 * - thus not a protocol for which we need to keep 
	 * state
	 * FL_HASH_ALL => key[0] != 0 for TCP || UDP || SCTP
	 */
	if (hash == 0 || (key[0] == 0 && (ft->ft_flags & FL_HASH_ALL)))
		return (NULL);

	V_flowtable_lookups++;
	FL_ENTRY_LOCK(ft, hash);
	if ((fle = FL_ENTRY(ft, hash)) == NULL) {
		FL_ENTRY_UNLOCK(ft, hash);
		goto uncached;
	}
keycheck:	
	FLDPRINTF(ft, FL_DEBUG, "doing keycheck on fle=%p hash=%x\n",
		    fle, fle->f_fhash);
	rt = __DEVOLATILE(struct rtentry *, fle->f_rt);
	lle = __DEVOLATILE(struct llentry *, fle->f_lle);
	if ((rt != NULL)
	    && fle->f_fhash == hash
	    && flowtable_key_equal(fle, key)
	    && (proto == fle->f_proto)
	    && (fibnum == fle->f_fibnum)
	    && (rt->rt_flags & RTF_UP)
	    && (rt->rt_ifp != NULL)) {
		V_flowtable_hits++;
		fle->f_uptime = time_uptime;
		fle->f_flags |= flags;
		FL_ENTRY_UNLOCK(ft, hash);
		return (fle);
	} else if (fle->f_next != NULL) {
		fle = fle->f_next;
		goto keycheck;
	}
	FL_ENTRY_UNLOCK(ft, hash);
uncached:
	if (flags & FL_NOAUTO)
		return (NULL);

	V_flowtable_misses++;
	/*
	 * This bit of code ends up locking the
	 * same route 3 times (just like ip_output + ether_output)
	 * - at lookup
	 * - in rt_check when called by arpresolve
	 * - dropping the refcount for the rtentry
	 *
	 * This could be consolidated to one if we wrote a variant
	 * of arpresolve with an rt_check variant that expected to
	 * receive the route locked
	 */

	ft->ft_rtalloc(ro, hash, fibnum);
	if (ro->ro_rt == NULL) 
		error = ENETUNREACH;
	else {
		struct llentry *lle = NULL;
		struct sockaddr *l3addr;
		struct rtentry *rt = ro->ro_rt;
		struct ifnet *ifp = rt->rt_ifp;

		if (ifp->if_flags & (IFF_POINTOPOINT | IFF_LOOPBACK)) {
			RTFREE(rt);
			ro->ro_rt = NULL;
			return (NULL);
		}
		if (rt->rt_flags & RTF_GATEWAY)
			l3addr = rt->rt_gateway;
		else
			l3addr = &ro->ro_dst;
		llentry_update(&lle, LLTABLE(ifp), l3addr, ifp);
		ro->ro_lle = lle;

		if (lle == NULL) {
			RTFREE(rt);
			ro->ro_rt = NULL;
			return (NULL);
		}
		error = flowtable_insert(ft, hash, key, fibnum, ro, flags);
				
		if (error) {
			RTFREE(rt);
			LLE_FREE(lle);
			ro->ro_rt = NULL;
			ro->ro_lle = NULL;
		}
	} 

	return ((error) ? NULL : fle);
}

/*
 * used by the bit_alloc macro
 */
#define calloc(count, size) malloc((count)*(size), M_DEVBUF, M_WAITOK|M_ZERO)
	
struct flowtable *
flowtable_alloc(char *name, int nentry, int flags)
{
	struct flowtable *ft, *fttail;
	int i;

	if (V_flow_hashjitter == 0)
		V_flow_hashjitter = arc4random();

	KASSERT(nentry > 0, ("nentry must be > 0, is %d\n", nentry));

	ft = malloc(sizeof(struct flowtable),
	    M_RTABLE, M_WAITOK | M_ZERO);

	ft->ft_name = name;
	ft->ft_flags = flags;
	ft->ft_size = nentry;
#ifdef RADIX_MPATH
	ft->ft_rtalloc = rtalloc_mpath_fib;
#else
	ft->ft_rtalloc = in_rtalloc_ign_wrapper;
#endif
	if (flags & FL_PCPU) {
		ft->ft_lock = flowtable_pcpu_lock;
		ft->ft_unlock = flowtable_pcpu_unlock;

		for (i = 0; i <= mp_maxid; i++) {
			ft->ft_table.pcpu[i] =
			    malloc(nentry*sizeof(struct flentry *),
				M_RTABLE, M_WAITOK | M_ZERO);
			ft->ft_masks[i] = bit_alloc(nentry);
		}
	} else {
		ft->ft_lock_count = 2*(powerof2(mp_maxid + 1) ? (mp_maxid + 1):
		    (fls(mp_maxid + 1) << 1));
		
		ft->ft_lock = flowtable_global_lock;
		ft->ft_unlock = flowtable_global_unlock;
		ft->ft_table.global =
			    malloc(nentry*sizeof(struct flentry *),
				M_RTABLE, M_WAITOK | M_ZERO);
		ft->ft_locks = malloc(ft->ft_lock_count*sizeof(struct mtx),
				M_RTABLE, M_WAITOK | M_ZERO);
		for (i = 0; i < ft->ft_lock_count; i++)
			mtx_init(&ft->ft_locks[i], "flow", NULL, MTX_DEF|MTX_DUPOK);

		ft->ft_masks[0] = bit_alloc(nentry);
	}
	ft->ft_tmpmask = bit_alloc(nentry);

	/*
	 * In the local transmit case the table truly is 
	 * just a cache - so everything is eligible for
	 * replacement after 5s of non-use
	 */
	if (flags & FL_HASH_ALL) {
		ft->ft_udp_idle = V_flowtable_udp_expire;
		ft->ft_syn_idle = V_flowtable_syn_expire;
		ft->ft_fin_wait_idle = V_flowtable_fin_wait_expire;
		ft->ft_tcp_idle = V_flowtable_fin_wait_expire;
	} else {
		ft->ft_udp_idle = ft->ft_fin_wait_idle =
		    ft->ft_syn_idle = ft->ft_tcp_idle = 30;
		
	}

	/*
	 * hook in to the cleaner list
	 */
	if (V_flow_list_head == NULL)
		V_flow_list_head = ft;
	else {
		fttail = V_flow_list_head;
		while (fttail->ft_next != NULL)
			fttail = fttail->ft_next;
		fttail->ft_next = ft;
	}

	return (ft);
}

/*
 * The rest of the code is devoted to garbage collection of expired entries.
 * It is a new additon made necessary by the switch to dynamically allocating
 * flow tables.
 * 
 */
static void
fle_free(struct flentry *fle)
{
	struct rtentry *rt;
	struct llentry *lle;

	rt = __DEVOLATILE(struct rtentry *, fle->f_rt);
	lle = __DEVOLATILE(struct llentry *, fle->f_lle);
	RTFREE(rt);
	LLE_FREE(lle);
	uma_zfree((fle->f_flags & FL_IPV6) ?
	    V_flow_ipv6_zone : V_flow_ipv4_zone, fle);
}

static void
flowtable_free_stale(struct flowtable *ft, struct rtentry *rt)
{
	int curbit = 0, count;
	struct flentry *fle,  **flehead, *fleprev;
	struct flentry *flefreehead, *flefreetail, *fletmp;
	bitstr_t *mask, *tmpmask;
	
	flefreehead = flefreetail = NULL;
	mask = flowtable_mask(ft);
	tmpmask = ft->ft_tmpmask;
	memcpy(tmpmask, mask, ft->ft_size/8);
	/*
	 * XXX Note to self, bit_ffs operates at the byte level
	 * and thus adds gratuitous overhead
	 */
	bit_ffs(tmpmask, ft->ft_size, &curbit);
	while (curbit != -1) {
		if (curbit >= ft->ft_size || curbit < -1) {
			log(LOG_ALERT,
			    "warning: bad curbit value %d \n",
			    curbit);
			break;
		}
		
		FL_ENTRY_LOCK(ft, curbit);
		flehead = flowtable_entry(ft, curbit);
		fle = fleprev = *flehead;

		V_flowtable_free_checks++;
#ifdef DIAGNOSTIC
		if (fle == NULL && curbit > 0) {
			log(LOG_ALERT,
			    "warning bit=%d set, but no fle found\n",
			    curbit);
		}
#endif		
		while (fle != NULL) {
			if (rt != NULL) {
				if (__DEVOLATILE(struct rtentry *, fle->f_rt) != rt) {
					fleprev = fle;
					fle = fle->f_next;
					continue;
				}
			} else if (!flow_stale(ft, fle)) {
				fleprev = fle;
				fle = fle->f_next;
				continue;
			}
			/*
			 * delete head of the list
			 */
			if (fleprev == *flehead) {
				fletmp = fleprev;
				if (fle == fleprev) {
					fleprev = *flehead = fle->f_next;
				} else
					fleprev = *flehead = fle;
				fle = fle->f_next;
			} else {
				/*
				 * don't advance fleprev
				 */
				fletmp = fle;
				fleprev->f_next = fle->f_next;
				fle = fleprev->f_next;
			}
			
			if (flefreehead == NULL)
				flefreehead = flefreetail = fletmp;
			else {
				flefreetail->f_next = fletmp;
				flefreetail = fletmp;
			}
			fletmp->f_next = NULL;
		}
		if (*flehead == NULL)
			bit_clear(mask, curbit);
		FL_ENTRY_UNLOCK(ft, curbit);
		bit_clear(tmpmask, curbit);
		bit_ffs(tmpmask, ft->ft_size, &curbit);
	}
	count = 0;
	while ((fle = flefreehead) != NULL) {
		flefreehead = fle->f_next;
		count++;
		V_flowtable_frees++;
		fle_free(fle);
	}
	if (V_flowtable_debug && count)
		log(LOG_DEBUG, "freed %d flow entries\n", count);
}

void
flowtable_route_flush(struct flowtable *ft, struct rtentry *rt)
{
	int i;
	if (ft->ft_flags & FL_PCPU) {
		for (i = 0; i <= mp_maxid; i++) {
			if (CPU_ABSENT(i))
				continue;
			
			if (smp_started == 1) {
				thread_lock(curthread);
				sched_bind(curthread, i);
				thread_unlock(curthread);
			}

			flowtable_free_stale(ft, rt);

			if (smp_started == 1) {
				thread_lock(curthread);
				sched_unbind(curthread);
				thread_unlock(curthread);
			}
		}
	} else {
		flowtable_free_stale(ft, rt);
	}
}

static void
flowtable_clean_vnet(void)
{
	struct flowtable *ft;
	int i;

	ft = V_flow_list_head;
	while (ft != NULL) {
		if (ft->ft_flags & FL_PCPU) {
			for (i = 0; i <= mp_maxid; i++) {
				if (CPU_ABSENT(i))
					continue;

				if (smp_started == 1) {
					thread_lock(curthread);
					sched_bind(curthread, i);
					thread_unlock(curthread);
				}

				flowtable_free_stale(ft, NULL);

				if (smp_started == 1) {
					thread_lock(curthread);
					sched_unbind(curthread);
					thread_unlock(curthread);
				}
			}
		} else {
			flowtable_free_stale(ft, NULL);
		}
		ft = ft->ft_next;
	}
}

static void
flowtable_cleaner(void)
{
	VNET_ITERATOR_DECL(vnet_iter);

	if (bootverbose)
		log(LOG_INFO, "flowtable cleaner started\n");
	while (1) {
		VNET_LIST_RLOCK();
		VNET_FOREACH(vnet_iter) {
			CURVNET_SET(vnet_iter);
			flowtable_clean_vnet();
			CURVNET_RESTORE();
		}
		VNET_LIST_RUNLOCK();

		flowclean_cycles++;
		/*
		 * The 10 second interval between cleaning checks
		 * is arbitrary
		 */
		mtx_lock(&flowclean_lock);
		cv_broadcast(&flowclean_cv);
		cv_timedwait(&flowclean_cv, &flowclean_lock, 10*hz);
		mtx_unlock(&flowclean_lock);
	}
}

static void
flowtable_flush(void *unused __unused)
{
	uint64_t start;
	
	mtx_lock(&flowclean_lock);
	start = flowclean_cycles;
	while (start == flowclean_cycles) {
		cv_broadcast(&flowclean_cv);
		cv_wait(&flowclean_cv, &flowclean_lock);
	}
	mtx_unlock(&flowclean_lock);
}

static struct kproc_desc flow_kp = {
	"flowcleaner",
	flowtable_cleaner,
	&flowcleanerproc
};
SYSINIT(flowcleaner, SI_SUB_KTHREAD_IDLE, SI_ORDER_ANY, kproc_start, &flow_kp);

static void
flowtable_init_vnet(const void *unused __unused)
{

	V_flow_ipv4_zone = uma_zcreate("ip4flow", sizeof(struct flentry_v4),
	    NULL, NULL, NULL, NULL, 64, UMA_ZONE_MAXBUCKET);
	V_flow_ipv6_zone = uma_zcreate("ip6flow", sizeof(struct flentry_v6),
	    NULL, NULL, NULL, NULL, 64, UMA_ZONE_MAXBUCKET);	
	uma_zone_set_max(V_flow_ipv4_zone, V_flowtable_nmbflows);
	uma_zone_set_max(V_flow_ipv6_zone, V_flowtable_nmbflows);
	V_flowtable_ready = 1;
}
VNET_SYSINIT(flowtable_init_vnet, SI_SUB_KTHREAD_INIT, SI_ORDER_MIDDLE,
    flowtable_init_vnet, NULL);

static void
flowtable_init(const void *unused __unused)
{

	cv_init(&flowclean_cv, "flowcleanwait");
	mtx_init(&flowclean_lock, "flowclean lock", NULL, MTX_DEF);
	EVENTHANDLER_REGISTER(ifnet_departure_event, flowtable_flush, NULL,
	    EVENTHANDLER_PRI_ANY);
}
SYSINIT(flowtable_init, SI_SUB_KTHREAD_INIT, SI_ORDER_ANY,
    flowtable_init, NULL);


#ifdef VIMAGE
static void
flowtable_uninit(const void *unused __unused)
{

	V_flowtable_ready = 0;
	uma_zdestroy(V_flow_ipv4_zone);
	uma_zdestroy(V_flow_ipv6_zone);
}

VNET_SYSUNINIT(flowtable_uninit, SI_SUB_KTHREAD_INIT, SI_ORDER_ANY,
    flowtable_uninit, NULL);
#endif

#ifdef DDB
static bitstr_t *
flowtable_mask_pcpu(struct flowtable *ft, int cpuid)
{
	bitstr_t *mask;

	if (ft->ft_flags & FL_PCPU)
		mask = ft->ft_masks[cpuid];
	else
		mask = ft->ft_masks[0];

	return (mask);
}

static struct flentry **
flowtable_entry_pcpu(struct flowtable *ft, uint32_t hash, int cpuid)
{
	struct flentry **fle;
	int index = (hash % ft->ft_size);

	if (ft->ft_flags & FL_PCPU) {
		fle = &ft->ft_table.pcpu[cpuid][index];
	} else {
		fle = &ft->ft_table.global[index];
	}
	
	return (fle);
}

static void
flow_show(struct flowtable *ft, struct flentry *fle)
{
	int idle_time;
	int rt_valid;
	uint16_t sport, dport;
	uint32_t *hashkey;
	char saddr[4*sizeof "123"], daddr[4*sizeof "123"];

	idle_time = (int)(time_uptime - fle->f_uptime);
	rt_valid = fle->f_rt != NULL;

	hashkey = flowtable_get_hashkey(fle);
	if (fle->f_flags & FL_IPV6)
		goto skipaddr;

	inet_ntoa_r(*(struct in_addr *) &hashkey[2], daddr);
	if (ft->ft_flags & FL_HASH_ALL) {
		inet_ntoa_r(*(struct in_addr *) &hashkey[1], saddr);		
		sport = ntohs(((uint16_t *)hashkey)[0]);
		dport = ntohs(((uint16_t *)hashkey)[1]);
		db_printf("proto=%d %s:%d->%s:%d\n", fle->f_proto,
		    saddr, sport, daddr, dport);
	} else 
		db_printf("%s:\n", daddr);
	    
skipaddr:
	    db_printf("\tkey=%x:%x:%x hash=%08x idle_time=%03d rt=%p ifp=%p",
		hashkey[0], hashkey[1], hashkey[2], fle->f_fhash, idle_time,
	    fle->f_rt, rt_valid ? fle->f_rt->rt_ifp : NULL);
	if (rt_valid && (fle->f_rt->rt_flags & RTF_UP))
		db_printf(" RTF_UP ");
	if (fle->f_flags & FL_STALE)
		db_printf(" FL_STALE ");
	db_printf("\n");
}

static void
flowtable_show(struct flowtable *ft, int cpuid)
{
	int curbit = 0;
	struct flentry *fle,  **flehead;
	bitstr_t *mask, *tmpmask;

	db_printf("cpu: %d\n", cpuid);
	mask = flowtable_mask_pcpu(ft, cpuid);
	tmpmask = ft->ft_tmpmask;
	memcpy(tmpmask, mask, ft->ft_size/8);
	/*
	 * XXX Note to self, bit_ffs operates at the byte level
	 * and thus adds gratuitous overhead
	 */
	bit_ffs(tmpmask, ft->ft_size, &curbit);
	while (curbit != -1) {
		if (curbit >= ft->ft_size || curbit < -1) {
			db_printf("warning: bad curbit value %d \n",
			    curbit);
			break;
		}

		flehead = flowtable_entry_pcpu(ft, curbit, cpuid);
		fle = *flehead;

		while (fle != NULL) {	
			flow_show(ft, fle);
			fle = fle->f_next;
			continue;
		}
		bit_clear(tmpmask, curbit);
		bit_ffs(tmpmask, ft->ft_size, &curbit);
	}
}

static void
flowtable_show_vnet(void)
{
	struct flowtable *ft;
	int i;

	ft = V_flow_list_head;
	while (ft != NULL) {
		printf("name: %s\n", ft->ft_name);
		if (ft->ft_flags & FL_PCPU) {
			for (i = 0; i <= mp_maxid; i++) {
				if (CPU_ABSENT(i))
					continue;
				flowtable_show(ft, i);
			}
		} else {
			flowtable_show(ft, 0);
		}
		ft = ft->ft_next;
	}
}

DB_SHOW_COMMAND(flowtables, db_show_flowtables)
{
	VNET_ITERATOR_DECL(vnet_iter);

	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		flowtable_show_vnet();
		CURVNET_RESTORE();
	}
}
#endif
