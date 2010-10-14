/*
 * Copyright (c) 2004, 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2004, 2005 Voltaire, Inc. All rights reserved.
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

#include "ipoib.h"

#include <rdma/ib_cache.h>

#include <security/mac/mac_framework.h>

#include <linux/delay.h>
#include <linux/dma-mapping.h>

#ifdef CONFIG_INFINIBAND_IPOIB_DEBUG_DATA
static int data_debug_level;

module_param(data_debug_level, int, 0644);
MODULE_PARM_DESC(data_debug_level,
		 "Enable data path debug tracing if > 0");
#endif

static DEFINE_MUTEX(pkey_mutex);

struct ipoib_ah *ipoib_create_ah(struct ifnet *dev,
				 struct ib_pd *pd, struct ib_ah_attr *attr)
{
	struct ipoib_ah *ah;

	ah = kmalloc(sizeof *ah, GFP_KERNEL);
	if (!ah)
		return NULL;

	ah->dev       = dev;
	ah->last_send = 0;
	kref_init(&ah->ref);

	ah->ah = ib_create_ah(pd, attr);
	if (IS_ERR(ah->ah)) {
		kfree(ah);
		ah = NULL;
	} else
		ipoib_dbg(dev->if_softc, "Created ah %p\n", ah->ah);

	return ah;
}

void ipoib_free_ah(struct kref *kref)
{
	struct ipoib_ah *ah = container_of(kref, struct ipoib_ah, ref);
	struct ipoib_dev_priv *priv = ah->dev->if_softc;

	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	list_add_tail(&ah->list, &priv->dead_ahs);
	spin_unlock_irqrestore(&priv->lock, flags);
}

static void ipoib_ud_dma_unmap_rx(struct ipoib_dev_priv *priv,
				  u64 mapping[IPOIB_UD_RX_SG])
{
	ib_dma_unmap_single(priv->ca, mapping[0],
			    IPOIB_UD_BUF_SIZE(priv->max_ib_mtu),
			    DMA_FROM_DEVICE);
}

static void ipoib_ud_mb_put_frags(struct ipoib_dev_priv *priv,
				   struct mbuf *mb,
				   unsigned int length)
{

	mb->m_pkthdr.len = length;
	mb->m_len = length;
}

static int ipoib_ib_post_receive(struct ifnet *dev, int id)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ib_recv_wr *bad_wr;
	int ret;

	priv->rx_wr.wr_id   = id | IPOIB_OP_RECV;
	priv->rx_sge[0].addr = priv->rx_ring[id].mapping[0];

	ret = ib_post_recv(priv->qp, &priv->rx_wr, &bad_wr);
	if (unlikely(ret)) {
		ipoib_warn(priv, "receive failed for buf %d (%d)\n", id, ret);
		ipoib_ud_dma_unmap_rx(priv, priv->rx_ring[id].mapping);
		m_freem(priv->rx_ring[id].mb);
		priv->rx_ring[id].mb = NULL;
	}

	return ret;
}

static struct mbuf *ipoib_alloc_rx_mb(struct ifnet *dev, int id)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct mbuf *mb;
	int buf_size;
	u64 *mapping;

	/*
	 * XXX Should be calculated once and cached.
	 */
	buf_size = IPOIB_UD_BUF_SIZE(priv->max_ib_mtu);
	if (buf_size <= MCLBYTES)
		buf_size = MCLBYTES;
	else if (buf_size <= MJUMPAGESIZE)
		buf_size = MJUMPAGESIZE;
	else if (buf_size <= MJUM9BYTES)
		buf_size = MJUM9BYTES;
	else if (buf_size < MJUM16BYTES)
		buf_size = MJUM16BYTES;

	mb = m_getjcl(M_DONTWAIT, MT_DATA, M_PKTHDR, buf_size);
	if (unlikely(!mb))
		return NULL;

	mapping = priv->rx_ring[id].mapping;
	mapping[0] = ib_dma_map_single(priv->ca, mtod(mb, void *), buf_size,
				       DMA_FROM_DEVICE);
	if (unlikely(ib_dma_mapping_error(priv->ca, mapping[0])))
		goto error;

	priv->rx_ring[id].mb = mb;
	return mb;

