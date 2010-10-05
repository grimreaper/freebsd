/*
 * Copyright (c) 2004 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2004 Voltaire, Inc. All rights reserved.
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

static	int ipoib_resolvemulti(struct ifnet *, struct sockaddr **,
		struct sockaddr *);


#include <linux/module.h>

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>

#include <linux/if_arp.h>	/* For ARPHRD_xxx */

MODULE_AUTHOR("Roland Dreier");
MODULE_DESCRIPTION("IP-over-InfiniBand net driver");
MODULE_LICENSE("Dual BSD/GPL");

int ipoib_sendq_size = IPOIB_TX_RING_SIZE;
int ipoib_recvq_size = IPOIB_RX_RING_SIZE;

module_param_named(send_queue_size, ipoib_sendq_size, int, 0444);
MODULE_PARM_DESC(send_queue_size, "Number of descriptors in send queue");
module_param_named(recv_queue_size, ipoib_recvq_size, int, 0444);
MODULE_PARM_DESC(recv_queue_size, "Number of descriptors in receive queue");

#ifdef CONFIG_INFINIBAND_IPOIB_DEBUG
int ipoib_debug_level = 1;

module_param_named(debug_level, ipoib_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Enable debug tracing if > 0");
#endif

struct ipoib_path_iter {
	struct ifnet *dev;
	struct ipoib_path  path;
};

static const u8 ipv4_bcast_addr[] = {
	0x00, 0xff, 0xff, 0xff,
	0xff, 0x12, 0x40, 0x1b,	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff
};

struct workqueue_struct *ipoib_workqueue;

struct ib_sa_client ipoib_sa_client;

static void ipoib_add_one(struct ib_device *device);
static void ipoib_remove_one(struct ib_device *device);
static void ipoib_start(struct ifnet *dev);
static int ipoib_output(struct ifnet *ifp, struct mbuf *m,
	    struct sockaddr *dst, struct route *ro);
static int ipoib_ioctl(struct ifnet *ifp, u_long command, caddr_t data);
static void ipoib_input(struct ifnet *ifp, struct mbuf *m);

static struct ib_client ipoib_client = {
	.name   = "ipoib",
	.add    = ipoib_add_one,
	.remove = ipoib_remove_one
};

int ipoib_open(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	ipoib_dbg(priv, "bringing up interface\n");

	set_bit(IPOIB_FLAG_ADMIN_UP, &priv->flags);

	if (ipoib_pkey_dev_delay_open(dev))
		return 0;

	if (ipoib_ib_dev_open(dev))
		goto err_disable;

	if (ipoib_ib_dev_up(dev))
		goto err_stop;

#if 0 /* XXX */
	if (!test_bit(IPOIB_FLAG_SUBINTERFACE, &priv->flags)) {
		struct ipoib_dev_priv *cpriv;

		/* Bring up any child interfaces too */
		mutex_lock(&priv->vlan_mutex);
		list_for_each_entry(cpriv, &priv->child_intfs, list) {
			int flags;

			flags = cpriv->dev->flags;
			if (flags & IFF_UP)
				continue;

			dev_change_flags(cpriv->dev, flags | IFF_UP);
		}
		mutex_unlock(&priv->vlan_mutex);
	}
#endif
	dev->if_drv_flags |= IFF_DRV_RUNNING;
	dev->if_drv_flags &= ~IFF_DRV_OACTIVE;

	return 0;

err_stop:
	ipoib_ib_dev_stop(dev, 1);

err_disable:
	clear_bit(IPOIB_FLAG_ADMIN_UP, &priv->flags);

	return -EINVAL;
}

static void
ipoib_init(void *arg)
{
	struct ifnet *dev;
	struct ipoib_dev_priv *priv;

	priv = arg;
	dev = priv->dev;
	if ((dev->if_drv_flags & IFF_DRV_RUNNING) == 0)
		ipoib_open(dev);
}


static int ipoib_stop(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	ipoib_dbg(priv, "stopping interface\n");

	clear_bit(IPOIB_FLAG_ADMIN_UP, &priv->flags);

	dev->if_drv_flags &= ~(IFF_DRV_RUNNING | IFF_DRV_OACTIVE);

	ipoib_ib_dev_down(dev, 0);
	ipoib_ib_dev_stop(dev, 0);

#if 0 /* XXX */
	if (!test_bit(IPOIB_FLAG_SUBINTERFACE, &priv->flags)) {
		struct ipoib_dev_priv *cpriv;

		/* Bring down any child interfaces too */
		mutex_lock(&priv->vlan_mutex);
		list_for_each_entry(cpriv, &priv->child_intfs, list) {
			int flags;

			flags = cpriv->dev->if_flags;
			if (!(flags & IFF_UP))
				continue;

			dev_change_flags(cpriv->dev, flags & ~IFF_UP);
		}
		mutex_unlock(&priv->vlan_mutex);
	}
#endif

	return 0;
}

static int ipoib_change_mtu(struct ifnet *dev, int new_mtu)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	/* dev->if_mtu > 2K ==> connected mode */
	if (ipoib_cm_admin_enabled(dev)) {
		if (new_mtu > ipoib_cm_max_mtu(dev))
			return -EINVAL;

		if (new_mtu > priv->mcast_mtu)
			ipoib_warn(priv, "mtu > %d will cause multicast packet drops.\n",
				   priv->mcast_mtu);

		dev->if_mtu = new_mtu;
		return 0;
	}

	if (new_mtu > IPOIB_UD_MTU(priv->max_ib_mtu))
		return -EINVAL;

	priv->admin_mtu = new_mtu;

	dev->if_mtu = min(priv->mcast_mtu, priv->admin_mtu);

	queue_work(ipoib_workqueue, &priv->flush_light);

	return 0;
}

