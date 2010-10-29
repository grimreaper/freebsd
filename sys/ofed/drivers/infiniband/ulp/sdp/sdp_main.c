/*
 * Copyright (c) 2006 Mellanox Technologies Ltd.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/*
 *  This file is based on net/ipv4/tcp.c
 *  under the following permission notice:
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either  version
 *  2 of the License, or(at your option) any later version.
 */

#if defined(__ia64__)
/* csum_partial_copy_from_user is not exported on ia64.
   We don't really need it for SDP - mb_copy_to_page happens to call it
   but for SDP HW checksum is always set, so ... */

#include <linux/errno.h>
#include <linux/types.h>
#include <asm/checksum.h>

static inline
unsigned int csum_partial_copy_from_user_new (const char *src, char *dst,
						 int len, unsigned int sum,
						 int *errp)
{
	*errp = -EINVAL;
	return 0;
}

#define csum_partial_copy_from_user csum_partial_copy_from_user_new
#endif

#include <linux/tcp.h>
#include <asm/ioctls.h>
#include <linux/workqueue.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <net/protocol.h>
#include <net/inet_common.h>
#include <rdma/rdma_cm.h>
#include <rdma/ib_fmr_pool.h>
#include <rdma/ib_verbs.h>
#include <linux/pagemap.h>
#include <rdma/sdp_socket.h>
#include "sdp.h"
#include <linux/delay.h>

MODULE_AUTHOR("Michael S. Tsirkin");
MODULE_DESCRIPTION("InfiniBand SDP module");
MODULE_LICENSE("Dual BSD/GPL");

#ifdef CONFIG_INFINIBAND_SDP_DEBUG
SDP_MODPARAM_INT(sdp_debug_level, 0, "Enable debug tracing if > 0.");
#endif
#ifdef CONFIG_INFINIBAND_SDP_DEBUG_DATA
SDP_MODPARAM_INT(sdp_data_debug_level, 0,
		"Enable data path debug tracing if > 0.");
#endif

SDP_MODPARAM_SINT(recv_poll_hit, -1, "How many times recv poll helped.");
SDP_MODPARAM_SINT(recv_poll_miss, -1, "How many times recv poll missed.");
SDP_MODPARAM_SINT(recv_poll, 1000, "How many times to poll recv.");
SDP_MODPARAM_SINT(sdp_keepalive_time, SDP_KEEPALIVE_TIME,
	"Default idle time in seconds before keepalive probe sent.");
static int sdp_bzcopy_thresh = 0;
SDP_MODPARAM_INT(sdp_zcopy_thresh, SDP_DEF_ZCOPY_THRESH ,
	"Zero copy using RDMA threshold; 0=Off.");
#define SDP_RX_COAL_TIME_HIGH 128
SDP_MODPARAM_SINT(sdp_rx_coal_target, 0x50000,
	"Target number of bytes to coalesce with interrupt moderation.");
SDP_MODPARAM_SINT(sdp_rx_coal_time, 0x10, "rx coal time (jiffies).");
SDP_MODPARAM_SINT(sdp_rx_rate_low, 80000, "rx_rate low (packets/sec).");
SDP_MODPARAM_SINT(sdp_rx_coal_time_low, 0, "low moderation usec.");
SDP_MODPARAM_SINT(sdp_rx_rate_high, 100000, "rx_rate high (packets/sec).");
SDP_MODPARAM_SINT(sdp_rx_coal_time_high, 128, "high moderation usec.");
SDP_MODPARAM_SINT(sdp_rx_rate_thresh, (200000 / SDP_RX_COAL_TIME_HIGH),
	"rx rate thresh ().");
SDP_MODPARAM_SINT(sdp_sample_interval, (HZ / 4), "sample interval (jiffies).");

SDP_MODPARAM_SINT(hw_int_mod_count, -1,
		"forced hw int moderation val. -1 for auto (packets).");
SDP_MODPARAM_SINT(hw_int_mod_usec, -1,
		"forced hw int moderation val. -1 for auto (usec).");

struct workqueue_struct *sdp_wq;
struct workqueue_struct *rx_comp_wq;

struct list_head sock_list;
spinlock_t sock_list_lock;

static DEFINE_RWLOCK(device_removal_lock);

static inline unsigned int sdp_keepalive_time_when(const struct sdp_sock *ssk)
{
	return ssk->keepalive_time ? : sdp_keepalive_time;
}

inline void sdp_add_sock(struct sdp_sock *ssk)
{
	spin_lock_irq(&sock_list_lock);
	list_add_tail(&ssk->sock_list, &sock_list);
	spin_unlock_irq(&sock_list_lock);
}

inline void sdp_remove_sock(struct sdp_sock *ssk)
{
	spin_lock_irq(&sock_list_lock);
	BUG_ON(list_empty(&sock_list));
	list_del_init(&(ssk->sock_list));
	spin_unlock_irq(&sock_list_lock);
}

static int sdp_get_port(struct socket *sk, unsigned short snum)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	struct socketaddr_in *src_addr;
	int rc;

	struct socketaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(snum),
		.sin_addr.s_addr = inet_sk(sk)->rcv_saddr,
	};

	sdp_dbg(sk, "%s: %u.%u.%u.%u:%hu\n", __func__,
		NIPQUAD(addr.sin_addr.s_addr), ntohs(addr.sin_port));

	if (!ssk->id)
		ssk->id = rdma_create_id(sdp_cma_handler, sk, RDMA_PS_SDP);

	if (!ssk->id)
	       return -ENOMEM;

	/* IP core seems to bind many times to the same address */
	/* TODO: I don't really understand why. Find out. */
	if (!memcmp(&addr, &ssk->id->route.addr.src_addr, sizeof addr))
		return 0;

	rc = ssk->last_bind_err = rdma_bind_addr(ssk->id, (struct socketaddr *)&addr);
	if (rc) {
		sdp_dbg(sk, "Destroying qp\n");
		rdma_destroy_id(ssk->id);
		ssk->id = NULL;
		return rc;
	}

	src_addr = (struct socketaddr_in *)&(ssk->id->route.addr.src_addr);
	inet_sk(sk)->num = ntohs(src_addr->sin_port);
	return 0;
}

static void sdp_destroy_qp(struct sdp_sock *ssk)
{
	sdp_dbg(&ssk->isk.sk, "destroying qp\n");
	sdp_prf(&ssk->isk.sk, NULL, "destroying qp");

	ssk->qp_active = 0;

	del_timer(&ssk->tx_ring.timer);

	if (ssk->qp) {
		ib_destroy_qp(ssk->qp);
		ssk->qp = NULL;
	}

	sdp_rx_ring_destroy(ssk);
	sdp_tx_ring_destroy(ssk);

	sdp_remove_large_sock(ssk);
}

static void sdp_reset_keepalive_timer(struct socket *sk, unsigned long len)
{
	struct sdp_sock *ssk = sdp_sk(sk);

	sdp_dbg(sk, "%s\n", __func__);

	ssk->keepalive_tx_head = ring_head(ssk->tx_ring);
	ssk->keepalive_rx_head = ring_head(ssk->rx_ring);

	sk_reset_timer(sk, &sk->sk_timer, jiffies + len);
}

static void sdp_delete_keepalive_timer(struct socket *sk)
{
	struct sdp_sock *ssk = sdp_sk(sk);

	sdp_dbg(sk, "%s\n", __func__);

	ssk->keepalive_tx_head = 0;
	ssk->keepalive_rx_head = 0;

	sk_stop_timer(sk, &sk->sk_timer);
}

static void sdp_keepalive_timer(unsigned long data)
{
	struct socket *sk = (struct socket *)data;
	struct sdp_sock *ssk = sdp_sk(sk);

	sdp_dbg(sk, "%s\n", __func__);

	/* Only process if the socket is not in use */
	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		sdp_reset_keepalive_timer(sk, HZ / 20);
		goto out;
	}

	if (!sock_flag(sk, SOCK_KEEPOPEN) || sk->sk_state == TCPS_LISTEN ||
	    sk->sk_state == TCPS_CLOSED)
		goto out;

	if (ssk->keepalive_tx_head == ring_head(ssk->tx_ring) &&
	    ssk->keepalive_rx_head == ring_head(ssk->rx_ring))
		sdp_post_keepalive(ssk);

	sdp_reset_keepalive_timer(sk, sdp_keepalive_time_when(ssk));

out:
	bh_unlock_sock(sk);
	sock_put(sk, SOCK_REF_ALIVE);
}

static void sdp_init_keepalive_timer(struct socket *sk)
{
	sk->sk_timer.function = sdp_keepalive_timer;
	sk->sk_timer.data = (unsigned long)sk;
}

static void sdp_set_keepalive(struct socket *sk, int val)
{
	sdp_dbg(sk, "%s %d\n", __func__, val);

	if ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN))
		return;

	if (val && !sock_flag(sk, SOCK_KEEPOPEN))
		sdp_start_keepalive_timer(sk);
	else if (!val)
		sdp_delete_keepalive_timer(sk);
}

void sdp_start_keepalive_timer(struct socket *sk)
{
	sdp_reset_keepalive_timer(sk, sdp_keepalive_time_when(sdp_sk(sk)));
}

void sdp_set_default_moderation(struct sdp_sock *ssk)
{
	struct socket *sk = &ssk->isk.sk;
	struct sdp_moderation *mod = &ssk->auto_mod;
	int rx_buf_size;

	if (hw_int_mod_count > -1 || hw_int_mod_usec > -1) {
		int err;

		mod->adaptive_rx_coal = 0;

		if (hw_int_mod_count > 0 && hw_int_mod_usec > 0) {
			err = ib_modify_cq(ssk->rx_ring.cq, hw_int_mod_count,
					hw_int_mod_usec);
			if (err)
				sdp_warn(sk,
					"Failed modifying moderation for cq");
			else
				sdp_dbg(sk,
					"Using fixed interrupt moderation\n");
		}
		return;
	}

	mod->adaptive_rx_coal = 1;
	sdp_dbg(sk, "Using adaptive interrupt moderation\n");

	/* If we haven't received a specific coalescing setting
	 * (module param), we set the moderation paramters as follows:
	 * - moder_cnt is set to the number of mtu sized packets to
	 *   satisfy our coelsing target.
	 * - moder_time is set to a fixed value.
	 */
	rx_buf_size = (ssk->recv_frags * PAGE_SIZE) + sizeof(struct sdp_bsdh);
	mod->moder_cnt = sdp_rx_coal_target / rx_buf_size + 1;
	mod->moder_time = sdp_rx_coal_time;
	sdp_dbg(sk, "Default coalesing params for buf size:%d - "
			     "moder_cnt:%d moder_time:%d\n",
		 rx_buf_size, mod->moder_cnt, mod->moder_time);

	/* Reset auto-moderation params */
	mod->pkt_rate_low = sdp_rx_rate_low;
	mod->rx_usecs_low = sdp_rx_coal_time_low;
	mod->pkt_rate_high = sdp_rx_rate_high;
	mod->rx_usecs_high = sdp_rx_coal_time_high;
	mod->sample_interval = sdp_sample_interval;

	mod->last_moder_time = SDP_AUTO_CONF;
	mod->last_moder_jiffies = 0;
	mod->last_moder_packets = 0;
	mod->last_moder_tx_packets = 0;
	mod->last_moder_bytes = 0;
}

/* If tx and rx packet rates are not balanced, assume that
 * traffic is mainly BW bound and apply maximum moderation.
 * Otherwise, moderate according to packet rate */