error:
	m_freem(mb);
	return NULL;
}

static int ipoib_ib_post_receives(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	int i;

	for (i = 0; i < ipoib_recvq_size; ++i) {
		if (!ipoib_alloc_rx_mb(dev, i)) {
			ipoib_warn(priv, "failed to allocate receive buffer %d\n", i);
			return -ENOMEM;
		}
		if (ipoib_ib_post_receive(dev, i)) {
			ipoib_warn(priv, "ipoib_ib_post_receive failed for buf %d\n", i);
			return -EIO;
		}
	}

	return 0;
}

static void ipoib_ib_handle_rx_wc(struct ifnet *dev, struct ib_wc *wc)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	unsigned int wr_id = wc->wr_id & ~IPOIB_OP_RECV;
	struct ipoib_header *eh;
	struct mbuf *mb;
	u64 mapping[IPOIB_UD_RX_SG];

	ipoib_dbg_data(priv, "recv completion: id %d, status: %d\n",
		       wr_id, wc->status);

	if (unlikely(wr_id >= ipoib_recvq_size)) {
		ipoib_warn(priv, "recv completion event with wrid %d (> %d)\n",
			   wr_id, ipoib_recvq_size);
		return;
	}

	mb  = priv->rx_ring[wr_id].mb;

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		if (wc->status != IB_WC_WR_FLUSH_ERR)
			ipoib_warn(priv, "failed recv event "
				   "(status=%d, wrid=%d vend_err %x)\n",
				   wc->status, wr_id, wc->vendor_err);
		ipoib_ud_dma_unmap_rx(priv, priv->rx_ring[wr_id].mapping);
		m_freem(mb);
		priv->rx_ring[wr_id].mb = NULL;
		return;
	}

	/*
	 * Drop packets that this interface sent, ie multicast packets
	 * that the HCA has replicated.
	 */
	if (wc->slid == priv->local_lid && wc->src_qp == priv->qp->qp_num)
		goto repost;

	memcpy(mapping, priv->rx_ring[wr_id].mapping,
	       IPOIB_UD_RX_SG * sizeof *mapping);

	/*
	 * If we can't allocate a new RX buffer, dump
	 * this packet and reuse the old buffer.
	 */
	if (unlikely(!ipoib_alloc_rx_mb(dev, wr_id))) {
		dev->if_iqdrops++;
		goto repost;
	}

	ipoib_dbg_data(priv, "received %d bytes, SLID 0x%04x\n",
		       wc->byte_len, wc->slid);

	ipoib_ud_dma_unmap_rx(priv, mapping);
	ipoib_ud_mb_put_frags(priv, mb, wc->byte_len);

	++dev->if_ipackets;
	dev->if_ibytes += mb->m_pkthdr.len;
	mb->m_pkthdr.rcvif = dev;
	m_adj(mb, sizeof(struct ib_grh) - INFINIBAND_ALEN);
	eh = mtod(mb, struct ipoib_header *);
	bzero(eh->hwaddr, 4);	/* Zero the queue pair, only dgid is in grh */
/* XXX
	if (test_bit(IPOIB_FLAG_CSUM, &priv->flags) && likely(wc->csum_ok))
		mb->ip_summed = CHECKSUM_UNNECESSARY;
*/
	dev->if_input(dev, mb);

repost:
	if (unlikely(ipoib_ib_post_receive(dev, wr_id)))
		ipoib_warn(priv, "ipoib_ib_post_receive failed "
			   "for buf %d\n", wr_id);
}

static int ipoib_dma_map_tx(struct ib_device *ca,
			    struct ipoib_tx_buf *tx_req)
{
	struct mbuf *mb = tx_req->mb;
	u64 *mapping = tx_req->mapping;
	struct mbuf *m;
	int error;
	int i;

