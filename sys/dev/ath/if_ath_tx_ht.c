/*-
 * Copyright (c) 2011 Adrian Chadd, Xenion Pty Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    similar to the "NO WARRANTY" disclaimer below ("Disclaimer") and any
 *    redistribution must be conditioned upon including a substantially
 *    similar Disclaimer requirement for further binary redistribution.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF NONINFRINGEMENT, MERCHANTIBILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGES.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"
#include "opt_ath.h"
#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/errno.h>
#include <sys/callout.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kthread.h>
#include <sys/taskqueue.h>
#include <sys/priv.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_llc.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_regdomain.h>
#ifdef IEEE80211_SUPPORT_SUPERG
#include <net80211/ieee80211_superg.h>
#endif
#ifdef IEEE80211_SUPPORT_TDMA
#include <net80211/ieee80211_tdma.h>
#endif

#include <net/bpf.h>

#ifdef INET
#include <netinet/in.h>
#include <netinet/if_ether.h>
#endif

#include <dev/ath/if_athvar.h>
#include <dev/ath/ath_hal/ah_devid.h>		/* XXX for softled */
#include <dev/ath/ath_hal/ah_diagcodes.h>

#ifdef ATH_TX99_DIAG
#include <dev/ath/ath_tx99/ath_tx99.h>
#endif

#include <dev/ath/if_ath_tx.h>		/* XXX for some support functions */
#include <dev/ath/if_ath_tx_ht.h>

/*
 * XXX net80211?
 */
#define	IEEE80211_AMPDU_SUBFRAME_DEFAULT		32

#define	ATH_AGGR_DELIM_SZ	4	/* delimiter size   */
#define	ATH_AGGR_MINPLEN	256	/* in bytes, minimum packet length */
#define	ATH_AGGR_ENCRYPTDELIM	10	/* number of delimiters for encryption padding */

/*
 * returns delimiter padding required given the packet length
 */
#define	ATH_AGGR_GET_NDELIM(_len)					\
	    (((((_len) + ATH_AGGR_DELIM_SZ) < ATH_AGGR_MINPLEN) ?	\
	    (ATH_AGGR_MINPLEN - (_len) - ATH_AGGR_DELIM_SZ) : 0) >> 2)

#define	PADBYTES(_len)		((4 - ((_len) % 4)) % 4)

int ath_max_4ms_framelen[4][32] = {
	[MCS_HT20] = {
		3212,  6432,  9648,  12864,  19300,  25736,  28952,  32172,
		6424,  12852, 19280, 25708,  38568,  51424,  57852,  64280,
		9628,  19260, 28896, 38528,  57792,  65532,  65532,  65532,
		12828, 25656, 38488, 51320,  65532,  65532,  65532,  65532,
	},
	[MCS_HT20_SGI] = {
		3572,  7144,  10720,  14296,  21444,  28596,  32172,  35744,
		7140,  14284, 21428,  28568,  42856,  57144,  64288,  65532,
		10700, 21408, 32112,  42816,  64228,  65532,  65532,  65532,
		14256, 28516, 42780,  57040,  65532,  65532,  65532,  65532,
	},
	[MCS_HT40] = {
		6680,  13360,  20044,  26724,  40092,  53456,  60140,  65532,
		13348, 26700,  40052,  53400,  65532,  65532,  65532,  65532,
		20004, 40008,  60016,  65532,  65532,  65532,  65532,  65532,
		26644, 53292,  65532,  65532,  65532,  65532,  65532,  65532,
	},
	[MCS_HT40_SGI] = {
		7420,  14844,  22272,  29696,  44544,  59396,  65532,  65532,
		14832, 29668,  44504,  59340,  65532,  65532,  65532,  65532,
		22232, 44464,  65532,  65532,  65532,  65532,  65532,  65532,
		29616, 59232,  65532,  65532,  65532,  65532,  65532,  65532,
	}
};

/*
 * Return the number of delimiters to be added to
 * meet the minimum required mpdudensity.
 * Caller should make sure that the rate is HT.
 *
 * TODO: is this delimiter calculation supposed to be the
 * total frame length, the hdr length, the data length (including
 * delimiters, padding, CRC, etc) or ?
 */