static inline int calc_moder_time(int rate, struct sdp_moderation *mod,
		int tx_pkt_diff, int rx_pkt_diff)
{
	if (2 * tx_pkt_diff > 3 * rx_pkt_diff ||
			2 * rx_pkt_diff > 3 * tx_pkt_diff)
		return mod->rx_usecs_high;

	if (rate < mod->pkt_rate_low)
		return mod->rx_usecs_low;

	if (rate > mod->pkt_rate_high)
		return mod->rx_usecs_high;

	return (rate - mod->pkt_rate_low) *
		(mod->rx_usecs_high - mod->rx_usecs_low) /
		(mod->pkt_rate_high - mod->pkt_rate_low) +
		mod->rx_usecs_low;
}

static void sdp_auto_moderation(struct sdp_sock *ssk)
{
	struct sdp_moderation *mod = &ssk->auto_mod;

	unsigned long period = jiffies - mod->last_moder_jiffies;
	unsigned long packets;
	unsigned long rate;
	unsigned long avg_pkt_size;
	unsigned long tx_pkt_diff;
	unsigned long rx_pkt_diff;
	int moder_time;
	int err;

	if (!mod->adaptive_rx_coal)
		return;

	if (period < mod->sample_interval)
		return;

	if (!mod->last_moder_jiffies || !period)
		goto out;

	tx_pkt_diff = ((unsigned long) (ssk->tx_packets -
					mod->last_moder_tx_packets));
	rx_pkt_diff = ((unsigned long) (ssk->rx_packets -
					mod->last_moder_packets));
	packets = max(tx_pkt_diff, rx_pkt_diff);
	rate = packets * HZ / period;
	avg_pkt_size = packets ? ((unsigned long) (ssk->rx_bytes -
				 mod->last_moder_bytes)) / packets : 0;

	/* Apply auto-moderation only when packet rate exceeds a rate that
	 * it matters */
	if (rate > sdp_rx_rate_thresh) {
		moder_time = calc_moder_time(rate, mod, tx_pkt_diff,
				rx_pkt_diff);
	} else {
		/* When packet rate is low, use default moderation rather
		 * than 0 to prevent interrupt storms if traffic suddenly
		 * increases */
		moder_time = mod->moder_time;
	}

	sdp_dbg_data(&ssk->isk.sk, "tx rate:%lu rx_rate:%lu\n",
			tx_pkt_diff * HZ / period, rx_pkt_diff * HZ / period);

	sdp_dbg_data(&ssk->isk.sk, "Rx moder_time changed from:%d to %d "
			"period:%lu [jiff] packets:%lu avg_pkt_size:%lu "
			"rate:%lu [p/s])\n",
			mod->last_moder_time, moder_time, period, packets,
			avg_pkt_size, rate);

	if (moder_time != mod->last_moder_time) {
		mod->last_moder_time = moder_time;
		err = ib_modify_cq(ssk->rx_ring.cq, mod->moder_cnt, moder_time);
		if (err) {
			sdp_dbg_data(&ssk->isk.sk,
					"Failed modifying moderation for cq");
		}
	}

out:
	mod->last_moder_packets = ssk->rx_packets;
	mod->last_moder_tx_packets = ssk->tx_packets;
	mod->last_moder_bytes = ssk->rx_bytes;
	mod->last_moder_jiffies = jiffies;
}

void sdp_reset_sk(struct socket *sk, int rc)
{
	struct sdp_sock *ssk = sdp_sk(sk);

	sdp_dbg(sk, "%s\n", __func__);

	read_lock(&device_removal_lock);

	if (ssk->tx_ring.cq)
		sdp_xmit_poll(ssk, 1);

	sdp_abort_srcavail(sk);

	sdp_abort_rdma_read(sk);

	if (!(sk->sk_shutdown & RCV_SHUTDOWN) || !sk_stream_memory_free(sk)) {
		sdp_dbg(sk, "setting state to error\n");
		sdp_set_error(sk, rc);
	}

	sk->sk_state_change(sk);

	/* Don't destroy socket before destroy work does its job */
	sock_hold(sk, SOCK_REF_RESET);
	queue_work(sdp_wq, &ssk->destroy_work);

	read_unlock(&device_removal_lock);
}

/* Like tcp_reset */
/* When we get a reset (completion with error) we do this. */
void sdp_reset(struct socket *sk)
{
	int err;

	sdp_dbg(sk, "%s state=%d\n", __func__, sk->sk_state);

	if (sk->sk_state != TCPS_ESTABLISHED)
		return;

	/* We want the right error as BSD sees it (and indeed as we do). */

	/* On fin we currently only set RCV_SHUTDOWN, so .. */
	err = (sk->sk_shutdown & RCV_SHUTDOWN) ? EPIPE : ECONNRESET;

	sdp_set_error(sk, -err);
	wake_up(&sdp_sk(sk)->wq);
	sk->sk_state_change(sk);
}

/* TODO: linger? */
static void sdp_close_sk(struct socket *sk)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	struct rdma_cm_id *id = NULL;
	sdp_dbg(sk, "%s\n", __func__);

	lock_sock(sk);

	sk->sk_send_head = NULL;
	mb_queue_purge(&sk->sk_write_queue);
        /*
         * If sendmsg cached page exists, toss it.
         */
        if (sk->sk_sndmsg_page) {
                __free_page(sk->sk_sndmsg_page);
                sk->sk_sndmsg_page = NULL;
        }

	id = ssk->id;
	if (ssk->id) {
		id->qp = NULL;
		ssk->id = NULL;
		release_sock(sk);
		rdma_destroy_id(id);
		lock_sock(sk);
	}

	mb_queue_purge(&sk->sk_receive_queue);

	sdp_destroy_qp(ssk);

	sdp_dbg(sk, "%s done; releasing sock\n", __func__);
	release_sock(sk);

	flush_scheduled_work();
}

static void sdp_destruct(struct socket *sk)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	struct sdp_sock *s, *t;

	sdp_dbg(sk, "%s\n", __func__);
	if (ssk->destructed_already) {
		sdp_warn(sk, "redestructing sk!\n");
		return;
	}

	cancel_delayed_work(&ssk->srcavail_cancel_work);

	ssk->destructed_already = 1;

	sdp_remove_sock(ssk);

	sdp_close_sk(sk);

	if (ssk->parent)
		goto done;

	list_for_each_entry_safe(s, t, &ssk->backlog_queue, backlog_queue) {
		sk_common_release(&s->isk.sk);
	}
	list_for_each_entry_safe(s, t, &ssk->accept_queue, accept_queue) {
		sk_common_release(&s->isk.sk);
	}

done:
	sdp_dbg(sk, "%s done\n", __func__);
}

static inline void sdp_start_dreq_wait_timeout(struct sdp_sock *ssk, int timeo)
{
	sdp_dbg(&ssk->isk.sk, "Starting dreq wait timeout\n");

	queue_delayed_work(sdp_wq, &ssk->dreq_wait_work, timeo);
	ssk->dreq_wait_timeout = 1;
}

static void sdp_send_disconnect(struct socket *sk)
{
	sock_hold(sk, SOCK_REF_DREQ_TO);
	sdp_start_dreq_wait_timeout(sdp_sk(sk), SDP_FIN_WAIT_TIMEOUT);

	sdp_sk(sk)->sdp_disconnect = 1;
	sdp_post_sends(sdp_sk(sk), 0);
}

/*
 *	State processing on a close.
 *	TCPS_ESTABLISHED -> TCPS_FIN_WAIT_1 -> TCPS_CLOSED
 */
static int sdp_close_state(struct socket *sk)
{
	switch (sk->sk_state) {
	case TCPS_ESTABLISHED:
		sdp_exch_state(sk, TCPF_ESTABLISHED, TCPS_FIN_WAIT_1);
		break;
	case TCPS_CLOSED_WAIT:
		sdp_exch_state(sk, TCPF_CLOSE_WAIT, TCPS_LAST_ACK);
		break;
	default:
		return 0;
	}

	return 1;
}

/*
 * In order to prevent asynchronous-events handling after the last reference
 * count removed, we destroy rdma_id so cma_handler() won't be invoked.
 * This function should be called under lock_sock(sk).
 */
static inline void disable_cma_handler(struct socket *sk)
{
	if (sdp_sk(sk)->id) {
		struct rdma_cm_id *id = sdp_sk(sk)->id;
		sdp_sk(sk)->id = NULL;
		release_sock(sk);
		rdma_destroy_id(id);
		lock_sock(sk);
	}
}

/* Like tcp_close */
static void sdp_close(struct socket *sk, long timeout)
{
	struct mbuf *mb;
	int data_was_unread = 0;

	lock_sock(sk);

	sdp_dbg(sk, "%s\n", __func__);
	sdp_prf(sk, NULL, __func__);

	sdp_delete_keepalive_timer(sk);

	sk->sk_shutdown = SHUTDOWN_MASK;

	if ((1 << sk->sk_state) & (TCPF_TIME_WAIT | TCPF_CLOSE)) {
		/* this could happen if socket was closed by a CM teardown
		   and after that the user called close() */
		disable_cma_handler(sk);
		goto out;
	}

	if (sk->sk_state == TCPS_LISTEN || sk->sk_state == TCPS_SYN_SENT) {
		sdp_exch_state(sk, TCPF_LISTEN | TCPF_SYN_SENT, TCPS_CLOSED);
		disable_cma_handler(sk);

		/* Special case: stop listening.
		   This is done by sdp_destruct. */
		goto adjudge_to_death;
	}

	sock_hold(sk, SOCK_REF_CMA);

	/*  We need to flush the recv. buffs.  We do this only on the
	 *  descriptor close, not protocol-sourced closes, because the
	 *  reader process may not have drained the data yet!
	 */
	while ((mb = mb_dequeue(&sk->sk_receive_queue)) != NULL) {
		struct sdp_bsdh *h = (struct sdp_bsdh *)mb_transport_header(mb);
		if (h->mid == SDP_MID_DISCONN) {
				sdp_handle_disconn(sk);
		} else {
			sdp_dbg(sk, "Data was unread. mb: %p\n", mb);
			data_was_unread = 1;
		}
		m_freem(mb);
	}

	sk_mem_reclaim(sk);

	/* As outlined in draft-ietf-tcpimpl-prob-03.txt, section
	 * 3.10, we send a RST here because data was lost.  To
	 * witness the awful effects of the old behavior of always
	 * doing a FIN, run an older 2.1.x kernel or 2.0.x, start
	 * a bulk GET in an FTP client, suspend the process, wait
	 * for the client to advertise a zero window, then kill -9
	 * the FTP client, wheee...  Note: timeout is always zero
	 * in such a case.
	 */
	if (data_was_unread ||
		(sock_flag(sk, SOCK_LINGER) && !sk->sk_lingertime)) {
		/* Unread data was tossed, zap the connection. */
		NET_INC_STATS_USER(sock_net(sk), LINUX_MIB_TCPABORTONCLOSE);
		sdp_exch_state(sk, TCPF_CLOSE_WAIT | TCPF_ESTABLISHED,
			       TCPS_TIME_WAIT);

		/* Go into abortive close */
		sk->sk_prot->disconnect(sk, 0);
	} else if (sdp_close_state(sk)) {
		/* We FIN if the application ate all the data before
		 * zapping the connection.
		 */

		sdp_send_disconnect(sk);
	}

	/* TODO: state should move to CLOSE or CLOSE_WAIT etc on disconnect.
	   Since it currently doesn't, do it here to avoid blocking below. */
	if (!sdp_sk(sk)->id)
		sdp_exch_state(sk, TCPF_FIN_WAIT1 | TCPF_LAST_ACK |
			       TCPF_CLOSE_WAIT, TCPS_CLOSED);

	sk_stream_wait_close(sk, timeout);

adjudge_to_death:
	/* It is the last release_sock in its life. It will remove backlog. */
	release_sock(sk);
	/* Now socket is owned by kernel and we acquire lock
	   to finish close. No need to check for user refs.
	 */
	lock_sock(sk);

	sock_orphan(sk);

	/*	This is a (useful) BSD violating of the RFC. There is a
	 *	problem with TCP as specified in that the other end could
	 *	keep a socket open forever with no application left this end.
	 *	We use a 3 minute timeout (about the same as BSD) then kill
	 *	our end. If they send after that then tough - BUT: long enough
	 *	that we won't make the old 4*rto = almost no time - whoops
	 *	reset mistake.
	 *
	 *	Nope, it was not mistake. It is really desired behaviour
	 *	f.e. on http servers, when such sockets are useless, but
	 *	consume significant resources. Let's do it with special
	 *	linger2	option.					--ANK
	 */
	if (sk->sk_state == TCPS_FIN_WAIT_1) {
		/* TODO: liger2 unimplemented.
		   We should wait 3.5 * rto. How do I know rto? */
		/* TODO: tcp_fin_time to get timeout */
		sdp_dbg(sk, "%s: entering time wait refcnt %d\n", __func__,
			atomic_read(&sk->sk_refcnt));
		percpu_counter_inc(sk->sk_prot->orphan_count);
	}

	/* TODO: limit number of orphaned sockets.
	   TCP has sysctl_tcp_mem and sysctl_tcp_max_orphans */

out:
	release_sock(sk);

	sk_common_release(sk);
}