	for (m = mb, i = 0; m != NULL; m = m->m_next, i++);
	i--;
	if (i >= MAX_MB_FRAGS) {
		tx_req->mb = mb = m_defrag(mb, M_DONTWAIT);
		if (mb == NULL)
			return -EIO;
		for (m = mb, i = 0; m != NULL; m = m->m_next, i++);
		if (i >= MAX_MB_FRAGS)
			return -EIO;
	}
	error = 0;
	for (m = mb, i = 0; m != NULL; m = m->m_next, i++) {
		mapping[i] = ib_dma_map_single(ca, mtod(m, void *),
					       m->m_len, DMA_TO_DEVICE);
		if (unlikely(ib_dma_mapping_error(ca, mapping[i]))) {
			error = -EIO;
			break;
		}
	}
	if (error) {
		int end;

		end = i;
		for (m = mb, i = 0; i < end; m = m->m_next, i++)
			ib_dma_unmap_single(ca, mapping[i], m->m_len,
					    DMA_TO_DEVICE);
	}
	return error;
}

static void ipoib_dma_unmap_tx(struct ib_device *ca,
			       struct ipoib_tx_buf *tx_req)
{
	struct mbuf *mb = tx_req->mb;
	u64 *mapping = tx_req->mapping;
	struct mbuf *m;
	int i;

	for (m = mb, i = 0; m != NULL; m = m->m_next, i++)
		ib_dma_unmap_single(ca, mapping[i], m->m_len, DMA_TO_DEVICE);
}

static void ipoib_ib_handle_tx_wc(struct ifnet *dev, struct ib_wc *wc)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	unsigned int wr_id = wc->wr_id;
	struct ipoib_tx_buf *tx_req;

	ipoib_dbg_data(priv, "send completion: id %d, status: %d\n",
		       wr_id, wc->status);

	if (unlikely(wr_id >= ipoib_sendq_size)) {
		ipoib_warn(priv, "send completion event with wrid %d (> %d)\n",
			   wr_id, ipoib_sendq_size);
		return;
	}

	tx_req = &priv->tx_ring[wr_id];

	ipoib_dma_unmap_tx(priv->ca, tx_req);

	++dev->if_opackets;
	dev->if_obytes += tx_req->mb->m_pkthdr.len;

	m_freem(tx_req->mb);

	++priv->tx_tail;
	if (unlikely(--priv->tx_outstanding == ipoib_sendq_size >> 1) &&
	    (dev->if_drv_flags & IFF_DRV_OACTIVE) &&
	    test_bit(IPOIB_FLAG_ADMIN_UP, &priv->flags))
		dev->if_drv_flags &= ~IFF_DRV_OACTIVE;

	if (wc->status != IB_WC_SUCCESS &&
	    wc->status != IB_WC_WR_FLUSH_ERR)
		ipoib_warn(priv, "failed send event "
			   "(status=%d, wrid=%d vend_err %x)\n",
			   wc->status, wr_id, wc->vendor_err);
}

static int poll_tx(struct ipoib_dev_priv *priv)
{
	int n, i;

	n = ib_poll_cq(priv->send_cq, MAX_SEND_CQE, priv->send_wc);
	for (i = 0; i < n; ++i)
		ipoib_ib_handle_tx_wc(priv->dev, priv->send_wc + i);

	return n == MAX_SEND_CQE;
}

static void
ipoib_poll(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	int n, i;

poll_more:
	for (;;) {
		n = ib_poll_cq(priv->recv_cq, IPOIB_NUM_WC, priv->ibwc);

		for (i = 0; i < n; i++) {
			struct ib_wc *wc = priv->ibwc + i;

			if (wc->wr_id & IPOIB_OP_RECV) {
				if (wc->wr_id & IPOIB_OP_CM)
					ipoib_cm_handle_rx_wc(dev, wc);
				else
					ipoib_ib_handle_rx_wc(dev, wc);
			} else
				ipoib_cm_handle_tx_wc(priv->dev, wc);
		}

		if (n != IPOIB_NUM_WC)
			break;
	}

	if (ib_req_notify_cq(priv->recv_cq,
	    IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS))
		goto poll_more;
}