static int
ipoib_ioctl(struct ifnet *ifp, u_long command, caddr_t data)
{
	struct ifaddr *ifa = (struct ifaddr *) data;
	struct ifreq *ifr = (struct ifreq *) data;
	int error = 0;

	switch (command) {
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
				error = -ipoib_open(ifp);
		} else
			if (ifp->if_drv_flags & IFF_DRV_RUNNING)
				ipoib_stop(ifp);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (ifp->if_drv_flags & IFF_DRV_RUNNING) {
			/* XXX Update multicast info. */
		}
		break;
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;

		switch (ifa->ifa_addr->sa_family) {
#ifdef INET
		case AF_INET:
			ifp->if_init(ifp->if_softc);	/* before arpwhohas */
			arp_ifinit(ifp, ifa);
			break;
#endif
		default:
			ifp->if_init(ifp->if_softc);
			break;
		}
		break;

	case SIOCGIFADDR:
		{
			struct sockaddr *sa;

			sa = (struct sockaddr *) & ifr->ifr_data;
			bcopy(IF_LLADDR(ifp),
			      (caddr_t) sa->sa_data, INFINIBAND_ALEN);
		}
		break;

	case SIOCSIFMTU:
		/*
		 * Set the interface MTU.
		 */
		error = -ipoib_change_mtu(ifp, ifr->ifr_mtu);
		break;
	default:
		error = EINVAL;
		break;
	}
	return (error);
}


static struct ipoib_path *__path_find(struct ifnet *dev, void *gid)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct rb_node *n = priv->path_tree.rb_node;
	struct ipoib_path *path;
	int ret;

	while (n) {
		path = rb_entry(n, struct ipoib_path, rb_node);

		ret = memcmp(gid, path->pathrec.dgid.raw,
			     sizeof (union ib_gid));

		if (ret < 0)
			n = n->rb_left;
		else if (ret > 0)
			n = n->rb_right;
		else
			return path;
	}

	return NULL;
}

static int __path_add(struct ifnet *dev, struct ipoib_path *path)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct rb_node **n = &priv->path_tree.rb_node;
	struct rb_node *pn = NULL;
	struct ipoib_path *tpath;
	int ret;

	while (*n) {
		pn = *n;
		tpath = rb_entry(pn, struct ipoib_path, rb_node);

		ret = memcmp(path->pathrec.dgid.raw, tpath->pathrec.dgid.raw,
			     sizeof (union ib_gid));
		if (ret < 0)
			n = &pn->rb_left;
		else if (ret > 0)
			n = &pn->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&path->rb_node, pn, n);
	rb_insert_color(&path->rb_node, &priv->path_tree);

	list_add_tail(&path->list, &priv->path_list);

	return 0;
}

static void path_free(struct ifnet *dev, struct ipoib_path *path)
{

	_IF_DRAIN(&path->queue);

	if (path->ah)
		ipoib_put_ah(path->ah);
	if (ipoib_cm_get(path))
		ipoib_cm_destroy_tx(ipoib_cm_get(path));

	kfree(path);
}

#ifdef CONFIG_INFINIBAND_IPOIB_DEBUG

struct ipoib_path_iter *ipoib_path_iter_init(struct ifnet *dev)
{
	struct ipoib_path_iter *iter;

	iter = kmalloc(sizeof *iter, GFP_KERNEL);
	if (!iter)
		return NULL;

	iter->dev = dev;
	memset(iter->path.pathrec.dgid.raw, 0, 16);

	if (ipoib_path_iter_next(iter)) {
		kfree(iter);
		return NULL;
	}

	return iter;
}

int ipoib_path_iter_next(struct ipoib_path_iter *iter)
{
	struct ipoib_dev_priv *priv = iter->dev->if_softc;
	struct rb_node *n;
	struct ipoib_path *path;
	int ret = 1;

	spin_lock_irq(&priv->lock);

	n = rb_first(&priv->path_tree);

	while (n) {
		path = rb_entry(n, struct ipoib_path, rb_node);

		if (memcmp(iter->path.pathrec.dgid.raw, path->pathrec.dgid.raw,
			   sizeof (union ib_gid)) < 0) {
			iter->path = *path;
			ret = 0;
			break;
		}

		n = rb_next(n);
	}

	spin_unlock_irq(&priv->lock);

	return ret;
}

void ipoib_path_iter_read(struct ipoib_path_iter *iter,
			  struct ipoib_path *path)
{
	*path = iter->path;
}

#endif /* CONFIG_INFINIBAND_IPOIB_DEBUG */

void ipoib_mark_paths_invalid(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ipoib_path *path, *tp;

	spin_lock_irq(&priv->lock);

	list_for_each_entry_safe(path, tp, &priv->path_list, list) {
		ipoib_dbg(priv, "mark path LID 0x%04x GID %pI6 invalid\n",
			be16_to_cpu(path->pathrec.dlid),
			path->pathrec.dgid.raw);
		path->valid =  0;
	}

	spin_unlock_irq(&priv->lock);
}

void ipoib_flush_paths(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ipoib_path *path, *tp;
	LIST_HEAD(remove_list);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	list_splice_init(&priv->path_list, &remove_list);

	list_for_each_entry(path, &remove_list, list)
		rb_erase(&path->rb_node, &priv->path_tree);

	list_for_each_entry_safe(path, tp, &remove_list, list) {
		if (path->query)
			ib_sa_cancel_query(path->query_id, path->query);
		spin_unlock_irqrestore(&priv->lock, flags);
		wait_for_completion(&path->done);
		path_free(dev, path);
		spin_lock_irqsave(&priv->lock, flags);
	}

	spin_unlock_irqrestore(&priv->lock, flags);
}