static int sdp_connect(struct socket *sk, struct socketaddr *uaddr, int addr_len)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	struct socketaddr_in src_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(inet_sk(sk)->sport),
		.sin_addr.s_addr = inet_sk(sk)->saddr,
	};
	int rc;
	flush_workqueue(sdp_wq);

        if (addr_len < sizeof(struct socketaddr_in))
                return -EINVAL;

        if (uaddr->sa_family != AF_INET && uaddr->sa_family != AF_INET_SDP)
                return -EAFNOSUPPORT;

	if (!ssk->id) {
		rc = sdp_get_port(sk, 0);
		if (rc)
			return rc;
		inet_sk(sk)->sport = htons(inet_sk(sk)->num);
	}

	sdp_dbg(sk, "%s %u.%u.%u.%u:%hu -> %u.%u.%u.%u:%hu\n", __func__,
		NIPQUAD(src_addr.sin_addr.s_addr),
		ntohs(src_addr.sin_port),
		NIPQUAD(((struct socketaddr_in *)uaddr)->sin_addr.s_addr),
		ntohs(((struct socketaddr_in *)uaddr)->sin_port));

	if (!ssk->id) {
		printk("??? ssk->id == NULL. Ohh\n");
		return -EINVAL;
	}

	rc = rdma_resolve_addr(ssk->id, (struct socketaddr *)&src_addr,
			       uaddr, SDP_RESOLVE_TIMEOUT);
	if (rc) {
		sdp_dbg(sk, "rdma_resolve_addr failed: %d\n", rc);
		return rc;
	}

	sdp_exch_state(sk, TCPF_CLOSE, TCPS_SYN_SENT);
	return 0;
}

static int sdp_disconnect(struct socket *sk, int flags)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	int rc = 0;
	struct sdp_sock *s, *t;
	struct rdma_cm_id *id;

	sdp_dbg(sk, "%s\n", __func__);

	if (sk->sk_state != TCPS_LISTEN) {
		if (ssk->id) {
			sdp_sk(sk)->qp_active = 0;
			rc = rdma_disconnect(ssk->id);
		}

		return rc;
	}

	sdp_exch_state(sk, TCPF_LISTEN, TCPS_CLOSED);
	id = ssk->id;
	ssk->id = NULL;
	release_sock(sk); /* release socket since locking semantics is parent
			     inside child */
	if (id)
		rdma_destroy_id(id);

	list_for_each_entry_safe(s, t, &ssk->backlog_queue, backlog_queue) {
		sk_common_release(&s->isk.sk);
	}
	list_for_each_entry_safe(s, t, &ssk->accept_queue, accept_queue) {
		sk_common_release(&s->isk.sk);
	}

	lock_sock(sk);

	return 0;
}

/* Like inet_csk_wait_for_connect */
static int sdp_wait_for_connect(struct socket *sk, long timeo)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	DEFINE_WAIT(wait);
	int err;

	sdp_dbg(sk, "%s\n", __func__);
	/*
	 * True wake-one mechanism for incoming connections: only
	 * one process gets woken up, not the 'whole herd'.
	 * Since we do not 'race & poll' for established sockets
	 * anymore, the common case will execute the loop only once.
	 *
	 * Subtle issue: "add_wait_queue_exclusive()" will be added
	 * after any current non-exclusive waiters, and we know that
	 * it will always _stay_ after any new non-exclusive waiters
	 * because all non-exclusive waiters are added at the
	 * beginning of the wait-queue. As such, it's ok to "drop"
	 * our exclusiveness temporarily when we get woken up without
	 * having to remove and re-insert us on the wait queue.
	 */
	for (;;) {
		prepare_to_wait_exclusive(sk->sk_sleep, &wait,
					  TASK_INTERRUPTIBLE);
		release_sock(sk);
		if (list_empty(&ssk->accept_queue)) {
			timeo = schedule_timeout(timeo);
		}
		lock_sock(sk);
		err = 0;
		if (!list_empty(&ssk->accept_queue))
			break;
		err = -EINVAL;
		if (sk->sk_state != TCPS_LISTEN)
			break;
		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			break;
		err = -EAGAIN;
		if (!timeo)
			break;
	}
	finish_wait(sk->sk_sleep, &wait);
	sdp_dbg(sk, "%s returns %d\n", __func__, err);
	return err;
}

/* Consider using request_sock_queue instead of duplicating all this */
/* Like inet_csk_accept */
static struct socket *sdp_accept(struct socket *sk, int flags, int *err)
{
	struct sdp_sock *newssk = NULL, *ssk;
	struct socket *newsk;
	int error;

	sdp_dbg(sk, "%s state %d expected %d *err %d\n", __func__,
		sk->sk_state, TCPS_LISTEN, *err);

	ssk = sdp_sk(sk);
	lock_sock(sk);

	/* We need to make sure that this socket is listening,
	 * and that it has something pending.
	 */
	error = -EINVAL;
	if (sk->sk_state != TCPS_LISTEN)
		goto out_err;

	/* Find already established connection */
	if (list_empty(&ssk->accept_queue)) {
		long timeo = sock_rcvtimeo(sk, flags & O_NONBLOCK);

		/* If this is a non blocking socket don't sleep */
		error = -EAGAIN;
		if (!timeo)
			goto out_err;

		error = sdp_wait_for_connect(sk, timeo);
		if (error)
			goto out_err;
	}

	newssk = list_entry(ssk->accept_queue.next, struct sdp_sock,
			accept_queue);
	list_del_init(&newssk->accept_queue);
	newssk->parent = NULL;
	sk_acceptq_removed(sk);
	newsk = &newssk->isk.sk;
out:
	release_sock(sk);
	if (newsk) {
		lock_sock(newsk);
		if (newssk->rx_ring.cq) {
			newssk->poll_cq = 1;
			sdp_arm_rx_cq(&newssk->isk.sk);
		}
		release_sock(newsk);
	}
	sdp_dbg(sk, "%s: status %d sk %p newsk %p\n", __func__,
		*err, sk, newsk);
	return newsk;
out_err:
	sdp_dbg(sk, "%s: error %d\n", __func__, error);
	newsk = NULL;
	*err = error;
	goto out;
}

/* Like tcp_ioctl */
static int sdp_ioctl(struct socket *sk, int cmd, unsigned long arg)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	int answ;

	sdp_dbg(sk, "%s\n", __func__);

	switch (cmd) {
	case SIOCINQ:
		if (sk->sk_state == TCPS_LISTEN)
			return -EINVAL;

		lock_sock(sk);
		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else if (sock_flag(sk, SOCK_URGINLINE) ||
			 !ssk->urg_data ||
			 before(ssk->urg_seq, ssk->copied_seq) ||
			 !before(ssk->urg_seq, rcv_nxt(ssk))) {
			answ = rcv_nxt(ssk) - ssk->copied_seq;

			/* Subtract 1, if FIN is in queue. */
			if (answ && !mb_queue_empty(&sk->sk_receive_queue))
				answ -=
			(mb_transport_header(sk->sk_receive_queue.prev))[0]
		        == SDP_MID_DISCONN ? 1 : 0;
		} else
			answ = ssk->urg_seq - ssk->copied_seq;
		release_sock(sk);
		break;
	case SIOCATMARK:
		answ = ssk->urg_data && ssk->urg_seq == ssk->copied_seq;
		break;
	case SIOCOUTQ:
		if (sk->sk_state == TCPS_LISTEN)
			return -EINVAL;

		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else
			answ = ssk->write_seq - ssk->tx_ring.una_seq;
		break;
	default:
		return -ENOIOCTLCMD;
	}
	/* TODO: Need to handle:
	   case SIOCOUTQ:
	 */
	return put_user(answ, (int __user *)arg);
}

void sdp_cancel_dreq_wait_timeout(struct sdp_sock *ssk)
{
	if (!ssk->dreq_wait_timeout)
		return;

	sdp_dbg(&ssk->isk.sk, "cancelling dreq wait timeout\n");

	ssk->dreq_wait_timeout = 0;
	if (cancel_delayed_work_sync(&ssk->dreq_wait_work)) {
		/* The timeout hasn't reached - need to clean ref count */
		sock_put(&ssk->isk.sk, SOCK_REF_DREQ_TO);
	}

	percpu_counter_dec(ssk->isk.sk.sk_prot->orphan_count);
}

static void sdp_destroy_work(struct work_struct *work)
{
	struct sdp_sock *ssk = container_of(work, struct sdp_sock,
			destroy_work);
	struct socket *sk = &ssk->isk.sk;
	sdp_dbg(sk, "%s: refcnt %d\n", __func__, atomic_read(&sk->sk_refcnt));

	sdp_destroy_qp(ssk);

	/* Can be sure that rx_comp_work won't be queued from here cause
	 * ssk->rx_ring.cq is NULL from here
	 */
	cancel_work_sync(&ssk->rx_comp_work);

	memset((void *)&ssk->id, 0, sizeof(*ssk) - offsetof(typeof(*ssk), id));

	sdp_cancel_dreq_wait_timeout(ssk);

	cancel_delayed_work(&ssk->srcavail_cancel_work);

	if (sk->sk_state == TCPS_TIME_WAIT)
		sock_put(sk, SOCK_REF_CMA);

	/* In normal close current state is TCPS_TIME_WAIT or TCPS_CLOSED
	   but if a CM connection is dropped below our legs state could
	   be any state */
	sdp_exch_state(sk, ~0, TCPS_CLOSED);
	sock_put(sk, SOCK_REF_RESET);
}