void ipoib_ib_completion(struct ib_cq *cq, void *dev_ptr)
{
	struct ifnet *dev = dev_ptr;
	struct ipoib_dev_priv *priv = dev->if_softc;

	spin_lock(&priv->lock);
	ipoib_poll(dev);
	spin_unlock(&priv->lock);
}

static void drain_tx_cq(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	spin_lock(&priv->lock);
	while (poll_tx(priv))
		; /* nothing */

	if (dev->if_drv_flags & IFF_DRV_OACTIVE)
		mod_timer(&priv->poll_timer, jiffies + 1);

	spin_unlock(&priv->lock);
}

void ipoib_send_comp_handler(struct ib_cq *cq, void *dev_ptr)
{
	struct ipoib_dev_priv *priv = ((struct ifnet *)dev_ptr)->if_softc;

	mod_timer(&priv->poll_timer, jiffies);
}

static inline int post_send(struct ipoib_dev_priv *priv,
			    unsigned int wr_id,
			    struct ib_ah *address, u32 qpn,
			    struct ipoib_tx_buf *tx_req,
			    void *head, int hlen)
{
	struct ib_send_wr *bad_wr;
	struct mbuf *mb = tx_req->mb;
	u64 *mapping = tx_req->mapping;
	struct mbuf *m;
	int i;

	for (m = mb, i = 0; m != NULL; m = m->m_next, i++) {
		priv->tx_sge[i].addr         = mapping[i];
		priv->tx_sge[i].length       = m->m_len;
	}
	priv->tx_wr.num_sge	     = i;
	priv->tx_wr.wr_id 	     = wr_id;
	priv->tx_wr.wr.ud.remote_qpn = qpn;
	priv->tx_wr.wr.ud.ah 	     = address;

	if (head) {
		priv->tx_wr.wr.ud.mss	 = 0; /* XXX mb_shinfo(mb)->gso_size; */
		priv->tx_wr.wr.ud.header = head;
		priv->tx_wr.wr.ud.hlen	 = hlen;
		priv->tx_wr.opcode	 = IB_WR_LSO;
	} else
		priv->tx_wr.opcode	 = IB_WR_SEND;

	return ib_post_send(priv->qp, &priv->tx_wr, &bad_wr);
}

void
ipoib_send(struct ifnet *dev, struct mbuf *mb,
    struct ipoib_ah *address, u32 qpn)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ipoib_tx_buf *tx_req;
	int hlen;
	void *phead;

	m_adj(mb, sizeof (struct ipoib_pseudoheader));
	if (0 /* XXX segment offload mb_is_gso(mb) */) {
		/* XXX hlen = mb_transport_offset(mb) + tcp_hdrlen(mb); */
		phead = mtod(mb, void *);
		if (mb->m_len < hlen) {
			ipoib_warn(priv, "linear data too small\n");
			++dev->if_oerrors;
			m_freem(mb);
			return;
		}
		m_adj(mb, hlen);
	} else {
		if (unlikely(mb->m_pkthdr.len > priv->mcast_mtu + IPOIB_ENCAP_LEN)) {
			ipoib_warn(priv, "packet len %d (> %d) too long to send, dropping\n",
				   mb->m_pkthdr.len, priv->mcast_mtu + IPOIB_ENCAP_LEN);
			++dev->if_oerrors;
			ipoib_cm_mb_too_long(dev, mb, priv->mcast_mtu);
			return;
		}
		phead = NULL;
		hlen  = 0;
	}

	ipoib_dbg_data(priv, "sending packet, length=%d address=%p qpn=0x%06x\n",
		       mb->m_pkthdr.len, address, qpn);

	/*
	 * We put the mb into the tx_ring _before_ we call post_send()
	 * because it's entirely possible that the completion handler will
	 * run before we execute anything after the post_send().  That
	 * means we have to make sure everything is properly recorded and
	 * our state is consistent before we call post_send().
	 */
	tx_req = &priv->tx_ring[priv->tx_head & (ipoib_sendq_size - 1)];
	tx_req->mb = mb;
	if (unlikely(ipoib_dma_map_tx(priv->ca, tx_req))) {
		++dev->if_oerrors;
		if (tx_req->mb)
			m_freem(tx_req->mb);
		return;
	}