static void path_rec_completion(int status,
				struct ib_sa_path_rec *pathrec,
				void *path_ptr)
{
	struct ipoib_path *path = path_ptr;
	struct ifnet *dev = path->dev;
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ipoib_ah *ah = NULL;
	struct ipoib_ah *old_ah = NULL;
	struct ifqueue mbqueue;
	struct mbuf *mb;
	unsigned long flags;

	if (!status)
		ipoib_dbg(priv, "PathRec LID 0x%04x for GID %pI6\n",
			  be16_to_cpu(pathrec->dlid), pathrec->dgid.raw);
	else
		ipoib_dbg(priv, "PathRec status %d for GID %pI6\n",
			  status, path->pathrec.dgid.raw);

	bzero(&mbqueue, sizeof(mbqueue));

	if (!status) {
		struct ib_ah_attr av;

		if (!ib_init_ah_from_path(priv->ca, priv->port, pathrec, &av, 0))
			ah = ipoib_create_ah(dev, priv->pd, &av);
	}

	spin_lock_irqsave(&priv->lock, flags);

	if (ah) {
		path->pathrec = *pathrec;

		old_ah   = path->ah;
		path->ah = ah;

		ipoib_dbg(priv, "created address handle %p for LID 0x%04x, SL %d\n",
			  ah, be16_to_cpu(pathrec->dlid), pathrec->sl);

		for (;;) {
			_IF_DEQUEUE(&path->queue, mb);
			if (mb == NULL)
				break;
			_IF_ENQUEUE(&mbqueue, mb);
		}

#if 0 /* XXX */
		if (ipoib_cm_enabled(dev, path->hwaddr) && !ipoib_cm_get(path))
			ipoib_cm_set(path, ipoib_cm_create_tx(dev, path));
#endif

		path->valid = 1;
	}

	path->query = NULL;
	complete(&path->done);

	spin_unlock_irqrestore(&priv->lock, flags);

	if (old_ah)
		ipoib_put_ah(old_ah);

	for (;;) {
		_IF_DEQUEUE(&mbqueue, mb);
		if (mb == NULL)
			break;
		mb->m_pkthdr.rcvif = dev;
		if (dev->if_transmit(dev, mb))
			ipoib_warn(priv, "dev_queue_xmit failed "
				   "to requeue packet\n");
	}
}

static struct ipoib_path *path_rec_create(struct ifnet *dev, void *gid)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ipoib_path *path;

	if (!priv->broadcast)
		return NULL;

	path = kzalloc(sizeof *path, GFP_ATOMIC);
	if (!path)
		return NULL;

	path->dev = dev;

	bzero(&path->queue, sizeof(path->queue));

	memcpy(path->pathrec.dgid.raw, gid, sizeof (union ib_gid));
	path->pathrec.sgid	    = priv->local_gid;
	path->pathrec.pkey	    = cpu_to_be16(priv->pkey);
	path->pathrec.numb_path     = 1;
	path->pathrec.traffic_class = priv->broadcast->mcmember.traffic_class;

	return path;
}

static int path_rec_start(struct ifnet *dev,
			  struct ipoib_path *path)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	ib_sa_comp_mask comp_mask = IB_SA_PATH_REC_MTU_SELECTOR | IB_SA_PATH_REC_MTU;
	struct ib_sa_path_rec p_rec;

	p_rec = path->pathrec;
	p_rec.mtu_selector = IB_SA_GT;

	switch (roundup_pow_of_two(dev->if_mtu + IPOIB_ENCAP_LEN)) {
	case 512:
		p_rec.mtu = IB_MTU_256;
		break;
	case 1024:
		p_rec.mtu = IB_MTU_512;
		break;
	case 2048:
		p_rec.mtu = IB_MTU_1024;
		break;
	case 4096:
		p_rec.mtu = IB_MTU_2048;
		break;
	default:
		/* Wildcard everything */
		comp_mask = 0;
		p_rec.mtu = 0;
		p_rec.mtu_selector = 0;
	}

	ipoib_dbg(priv, "Start path record lookup for %pI6 MTU > %d\n",
		  p_rec.dgid.raw,
		  comp_mask ? ib_mtu_enum_to_int(p_rec.mtu) : 0);

	init_completion(&path->done);

	path->query_id =
		ib_sa_path_rec_get(&ipoib_sa_client, priv->ca, priv->port,
				   &p_rec, comp_mask		|
				   IB_SA_PATH_REC_DGID		|
				   IB_SA_PATH_REC_SGID		|
				   IB_SA_PATH_REC_NUMB_PATH	|
				   IB_SA_PATH_REC_TRAFFIC_CLASS |
				   IB_SA_PATH_REC_PKEY,
				   1000, GFP_ATOMIC,
				   path_rec_completion,
				   path, &path->query);
	if (path->query_id < 0) {
		ipoib_warn(priv, "ib_sa_path_rec_get failed: %d\n", path->query_id);
		path->query = NULL;
		complete(&path->done);
		return path->query_id;
	}

	return 0;
}