static void sdp_dreq_wait_timeout_work(struct work_struct *work)
{
	struct sdp_sock *ssk =
		container_of(work, struct sdp_sock, dreq_wait_work.work);
	struct socket *sk = &ssk->isk.sk;
	
	if (!ssk->dreq_wait_timeout)
		goto out;

	lock_sock(sk);

	if (!ssk->dreq_wait_timeout ||
	    !((1 << sk->sk_state) & (TCPF_FIN_WAIT1 | TCPF_LAST_ACK))) {
		release_sock(sk);
		goto out;
	}

	sdp_dbg(sk, "timed out waiting for FIN/DREQ. "
		 "going into abortive close.\n");

	sdp_sk(sk)->dreq_wait_timeout = 0;

	if (sk->sk_state == TCPS_FIN_WAIT_1)
		percpu_counter_dec(ssk->isk.sk.sk_prot->orphan_count);

	sdp_exch_state(sk, TCPF_LAST_ACK | TCPF_FIN_WAIT1, TCPS_TIME_WAIT);

	release_sock(sk);

	if (sdp_sk(sk)->id) {
		sdp_dbg(sk, "Destroyed QP\n");
		sdp_sk(sk)->qp_active = 0;
		rdma_disconnect(sdp_sk(sk)->id);
	} else
		sock_put(sk, SOCK_REF_CMA);

out:
	sock_put(sk, SOCK_REF_DREQ_TO);
}

/*
 * Only SDP interact with this receive queue. Don't want
 * lockdep warnings that using spinlock irqsave
 */
static struct lock_class_key ib_sdp_sk_receive_queue_lock_key;

static struct lock_class_key ib_sdp_sk_callback_lock_key;

static void sdp_destroy_work(struct work_struct *work);
static void sdp_dreq_wait_timeout_work(struct work_struct *work);

int sdp_init_sock(struct socket *sk)
{
	struct sdp_sock *ssk = sdp_sk(sk);

	sdp_dbg(sk, "%s\n", __func__);

	INIT_LIST_HEAD(&ssk->accept_queue);
	INIT_LIST_HEAD(&ssk->backlog_queue);
	INIT_DELAYED_WORK(&ssk->dreq_wait_work, sdp_dreq_wait_timeout_work);
	INIT_DELAYED_WORK(&ssk->srcavail_cancel_work, srcavail_cancel_timeout);
	INIT_WORK(&ssk->destroy_work, sdp_destroy_work);

	lockdep_set_class(&sk->sk_receive_queue.lock,
					&ib_sdp_sk_receive_queue_lock_key);

	lockdep_set_class(&sk->sk_callback_lock,
					&ib_sdp_sk_callback_lock_key);

	sk->sk_route_caps |= NETIF_F_SG | NETIF_F_NO_CSUM;

	mb_queue_head_init(&ssk->rx_ctl_q);

	atomic_set(&ssk->mseq_ack, 0);

	sdp_rx_ring_init(ssk);
	ssk->tx_ring.buffer = NULL;
	ssk->sdp_disconnect = 0;
	ssk->destructed_already = 0;
	ssk->destruct_in_process = 0;
	spin_lock_init(&ssk->lock);
	spin_lock_init(&ssk->tx_sa_lock);
	ssk->tx_compl_pending = 0;

	atomic_set(&ssk->somebody_is_doing_posts, 0);

	ssk->tx_ring.rdma_inflight = NULL;

	init_timer(&ssk->tx_ring.timer);
	init_timer(&ssk->nagle_timer);
	init_timer(&sk->sk_timer);
	ssk->zcopy_thresh = -1; /* use global sdp_zcopy_thresh */
	ssk->last_bind_err = 0;

	return 0;
}

static void sdp_shutdown(struct socket *sk, int how)
{
	struct sdp_sock *ssk = sdp_sk(sk);

	sdp_dbg(sk, "%s\n", __func__);
	if (!(how & SEND_SHUTDOWN))
		return;

	/* If we've already sent a FIN, or it's a closed state, skip this. */
	if (!((1 << sk->sk_state) &
	    (TCPF_ESTABLISHED | TCPF_SYN_SENT |
	     TCPF_SYN_RECV | TCPF_CLOSE_WAIT))) {
		return;
	}

	if (!sdp_close_state(sk))
	    return;

	/*
	 * Just turn off CORK here.
	 *   We could check for socket shutting down in main data path,
	 * but this costs no extra cycles there.
	 */
	ssk->nonagle &= ~TCP_NAGLE_CORK;
	if (ssk->nonagle & TCP_NAGLE_OFF)
		ssk->nonagle |= TCP_NAGLE_PUSH;

	sdp_send_disconnect(sk);
}

static void sdp_mark_push(struct sdp_sock *ssk, struct mbuf *mb)
{
	SDP_SKB_CB(mb)->flags |= TCPCB_FLAG_PSH;
	ssk->pushed_seq = ssk->write_seq;
	sdp_do_posts(ssk);
}

static inline void sdp_push_pending_frames(struct socket *sk)
{
	struct mbuf *mb = sk->sk_send_head;
	if (mb) {
		sdp_mark_push(sdp_sk(sk), mb);
	}
}

/* SOL_SOCKET level options are handled by sock_setsockopt */
static int sdp_setsockopt(struct socket *sk, int level, int optname,
			  char __user *optval, int optlen)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	int val;
	int err = 0;

	sdp_dbg(sk, "%s\n", __func__);
	if (optlen < sizeof(int))
		return -EINVAL;

	if (get_user(val, (int __user *)optval))
		return -EFAULT;

	lock_sock(sk);

	/* SOCK_KEEPALIVE is really a SOL_SOCKET level option but there
	 * is a problem handling it at that level.  In order to start
	 * the keepalive timer on an SDP socket, we must call an SDP
	 * specific routine.  Since sock_setsockopt() can not be modifed
	 * to understand SDP, the application must pass that option
	 * through to us.  Since SO_KEEPALIVE and TCP_DEFER_ACCEPT both
	 * use the same optname, the level must not be SOL_TCP or SOL_SOCKET
	 */
	if (level == PF_INET_SDP && optname == SO_KEEPALIVE) {
		sdp_set_keepalive(sk, val);
		if (val)
			sock_set_flag(sk, SOCK_KEEPOPEN);
		else
			sock_reset_flag(sk, SOCK_KEEPOPEN);
		goto out;
	}

	if (level != SOL_TCP) {
		err = -ENOPROTOOPT;
		goto out;
	}

	switch (optname) {
	case TCP_NODELAY:
		if (val) {
			/* TCP_NODELAY is weaker than TCP_CORK, so that
			 * this option on corked socket is remembered, but
			 * it is not activated until cork is cleared.
			 *
			 * However, when TCP_NODELAY is set we make
			 * an explicit push, which overrides even TCP_CORK
			 * for currently queued segments.
			 */
			ssk->nonagle |= TCP_NAGLE_OFF|TCP_NAGLE_PUSH;
			sdp_push_pending_frames(sk);
		} else {
			ssk->nonagle &= ~TCP_NAGLE_OFF;
		}
		break;
	case TCP_CORK:
		/* When set indicates to always queue non-full frames.
		 * Later the user clears this option and we transmit
		 * any pending partial frames in the queue.  This is
		 * meant to be used alongside sendfile() to get properly
		 * filled frames when the user (for example) must write
		 * out headers with a write() call first and then use
		 * sendfile to send out the data parts.
		 *
		 * TCP_CORK can be set together with TCP_NODELAY and it is
		 * stronger than TCP_NODELAY.
		 */
		if (val) {
			ssk->nonagle |= TCP_NAGLE_CORK;
		} else {
			ssk->nonagle &= ~TCP_NAGLE_CORK;
			if (ssk->nonagle&TCP_NAGLE_OFF)
				ssk->nonagle |= TCP_NAGLE_PUSH;
			sdp_push_pending_frames(sk);
		}
		break;
	case TCP_KEEPIDLE:
		if (val < 1 || val > MAX_TCP_KEEPIDLE)
			err = -EINVAL;
		else {
			ssk->keepalive_time = val * HZ;

			if (sock_flag(sk, SOCK_KEEPOPEN) &&
			    !((1 << sk->sk_state) &
				    (TCPF_CLOSE | TCPF_LISTEN))) {
				sdp_reset_keepalive_timer(sk,
				ssk->keepalive_time);
			}
		}
		break;
	case SDP_ZCOPY_THRESH:
		if (val < SDP_MIN_ZCOPY_THRESH || val > SDP_MAX_ZCOPY_THRESH)
			err = -EINVAL;
		else
			ssk->zcopy_thresh = val;
		break;
	default:
		err = -ENOPROTOOPT;
		break;
	}

out:
	release_sock(sk);
	return err;
}

/* SOL_SOCKET level options are handled by sock_getsockopt */
static int sdp_getsockopt(struct socket *sk, int level, int optname,
			  char __user *optval, int __user *option)
{
	/* TODO */
	struct sdp_sock *ssk = sdp_sk(sk);
	int val, len;

	sdp_dbg(sk, "%s\n", __func__);

	if (level != SOL_TCP)
		return -EOPNOTSUPP;

	if (get_user(len, option))
		return -EFAULT;

	len = min_t(unsigned int, len, sizeof(int));

	if (len < 0)
		return -EINVAL;

	switch (optname) {
	case TCP_NODELAY:
		val = !!(ssk->nonagle&TCP_NAGLE_OFF);
		break;
	case TCP_CORK:
		val = !!(ssk->nonagle&TCP_NAGLE_CORK);
		break;
	case TCP_KEEPIDLE:
		val = (ssk->keepalive_time ? : sdp_keepalive_time) / HZ;
		break;
	case SDP_ZCOPY_THRESH:
		val = ssk->zcopy_thresh;
		break;
	case SDP_LAST_BIND_ERR:
		val = ssk->last_bind_err;
		break;
	default:
		return -ENOPROTOOPT;
	}

	if (put_user(len, option))
		return -EFAULT;
	if (copy_to_user(optval, &val, len))
		return -EFAULT;
	return 0;
}

static inline int poll_recv_cq(struct socket *sk)
{
	int i;
	for (i = 0; i < recv_poll; ++i) {
		if (!mb_queue_empty(&sk->sk_receive_queue)) {
			++recv_poll_hit;
			return 0;
		}
	}
	++recv_poll_miss;
	return 1;
}

/* Like tcp_recv_urg */
/*
 *	Handle reading urgent data. BSD has very simple semantics for
 *	this, no blocking and very strange errors 8)
 */

static int sdp_recv_urg(struct socket *sk, long timeo,
			struct msghdr *msg, int len, int flags,
			int *addr_len)
{
	struct sdp_sock *ssk = sdp_sk(sk);

	poll_recv_cq(sk);

	/* No URG data to read. */
	if (sock_flag(sk, SOCK_URGINLINE) || !ssk->urg_data ||
	    ssk->urg_data == TCP_URG_READ)
		return -EINVAL;	/* Yes this is right ! */

	if (sk->sk_state == TCPS_CLOSED && !sock_flag(sk, SOCK_DONE))
		return -ENOTCONN;

	if (ssk->urg_data & TCP_URG_VALID) {
		int err = 0;
		char c = ssk->urg_data;

		if (!(flags & MSG_PEEK))
			ssk->urg_data = TCP_URG_READ;

		/* Read urgent data. */
		msg->msg_flags |= MSG_OOB;

		if (len > 0) {
			if (!(flags & MSG_TRUNC))
				err = memcpy_toiovec(msg->msg_iov, &c, 1);
			len = 1;
		} else
			msg->msg_flags |= MSG_TRUNC;

		return err ? -EFAULT : len;
	}

	if (sk->sk_state == TCPS_CLOSED || (sk->sk_shutdown & RCV_SHUTDOWN))
		return 0;

	/* Fixed the recv(..., MSG_OOB) behaviour.  BSD docs and
	 * the available implementations agree in this case:
	 * this call should never block, independent of the
	 * blocking state of the socket.
	 * Mike <pall@rz.uni-karlsruhe.de>
	 */
	return -EAGAIN;
}