/* XXX NO checksum offload yet.
	if (mb->ip_summed == CHECKSUM_PARTIAL)
		priv->tx_wr.send_flags |= IB_SEND_IP_CSUM;
	else
		priv->tx_wr.send_flags &= ~IB_SEND_IP_CSUM;
*/

	if (++priv->tx_outstanding == ipoib_sendq_size) {
		ipoib_dbg(priv, "TX ring full, stopping kernel net queue\n");
		if (ib_req_notify_cq(priv->send_cq, IB_CQ_NEXT_COMP))
			ipoib_warn(priv, "request notify on send CQ failed\n");
		dev->if_drv_flags |= IFF_DRV_OACTIVE;
	}

	if (unlikely(post_send(priv, priv->tx_head & (ipoib_sendq_size - 1),
			       address->ah, qpn, tx_req, phead, hlen))) {
		ipoib_warn(priv, "post_send failed\n");
		++dev->if_oerrors;
		--priv->tx_outstanding;
		ipoib_dma_unmap_tx(priv->ca, tx_req);
		m_freem(mb);
		if (dev->if_drv_flags & IFF_DRV_OACTIVE)
			dev->if_drv_flags &= ~IFF_DRV_OACTIVE;
	} else {
		/* dev->trans_start = jiffies; */

		address->last_send = priv->tx_head;
		++priv->tx_head;
	}

	if (unlikely(priv->tx_outstanding > MAX_SEND_CQE))
		while (poll_tx(priv))
			; /* nothing */
}

static void __ipoib_reap_ah(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ipoib_ah *ah, *tah;
	LIST_HEAD(remove_list);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	list_for_each_entry_safe(ah, tah, &priv->dead_ahs, list)
		if ((int) priv->tx_tail - (int) ah->last_send >= 0) {
			list_del(&ah->list);
			ib_destroy_ah(ah->ah);
			kfree(ah);
		}

	spin_unlock_irqrestore(&priv->lock, flags);
}

void ipoib_reap_ah(struct work_struct *work)
{
	struct ipoib_dev_priv *priv =
		container_of(work, struct ipoib_dev_priv, ah_reap_task.work);
	struct ifnet *dev = priv->dev;

	__ipoib_reap_ah(dev);

	if (!test_bit(IPOIB_STOP_REAPER, &priv->flags))
		queue_delayed_work(ipoib_workqueue, &priv->ah_reap_task,
				   HZ);
}

static void ipoib_ah_dev_cleanup(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	unsigned long begin;

	begin = jiffies;

	while (!list_empty(&priv->dead_ahs)) {
		__ipoib_reap_ah(dev);

		if (time_after(jiffies, begin + HZ)) {
			ipoib_warn(priv, "timing out; will leak address handles\n");
			break;
		}

		msleep(1);
	}
}

static void ipoib_ib_tx_timer_func(unsigned long ctx)
{
	drain_tx_cq((struct ifnet *)ctx);
}

int ipoib_ib_dev_open(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	int ret;

	if (ib_find_pkey(priv->ca, priv->port, priv->pkey, &priv->pkey_index)) {
		ipoib_warn(priv, "P_Key 0x%04x not found\n", priv->pkey);
		clear_bit(IPOIB_PKEY_ASSIGNED, &priv->flags);
		return -1;
	}
	set_bit(IPOIB_PKEY_ASSIGNED, &priv->flags);

	ret = ipoib_init_qp(dev);
	if (ret) {
		ipoib_warn(priv, "ipoib_init_qp returned %d\n", ret);
		return -1;
	}

	ret = ipoib_ib_post_receives(dev);
	if (ret) {
		ipoib_warn(priv, "ipoib_ib_post_receives returned %d\n", ret);
		ipoib_ib_dev_stop(dev, 1);
		return -1;
	}

	ret = ipoib_cm_dev_open(dev);
	if (ret) {
		ipoib_warn(priv, "ipoib_cm_dev_open returned %d\n", ret);
		ipoib_ib_dev_stop(dev, 1);
		return -1;
	}

	clear_bit(IPOIB_STOP_REAPER, &priv->flags);
	queue_delayed_work(ipoib_workqueue, &priv->ah_reap_task, HZ);

	return 0;
}