static void
unicast_send(struct mbuf *mb, struct ifnet *dev, struct ipoib_header *eh)
{
	struct ipoib_dev_priv *priv = dev->if_softc;
	struct ipoib_path *path;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	path = __path_find(dev, eh->hwaddr + 4);
	if (!path || !path->valid) {
		int new_path = 0;

		if (!path) {
			path = path_rec_create(dev, eh->hwaddr + 4);
			new_path = 1;
		}
		if (path) {
			_IF_ENQUEUE(&path->queue, mb);
			if (!path->query && path_rec_start(dev, path)) {
				spin_unlock_irqrestore(&priv->lock, flags);
				if (new_path)
					path_free(dev, path);
				return;
			} else
				__path_add(dev, path);
		} else {
			++dev->if_oerrors;
			m_free(mb);
		}

		spin_unlock_irqrestore(&priv->lock, flags);
		return;
	}

	if (ipoib_cm_get(path) && ipoib_cm_up(path)) {
		ipoib_cm_send(dev, mb, ipoib_cm_get(path));
	} else if (path->ah) {
		ipoib_send(dev, mb, path->ah, IPOIB_QPN(eh->hwaddr));
	} else if ((path->query || !path_rec_start(dev, path)) &&
		    path->queue.ifq_len < IPOIB_MAX_PATH_REC_QUEUE) {
		_IF_ENQUEUE(&path->queue, mb);
	} else {
		++dev->if_oerrors;
		m_free(mb);
	}

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int
ipoib_send_one(struct ifnet *dev, struct mbuf *mb)
{
	struct ipoib_dev_priv *priv;
	struct ipoib_header *eh;

	eh = mtod(mb, struct ipoib_header *);
	if (IPOIB_IS_MULTICAST(eh->hwaddr)) {
		/* Add in the P_Key for multicast*/
		priv = dev->if_softc;
		eh->hwaddr[8] = (priv->pkey >> 8) & 0xff;
		eh->hwaddr[9] = priv->pkey & 0xff;

		ipoib_mcast_send(dev, eh->hwaddr + 4, mb);
	} else
		unicast_send(mb, dev, eh);

	return 0;
}

static void
ipoib_start(struct ifnet *dev)
{
	struct mbuf *mb;

	if ((dev->if_drv_flags & (IFF_DRV_RUNNING|IFF_DRV_OACTIVE)) !=
	    IFF_DRV_RUNNING)
		return;

	while (!IFQ_DRV_IS_EMPTY(&dev->if_snd)) {
		IFQ_DRV_DEQUEUE(&dev->if_snd, mb);
		if (mb == NULL)
			break;

		ipoib_send_one(dev, mb);
#if 0 /* XXX */
			dev->if_drv_flags |= IFF_DRV_OACTIVE;
			IFQ_DRV_PREPEND(&dev->if_snd, m_head);
			break;
#endif
		BPF_MTAP(dev, mb);
	}
}


#if 0 /* XXX */
static void ipoib_timeout(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	ipoib_warn(priv, "transmit timeout: latency %d msecs\n",
		   jiffies_to_msecs(jiffies - dev->trans_start));
	ipoib_warn(priv, "queue stopped %d, tx_head %u, tx_tail %u\n",
		   netif_queue_stopped(dev),
		   priv->tx_head, priv->tx_tail);
	/* XXX reset QP, etc. */
}

static void ipoib_set_mcast_list(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	if (!test_bit(IPOIB_FLAG_OPER_UP, &priv->flags)) {
		ipoib_dbg(priv, "IPOIB_FLAG_OPER_UP not set");
		return;
	}

	queue_work(ipoib_workqueue, &priv->restart_task);
}
#endif

int ipoib_dev_init(struct ifnet *dev, struct ib_device *ca, int port)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	/* Allocate RX/TX "rings" to hold queued mbs */
	priv->rx_ring =	kzalloc(ipoib_recvq_size * sizeof *priv->rx_ring,
				GFP_KERNEL);
	if (!priv->rx_ring) {
		printk(KERN_WARNING "%s: failed to allocate RX ring (%d entries)\n",
		       ca->name, ipoib_recvq_size);
		goto out;
	}

	priv->tx_ring = kzalloc(ipoib_sendq_size * sizeof *priv->tx_ring, GFP_KERNEL);
	if (!priv->tx_ring) {
		printk(KERN_WARNING "%s: failed to allocate TX ring (%d entries)\n",
		       ca->name, ipoib_sendq_size);
		goto out_rx_ring_cleanup;
	}
	memset(priv->tx_ring, 0, ipoib_sendq_size * sizeof *priv->tx_ring);

	/* priv->tx_head, tx_tail & tx_outstanding are already 0 */

	if (ipoib_ib_dev_init(dev, ca, port))
		goto out_tx_ring_cleanup;

	return 0;

out_tx_ring_cleanup:
	kfree(priv->tx_ring);

out_rx_ring_cleanup:
	kfree(priv->rx_ring);

out:
	return -ENOMEM;
}

static void
ipoib_detach(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc;

	bpfdetach(dev);
	if_detach(dev);
	if_free(dev);
	free(priv, M_TEMP);
}

void ipoib_dev_cleanup(struct ifnet *dev)
{
	struct ipoib_dev_priv *priv = dev->if_softc, *cpriv, *tcpriv;

	/* Delete any child interfaces first */
	list_for_each_entry_safe(cpriv, tcpriv, &priv->child_intfs, list) {
		ipoib_dev_cleanup(cpriv->dev);
		ipoib_detach(cpriv->dev);
	}

	ipoib_ib_dev_cleanup(dev);

	kfree(priv->rx_ring);
	kfree(priv->tx_ring);

	priv->rx_ring = NULL;
	priv->tx_ring = NULL;
}