static int
ath_compute_num_delims(struct ath_softc *sc, struct ath_buf *first_bf,
    uint16_t pktlen)
{
	int ndelim;

	/* Select standard number of delimiters based on frame length */
	ndelim = ATH_AGGR_GET_NDELIM(pktlen);

	/*
	 * If encryption is enabled, add extra delimiters to let the
	 * crypto hardware catch up. This could be tuned per-MAC and
	 * per-rate, but for now we'll simply assume encryption is
	 * always enabled.
	 */
	ndelim += ATH_AGGR_ENCRYPTDELIM;

	/*
	 * If the MPDU density is 0, we can return here.
	 * Otherwise, we need to convert the desired mpdudensity
	 * into a byte length, based on the rate in the subframe.
	 *
	 * XXX TODO: since the rate scenario hasn't been configured
	 * XXX yet, this is likely not going to work. So, for now,
	 * XXX ignore it.
	 */
	return ndelim;
}

/*
 * Setup a 11n rate series structure
 *
 * This should be called for both legacy and MCS rates.
 *
 * It, along with ath_buf_set_rate, must be called -after- a burst
 * or aggregate is setup.
 */
static void
ath_rateseries_setup(struct ath_softc *sc, struct ieee80211_node *ni,
    struct ath_buf *bf, HAL_11N_RATE_SERIES *series)
{
#define	HT_RC_2_STREAMS(_rc)	((((_rc) & 0x78) >> 3) + 1)
	struct ieee80211com *ic = ni->ni_ic;
	struct ath_hal *ah = sc->sc_ah;
	HAL_BOOL shortPreamble = AH_FALSE;
	const HAL_RATE_TABLE *rt = sc->sc_currates;
	int i;
	int pktlen;
	int flags = bf->bf_state.bfs_flags;
	struct ath_rc_series *rc = bf->bf_state.bfs_rc;

	if ((ic->ic_flags & IEEE80211_F_SHPREAMBLE) &&
	    (ni->ni_capinfo & IEEE80211_CAPINFO_SHORT_PREAMBLE))
		shortPreamble = AH_TRUE;

	/*
	 * If this is the first frame in an aggregate series,
	 * use the aggregate length.
	 */
	if (bf->bf_state.bfs_aggr)
		pktlen = bf->bf_state.bfs_al;
	else
		pktlen = bf->bf_state.bfs_pktlen;

	memset(series, 0, sizeof(HAL_11N_RATE_SERIES) * 4);
	for (i = 0; i < 4;  i++) {
		/* Only set flags for actual TX attempts */
		if (rc[i].tries == 0)
			continue;

		series[i].Tries = rc[i].tries;

		/*
		 * XXX this isn't strictly correct - sc_txchainmask
		 * XXX isn't the currently active chainmask;
		 * XXX it's the interface chainmask at startup.
		 * XXX It's overridden in the HAL rate scenario function
		 * XXX for now.
		 */
		series[i].ChSel = sc->sc_txchainmask;

		if (flags & (HAL_TXDESC_RTSENA | HAL_TXDESC_CTSENA))
			series[i].RateFlags |= HAL_RATESERIES_RTS_CTS;

		/*
		 * Transmit 40MHz frames only if the node has negotiated
		 * it rather than whether the node is capable of it or not.
	 	 * It's subtly different in the hostap case.
	 	 */
		if (ni->ni_chw == 40)
			series[i].RateFlags |= HAL_RATESERIES_2040;

		/*
		 * Set short-GI only if the node has advertised it
		 * the channel width is suitable, and we support it.
		 * We don't currently have a "negotiated" set of bits -
		 * ni_htcap is what the remote end sends, not what this
		 * node is capable of.
		 */
		if (ni->ni_chw == 40 &&
		    ic->ic_htcaps & IEEE80211_HTCAP_SHORTGI40 &&
		    ni->ni_htcap & IEEE80211_HTCAP_SHORTGI40)
			series[i].RateFlags |= HAL_RATESERIES_HALFGI;

		if (ni->ni_chw == 20 &&
		    ic->ic_htcaps & IEEE80211_HTCAP_SHORTGI20 &&
		    ni->ni_htcap & IEEE80211_HTCAP_SHORTGI20)
			series[i].RateFlags |= HAL_RATESERIES_HALFGI;

		series[i].Rate = rt->info[rc[i].rix].rateCode;

		/* PktDuration doesn't include slot, ACK, RTS, etc timing - it's just the packet duration */
		if (series[i].Rate & IEEE80211_RATE_MCS) {
			series[i].PktDuration =
			    ath_computedur_ht(pktlen
				, series[i].Rate
				, HT_RC_2_STREAMS(series[i].Rate)
				, series[i].RateFlags & HAL_RATESERIES_2040
				, series[i].RateFlags & HAL_RATESERIES_HALFGI);
		} else {
			if (shortPreamble)
				series[i].Rate |=
				    rt->info[rc[i].rix].shortPreamble;
			series[i].PktDuration = ath_hal_computetxtime(ah,
			    rt, pktlen, rc[i].rix, shortPreamble);
		}
	}
#undef	HT_RC_2_STREAMS
}