static inline void sdp_mark_urg(struct socket *sk, struct sdp_sock *ssk,
		int flags)
{
	if (unlikely(flags & MSG_OOB)) {
		struct mbuf *mb = sk->sk_write_queue.prev;
		SDP_SKB_CB(mb)->flags |= TCPCB_FLAG_URG;
	}
}

static inline void sdp_push(struct socket *sk, struct sdp_sock *ssk, int flags)
{
	if (sk->sk_send_head)
		sdp_mark_urg(sk, ssk, flags);
	sdp_do_posts(sdp_sk(sk));
}

void mb_entail(struct socket *sk, struct sdp_sock *ssk, struct mbuf *mb)
{
        __mb_queue_tail(&sk->sk_write_queue, mb);
	sk->sk_wmem_queued += mb->truesize;
        sk_mem_charge(sk, mb->truesize);
        if (!sk->sk_send_head)
                sk->sk_send_head = mb;
        if (ssk->nonagle & TCP_NAGLE_PUSH)
                ssk->nonagle &= ~TCP_NAGLE_PUSH;
}

static inline struct bzcopy_state *sdp_bz_cleanup(struct bzcopy_state *bz)
{
	int i;
	struct sdp_sock *ssk = (struct sdp_sock *)bz->ssk;

	/* Wait for in-flight sends; should be quick */
	if (bz->busy) {
		struct socket *sk = &ssk->isk.sk;
		unsigned long timeout = jiffies + SDP_BZCOPY_POLL_TIMEOUT;

		while (jiffies < timeout) {
			sdp_xmit_poll(sdp_sk(sk), 1);
			if (!bz->busy)
				break;
			SDPSTATS_COUNTER_INC(bzcopy_poll_miss);
		}

		if (bz->busy)
			sdp_warn(sk, "Could not reap %d in-flight sends\n",
				 bz->busy);
	}

	if (bz->pages) {
		for (i = 0; i < bz->page_cnt; i++) {
			put_page(bz->pages[i]);
		}

		kfree(bz->pages);
	}

	kfree(bz);

	return NULL;
}

static int sdp_get_user_pages(struct page **pages, const unsigned int nr_pages,
			      unsigned long uaddr, int rw)
{
	int res, i;

        /* Try to fault in all of the necessary pages */
	down_read(&current->mm->mmap_sem);
        /* rw==READ means read from drive, write into memory area */
	res = get_user_pages(
		current,
		current->mm,
		uaddr,
		nr_pages,
		rw == READ,
		0, /* don't force */
		pages,
		NULL);
	up_read(&current->mm->mmap_sem);

	/* Errors and no page mapped should return here */
	if (res < nr_pages)
		return res;

        for (i=0; i < nr_pages; i++) {
                /* FIXME: flush superflous for rw==READ,
                 * probably wrong function for rw==WRITE
                 */
		flush_dcache_page(pages[i]);
        }

	return nr_pages;
}

static int sdp_get_pages(struct socket *sk, struct page **pages, int page_cnt,
		unsigned long addr)
{
	int done_pages = 0;

	sdp_dbg_data(sk, "count: 0x%x addr: 0x%lx\n", page_cnt, addr);

	addr &= PAGE_MASK;
	if (segment_eq(get_fs(), KERNEL_DS)) {
		for (done_pages = 0; done_pages < page_cnt; done_pages++) {
			pages[done_pages] = virt_to_page(addr);
			if (!pages[done_pages])
				break;
			get_page(pages[done_pages]);
			addr += PAGE_SIZE;
		}
	} else {
		done_pages = sdp_get_user_pages(pages, page_cnt, addr, WRITE);
	}

	if (unlikely(done_pages != page_cnt))
		goto err;

	return 0;

err:
	sdp_warn(sk, "Error getting pages. done_pages: %d page_cnt: %d\n",
			done_pages, page_cnt);
	for (; done_pages > 0; done_pages--)
		page_cache_release(pages[done_pages - 1]);

	return -1;
}

static struct bzcopy_state *sdp_bz_setup(struct sdp_sock *ssk,
					 char __user *base,
					 int len,
					 int size_goal)
{
	struct bzcopy_state *bz;
	unsigned long addr;
	int thresh;
	mm_segment_t cur_fs;
	int rc = 0;

	thresh = sdp_bzcopy_thresh;
	if (thresh == 0 || len < thresh || !capable(CAP_IPC_LOCK)) {
		SDPSTATS_COUNTER_INC(sendmsg_bcopy_segment);
		return NULL;
	}
	SDPSTATS_COUNTER_INC(sendmsg_bzcopy_segment);

	cur_fs = get_fs();

	/*
	 *   Since we use the TCP segmentation fields of the mb to map user
	 * pages, we must make sure that everything we send in a single chunk
	 * fits into the frags array in the mb.
	 */
	size_goal = size_goal / PAGE_SIZE + 1;
	if (size_goal >= MAX_SKB_FRAGS)
		return NULL;

	bz = kzalloc(sizeof(*bz), GFP_KERNEL);
	if (!bz)
		return ERR_PTR(-ENOMEM);

	addr = (unsigned long)base;

	bz->u_base     = base;
	bz->u_len      = len;
	bz->left       = len;
	bz->cur_offset = addr & ~PAGE_MASK;
	bz->busy       = 0;
	bz->ssk        = ssk;
	bz->page_cnt   = PAGE_ALIGN(len + bz->cur_offset) >> PAGE_SHIFT;
	bz->pages      = kcalloc(bz->page_cnt, sizeof(struct page *),
			GFP_KERNEL);

	if (!bz->pages) {
		kfree(bz);
		return ERR_PTR(-ENOMEM);
	}

	rc = sdp_get_pages(&ssk->isk.sk, bz->pages, bz->page_cnt,
			(unsigned long)base);

	if (unlikely(rc))
		goto err;

	return bz;

err:
	kfree(bz->pages);
	kfree(bz);
	return ERR_PTR(-EFAULT);
}

#define TCP_PAGE(sk)	(sk->sk_sndmsg_page)
#define TCP_OFF(sk)	(sk->sk_sndmsg_off)
static inline int sdp_bcopy_get(struct socket *sk, struct mbuf *mb,
				char __user *from, int copy)
{
	int err;
	struct sdp_sock *ssk = sdp_sk(sk);

	/* Where to copy to? */
	if (mb_tailroom(mb) > 0) {
		/* We have some space in mb head. Superb! */
		if (copy > mb_tailroom(mb))
			copy = mb_tailroom(mb);
		if ((err = mb_add_data(mb, from, copy)) != 0)
			return SDP_ERR_FAULT;
	} else {
		int merge = 0;
		int i = mb_shinfo(mb)->nr_frags;
		struct page *page = TCP_PAGE(sk);
		int off = TCP_OFF(sk);

		if (mb_can_coalesce(mb, i, page, off) &&
		    off != PAGE_SIZE) {
			/* We can extend the last page
			 * fragment. */
			merge = 1;
		} else if (i == ssk->send_frags) {
			/* Need to add new fragment and cannot
			 * do this because all the page slots are
			 * busy. */
			sdp_mark_push(ssk, mb);
			return SDP_NEW_SEG;
		} else if (page) {
			if (off == PAGE_SIZE) {
				put_page(page);
				TCP_PAGE(sk) = page = NULL;
				off = 0;
			}
		} else
			off = 0;

		if (copy > PAGE_SIZE - off)
			copy = PAGE_SIZE - off;

		if (!sk_wmem_schedule(sk, copy))
			return SDP_DO_WAIT_MEM;

		if (!page) {
			/* Allocate new cache page. */
			if (!(page = sk_stream_alloc_page(sk)))
				return SDP_DO_WAIT_MEM;
		}

		/* Time to copy data. We are close to
		 * the end! */
		SDPSTATS_COUNTER_ADD(memcpy_count, copy);
		err = mb_copy_to_page(sk, from, mb, page,
				       off, copy);
		if (err) {
			/* If this page was new, give it to the
			 * socket so it does not get leaked.
			 */
			if (!TCP_PAGE(sk)) {
				TCP_PAGE(sk) = page;
				TCP_OFF(sk) = 0;
			}
			return SDP_ERR_ERROR;
		}

		/* Update the mb. */
		if (merge) {
			mb_shinfo(mb)->frags[i - 1].size +=
							copy;
		} else {
			mb_fill_page_desc(mb, i, page, off, copy);
			if (TCP_PAGE(sk)) {
				get_page(page);
			} else if (off + copy < PAGE_SIZE) {
				get_page(page);
				TCP_PAGE(sk) = page;
			}
		}

		TCP_OFF(sk) = off + copy;
	}

	return copy;
}

static inline int sdp_bzcopy_get(struct socket *sk, struct mbuf *mb,
				 char __user *from, int copy,
				 struct bzcopy_state *bz)
{
	int this_page, left;
	struct sdp_sock *ssk = sdp_sk(sk);

	/* Push the first chunk to page align all following - TODO: review */
	if (mb_shinfo(mb)->nr_frags == ssk->send_frags) {
		sdp_mark_push(ssk, mb);
		return SDP_NEW_SEG;
	}

	left = copy;
	BUG_ON(left > bz->left);

	while (left) {
		if (mb_shinfo(mb)->nr_frags == ssk->send_frags) {
			copy = copy - left;
			break;
		}

		this_page = PAGE_SIZE - bz->cur_offset;

		if (left <= this_page)
			this_page = left;

		if (!sk_wmem_schedule(sk, copy))
			return SDP_DO_WAIT_MEM;

		/* put_page in mb_release_data() (called by m_freem) */
		get_page(bz->pages[bz->cur_page]);
		mb_fill_page_desc(mb, mb_shinfo(mb)->nr_frags,
				   bz->pages[bz->cur_page], bz->cur_offset,
				   this_page);

		BUG_ON(mb_shinfo(mb)->nr_frags >= MAX_SKB_FRAGS);
		BUG_ON(bz->cur_offset > PAGE_SIZE);

		bz->cur_offset += this_page;
		if (bz->cur_offset == PAGE_SIZE) {
			bz->cur_offset = 0;
			bz->cur_page++;

			BUG_ON(bz->cur_page > bz->page_cnt);
		}

		left -= this_page;

		mb->len             += this_page;
		mb->data_len        += this_page;
		mb->truesize        += this_page;
		sk->sk_wmem_queued   += this_page;
		sk->sk_forward_alloc -= this_page;
	}

	bz->left -= copy;
	bz->busy++;
	return copy;
}

/* like sk_stream_wait_memory - except:
 * - if credits_needed provided - wait for enough credits
 * - TX irq will use this (in sendmsg context) to do the actual tx
 *   comp poll and post
 */