#if 0
static int get_mb_hdr(struct mbuf *mb, void **iphdr,
		       void **tcph, u64 *hdr_flags, void *priv)
{
	unsigned int ip_len;
	struct iphdr *iph;

	if (unlikely(mb->protocol != htons(ETH_P_IP)))
		return -1;

	/*
	 * In the future we may add an else clause that verifies the
	 * checksum and allows devices which do not calculate checksum
	 * to use LRO.
	 */
	if (unlikely(mb->ip_summed != CHECKSUM_UNNECESSARY))
		return -1;

	/* Check for non-TCP packet */
	mb_reset_network_header(mb);
	iph = ip_hdr(mb);
	if (iph->protocol != IPPROTO_TCP)
		return -1;

	ip_len = ip_hdrlen(mb);
	mb_set_transport_header(mb, ip_len);
	*tcph = tcp_hdr(mb);

	/* check if IP header and TCP header are complete */
	if (ntohs(iph->tot_len) < ip_len + tcp_hdrlen(mb))
		return -1;

	*hdr_flags = LRO_IPV4 | LRO_TCP;
	*iphdr = iph;

	return 0;
}
#endif

struct ipoib_dev_priv *
ipoib_intf_alloc(const char *name)
{
	struct ipoib_dev_priv *priv;
	struct sockaddr_dl *sdl;
	struct ifnet *dev;

	priv = malloc(sizeof(struct ipoib_dev_priv), M_TEMP, M_ZERO|M_WAITOK);
	dev = priv->dev = if_alloc(IFT_INFINIBAND);
	if (!dev) {
		free(priv, M_TEMP);
		return NULL;
	}
	dev->if_softc = priv;
	if_initname(dev, name, dev->if_index);
	dev->if_flags = IFF_BROADCAST | IFF_MULTICAST;
	dev->if_addrlen = INFINIBAND_ALEN;
	dev->if_hdrlen = IPOIB_HEADER_LEN;
	if_attach(dev);
	dev->if_init = ipoib_init;
	dev->if_ioctl = ipoib_ioctl;
	dev->if_start = ipoib_start;
	dev->if_output = ipoib_output;
	dev->if_input = ipoib_input;
	dev->if_resolvemulti = ipoib_resolvemulti;
	dev->if_baudrate = IF_Gbps(10LL);
	dev->if_broadcastaddr = priv->broadcastaddr;
	memcpy(priv->broadcastaddr, ipv4_bcast_addr, INFINIBAND_ALEN);
	dev->if_snd.ifq_maxlen = ipoib_sendq_size * 2;
	sdl = (struct sockaddr_dl *)dev->if_addr->ifa_addr;
	sdl->sdl_type = IFT_INFINIBAND;
	sdl->sdl_alen = dev->if_addrlen;
	priv->dev = dev;

	bpfattach(dev, DLT_EN10MB, IPOIB_HEADER_LEN);

	spin_lock_init(&priv->lock);

	mutex_init(&priv->vlan_mutex);

	INIT_LIST_HEAD(&priv->path_list);
	INIT_LIST_HEAD(&priv->child_intfs);
	INIT_LIST_HEAD(&priv->dead_ahs);
	INIT_LIST_HEAD(&priv->multicast_list);

	INIT_DELAYED_WORK(&priv->pkey_poll_task, ipoib_pkey_poll);
	INIT_DELAYED_WORK(&priv->mcast_task,   ipoib_mcast_join_task);
	INIT_WORK(&priv->carrier_on_task, ipoib_mcast_carrier_on_task);
	INIT_WORK(&priv->flush_light,   ipoib_ib_dev_flush_light);
	INIT_WORK(&priv->flush_normal,   ipoib_ib_dev_flush_normal);
	INIT_WORK(&priv->flush_heavy,   ipoib_ib_dev_flush_heavy);
	INIT_WORK(&priv->restart_task, ipoib_mcast_restart_task);
	INIT_DELAYED_WORK(&priv->ah_reap_task, ipoib_reap_ah);


	return dev->if_softc;
}

#if 0
static ssize_t create_child(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int pkey;
	int ret;

	if (sscanf(buf, "%i", &pkey) != 1)
		return -EINVAL;

	if (pkey < 0 || pkey > 0xffff)
		return -EINVAL;

	/*
	 * Set the full membership bit, so that we join the right
	 * broadcast group, etc.
	 */
	pkey |= 0x8000;

	ret = ipoib_vlan_add(to_net_dev(dev), pkey);

	return ret ? ret : count;
}
static DEVICE_ATTR(create_child, S_IWUGO, NULL, create_child);

static ssize_t delete_child(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int pkey;
	int ret;

	if (sscanf(buf, "%i", &pkey) != 1)
		return -EINVAL;

	if (pkey < 0 || pkey > 0xffff)
		return -EINVAL;

	ret = ipoib_vlan_delete(to_net_dev(dev), pkey);

	return ret ? ret : count;

}
static DEVICE_ATTR(delete_child, S_IWUGO, NULL, delete_child);
#endif

int ipoib_set_dev_features(struct ipoib_dev_priv *priv, struct ib_device *hca)
{
	struct ib_device_attr *device_attr;
	int result = -ENOMEM;

	device_attr = kmalloc(sizeof *device_attr, GFP_KERNEL);
	if (!device_attr) {
		printk(KERN_WARNING "%s: allocation of %zu bytes failed\n",
		       hca->name, sizeof *device_attr);
		return result;
	}

	result = ib_query_device(hca, device_attr);
	if (result) {
		printk(KERN_WARNING "%s: ib_query_device failed (ret = %d)\n",
		       hca->name, result);
		kfree(device_attr);
		return result;
	}
	priv->hca_caps = device_attr->device_cap_flags;

	kfree(device_attr);

#if 0 /* XXX */
	if (priv->hca_caps & IB_DEVICE_UD_IP_CSUM) {
		set_bit(IPOIB_FLAG_CSUM, &priv->flags);
		priv->dev->features |= NETIF_F_SG | NETIF_F_IP_CSUM;
	}

	if (priv->dev->features & NETIF_F_SG && priv->hca_caps & IB_DEVICE_UD_TSO)
		priv->dev->features |= NETIF_F_TSO;
#endif

	return 0;
}