static void ipoib_pkey_dev_check_presence(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	u16 pkey_index = 0;

	if (ib_find_pkey(priv->ca, priv->port, priv->pkey, &pkey_index))
		clear_bit(IPOIB_PKEY_ASSIGNED, &priv->flags);
	else
		set_bit(IPOIB_PKEY_ASSIGNED, &priv->flags);
}

int ipoib_ib_dev_up(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	ipoib_pkey_dev_check_presence(dev);

	if (!test_bit(IPOIB_PKEY_ASSIGNED, &priv->flags)) {
		ipoib_dbg(priv, "PKEY is not assigned.\n");
		return 0;
	}

	set_bit(IPOIB_FLAG_OPER_UP, &priv->flags);

	return ipoib_mcast_start_thread(dev);
}

int ipoib_ib_dev_down(struct ifnet *dev, int flush)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	ipoib_dbg(priv, "downing ib_dev\n");

	clear_bit(IPOIB_FLAG_OPER_UP, &priv->flags);

	/* Shutdown the P_Key thread if still active */
	if (!test_bit(IPOIB_PKEY_ASSIGNED, &priv->flags)) {
		mutex_lock(&pkey_mutex);
		set_bit(IPOIB_PKEY_STOP, &priv->flags);
		cancel_delayed_work(&priv->pkey_poll_task);
		mutex_unlock(&pkey_mutex);
		if (flush)
			flush_workqueue(ipoib_workqueue);
	}

	ipoib_mcast_stop_thread(dev, flush);
	ipoib_mcast_dev_flush(dev);

	ipoib_flush_paths(dev);

	return 0;
}

static int recvs_pending(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	int pending = 0;
	int i;

	for (i = 0; i < ipoib_recvq_size; ++i)
		if (priv->rx_ring[i].mb)
			++pending;

	return pending;
}

void ipoib_drain_cq(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	int i, n;

	spin_lock(&priv->lock);
	do {
		n = ib_poll_cq(priv->recv_cq, IPOIB_NUM_WC, priv->ibwc);
		for (i = 0; i < n; ++i) {
			/*
			 * Convert any successful completions to flush
			 * errors to avoid passing packets up the
			 * stack after bringing the device down.
			 */
			if (priv->ibwc[i].status == IB_WC_SUCCESS)
				priv->ibwc[i].status = IB_WC_WR_FLUSH_ERR;

			if (priv->ibwc[i].wr_id & IPOIB_OP_RECV) {
				if (priv->ibwc[i].wr_id & IPOIB_OP_CM)
					ipoib_cm_handle_rx_wc(dev, priv->ibwc + i);
				else
					ipoib_ib_handle_rx_wc(dev, priv->ibwc + i);
			} else
				ipoib_cm_handle_tx_wc(dev, priv->ibwc + i);
		}
	} while (n == IPOIB_NUM_WC);

	while (poll_tx(priv))
		; /* nothing */

	spin_unlock(&priv->lock);
}