int sdp_tx_wait_memory(struct sdp_sock *ssk, long *timeo_p, int *credits_needed)
{
	struct socket *sk = &ssk->isk.sk;
	int err = 0;
	long vm_wait = 0;
	long current_timeo = *timeo_p;
	DEFINE_WAIT(wait);

	if (sk_stream_memory_free(sk))
		current_timeo = vm_wait = (net_random() % (HZ / 5)) + 2;

	while (1) {
		set_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags);

		prepare_to_wait(sk->sk_sleep, &wait, TASK_INTERRUPTIBLE);

		if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
			goto do_error;
		if (!*timeo_p)
			goto do_nonblock;
		if (signal_pending(current))
			goto do_interrupted;
		clear_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags);

		sdp_do_posts(ssk);

		if (credits_needed) {
			if (tx_slots_free(ssk) >= *credits_needed)
				break;
		} else {
			if (sk_stream_memory_free(sk) && !vm_wait)
				break;
		}

		posts_handler_put(ssk);

		set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
		sk->sk_write_pending++;

		sdp_prf1(sk, NULL, "Going to sleep");

		if (tx_credits(ssk) > SDP_MIN_TX_CREDITS)
			sdp_arm_tx_cq(sk);

		if (credits_needed) {
			sk_wait_event(sk, &current_timeo,
					!sk->sk_err && 
					!(sk->sk_shutdown & SEND_SHUTDOWN) &&
					!ssk->tx_compl_pending &&
					tx_slots_free(ssk) >= *credits_needed &&
					vm_wait);
		} else {
			sk_wait_event(sk, &current_timeo,
					!sk->sk_err && 
					!(sk->sk_shutdown & SEND_SHUTDOWN) &&
					!ssk->tx_compl_pending &&
					sk_stream_memory_free(sk) &&
					vm_wait);
		}

		sdp_prf1(sk, NULL, "Woke up");
		sk->sk_write_pending--;

		posts_handler_get(ssk);

		if (!ssk->qp_active)
			goto do_error;

		if (vm_wait) {
			vm_wait -= current_timeo;
			current_timeo = *timeo_p;
			if (current_timeo != MAX_SCHEDULE_TIMEOUT &&
			    (current_timeo -= vm_wait) < 0)
				current_timeo = 0;
			vm_wait = 0;
		}
		*timeo_p = current_timeo;
	}
out:
	finish_wait(sk->sk_sleep, &wait);
	return err;

do_error:
	err = -EPIPE;
	goto out;
do_nonblock:
	err = -EAGAIN;
	goto out;
do_interrupted:
	err = sock_intr_errno(*timeo_p);
	goto out;
}

/* Like tcp_sendmsg */
/* TODO: check locking */
static int sdp_sendmsg(struct kiocb *iocb, struct socket *sk, struct msghdr *msg,
		size_t size)
{
	int i;
	struct sdp_sock *ssk = sdp_sk(sk);
	struct mbuf *mb;
	int iovlen, flags;
	int size_goal;
	int err, copied;
	long timeo;
	struct bzcopy_state *bz = NULL;
	int zcopy_thresh = -1 != ssk->zcopy_thresh ? ssk->zcopy_thresh : sdp_zcopy_thresh;
	SDPSTATS_COUNTER_INC(sendmsg);

	lock_sock(sk);
	sdp_dbg_data(sk, "%s size = 0x%lx\n", __func__, size);

	posts_handler_get(ssk);

	flags = msg->msg_flags;
	timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT);

	/* Wait for a connection to finish. */
	if ((1 << sk->sk_state) & ~(TCPF_ESTABLISHED | TCPF_CLOSE_WAIT))
		if ((err = sk_stream_wait_connect(sk, &timeo)) != 0)
			goto out_err;

	/* This should be in poll */
	clear_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags);

	size_goal = ssk->xmit_size_goal;

	/* Ok commence sending. */
	iovlen = msg->msg_iovlen;
	copied = 0;

	err = -EPIPE;
	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		goto do_error;

	for (i = 0; i < msg->msg_iovlen; i++) {	
		struct iovec *iov = &msg->msg_iov[i];
		int seglen = iov->iov_len;
		char __user *from = iov->iov_base;

		sdp_dbg_data(sk, "Sending iov: %d/%ld %p\n", i, msg->msg_iovlen, from);

		SDPSTATS_HIST(sendmsg_seglen, seglen);

		if (zcopy_thresh && seglen > zcopy_thresh &&
				seglen > SDP_MIN_ZCOPY_THRESH && tx_slots_free(ssk)) {
			int zcopied = 0;

			zcopied = sdp_sendmsg_zcopy(iocb, sk, iov);

			if (zcopied < 0) {
				sdp_dbg_data(sk, "ZCopy send err: %d\n", zcopied);
				err = zcopied;
				goto out_err;
			}
			
			copied += zcopied;
			seglen = iov->iov_len;
			from = iov->iov_base;

			sdp_dbg_data(sk, "ZCopied: 0x%x/0x%x\n", zcopied, seglen);
		}

		/* Limiting the size_goal is reqired when using 64K pages*/
		if (size_goal > SDP_MAX_PAYLOAD)
			size_goal = SDP_MAX_PAYLOAD;

		if (bz)
			sdp_bz_cleanup(bz);
		bz = sdp_bz_setup(ssk, from, seglen, size_goal);
		if (IS_ERR(bz)) {
			bz = NULL;
			err = PTR_ERR(bz);
			goto do_error;
		}

		while (seglen > 0) {
			int copy;

			mb = sk->sk_write_queue.prev;

			if (!sk->sk_send_head ||
			    (copy = size_goal - (mb->len - sizeof(struct sdp_bsdh))) <= 0 ||
			    bz != BZCOPY_STATE(mb)) {
new_segment:
				/*
				 * Allocate a new segment
				 *   For bcopy, we stop sending once we have
				 * SO_SENDBUF bytes in flight.  For bzcopy
				 * we stop sending once we run out of remote
				 * receive credits.
				 */
				if (bz) {
					if (tx_slots_free(ssk) < bz->busy)
						goto wait_for_sndbuf;
				} else {
					if (!sk_stream_memory_free(sk))
						goto wait_for_sndbuf;
				}

				mb = sdp_alloc_mb_data(sk, 0);
				if (!mb)
					goto wait_for_memory;

				BZCOPY_STATE(mb) = bz;

				/*
				 * Check whether we can use HW checksum.
				 */
				if (sk->sk_route_caps &
				    (NETIF_F_IP_CSUM | NETIF_F_NO_CSUM |
				     NETIF_F_HW_CSUM))
					mb->ip_summed = CHECKSUM_PARTIAL;

				mb_entail(sk, ssk, mb);
				copy = size_goal;

				sdp_dbg_data(sk, "created new mb: %p"
					" len = 0x%lx, sk_send_head: %p "
					"copy: 0x%x size_goal: 0x%x\n",
					mb, mb->len - sizeof(struct sdp_bsdh),
					sk->sk_send_head, copy, size_goal);


			} else {
				sdp_dbg_data(sk, "adding to existing mb: %p"
					" len = 0x%lx, sk_send_head: %p "
					"copy: 0x%x\n",
					mb, mb->len - sizeof(struct sdp_bsdh),
				       	sk->sk_send_head, copy);
			}

			/* Try to append data to the end of mb. */
			if (copy > seglen)
				copy = seglen;

			/* OOB data byte should be the last byte of
			   the data payload */
			if (unlikely(SDP_SKB_CB(mb)->flags & TCPCB_FLAG_URG) &&
			    !(flags & MSG_OOB)) {
				sdp_mark_push(ssk, mb);
				goto new_segment;
			}

			copy = (bz) ? sdp_bzcopy_get(sk, mb, from, copy, bz) :
				      sdp_bcopy_get(sk, mb, from, copy);
			if (unlikely(copy < 0)) {
				if (!++copy)
					goto wait_for_memory;
				if (!++copy)
					goto new_segment;
				if (!++copy)
					goto do_fault;
				goto do_error;
			}

			if (!copied)
				SDP_SKB_CB(mb)->flags &= ~TCPCB_FLAG_PSH;

			ssk->write_seq += copy;
			SDP_SKB_CB(mb)->end_seq += copy;
			/*unused: mb_shinfo(mb)->gso_segs = 0;*/

			from += copy;
			copied += copy;
			seglen -= copy;
			if (seglen == 0 && iovlen == 0)
				goto out;

			continue;

wait_for_sndbuf:
			set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
wait_for_memory:
			sdp_prf(sk, mb, "wait for mem");
			SDPSTATS_COUNTER_INC(send_wait_for_mem);
			if (copied)
				sdp_push(sk, ssk, flags & ~MSG_MORE);

			err = sdp_tx_wait_memory(ssk, &timeo,
					bz ? &bz->busy : NULL);
			if (err)
				goto do_error;

			size_goal = ssk->xmit_size_goal;
		}
	}

out:
	if (copied) {
		sdp_push(sk, ssk, flags);

		if (bz)
			bz = sdp_bz_cleanup(bz);
	}

	sdp_auto_moderation(ssk);

	posts_handler_put(ssk);

	release_sock(sk);

	sdp_dbg_data(sk, "copied: 0x%x\n", copied);	
	return copied;

do_fault:
	sdp_prf(sk, mb, "prepare fault");

	if (mb->len <= sizeof(struct sdp_bsdh)) {
		if (sk->sk_send_head == mb)
			sk->sk_send_head = NULL;
		__mb_unlink(mb, &sk->sk_write_queue);
		sk_wmem_free_mb(sk, mb);
	}

do_error:
	if (copied)
		goto out;
out_err:
	if (bz)
		bz = sdp_bz_cleanup(bz);
	err = sk_stream_error(sk, flags, err);

	posts_handler_put(ssk);

	release_sock(sk);

	return err;
}

static inline int sdp_abort_rx_srcavail(struct socket *sk, struct mbuf *mb)
{
	struct sdp_bsdh *h = (struct sdp_bsdh *)mb_transport_header(mb);

	sdp_dbg_data(sk, "SrcAvail aborted\n");

	h->mid = SDP_MID_DATA;
	kfree(RX_SRCAVAIL_STATE(mb));
	RX_SRCAVAIL_STATE(mb) = NULL;

	return 0;
}