#if 0
static void
ath_rateseries_print(HAL_11N_RATE_SERIES *series)
{
	int i;
	for (i = 0; i < 4; i++) {
		printf("series %d: rate %x; tries %d; pktDuration %d; chSel %d; rateFlags %x\n",
		    i,
		    series[i].Rate,
		    series[i].Tries,
		    series[i].PktDuration,
		    series[i].ChSel,
		    series[i].RateFlags);
	}
}
#endif

/*
 * Setup the 11n rate scenario and burst duration for the given TX descriptor
 * list.
 *
 * This isn't useful for sending beacon frames, which has different needs
 * wrt what's passed into the rate scenario function.
 */

void
ath_buf_set_rate(struct ath_softc *sc, struct ieee80211_node *ni,
    struct ath_buf *bf)
{
	HAL_11N_RATE_SERIES series[4];
	struct ath_desc *ds = bf->bf_desc;
	struct ath_desc *lastds = NULL;
	struct ath_hal *ah = sc->sc_ah;
	int is_pspoll = (bf->bf_state.bfs_atype == HAL_PKT_TYPE_PSPOLL);
	int ctsrate = bf->bf_state.bfs_ctsrate;
	int flags = bf->bf_state.bfs_flags;

	/* Setup rate scenario */
	memset(&series, 0, sizeof(series));

	ath_rateseries_setup(sc, ni, bf, series);

	/* Enforce AR5416 aggregate limit - can't do RTS w/ an agg frame > 8k */

	/* Enforce RTS and CTS are mutually exclusive */

	/* Get a pointer to the last tx descriptor in the list */
	lastds = bf->bf_lastds;

#if 0
	printf("pktlen: %d; flags 0x%x\n", pktlen, flags);
	ath_rateseries_print(series);
#endif

	/* Set rate scenario */
	ath_hal_set11nratescenario(ah, ds,
	    !is_pspoll,	/* whether to override the duration or not */
			/* don't allow hardware to override the duration on ps-poll packets */
	    ctsrate,	/* rts/cts rate */
	    series,	/* 11n rate series */
	    4,		/* number of series */
	    flags);

	/* Setup the last descriptor in the chain */
	ath_hal_setuplasttxdesc(ah, lastds, ds);

	/* Set burst duration */
	/*
	 * This is only required when doing 11n burst, not aggregation
	 * ie, if there's a second frame in a RIFS or A-MPDU burst
	 * w/ >1 A-MPDU frame bursting back to back.
	 * Normal A-MPDU doesn't do bursting -between- aggregates.
	 *
	 * .. and it's highly likely this won't ever be implemented
	 */
	//ath_hal_set11nburstduration(ah, ds, 8192);
}

/*
 * Form an aggregate packet list.
 *
 * This function enforces the aggregate restrictions/requirements.
 *
 * These are:
 *
 * + The aggregate size maximum (64k for AR9160 and later, 8K for
 *   AR5416 when doing RTS frame protection.)
 * + Maximum number of sub-frames for an aggregate
 * + The aggregate delimiter size, giving MACs time to do whatever is
 *   needed before each frame
 * + Enforce the BAW limit
 *
 * Each descriptor queued should have the DMA setup.
 * The rate series, descriptor setup, linking, etc is all done
 * externally. This routine simply chains them together.
 * ath_tx_setds_11n() will take care of configuring the per-
 * descriptor setup, and ath_buf_set_rate() will configure the
 * rate control.
 *
 * Note that the TID lock is only grabbed when dequeuing packets from
 * the TID queue. If some code in another thread adds to the head of this
 * list, very strange behaviour will occur. Since retransmission is the
 * only reason this will occur, and this routine is designed to be called
 * from within the scheduler task, it won't ever clash with the completion
 * task.
 *
 * So if you want to call this from an upper layer context (eg, to direct-
 * dispatch aggregate frames to the hardware), please keep this in mind.
 */