static struct ifnet *ipoib_add_port(const char *format,
					 struct ib_device *hca, u8 port)
{
	struct ipoib_dev_priv *priv;
	struct ib_port_attr attr;
	int result = -ENOMEM;

	priv = ipoib_intf_alloc(format);
	if (!priv)
		goto alloc_mem_failed;

	if (!ib_query_port(hca, port, &attr))
		priv->max_ib_mtu = ib_mtu_enum_to_int(attr.max_mtu);
	else {
		printk(KERN_WARNING "%s: ib_query_port %d failed\n",
		       hca->name, port);
		goto device_init_failed;
	}

	/* MTU will be reset when mcast join happens */
	priv->dev->if_mtu  = IPOIB_UD_MTU(priv->max_ib_mtu);
	priv->mcast_mtu  = priv->admin_mtu = priv->dev->if_mtu;

	result = ib_query_pkey(hca, port, 0, &priv->pkey);
	if (result) {
		printk(KERN_WARNING "%s: ib_query_pkey port %d failed (ret = %d)\n",
		       hca->name, port, result);
		goto device_init_failed;
	}

	if (ipoib_set_dev_features(priv, hca))
		goto device_init_failed;

	/*
	 * Set the full membership bit, so that we join the right
	 * broadcast group, etc.
	 */
	priv->pkey |= 0x8000;

	priv->broadcastaddr[8] = priv->pkey >> 8;
	priv->broadcastaddr[9] = priv->pkey & 0xff;

	result = ib_query_gid(hca, port, 0, &priv->local_gid);
	if (result) {
		printk(KERN_WARNING "%s: ib_query_gid port %d failed (ret = %d)\n",
		       hca->name, port, result);
		goto device_init_failed;
	} else
		memcpy(IF_LLADDR(priv->dev) + 4, priv->local_gid.raw, sizeof (union ib_gid));

	result = ipoib_dev_init(priv->dev, hca, port);
	if (result < 0) {
		printk(KERN_WARNING "%s: failed to initialize port %d (ret = %d)\n",
		       hca->name, port, result);
		goto device_init_failed;
	}

	INIT_IB_EVENT_HANDLER(&priv->event_handler,
			      priv->ca, ipoib_event);
	result = ib_register_event_handler(&priv->event_handler);
	if (result < 0) {
		printk(KERN_WARNING "%s: ib_register_event_handler failed for "
		       "port %d (ret = %d)\n",
		       hca->name, port, result);
		goto event_failed;
	}
	if_printf(priv->dev, "Attached to %s port %d\n", hca->name, port);

	return priv->dev;

event_failed:
	ipoib_dev_cleanup(priv->dev);

device_init_failed:
	ipoib_detach(priv->dev);

alloc_mem_failed:
	return ERR_PTR(result);
}

static void ipoib_add_one(struct ib_device *device)
{
	struct list_head *dev_list;
	struct ifnet *dev;
	struct ipoib_dev_priv *priv;
	int s, e, p;

	if (rdma_node_get_transport(device->node_type) != RDMA_TRANSPORT_IB)
		return;

	dev_list = kmalloc(sizeof *dev_list, GFP_KERNEL);
	if (!dev_list)
		return;

	INIT_LIST_HEAD(dev_list);

	if (device->node_type == RDMA_NODE_IB_SWITCH) {
		s = 0;
		e = 0;
	} else {
		s = 1;
		e = device->phys_port_cnt;
	}

	for (p = s; p <= e; ++p) {
		if (rdma_port_link_layer(device, p) != IB_LINK_LAYER_INFINIBAND)
			continue;
		dev = ipoib_add_port("ib", device, p);
		if (!IS_ERR(dev)) {
			priv = dev->if_softc;
			list_add_tail(&priv->list, dev_list);
		}
	}

	ib_set_client_data(device, &ipoib_client, dev_list);
}

static void ipoib_remove_one(struct ib_device *device)
{
	struct ipoib_dev_priv *priv, *tmp;
	struct list_head *dev_list;

	if (rdma_node_get_transport(device->node_type) != RDMA_TRANSPORT_IB)
		return;

	dev_list = ib_get_client_data(device, &ipoib_client);

	list_for_each_entry_safe(priv, tmp, dev_list, list) {
		if (rdma_port_link_layer(device, priv->port) != IB_LINK_LAYER_INFINIBAND)
			continue;

		ib_unregister_event_handler(&priv->event_handler);

		/* dev_change_flags(priv->dev, priv->dev->flags & ~IFF_UP); */

		flush_workqueue(ipoib_workqueue);

		ipoib_dev_cleanup(priv->dev);
		ipoib_detach(priv->dev);
	}

	kfree(dev_list);
}