int ipoib_ib_dev_stop(struct ifnet *dev, int flush)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ib_qp_attr qp_attr;
	unsigned long begin;
	struct ipoib_tx_buf *tx_req;
	int i;

	ipoib_cm_dev_stop(dev);

	/*
	 * Move our QP to the error state and then reinitialize in
	 * when all work requests have completed or have been flushed.
	 */
	qp_attr.qp_state = IB_QPS_ERR;
	if (ib_modify_qp(priv->qp, &qp_attr, IB_QP_STATE))
		ipoib_warn(priv, "Failed to modify QP to ERROR state\n");

	/* Wait for all sends and receives to complete */
	begin = jiffies;

	while (priv->tx_head != priv->tx_tail || recvs_pending(dev)) {
		if (time_after(jiffies, begin + 5 * HZ)) {
			ipoib_warn(priv, "timing out; %d sends %d receives not completed\n",
				   priv->tx_head - priv->tx_tail, recvs_pending(dev));

			/*
			 * assume the HW is wedged and just free up
			 * all our pending work requests.
			 */
			while ((int) priv->tx_tail - (int) priv->tx_head < 0) {
				tx_req = &priv->tx_ring[priv->tx_tail &
							(ipoib_sendq_size - 1)];
				ipoib_dma_unmap_tx(priv->ca, tx_req);
				m_freem(tx_req->mb);
				++priv->tx_tail;
				--priv->tx_outstanding;
			}

			for (i = 0; i < ipoib_recvq_size; ++i) {
				struct ipoib_rx_buf *rx_req;

				rx_req = &priv->rx_ring[i];
				if (!rx_req->mb)
					continue;
				ipoib_ud_dma_unmap_rx(priv,
						      priv->rx_ring[i].mapping);
				m_freem(rx_req->mb);
				rx_req->mb = NULL;
			}

			goto timeout;
		}

		ipoib_drain_cq(dev);

		msleep(1);
	}

	ipoib_dbg(priv, "All sends and receives done.\n");

timeout:
	del_timer_sync(&priv->poll_timer);
	qp_attr.qp_state = IB_QPS_RESET;
	if (ib_modify_qp(priv->qp, &qp_attr, IB_QP_STATE))
		ipoib_warn(priv, "Failed to modify QP to RESET state\n");

	/* Wait for all AHs to be reaped */
	set_bit(IPOIB_STOP_REAPER, &priv->flags);
	cancel_delayed_work(&priv->ah_reap_task);
	if (flush)
		flush_workqueue(ipoib_workqueue);

	ipoib_ah_dev_cleanup(dev);

	ib_req_notify_cq(priv->recv_cq, IB_CQ_NEXT_COMP);

	return 0;
}

int ipoib_ib_dev_init(struct ifnet *dev, struct ib_device *ca, int port)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	priv->ca = ca;
	priv->port = port;
	priv->qp = NULL;

	if (ipoib_transport_dev_init(dev, ca)) {
		printk(KERN_WARNING "%s: ipoib_transport_dev_init failed\n", ca->name);
		return -ENODEV;
	}

	setup_timer(&priv->poll_timer, ipoib_ib_tx_timer_func,
		    (unsigned long) dev);

	if (dev->if_flags & IFF_UP) {
		if (ipoib_ib_dev_open(dev)) {
			ipoib_transport_dev_cleanup(dev);
			return -ENODEV;
		}
	}

	return 0;
}

static void __ipoib_ib_dev_flush(struct ipoib_dev_priv *priv,
				enum ipoib_flush_level level)
{
	struct ipoib_dev_priv *cpriv;
	struct ifnet *dev = priv->dev;
	u16 new_index;

	mutex_lock(&priv->vlan_mutex);

	/*
	 * Flush any child interfaces too -- they might be up even if
	 * the parent is down.
	 */
	list_for_each_entry(cpriv, &priv->child_intfs, list)
		__ipoib_ib_dev_flush(cpriv, level);

	mutex_unlock(&priv->vlan_mutex);

	if (!test_bit(IPOIB_FLAG_INITIALIZED, &priv->flags)) {
		ipoib_dbg(priv, "Not flushing - IPOIB_FLAG_INITIALIZED not set.\n");
		return;
	}

	if (!test_bit(IPOIB_FLAG_ADMIN_UP, &priv->flags)) {
		ipoib_dbg(priv, "Not flushing - IPOIB_FLAG_ADMIN_UP not set.\n");
		return;
	}

	if (level == IPOIB_FLUSH_HEAVY) {
		if (ib_find_pkey(priv->ca, priv->port, priv->pkey, &new_index)) {
			clear_bit(IPOIB_PKEY_ASSIGNED, &priv->flags);
			ipoib_ib_dev_down(dev, 0);
			ipoib_ib_dev_stop(dev, 0);
			if (ipoib_pkey_dev_delay_open(dev))
				return;
		}

		/* restart QP only if P_Key index is changed */
		if (test_and_set_bit(IPOIB_PKEY_ASSIGNED, &priv->flags) &&
		    new_index == priv->pkey_index) {
			ipoib_dbg(priv, "Not flushing - P_Key index not changed.\n");
			return;
		}
		priv->pkey_index = new_index;
	}