ATH_AGGR_STATUS
ath_tx_form_aggr(struct ath_softc *sc, struct ath_node *an, struct ath_tid *tid,
    ath_bufhead *bf_q)
{
	//struct ieee80211_node *ni = &an->an_node;
	struct ath_buf *bf, *bf_first = NULL, *bf_prev = NULL;
	int nframes = 0;
	uint16_t aggr_limit = 0, al = 0, bpad = 0, al_delta, h_baw;
	struct ieee80211_tx_ampdu *tap;
	int status = ATH_AGGR_DONE;
	int prev_frames = 0;	/* XXX for AR5416 burst, not done here */
	int prev_al = 0;	/* XXX also for AR5416 burst */

	tap = ath_tx_get_tx_tid(an, tid->tid);
	if (tap == NULL) {
		status = ATH_AGGR_ERROR;
		goto finish;
	}

	h_baw = tap->txa_wnd / 2;

	/* Calculate aggregation limit */
	/*
	 * XXX TODO: do not exceed 4ms packet length
	 */
	aggr_limit = 65530;		/* XXX just for now, for testing */

	for (;;) {
		ATH_TXQ_LOCK(tid);
		bf = TAILQ_FIRST(&tid->axq_q);
		if (bf_first == NULL)
			bf_first = bf;
		if (bf == NULL) {
			ATH_TXQ_UNLOCK(tid);
			status = ATH_AGGR_DONE;
			break;
		}

		/* Set this early just so things don't get confused */
		bf->bf_next = NULL;

		/*
		 * Don't unlock the tid lock until we're sure we are going
		 * to queue this frame.
		 */

		/*
		 * If the frame doesn't have a sequence number that we're
		 * tracking in the BAW (eg NULL QOS data frame), we can't
		 * aggregate it. Stop the aggregation process; the sender
		 * can then TX what's in the list thus far and then
		 * TX the frame individually.
		 */
		if (! bf->bf_state.bfs_dobaw) {
			ATH_TXQ_UNLOCK(tid);
			status = ATH_AGGR_NONAGGR;
			break;
		}

		/*
		 * If the packet has a sequence number, do not
		 * step outside of the block-ack window.
		 */
		if (! BAW_WITHIN(tap->txa_start, tap->txa_wnd,
		    SEQNO(bf->bf_state.bfs_seqno))) {
		    ATH_TXQ_UNLOCK(tid);
		    status = ATH_AGGR_BAW_CLOSED;
		    break;
		}

		/*
		 * XXX TODO: AR5416 has an 8K aggregation size limit
		 * when RTS is enabled, and RTS is required for dual-stream
		 * rates.
		 *
		 * For now, limit all aggregates for the AR5416 to be 8K.
		 */

		/*
		 * do not exceed aggregation limit
		 */
		al_delta = ATH_AGGR_DELIM_SZ + bf->bf_state.bfs_pktlen;
		if (nframes &&
		    (aggr_limit < (al + bpad + al_delta + prev_al))) {
			ATH_TXQ_UNLOCK(tid);
			status = ATH_AGGR_LIMITED;
			break;
		}

		/*
		 * Do not exceed subframe limit.
		 */
		if ((nframes + prev_frames) >= MIN((h_baw),
		    IEEE80211_AMPDU_SUBFRAME_DEFAULT)) {
			ATH_TXQ_UNLOCK(tid);
			status = ATH_AGGR_LIMITED;
			break;
		}


		/*
		 * this packet is part of an aggregate.
		 */
		ATH_TXQ_REMOVE(tid, bf, bf_list);
		ATH_TXQ_UNLOCK(tid);

		ath_tx_addto_baw(sc, an, tid, bf);
		TAILQ_INSERT_TAIL(bf_q, bf, bf_list);
		nframes ++;

		/* Completion handler */
		bf->bf_comp = ath_tx_aggr_comp;

		/*
		 * add padding for previous frame to aggregation length
		 */
		al += bpad + al_delta;

		/*
		 * Calculate delimiters needed for the current frame
		 */
		bf->bf_state.bfs_ndelim =
		    ath_compute_num_delims(sc, bf_first,
		    bf->bf_state.bfs_pktlen);

		/*
		 * Calculate the padding needed from this set of delimiters,
		 * used when calculating if the next frame will fit in
		 * the aggregate.
		 */
		bpad = PADBYTES(al_delta) + (bf->bf_state.bfs_ndelim << 2);

		/*
		 * Chain the buffers together
		 */
		if (bf_prev)
			bf_prev->bf_next = bf;
		bf_prev = bf;

		/*
		 * XXX TODO: if any sub-frames have RTS/CTS enabled;
		 * enable it for the entire aggregate.
		 */

#if 0
		/*
		 * terminate aggregation on a small packet boundary
		 */
		if (bf->bf_state.bfs_pktlen < ATH_AGGR_MINPLEN) {
			status = ATH_AGGR_SHORTPKT;
			break;
		}
#endif

	}

finish:
	/*
	 * Just in case the list was empty when we tried to
	 * dequeue a packet ..
	 */
	if (bf_first) {
		bf_first->bf_state.bfs_al = al;
		bf_first->bf_state.bfs_nframes = nframes;
	}
	return status;
}