static int __init ipoib_init_module(void)
{
	int ret;

	ipoib_recvq_size = roundup_pow_of_two(ipoib_recvq_size);
	ipoib_recvq_size = min(ipoib_recvq_size, IPOIB_MAX_QUEUE_SIZE);
	ipoib_recvq_size = max(ipoib_recvq_size, IPOIB_MIN_QUEUE_SIZE);

	ipoib_sendq_size = roundup_pow_of_two(ipoib_sendq_size);
	ipoib_sendq_size = min(ipoib_sendq_size, IPOIB_MAX_QUEUE_SIZE);
	ipoib_sendq_size = max(ipoib_sendq_size, max(2 * MAX_SEND_CQE,
						     IPOIB_MIN_QUEUE_SIZE));
#ifdef CONFIG_INFINIBAND_IPOIB_CM
	ipoib_max_conn_qp = min(ipoib_max_conn_qp, IPOIB_CM_MAX_CONN_QP);
#endif

	/*
	 * We create our own workqueue mainly because we want to be
	 * able to flush it when devices are being removed.  We can't
	 * use schedule_work()/flush_scheduled_work() because both
	 * unregister_netdev() and linkwatch_event take the rtnl lock,
	 * so flush_scheduled_work() can deadlock during device
	 * removal.
	 */
	ipoib_workqueue = create_singlethread_workqueue("ipoib");
	if (!ipoib_workqueue) {
		ret = -ENOMEM;
		goto err_fs;
	}

	ib_sa_register_client(&ipoib_sa_client);

	ret = ib_register_client(&ipoib_client);
	if (ret)
		goto err_sa;

	return 0;

err_sa:
	ib_sa_unregister_client(&ipoib_sa_client);
	destroy_workqueue(ipoib_workqueue);

err_fs:
	return ret;
}

static void __exit ipoib_cleanup_module(void)
{
	ib_unregister_client(&ipoib_client);
	ib_sa_unregister_client(&ipoib_sa_client);
	destroy_workqueue(ipoib_workqueue);
}

/*
 * Infiniband output routine.
 */
static int
ipoib_output(struct ifnet *ifp, struct mbuf *m,
	struct sockaddr *dst, struct route *ro)
{
	u_char edst[INFINIBAND_ALEN];
	struct llentry *lle = NULL;
	struct rtentry *rt0 = NULL;
	struct ipoib_header *eh;
	int error = 0;
	short type;

	if (ro != NULL) {
		if (!(m->m_flags & (M_BCAST | M_MCAST)))
			lle = ro->ro_lle;
		rt0 = ro->ro_rt;
	}
#ifdef MAC
	error = mac_ifnet_check_transmit(ifp, m);
	if (error)
		goto bad;
#endif

	M_PROFILE(m);
	if (ifp->if_flags & IFF_MONITOR) {
		error = ENETDOWN;
		goto bad;
	}
	if (!((ifp->if_flags & IFF_UP) &&
	    (ifp->if_drv_flags & IFF_DRV_RUNNING))) {
		error = ENETDOWN;
		goto bad;
	}

	switch (dst->sa_family) {
#ifdef INET
	case AF_INET:
		if (lle != NULL && (lle->la_flags & LLE_VALID))
			memcpy(edst, &lle->ll_addr.mac8, sizeof(edst));
		else
			error = arpresolve(ifp, rt0, m, dst, edst, &lle);
		if (error)
			return (error == EWOULDBLOCK ? 0 : error);
		type = htons(ETHERTYPE_IP);
		break;
	case AF_ARP:
	{
		struct arphdr *ah;
		ah = mtod(m, struct arphdr *);
		ah->ar_hrd = htons(ARPHRD_INFINIBAND);

		switch(ntohs(ah->ar_op)) {
		case ARPOP_REVREQUEST:
		case ARPOP_REVREPLY:
			type = htons(ETHERTYPE_REVARP);
			break;
		case ARPOP_REQUEST:
		case ARPOP_REPLY:
		default:
			type = htons(ETHERTYPE_ARP);
			break;
		}

		if (m->m_flags & M_BCAST)
			bcopy(ifp->if_broadcastaddr, edst, INFINIBAND_ALEN);
		else
			bcopy(ar_tha(ah), edst, INFINIBAND_ALEN);

	}
	break;
#endif
#ifdef INET6
	case AF_INET6:
		if (lle != NULL && (lle->la_flags & LLE_VALID))
			memcpy(edst, &lle->ll_addr.mac8, sizeof(edst));
		else
			error = nd6_storelladdr(ifp, m, dst, (u_char *)edst, &lle);
		if (error)
			return error;
		type = htons(ETHERTYPE_IPV6);
		break;
#endif

	default:
		if_printf(ifp, "can't handle af%d\n", dst->sa_family);
		error = EAFNOSUPPORT;
		goto bad;
	}

#if 0 /* XXX */
	if (lle != NULL && (lle->la_flags & LLE_IFADDR)) {
		int csum_flags = 0;
		if (m->m_pkthdr.csum_flags & CSUM_IP)
			csum_flags |= (CSUM_IP_CHECKED|CSUM_IP_VALID);
		if (m->m_pkthdr.csum_flags & CSUM_DELAY_DATA)
			csum_flags |= (CSUM_DATA_VALID|CSUM_PSEUDO_HDR);
		if (m->m_pkthdr.csum_flags & CSUM_SCTP)
			csum_flags |= CSUM_SCTP_VALID;
		m->m_pkthdr.csum_flags |= csum_flags;
		m->m_pkthdr.csum_data = 0xffff;
		return (if_simloop(ifp, m, dst->sa_family, 0));
	}
#endif

	/*
	 * Add local net header.  If no space in first mbuf,
	 * allocate another.
	 */
	M_PREPEND(m, IPOIB_HEADER_LEN, M_DONTWAIT);
	if (m == NULL) {
		error = ENOBUFS;
		goto bad;
	}
	eh = mtod(m, struct ipoib_header *);
	(void)memcpy(&eh->proto, &type, sizeof(eh->proto));
	(void)memcpy(&eh->hwaddr, edst, sizeof (edst));

