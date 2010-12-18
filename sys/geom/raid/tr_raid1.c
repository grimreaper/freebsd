/*-
 * Copyright (c) 2010 Alexander Motin <mav@FreeBSD.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
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
#include <sys/bio.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/kobj.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <geom/geom.h>
#include "geom/raid/g_raid.h"
#include "g_raid_tr_if.h"

static MALLOC_DEFINE(M_TR_raid1, "tr_raid1_data", "GEOM_RAID raid1 data");

struct g_raid_tr_raid1_object {
	struct g_raid_tr_object	 trso_base;
	int			 trso_starting;
	int			 trso_stopped;
};

static g_raid_tr_taste_t g_raid_tr_taste_raid1;
static g_raid_tr_event_t g_raid_tr_event_raid1;
static g_raid_tr_start_t g_raid_tr_start_raid1;
static g_raid_tr_stop_t g_raid_tr_stop_raid1;
static g_raid_tr_iostart_t g_raid_tr_iostart_raid1;
static g_raid_tr_iodone_t g_raid_tr_iodone_raid1;
static g_raid_tr_free_t g_raid_tr_free_raid1;

static kobj_method_t g_raid_tr_raid1_methods[] = {
	KOBJMETHOD(g_raid_tr_taste,	g_raid_tr_taste_raid1),
	KOBJMETHOD(g_raid_tr_event,	g_raid_tr_event_raid1),
	KOBJMETHOD(g_raid_tr_start,	g_raid_tr_start_raid1),
	KOBJMETHOD(g_raid_tr_stop,	g_raid_tr_stop_raid1),
	KOBJMETHOD(g_raid_tr_iostart,	g_raid_tr_iostart_raid1),
	KOBJMETHOD(g_raid_tr_iodone,	g_raid_tr_iodone_raid1),
	KOBJMETHOD(g_raid_tr_free,	g_raid_tr_free_raid1),
	{ 0, 0 }
};

struct g_raid_tr_class g_raid_tr_raid1_class = {
	"RAID1",
	g_raid_tr_raid1_methods,
	sizeof(struct g_raid_tr_raid1_object),
	.trc_priority = 100
};

static int
g_raid_tr_taste_raid1(struct g_raid_tr_object *tr, struct g_raid_volume *volume)
{
	struct g_raid_tr_raid1_object *trs;

	trs = (struct g_raid_tr_raid1_object *)tr;
	if (tr->tro_volume->v_raid_level != G_RAID_VOLUME_RL_RAID1 ||
	    tr->tro_volume->v_raid_level_qualifier != G_RAID_VOLUME_RLQ_NONE)
		return (G_RAID_TR_TASTE_FAIL);
	trs->trso_starting = 1;
	return (G_RAID_TR_TASTE_SUCCEED);
}

static int
g_raid_tr_update_state_raid1(struct g_raid_volume *vol)
{
	struct g_raid_tr_raid1_object *trs;
	u_int s;
	int n;

	trs = (struct g_raid_tr_raid1_object *)vol->v_tr;
	if (trs->trso_stopped)
		s = G_RAID_VOLUME_S_STOPPED;
	else {
		n = g_raid_nsubdisks(vol, G_RAID_SUBDISK_S_ACTIVE);
		if (n == vol->v_disks_count) {
			s = G_RAID_VOLUME_S_OPTIMAL;
			trs->trso_starting = 0;
		} else {
			if (trs->trso_starting)
				s = G_RAID_VOLUME_S_STARTING;
			else if (n > 0)
				s = G_RAID_VOLUME_S_DEGRADED;
			else
				s = G_RAID_VOLUME_S_BROKEN;
		}
	}
	if (s != vol->v_state) {
		g_raid_event_send(vol, G_RAID_VOLUME_S_ALIVE(s) ?
		    G_RAID_VOLUME_E_UP : G_RAID_VOLUME_E_DOWN,
		    G_RAID_EVENT_VOLUME);
		g_raid_change_volume_state(vol, s);
	}
	return (0);
}

static int
g_raid_tr_event_raid1(struct g_raid_tr_object *tr,
    struct g_raid_subdisk *sd, u_int event)
{
	struct g_raid_tr_raid1_object *trs;
	struct g_raid_volume *vol;

	trs = (struct g_raid_tr_raid1_object *)tr;
	vol = tr->tro_volume;
	if (event == G_RAID_SUBDISK_E_NEW)
		g_raid_change_subdisk_state(sd, G_RAID_SUBDISK_S_ACTIVE);
	else
		g_raid_change_subdisk_state(sd, G_RAID_SUBDISK_S_NONE);
	g_raid_tr_update_state_raid1(vol);
	return (0);
}

static int
g_raid_tr_start_raid1(struct g_raid_tr_object *tr)
{
	struct g_raid_tr_raid1_object *trs;
	struct g_raid_volume *vol;

	trs = (struct g_raid_tr_raid1_object *)tr;
	vol = tr->tro_volume;
	trs->trso_starting = 0;
	g_raid_tr_update_state_raid1(vol);
	return (0);
}

static int
g_raid_tr_stop_raid1(struct g_raid_tr_object *tr)
{
	struct g_raid_tr_raid1_object *trs;
	struct g_raid_volume *vol;

	trs = (struct g_raid_tr_raid1_object *)tr;
	vol = tr->tro_volume;
	trs->trso_starting = 0;
	trs->trso_stopped = 1;
	g_raid_tr_update_state_raid1(vol);
	return (0);
}

static void
g_raid_tr_iostart_raid1_read(struct g_raid_tr_object *tr, struct bio *bp)
{
	struct g_raid_softc *sc;
	struct g_raid_volume *vol;
	struct g_raid_subdisk *sd;
	struct bio *cbp;
	int i;

	vol = tr->tro_volume;
	sc = vol->v_softc;
	sd = NULL;
	for (i = 0; i < vol->v_disks_count; i++) {
		if (vol->v_subdisks[i].sd_state != G_RAID_SUBDISK_S_ACTIVE)
			continue;
		sd = &vol->v_subdisks[i];
	}
	KASSERT(sd != NULL, ("No active disks in volume %s.", vol->v_name));

	cbp = g_clone_bio(bp);
	if (cbp == NULL) {
		g_raid_iodone(bp, ENOMEM);
		return;
	}

	g_raid_subdisk_iostart(sd, cbp);
}

static void
g_raid_tr_iostart_raid1_write(struct g_raid_tr_object *tr, struct bio *bp)
{
	struct g_raid_softc *sc;
	struct g_raid_volume *vol;
	struct g_raid_subdisk *sd;
	struct bio_queue_head queue;
	struct bio *cbp;
	int i;

	vol = tr->tro_volume;
	sc = vol->v_softc;
	/*
	 * Allocate all bios before sending any request, so we can
	 * return ENOMEM in nice and clean way.
	 */
	bioq_init(&queue);
	for (i = 0; i < vol->v_disks_count; i++) {
		sd = &vol->v_subdisks[i];
		switch (sd->sd_state) {
		case G_RAID_SUBDISK_S_ACTIVE:
			break;
//		case G_RAID_DISK_STATE_SYNCHRONIZING:
//			if (bp->bio_offset >= sync->ds_offset)
//				continue;
//			break;
		default:
			continue;
		}
		cbp = g_clone_bio(bp);
		if (cbp == NULL) {
			for (cbp = bioq_first(&queue); cbp != NULL;
			    cbp = bioq_first(&queue)) {
				bioq_remove(&queue, cbp);
				g_destroy_bio(cbp);
			}
			if (bp->bio_error == 0)
				bp->bio_error = ENOMEM;
			g_raid_iodone(bp, bp->bio_error);
			return;
		}
		cbp->bio_caller1 = sd;
		bioq_insert_tail(&queue, cbp);
	}
	for (cbp = bioq_first(&queue); cbp != NULL;
	    cbp = bioq_first(&queue)) {
		bioq_remove(&queue, cbp);
		sd = cbp->bio_caller1;
		cbp->bio_caller1 = NULL;
		g_raid_subdisk_iostart(sd, cbp);
	}

}