/* Like tcp_recvmsg */
/* Maybe use mb_recv_datagram here? */
/* Note this does not seem to handle vectored messages. Relevant? */
static int sdp_recvmsg(struct kiocb *iocb, struct socket *sk, struct msghdr *msg,
		       size_t len, int noblock, int flags,
		       int *addr_len)
{
	struct mbuf *mb = NULL;
	struct sdp_sock *ssk = sdp_sk(sk);
	long timeo;
	int target;
	unsigned long used;
	int err;
	u32 peek_seq;
	u32 *seq;
	int copied = 0;
	int rc;
	int avail_bytes_count = 0;  	/* Could be inlined in mb */
					/* or advertised for RDMA  */

	lock_sock(sk);
	sdp_dbg_data(sk, "iovlen: %ld iov_len: 0x%lx flags: 0x%x peek: 0x%x\n",
			msg->msg_iovlen, msg->msg_iov[0].iov_len, flags,
			MSG_PEEK);

	posts_handler_get(ssk);

	sdp_prf(sk, mb, "Read from user");

	err = -ENOTCONN;
	if (sk->sk_state == TCPS_LISTEN)
		goto out;

	timeo = sock_rcvtimeo(sk, noblock);
	/* Urgent data needs to be handled specially. */
	if (flags & MSG_OOB)
		goto recv_urg;

	seq = &ssk->copied_seq;
	if (flags & MSG_PEEK) {
		peek_seq = ssk->copied_seq;
		seq = &peek_seq;
	}

	target = sock_rcvlowat(sk, flags & MSG_WAITALL, len);

	do {
		struct rx_srcavail_state *rx_sa = NULL;
		u32 offset;

		/* Are we at urgent data? Stop if we have read anything or have
		 * SIGURG pending. */
		if (ssk->urg_data && ssk->urg_seq == *seq) {
			if (copied)
				break;
			if (signal_pending(current)) {
				copied = timeo ? sock_intr_errno(timeo) :
					-EAGAIN;
				break;
			}
		}

		mb = mb_peek(&sk->sk_receive_queue);
		do {
			struct sdp_bsdh *h;
			if (!mb)
				break;

			avail_bytes_count = 0;

			h = (struct sdp_bsdh *)mb_transport_header(mb);

			switch (h->mid) {
			case SDP_MID_DISCONN:
				sdp_dbg(sk, "Handle RX SDP_MID_DISCONN\n");
				sdp_prf(sk, NULL, "Handle RX SDP_MID_DISCONN");
				sdp_handle_disconn(sk);
				goto found_fin_ok;

			case SDP_MID_SRCAVAIL:
				rx_sa = RX_SRCAVAIL_STATE(mb);

				if (rx_sa->mseq < ssk->srcavail_cancel_mseq) {
					sdp_dbg_data(sk, "Ignoring src avail "
							"due to SrcAvailCancel\n");
					sdp_post_sendsm(sk);
					if (rx_sa->used < mb->len) {
						sdp_abort_rx_srcavail(sk, mb);
						sdp_prf(sk, mb, "Converted SA to DATA");
						goto sdp_mid_data;
					} else {
						sdp_prf(sk, mb, "Cancelled SA with no payload left");
						goto mb_cleanup;
					}
				}

				/* if has payload - handle as if MID_DATA */
				if (rx_sa->used < mb->len) {
					sdp_dbg_data(sk, "SrcAvail has "
							"payload: %d/%d\n",
							 rx_sa->used,
						mb->len);
					avail_bytes_count = mb->len;
				} else {
					sdp_dbg_data(sk, "Finished payload. "
						"RDMAing: %d/%d\n",
						rx_sa->used, rx_sa->len);

					if (flags & MSG_PEEK) {
						sdp_dbg_data(sk, "Peek on RDMA data - "
								"fallback to BCopy\n");
						sdp_abort_rx_srcavail(sk, mb);
						sdp_post_sendsm(sk);
						rx_sa = NULL;
					} else {
						avail_bytes_count = rx_sa->len;
					}
				}

				break;

			case SDP_MID_DATA:
sdp_mid_data:				
				rx_sa = NULL;
				avail_bytes_count = mb->len;
				break;
			default:
				break;
			}

			if (before(*seq, SDP_SKB_CB(mb)->seq)) {
				sdp_warn(sk, "mb: %p recvmsg bug: copied %X seq %X\n",
					mb, *seq, SDP_SKB_CB(mb)->seq);
				sdp_reset(sk);
				break;
			}

			offset = *seq - SDP_SKB_CB(mb)->seq;
			if (offset < avail_bytes_count)
				goto found_ok_mb;

 			if (unlikely(!(flags & MSG_PEEK))) {
				 	sdp_warn(sk, "Trying to read beyond SKB\n");
				 	sdp_prf(sk, mb, "Aborting recv");
					goto mb_cleanup;
			}

			WARN_ON(h->mid == SDP_MID_SRCAVAIL);

			mb = mb->next;
		} while (mb != (struct mbuf *)&sk->sk_receive_queue);

		if (copied >= target)
			break;

		if (copied) {
			if (sk->sk_err ||
			    sk->sk_state == TCPS_CLOSED ||
			    (sk->sk_shutdown & RCV_SHUTDOWN) ||
			    !timeo ||
			    signal_pending(current) ||
			    (flags & MSG_PEEK))
				break;
		} else {
			if (sock_flag(sk, SOCK_DONE))
				break;

			if (sk->sk_err) {
				copied = sock_error(sk);
				break;
			}

			if (sk->sk_shutdown & RCV_SHUTDOWN)
				break;

			if (sk->sk_state == TCPS_CLOSED) {
				if (!sock_flag(sk, SOCK_DONE)) {
					/* This occurs when user tries to read
					 * from never connected socket.
					 */
					copied = -ENOTCONN;
					break;
				}
				break;
			}

			if (!timeo) {
				copied = -EAGAIN;
				break;
			}

			if (signal_pending(current)) {
				copied = sock_intr_errno(timeo);
				break;
			}
		}

		rc = poll_recv_cq(sk);

		if (copied >= target && !recv_poll) {
			/* Do not sleep, just process backlog. */
			release_sock(sk);
			lock_sock(sk);
		} else if (rc) {
			sdp_dbg_data(sk, "sk_wait_data %ld\n", timeo);

			posts_handler_put(ssk);

			/* socket lock is released inside sk_wait_data */
			sk_wait_data(sk, &timeo);

			posts_handler_get(ssk);
			sdp_prf(sk, NULL, "got data");

			sdp_do_posts(ssk);
		}
		continue;

	found_ok_mb:
		sdp_dbg_data(sk, "bytes avail: %d\n", avail_bytes_count);
		sdp_dbg_data(sk, "buf len %Zd offset %d\n", len, offset);
		sdp_dbg_data(sk, "copied %d target %d\n", copied, target);
		used = avail_bytes_count - offset;
		if (len < used)
			used = len;

		sdp_dbg_data(sk, "%s: used %ld\n", __func__, used);

		if (ssk->urg_data) {
			u32 urg_offset = ssk->urg_seq - *seq;
			if (urg_offset < used) {
				if (!urg_offset) {
					if (!sock_flag(sk, SOCK_URGINLINE)) {
						++*seq;
						offset++;
						used--;
						if (!used)
							goto skip_copy;
					}
				} else
					used = urg_offset;
			}
		}
		if (!(flags & MSG_TRUNC)) {
			if (rx_sa && rx_sa->used >= mb->len) {
				/* No more payload - start rdma copy */
				sdp_dbg_data(sk, "RDMA copy of %lx bytes\n", used);
				err = sdp_rdma_to_iovec(sk, msg->msg_iov, mb, &used);
				sdp_dbg_data(sk, "used = %lx bytes\n", used);
				if (err == -EAGAIN) {
					sdp_dbg_data(sk, "RDMA Read aborted\n");
					goto mb_cleanup;
				}
			} else {
				sdp_dbg_data(sk, "memcpy 0x%lx bytes +0x%x -> %p\n",
						used, offset, msg->msg_iov[0].iov_base);

				err = mb_copy_datagram_iovec(mb, offset,
						/* TODO: skip header? */
						msg->msg_iov, used);
				if (rx_sa && !(flags & MSG_PEEK)) {
					rx_sa->used += used;
					rx_sa->reported += used;
				}
			}
			if (err) {
				sdp_dbg(sk, "%s: mb_copy_datagram_iovec failed"
					"offset %d size %ld status %d\n",
					__func__, offset, used, err);
				/* Exception. Bailout! */
				if (!copied)
					copied = -EFAULT;
				break;
			}
		}

		copied += used;
		len -= used;
		*seq += used;

		sdp_dbg_data(sk, "done copied %d target %d\n", copied, target);

		sdp_do_posts(sdp_sk(sk));
skip_copy:
		if (ssk->urg_data && after(ssk->copied_seq, ssk->urg_seq))
			ssk->urg_data = 0;


		if (rx_sa) {
			rc = sdp_post_rdma_rd_compl(ssk, rx_sa);
			BUG_ON(rc);

		}

		if (!rx_sa && used + offset < mb->len)
			continue;

		if (rx_sa && rx_sa->used < rx_sa->len)
			continue;

		offset = 0;

mb_cleanup:
		if (!(flags & MSG_PEEK)) {
			struct sdp_bsdh *h;
			h = (struct sdp_bsdh *)mb_transport_header(mb);
			sdp_prf1(sk, mb, "READ finished. mseq: %d mseq_ack:%d",
				ntohl(h->mseq), ntohl(h->mseq_ack));

			if (rx_sa) {
				if (!rx_sa->flags) /* else ssk->rx_sa might
						      point to another rx_sa */
					ssk->rx_sa = NULL;

				kfree(rx_sa);
				rx_sa = NULL;
				
			}

			sdp_dbg_data(sk, "unlinking mb %p\n", mb);
			mb_unlink(mb, &sk->sk_receive_queue);
			m_freem(mb);
		}
		continue;
found_fin_ok:
		++*seq;
		if (!(flags & MSG_PEEK)) {
			mb_unlink(mb, &sk->sk_receive_queue);
			m_freem(mb);
		}
		break;

	} while (len > 0);

	err = copied;
out:

	posts_handler_put(ssk);

	sdp_auto_moderation(ssk);

	release_sock(sk);
	sdp_dbg_data(sk, "recvmsg finished. ret = %d\n", err);	
	return err;

recv_urg:
	err = sdp_recv_urg(sk, timeo, msg, len, flags, addr_len);
	goto out;
}

static int sdp_listen(struct socket *sk, int backlog)
{
	struct sdp_sock *ssk = sdp_sk(sk);
	int rc;

	sdp_dbg(sk, "%s\n", __func__);

	if (!ssk->id) {
		rc = sdp_get_port(sk, 0);
		if (rc)
			return rc;
		inet_sk(sk)->sport = htons(inet_sk(sk)->num);
	}

	rc = rdma_listen(ssk->id, backlog);
	if (rc) {
		sdp_warn(sk, "rdma_listen failed: %d\n", rc);
		sdp_set_error(sk, rc);
	} else
		sdp_exch_state(sk, TCPF_CLOSE, TCPS_LISTEN);
	return rc;
}

/* We almost could use inet_listen, but that calls
   inet_csk_listen_start. Longer term we'll want to add
   a listen callback to struct proto, similiar to bind. */
static int sdp_inet_listen(struct socketet *sock, int backlog)
{
	struct socket *sk = sock->sk;
	unsigned char old_state;
	int err;

	lock_sock(sk);

	err = -EINVAL;
	if (sock->state != SS_UNCONNECTED)
		goto out;

	old_state = sk->sk_state;
	if (!((1 << old_state) & (TCPF_CLOSE | TCPF_LISTEN)))
		goto out;

	/* Really, if the socket is already in listen state
	 * we can only allow the backlog to be adjusted.
	 */
	if (old_state != TCPS_LISTEN) {
		err = sdp_listen(sk, backlog);
		if (err)
			goto out;
	}
	sk->sk_max_ack_backlog = backlog;
	err = 0;

out:
	release_sock(sk);
	return err;
}

static void sdp_unhash(struct socket *sk)
{
        sdp_dbg(sk, "%s\n", __func__);
}

static inline unsigned int sdp_listen_poll(const struct socket *sk)
{
	        return !list_empty(&sdp_sk(sk)->accept_queue) ?
			(POLLIN | POLLRDNORM) : 0;
}

static unsigned int sdp_poll(struct file *file, struct socketet *socket,
			     struct poll_table_struct *wait)
{
       unsigned int     mask;
       struct socket     *sk  = socket->sk;
       struct sdp_sock *ssk = sdp_sk(sk);

	sdp_dbg_data(socket->sk, "%s\n", __func__);

	mask = datagram_poll(file, socket, wait);

       /*
        * Adjust for memory in later kernels
        */
	if (!sk_stream_memory_free(sk))
		mask &= ~(POLLOUT | POLLWRNORM | POLLWRBAND);

	/* TODO: Slightly ugly: it would be nicer if there was function
	 * like datagram_poll that didn't include poll_wait,
	 * then we could reverse the order. */
       if (sk->sk_state == TCPS_LISTEN)
               return sdp_listen_poll(sk);

       if (ssk->urg_data & TCP_URG_VALID)
		mask |= POLLPRI;
	return mask;
}

static void sdp_enter_memory_pressure(struct socket *sk)
{
	sdp_dbg(sk, "%s\n", __func__);
}

void sdp_urg(struct sdp_sock *ssk, struct mbuf *mb)
{
	struct socket *sk = &ssk->isk.sk;
	u8 tmp;
	u32 ptr = mb->len - 1;

	ssk->urg_seq = SDP_SKB_CB(mb)->seq + ptr;

	if (mb_copy_bits(mb, ptr, &tmp, 1))
		BUG();
	ssk->urg_data = TCP_URG_VALID | tmp;
	if (!sock_flag(sk, SOCK_DEAD))
		sk->sk_data_ready(sk, 0);
}