	if (level == IPOIB_FLUSH_LIGHT) {
		ipoib_mark_paths_invalid(dev);
		ipoib_mcast_dev_flush(dev);
	}

	if (level >= IPOIB_FLUSH_NORMAL)
		ipoib_ib_dev_down(dev, 0);

	if (level == IPOIB_FLUSH_HEAVY) {
		ipoib_ib_dev_stop(dev, 0);
		ipoib_ib_dev_open(dev);
	}

	/*
	 * The device could have been brought down between the start and when
	 * we get here, don't bring it back up if it's not configured up
	 */
	if (test_bit(IPOIB_FLAG_ADMIN_UP, &priv->flags)) {
		if (level >= IPOIB_FLUSH_NORMAL)
			ipoib_ib_dev_up(dev);
		ipoib_mcast_restart_task(&priv->restart_task);
	}
}

void ipoib_ib_dev_flush_light(struct work_struct *work)
{
	struct ipoib_dev_priv *priv =
		container_of(work, struct ipoib_dev_priv, flush_light);

	__ipoib_ib_dev_flush(priv, IPOIB_FLUSH_LIGHT);
}

void ipoib_ib_dev_flush_normal(struct work_struct *work)
{
	struct ipoib_dev_priv *priv =
		container_of(work, struct ipoib_dev_priv, flush_normal);

	__ipoib_ib_dev_flush(priv, IPOIB_FLUSH_NORMAL);
}

void ipoib_ib_dev_flush_heavy(struct work_struct *work)
{
	struct ipoib_dev_priv *priv =
		container_of(work, struct ipoib_dev_priv, flush_heavy);

	__ipoib_ib_dev_flush(priv, IPOIB_FLUSH_HEAVY);
}

void ipoib_ib_dev_cleanup(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	ipoib_dbg(priv, "cleaning up ib_dev\n");

	ipoib_mcast_stop_thread(dev, 1);
	ipoib_mcast_dev_flush(dev);

	ipoib_ah_dev_cleanup(dev);
	ipoib_transport_dev_cleanup(dev);
}

/*
 * Delayed P_Key Assigment Interim Support
 *
 * The following is initial implementation of delayed P_Key assigment
 * mechanism. It is using the same approach implemented for the multicast
 * group join. The single goal of this implementation is to quickly address
 * Bug #2507. This implementation will probably be removed when the P_Key
 * change async notification is available.
 */

void ipoib_pkey_poll(struct work_struct *work)
{
	struct ipoib_dev_priv *priv =
		container_of(work, struct ipoib_dev_priv, pkey_poll_task.work);
	struct ifnet *dev = priv->dev;

	ipoib_pkey_dev_check_presence(dev);

	if (test_bit(IPOIB_PKEY_ASSIGNED, &priv->flags))
		ipoib_open(dev);
	else {
		mutex_lock(&pkey_mutex);
		if (!test_bit(IPOIB_PKEY_STOP, &priv->flags))
			queue_delayed_work(ipoib_workqueue,
					   &priv->pkey_poll_task,
					   HZ);
		mutex_unlock(&pkey_mutex);
	}
}

int ipoib_pkey_dev_delay_open(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	/* Look for the interface pkey value in the IB Port P_Key table and */
	/* set the interface pkey assigment flag                            */
	ipoib_pkey_dev_check_presence(dev);

	/* P_Key value not assigned yet - start polling */
	if (!test_bit(IPOIB_PKEY_ASSIGNED, &priv->flags)) {
		mutex_lock(&pkey_mutex);
		clear_bit(IPOIB_PKEY_STOP, &priv->flags);
		queue_delayed_work(ipoib_workqueue,
				   &priv->pkey_poll_task,
				   HZ);
		mutex_unlock(&pkey_mutex);
		return 1;
	}

	return 0;
}