static void
g_raid_tr_iostart_raid1(struct g_raid_tr_object *tr, struct bio *bp)
{
	struct g_raid_volume *vol;

	vol = tr->tro_volume;
	if (vol->v_state != G_RAID_VOLUME_S_OPTIMAL &&
	    vol->v_state != G_RAID_VOLUME_S_SUBOPTIMAL &&
	    vol->v_state != G_RAID_VOLUME_S_DEGRADED) {
		g_raid_iodone(bp, EIO);
		return;
	}
	switch (bp->bio_cmd) {
	case BIO_READ:
		g_raid_tr_iostart_raid1_read(tr, bp);
		break;
	case BIO_WRITE:
		g_raid_tr_iostart_raid1_write(tr, bp);
		break;
	case BIO_DELETE:
		g_raid_iodone(bp, EIO);
		break;
	default:
		KASSERT(1 == 0, ("Invalid command here: %u (volume=%s)",
		    bp->bio_cmd, vol->v_name));
		break;
	}
}

static void
g_raid_tr_iodone_raid1(struct g_raid_tr_object *tr,
    struct g_raid_subdisk *sd, struct bio *bp)
{
	struct bio *pbp;

	pbp = bp->bio_parent;
	pbp->bio_inbed++;
	if (pbp->bio_children == pbp->bio_inbed) {
		pbp->bio_completed = pbp->bio_length;
		g_raid_iodone(pbp, bp->bio_error);
	}
}

static int
g_raid_tr_free_raid1(struct g_raid_tr_object *tr)
{
	struct g_raid_tr_raid1_object *trs;

	trs = (struct g_raid_tr_raid1_object *)tr;

	return (0);
}

//G_RAID_TR_DECLARE(g_raid_tr_raid1);