static struct percpu_counter *sockets_allocated;
static atomic_t memory_allocated;
static struct percpu_counter *orphan_count;
static int memory_pressure;
struct proto sdp_proto = {
        .close       = sdp_close,
        .connect     = sdp_connect,
        .disconnect  = sdp_disconnect,
        .accept      = sdp_accept,
        .ioctl       = sdp_ioctl,
        .init        = sdp_init_sock,
        .shutdown    = sdp_shutdown,
        .setsockopt  = sdp_setsockopt,
        .getsockopt  = sdp_getsockopt,
        .sendmsg     = sdp_sendmsg,
        .recvmsg     = sdp_recvmsg,
	.unhash      = sdp_unhash,
        .get_port    = sdp_get_port,
	/* Wish we had this: .listen   = sdp_listen */
	.enter_memory_pressure = sdp_enter_memory_pressure,
	.memory_allocated = &memory_allocated,
	.memory_pressure = &memory_pressure,
        .sysctl_mem             = sysctl_tcp_mem,
        .sysctl_wmem            = sysctl_tcp_wmem,
        .sysctl_rmem            = sysctl_tcp_rmem,
	.max_header  = sizeof(struct sdp_bsdh),
        .obj_size    = sizeof(struct sdp_sock),
	.owner	     = THIS_MODULE,
	.name	     = "SDP",
};

static struct proto_ops sdp_proto_ops = {
	.family     = PF_INET,
	.owner      = THIS_MODULE,
	.release    = inet_release,
	.bind       = inet_bind,
	.connect    = inet_stream_connect, /* TODO: inet_datagram connect would
					      autobind, but need to fix get_port
					      with port 0 first. */
	.socketpair = sock_no_socketpair,
	.accept     = inet_accept,
	.getname    = inet_getname,
	.poll       = sdp_poll,
	.ioctl      = inet_ioctl,
	.listen     = sdp_inet_listen,
	.shutdown   = inet_shutdown,
	.setsockopt = sock_common_setsockopt,
	.getsockopt = sock_common_getsockopt,
	.sendmsg    = inet_sendmsg,
	.recvmsg    = sock_common_recvmsg,
	.mmap       = sock_no_mmap,
	.sendpage   = sock_no_sendpage,
};

static int sdp_create_socket(struct net *net, struct socketet *sock, int protocol)
{
	struct socket *sk;
	int rc;

	sdp_dbg(NULL, "type %d protocol %d\n", sock->type, protocol);

	if (net != &init_net)
		return -EAFNOSUPPORT;

	if (sock->type != SOCK_STREAM) {
		sdp_warn(NULL, "SDP: unsupported type %d.\n", sock->type);
		return -ESOCKTNOSUPPORT;
	}

	/* IPPROTO_IP is a wildcard match */
	if (protocol != IPPROTO_TCP && protocol != IPPROTO_IP) {
		sdp_warn(NULL, "SDP: unsupported protocol %d.\n", protocol);
		return -EPROTONOSUPPORT;
	}

	sk = sk_alloc(net, PF_INET_SDP, GFP_KERNEL, &sdp_proto);
	if (!sk) {
		sdp_warn(NULL, "SDP: failed to allocate socket.\n");
		return -ENOMEM;
	}
	sock_init_data(sock, sk);
	sk->sk_protocol = 0x0 /* TODO: inherit tcp socket to use IPPROTO_TCP */;

	memset((struct inet_sock *)sk + 1, 0,
			sizeof(struct sdp_sock) - sizeof(struct inet_sock));
	rc = sdp_init_sock(sk);
	if (rc) {
		sdp_warn(sk, "SDP: failed to init sock.\n");
		sk_common_release(sk);
		return -ENOMEM;
	}

	sk->sk_destruct = sdp_destruct;

	sdp_init_keepalive_timer(sk);

	sock->ops = &sdp_proto_ops;
	sock->state = SS_UNCONNECTED;

	sdp_add_sock(sdp_sk(sk));

	return 0;
}

static void sdp_add_device(struct ib_device *device)
{
	struct sdp_device *sdp_dev;
	struct ib_fmr_pool_param fmr_param;

	sdp_dev = kmalloc(sizeof *sdp_dev, GFP_KERNEL);
	if (!sdp_dev)
		return;

	sdp_dev->pd = ib_alloc_pd(device);
	if (IS_ERR(sdp_dev->pd)) {
		printk(KERN_WARNING "Unable to allocate PD: %ld.\n",
				PTR_ERR(sdp_dev->pd));
		goto err_pd_alloc;
	}

        sdp_dev->mr = ib_get_dma_mr(sdp_dev->pd, IB_ACCESS_LOCAL_WRITE);
        if (IS_ERR(sdp_dev->mr)) {
		printk(KERN_WARNING "Unable to get dma MR: %ld.\n",
				PTR_ERR(sdp_dev->mr));
                goto err_mr;
        }

	memset(&fmr_param, 0, sizeof fmr_param);
	fmr_param.pool_size	    = SDP_FMR_POOL_SIZE;
	fmr_param.dirty_watermark   = SDP_FMR_DIRTY_SIZE;
	fmr_param.cache		    = 1;
	fmr_param.max_pages_per_fmr = SDP_FMR_SIZE;
	fmr_param.page_shift	    = PAGE_SHIFT;
	fmr_param.access	    = (IB_ACCESS_LOCAL_WRITE |
				       IB_ACCESS_REMOTE_READ);

	sdp_dev->fmr_pool = ib_create_fmr_pool(sdp_dev->pd, &fmr_param);
	if (IS_ERR(sdp_dev->fmr_pool)) {
		printk(KERN_WARNING "Error creating fmr pool\n");
		sdp_dev->fmr_pool = NULL;
		goto err_fmr_create;
	}

	ib_set_client_data(device, &sdp_client, sdp_dev);

	return;

err_fmr_create:
	ib_dereg_mr(sdp_dev->mr);
err_mr:
	ib_dealloc_pd(sdp_dev->pd);
err_pd_alloc:
	kfree(sdp_dev);
}

static void sdp_remove_device(struct ib_device *device)
{
	struct list_head  *p;
	struct sdp_sock   *ssk;
	struct socket       *sk;
	struct rdma_cm_id *id;
	struct sdp_device *sdp_dev;

do_next:
	write_lock(&device_removal_lock);

	spin_lock_irq(&sock_list_lock);
	list_for_each(p, &sock_list) {
		ssk = list_entry(p, struct sdp_sock, sock_list);
		if (ssk->ib_device == device) {
			sk = &ssk->isk.sk;
			lock_sock(sk);
			/* ssk->id must be lock-protected,
			 * to enable mutex with sdp_close() */
			id = ssk->id;

			if (id) {
				ssk->id = NULL;
				release_sock(sk);
				spin_unlock_irq(&sock_list_lock);
				write_unlock(&device_removal_lock);
				rdma_destroy_id(id);

				goto do_next;
			}
			release_sock(sk);
		}
	}

kill_socks:
	list_for_each(p, &sock_list) {
		ssk = list_entry(p, struct sdp_sock, sock_list);

		if (ssk->ib_device == device && !ssk->destruct_in_process) {
			ssk->destruct_in_process = 1;
			sk = &ssk->isk.sk;

			sdp_cancel_dreq_wait_timeout(ssk);
			cancel_delayed_work(&ssk->srcavail_cancel_work);

			spin_unlock_irq(&sock_list_lock);

			sk->sk_shutdown |= RCV_SHUTDOWN;
			sdp_reset(sk);
			sock_hold(sk, SOCK_REF_ALIVE);
			sdp_close(sk,0);
			sdp_destroy_qp(ssk);
			ssk->sdp_dev = NULL;

			if ((1 << sk->sk_state) &
				(TCPF_FIN_WAIT1 | TCPF_CLOSE_WAIT |
				 TCPF_LAST_ACK | TCPF_TIME_WAIT)) {
				sock_put(sk, SOCK_REF_CMA);
			}
			sched_yield();

			spin_lock_irq(&sock_list_lock);

			goto kill_socks;
		}
	}

	spin_unlock_irq(&sock_list_lock);

	write_unlock(&device_removal_lock);

	sdp_dev = ib_get_client_data(device, &sdp_client);
	if (!sdp_dev)
		return;

	ib_flush_fmr_pool(sdp_dev->fmr_pool);
	ib_destroy_fmr_pool(sdp_dev->fmr_pool);

	ib_dereg_mr(sdp_dev->mr);

	ib_dealloc_pd(sdp_dev->pd);

	kfree(sdp_dev);
}

static struct net_proto_family sdp_net_proto = {
	.family = AF_INET_SDP,
	.create = sdp_create_socket,
	.owner  = THIS_MODULE,
};

struct ib_client sdp_client = {
	.name   = "sdp",
	.add    = sdp_add_device,
	.remove = sdp_remove_device
};

static int __init sdp_init(void)
{
	int rc = -ENOMEM;

	INIT_LIST_HEAD(&sock_list);
	spin_lock_init(&sock_list_lock);
	spin_lock_init(&sdp_large_sockets_lock);

	sockets_allocated = kzalloc(sizeof(*sockets_allocated), GFP_KERNEL);
	if (!sockets_allocated)
		goto no_mem_sockets_allocated;

	orphan_count = kzalloc(sizeof(*orphan_count), GFP_KERNEL);
	if (!orphan_count)
		goto no_mem_orphan_count;

	percpu_counter_init(sockets_allocated, 0);
	percpu_counter_init(orphan_count, 0);

	sdp_proto.sockets_allocated = sockets_allocated;
	sdp_proto.orphan_count = orphan_count;

	rx_comp_wq = create_singlethread_workqueue("rx_comp_wq");
	if (!rx_comp_wq)
		goto no_mem_rx_wq;

	sdp_wq = create_singlethread_workqueue("sdp_wq");
	if (!sdp_wq)
		goto no_mem_sdp_wq;

	rc = proto_register(&sdp_proto, 1);
	if (rc) {
		printk(KERN_WARNING "proto_register failed: %d\n", rc);
		goto error_proto_reg;
	}

	rc = sock_register(&sdp_net_proto);
	if (rc) {
		printk(KERN_WARNING "sock_register failed: %d\n", rc);
		goto error_sock_reg;
	}

	sdp_proc_init();

	atomic_set(&sdp_current_mem_usage, 0);

	ib_register_client(&sdp_client);

	return 0;

error_sock_reg:
	proto_unregister(&sdp_proto);
error_proto_reg:
	destroy_workqueue(sdp_wq);
no_mem_sdp_wq:
	destroy_workqueue(rx_comp_wq);
no_mem_rx_wq:
	kfree(orphan_count);
no_mem_orphan_count:
	kfree(sockets_allocated);
no_mem_sockets_allocated:
	return rc;
}

static void __exit sdp_exit(void)
{
	sock_unregister(PF_INET_SDP);
	proto_unregister(&sdp_proto);

	if (percpu_counter_read_positive(orphan_count))
		printk(KERN_WARNING "%s: orphan_count %lld\n", __func__,
		       percpu_counter_read_positive(orphan_count));

	destroy_workqueue(rx_comp_wq);
	destroy_workqueue(sdp_wq);

	flush_scheduled_work();

	BUG_ON(!list_empty(&sock_list));

	if (atomic_read(&sdp_current_mem_usage))
		printk(KERN_WARNING "%s: current mem usage %d\n", __func__,
		       atomic_read(&sdp_current_mem_usage));

	sdp_proc_unregister();

	ib_unregister_client(&sdp_client);

	percpu_counter_destroy(sockets_allocated);
	percpu_counter_destroy(orphan_count);

	kfree(orphan_count);
	kfree(sockets_allocated);
}

module_init(sdp_init);
module_exit(sdp_exit);