	/*
	 * Queue message on interface, update output statistics if
	 * successful, and start output if interface not yet active.
	 */
	return ((ifp->if_transmit)(ifp, m));
bad:
	if (m != NULL)
		m_freem(m);
	return (error);
}

/*
 * Upper layer processing for a received Infiniband packet.
 */
static void
ipoib_demux(struct ifnet *ifp, struct mbuf *m, u_short proto)
{
	int isr;

	/*
	 * Dispatch frame to upper layer.
	 */
	switch (proto) {
#ifdef INET
	case ETHERTYPE_IP:
		isr = NETISR_IP;
		break;

	case ETHERTYPE_ARP:
		if (ifp->if_flags & IFF_NOARP) {
			/* Discard packet if ARP is disabled on interface */
			m_freem(m);
			return;
		}
		isr = NETISR_ARP;
		break;
#endif
#ifdef INET6
	case ETHERTYPE_IPV6:
		isr = NETISR_IPV6;
		break;
#endif
	default:
		goto discard;
	}
	netisr_dispatch(isr, m);
	return;

discard:
	m_freem(m);
}

/*
 * Process a received Infiniband packet.
 */
static void
ipoib_input(struct ifnet *ifp, struct mbuf *m)
{
	struct ipoib_header *eh;

	if ((ifp->if_flags & IFF_UP) == 0) {
		m_freem(m);
		return;
	}
	CURVNET_SET_QUIET(ifp->if_vnet);

	eh = mtod(m, struct ipoib_header *);
	/*
	 * Reset layer specific mbuf flags to avoid confusing upper layers.
	 * Strip off Infiniband header.
	 */
	m->m_flags &= ~M_VLANTAG;
	m->m_flags &= ~(M_PROTOFLAGS);
	m_adj(m, IPOIB_HEADER_LEN);

	if (IPOIB_IS_MULTICAST(eh->hwaddr)) {
		if (memcmp(eh->hwaddr, ifp->if_broadcastaddr,
		    ifp->if_addrlen) == 0)
			m->m_flags |= M_BCAST;
		else
			m->m_flags |= M_MCAST;
		ifp->if_imcasts++;
	}

#ifdef MAC
	/*
	 * Tag the mbuf with an appropriate MAC label before any other
	 * consumers can get to it.
	 */
	mac_ifnet_create_mbuf(ifp, m);
#endif

	/*
	 * Give bpf a chance at the packet.
	 */
	BPF_MTAP(ifp, m);

	/* Allow monitor mode to claim this frame, after stats are updated. */
	if (ifp->if_flags & IFF_MONITOR) {
		if_printf(ifp, "discard frame at IFF_MONITOR\n");
		m_freem(m);
		CURVNET_RESTORE();
		return;
	}
	ipoib_demux(ifp, m, ntohs(eh->proto));
	CURVNET_RESTORE();
}

static int
ipoib_resolvemulti(struct ifnet *ifp, struct sockaddr **llsa,
	struct sockaddr *sa)
{
	struct sockaddr_dl *sdl;
#ifdef INET
	struct sockaddr_in *sin;
#endif
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif
	u_char *e_addr;

	switch(sa->sa_family) {
	case AF_LINK:
		/*
		 * No mapping needed. Just check that it's a valid MC address.
		 */
		sdl = (struct sockaddr_dl *)sa;
		e_addr = LLADDR(sdl);
		if (!IPOIB_IS_MULTICAST(e_addr))
			return EADDRNOTAVAIL;
		*llsa = 0;
		return 0;

#ifdef INET
	case AF_INET:
		sin = (struct sockaddr_in *)sa;
		if (!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
			return EADDRNOTAVAIL;
		sdl = malloc(sizeof *sdl, M_IFMADDR,
		       M_NOWAIT|M_ZERO);
		if (sdl == NULL)
			return ENOMEM;
		sdl->sdl_len = sizeof *sdl;
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = ifp->if_index;
		sdl->sdl_type = IFT_INFINIBAND;
		sdl->sdl_alen = INFINIBAND_ALEN;
		e_addr = LLADDR(sdl);
		ETHER_MAP_IP_MULTICAST(&sin->sin_addr, e_addr);
		*llsa = (struct sockaddr *)sdl;
		return 0;
#endif
#ifdef INET6
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)sa;
		if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr)) {
			/*
			 * An IP6 address of 0 means listen to all
			 * of the Infiniband multicast address used for IP6.
			 * (This is used for multicast routers.)
			 */
			ifp->if_flags |= IFF_ALLMULTI;
			*llsa = 0;
			return 0;
		}
		if (!IN6_IS_ADDR_MULTICAST(&sin6->sin6_addr))
			return EADDRNOTAVAIL;
		sdl = malloc(sizeof *sdl, M_IFMADDR,
		       M_NOWAIT|M_ZERO);
		if (sdl == NULL)
			return (ENOMEM);
		sdl->sdl_len = sizeof *sdl;
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = ifp->if_index;
		sdl->sdl_type = IFT_INFINIBAND;
		sdl->sdl_alen = INFINIBAND_ALEN;
		e_addr = LLADDR(sdl);
		ETHER_MAP_IPV6_MULTICAST(&sin6->sin6_addr, e_addr);
		*llsa = (struct sockaddr *)sdl;
		return 0;
#endif

	default:
		/*
		 * Well, the text isn't quite right, but it's the name
		 * that counts...
		 */
		return EAFNOSUPPORT;
	}
}

module_init(ipoib_init_module);
module_exit(ipoib_cleanup_module);
