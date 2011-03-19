/*-
 * Copyright (c) 2011 Alexander Motin <mav@FreeBSD.org>
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
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <geom/geom.h>
#include "geom/raid/g_raid.h"
#include "g_raid_md_if.h"

static MALLOC_DEFINE(M_MD_PROMISE, "md_promise_data", "GEOM_RAID Promise metadata");

#define	PROMISE_MAX_DISKS	8
#define	PROMISE_MAX_SUBDISKS	2
#define	PROMISE_META_OFFSET	14

struct promise_raid_disk {
	uint8_t		flags;			/* Subdisk status. */
#define PROMISE_F_VALID              0x00000001
#define PROMISE_F_ONLINE             0x00000002
#define PROMISE_F_ASSIGNED           0x00000004
#define PROMISE_F_SPARE              0x00000008
#define PROMISE_F_DUPLICATE          0x00000010
#define PROMISE_F_REDIR              0x00000020
#define PROMISE_F_DOWN               0x00000040
#define PROMISE_F_READY              0x00000080

	uint8_t		number;			/* Position in a volume. */
	uint8_t		channel;		/* ATA channel number. */
	uint8_t		device;			/* ATA device number. */
	uint64_t	id __packed;		/* Subdisk ID. */
} __packed;

struct promise_raid_conf {
	char		promise_id[24];
#define PROMISE_MAGIC                "Promise Technology, Inc."
#define FREEBSD_MAGIC                "FreeBSD ATA driver RAID "

	uint32_t	dummy_0;
	uint64_t	magic_0;
#define PROMISE_MAGIC0(x)            (((uint64_t)(x.channel) << 48) | \
				((uint64_t)(x.device != 0) << 56))
	uint16_t	magic_1;
	uint32_t	magic_2;
	uint8_t		filler1[470];

	uint32_t	integrity;
#define PROMISE_I_VALID              0x00000080

	struct promise_raid_disk	disk;	/* This subdisk info. */
	uint32_t	disk_offset;		/* Subdisk offset. */
	uint32_t	disk_sectors;		/* Subdisk size */
	uint32_t	rebuild_lba;		/* Rebuild position. */
	uint16_t	generation;		/* Generation number. */
	uint8_t		status;			/* Volume status. */
#define PROMISE_S_VALID              0x01
#define PROMISE_S_ONLINE             0x02
#define PROMISE_S_INITED             0x04
#define PROMISE_S_READY              0x08
#define PROMISE_S_DEGRADED           0x10
#define PROMISE_S_MARKED             0x20
#define PROMISE_S_MIGRATING          0x40
#define PROMISE_S_FUNCTIONAL         0x80

	uint8_t		type;			/* Voluem type. */
#define PROMISE_T_RAID0              0x00
#define PROMISE_T_RAID1              0x01
#define PROMISE_T_RAID3              0x02
#define PROMISE_T_RAID5              0x04
#define PROMISE_T_SPAN               0x08
#define PROMISE_T_JBOD               0x10

	uint8_t		total_disks;		/* Disks in this volume. */
	uint8_t		stripe_shift;		/* Strip size. */
	uint8_t		array_width;		/* Number of RAID0 stripes. */
	uint8_t		array_number;		/* Global volume number. */
	uint32_t	total_sectors;		/* Volume size. */
	uint16_t	cylinders;		/* Volume geometry: C. */
	uint8_t		heads;			/* Volume geometry: H. */
	uint8_t		sectors;		/* Volume geometry: S. */
	uint64_t	volume_id __packed;	/* Volume ID, */
	struct promise_raid_disk	disks[PROMISE_MAX_DISKS];
						/* Subdisks in this volume. */
	char		name[32];		/* Volume label. */

	uint32_t	filler2[8];
	uint32_t	magic_3;	/* Something related to rebuild. */
	uint64_t	rebuild_lba64;	/* Per-volume rebuild position. */
	uint32_t	magic_4;
	uint32_t	magic_5;
	uint32_t	filler3[325];
	uint32_t	checksum;
} __packed;

struct g_raid_md_promise_perdisk {
	int		 pd_updated;
	int		 pd_subdisks;
	struct promise_raid_conf	*pd_meta[PROMISE_MAX_SUBDISKS];
};

struct g_raid_md_promise_pervolume {
	struct promise_raid_conf	*pv_meta;
	uint64_t			 pv_id;
	uint16_t			 pv_generation;
	int				 pv_disks_present;
	int				 pv_started;
	struct callout			 pv_start_co;	/* STARTING state timer. */
	struct root_hold_token		*pv_rootmount; /* Root mount delay token. */
};

struct g_raid_md_promise_object {
	struct g_raid_md_object	 mdio_base;
	int			 mdio_disks_present;
	int			 mdio_started;
	int			 mdio_incomplete;
};

static g_raid_md_create_t g_raid_md_create_promise;
static g_raid_md_taste_t g_raid_md_taste_promise;
static g_raid_md_event_t g_raid_md_event_promise;
static g_raid_md_volume_event_t g_raid_md_volume_event_promise;
static g_raid_md_ctl_t g_raid_md_ctl_promise;
static g_raid_md_write_t g_raid_md_write_promise;
static g_raid_md_fail_disk_t g_raid_md_fail_disk_promise;
static g_raid_md_free_disk_t g_raid_md_free_disk_promise;
static g_raid_md_free_volume_t g_raid_md_free_volume_promise;
static g_raid_md_free_t g_raid_md_free_promise;

static kobj_method_t g_raid_md_promise_methods[] = {
	KOBJMETHOD(g_raid_md_create,	g_raid_md_create_promise),
	KOBJMETHOD(g_raid_md_taste,	g_raid_md_taste_promise),
	KOBJMETHOD(g_raid_md_event,	g_raid_md_event_promise),
	KOBJMETHOD(g_raid_md_volume_event,	g_raid_md_volume_event_promise),
	KOBJMETHOD(g_raid_md_ctl,	g_raid_md_ctl_promise),
	KOBJMETHOD(g_raid_md_write,	g_raid_md_write_promise),
	KOBJMETHOD(g_raid_md_fail_disk,	g_raid_md_fail_disk_promise),
	KOBJMETHOD(g_raid_md_free_disk,	g_raid_md_free_disk_promise),
	KOBJMETHOD(g_raid_md_free_volume,	g_raid_md_free_volume_promise),
	KOBJMETHOD(g_raid_md_free,	g_raid_md_free_promise),
	{ 0, 0 }
};

static struct g_raid_md_class g_raid_md_promise_class = {
	"Promise",
	g_raid_md_promise_methods,
	sizeof(struct g_raid_md_promise_object),
	.mdc_priority = 100
};


static void
g_raid_md_promise_print(struct promise_raid_conf *meta)
{
	int i;

	if (g_raid_debug < 1)
		return;

	printf("********* ATA Promise Metadata *********\n");
	printf("promise_id          <%.24s>\n", meta->promise_id);
	printf("disk                %02x %02x %02x %02x %016jx\n",
	    meta->disk.flags, meta->disk.number, meta->disk.channel,
	    meta->disk.device, meta->disk.id);
	printf("disk_offset         %u\n", meta->disk_offset);
	printf("disk_sectors        %u\n", meta->disk_sectors);
	printf("rebuild_lba         %u\n", meta->rebuild_lba);
	printf("generation          %u\n", meta->generation);
	printf("status              0x%02x\n", meta->status);
	printf("type                %u\n", meta->type);
	printf("total_disks         %u\n", meta->total_disks);
	printf("stripe_shift        %u\n", meta->stripe_shift);
	printf("array_width         %u\n", meta->array_width);
	printf("array_number        %u\n", meta->array_number);
	printf("total_sectors       %u\n", meta->total_sectors);
	printf("cylinders           %u\n", meta->cylinders);
	printf("heads               %u\n", meta->heads);
	printf("sectors             %u\n", meta->sectors);
	printf("volume_id           0x%016jx\n", meta->volume_id);
	printf("disks:\n");
	for (i = 0; i < PROMISE_MAX_DISKS; i++ ) {
		printf("                    %02x %02x %02x %02x %016jx\n",
		    meta->disks[i].flags, meta->disks[i].number,
		    meta->disks[i].channel, meta->disks[i].device,
		    meta->disks[i].id);
	}
	printf("name                <%.32s>\n", meta->name);
	printf("magic_3             0x%08x\n", meta->magic_3);
	printf("rebuild_lba64       %ju\n", meta->rebuild_lba64);
	printf("magic_4             0x%08x\n", meta->magic_4);
	printf("magic_5             0x%08x\n", meta->magic_5);
	printf("=================================================\n");
}

static struct promise_raid_conf *
promise_meta_copy(struct promise_raid_conf *meta)
{
	struct promise_raid_conf *nmeta;

	nmeta = malloc(sizeof(*nmeta), M_MD_PROMISE, M_WAITOK);
	memcpy(nmeta, meta, sizeof(*nmeta));
	return (nmeta);
}

static int
promise_meta_find_disk(struct promise_raid_conf *meta, uint64_t id)
{
	int pos;

	for (pos = 0; pos < meta->total_disks; pos++) {
		if (meta->disks[pos].id == id)
			return (pos);
	}
	return (-1);
}

static int
promise_meta_translate_disk(struct g_raid_volume *vol, int md_disk_pos)
{
	int disk_pos, width;

	if (md_disk_pos >= 0 && vol->v_raid_level == G_RAID_VOLUME_RL_RAID1E) {
		width = vol->v_disks_count / 2;
		disk_pos = (md_disk_pos / width) +
		    (md_disk_pos % width) * width;
	} else
		disk_pos = md_disk_pos;
	return (disk_pos);
}

static void
promise_meta_get_name(struct promise_raid_conf *meta, char *buf)
{
	int i;

	strncpy(buf, meta->name, 32);
	buf[32] = 0;
	for (i = 31; i >= 0; i--) {
		if (buf[i] > 0x20)
			break;
		buf[i] = 0;
	}
}

static void
promise_meta_put_name(struct promise_raid_conf *meta, char *buf)
{

	memset(meta->name, 0x20, 32);
	memcpy(meta->name, buf, MIN(strlen(buf), 32));
}

static int
promise_meta_read(struct g_consumer *cp, struct promise_raid_conf **metaarr)
{
	struct g_provider *pp;
	struct promise_raid_conf *meta;
	char *buf;
	int error, i, subdisks;
	uint32_t checksum, *ptr;

	pp = cp->provider;
	subdisks = 0;
next:
	/* Read metadata block. */
	buf = g_read_data(cp, pp->mediasize - pp->sectorsize *
	    (63 - subdisks * PROMISE_META_OFFSET),
	    pp->sectorsize * 4, &error);
	if (buf == NULL) {
		G_RAID_DEBUG(1, "Cannot read metadata from %s (error=%d).",
		    pp->name, error);
		return (subdisks);
	}
	meta = (struct promise_raid_conf *)buf;

	/* Check if this is an Promise RAID struct */
	if (strncmp(meta->promise_id, PROMISE_MAGIC, strlen(PROMISE_MAGIC)) &&
	    strncmp(meta->promise_id, FREEBSD_MAGIC, strlen(FREEBSD_MAGIC))) {
		if (subdisks == 0)
			G_RAID_DEBUG(1,
			    "Promise signature check failed on %s", pp->name);
		g_free(buf);
		return (subdisks);
	}
	meta = malloc(sizeof(*meta), M_MD_PROMISE, M_WAITOK);
	memcpy(meta, buf, MIN(sizeof(*meta), pp->sectorsize * 4));
	g_free(buf);

	/* Check metadata checksum. */
	for (checksum = 0, ptr = (uint32_t *)meta, i = 0; i < 511; i++)
		checksum += *ptr++;
	if (checksum != meta->checksum) {
		G_RAID_DEBUG(1, "Promise checksum check failed on %s", pp->name);
		free(meta, M_MD_PROMISE);
		return (subdisks);
	}

	if ((meta->integrity & PROMISE_I_VALID) == 0) {
		G_RAID_DEBUG(1, "Promise metadata is invalid on %s", pp->name);
		free(meta, M_MD_PROMISE);
		return (subdisks);
	}

	if (meta->total_disks > PROMISE_MAX_DISKS) {
		G_RAID_DEBUG(1, "Wrong number of disks on %s (%d)",
		    pp->name, meta->total_disks);
		free(meta, M_MD_PROMISE);
		return (subdisks);
	}

	/* Save this part and look for next. */
	*metaarr = meta;
	metaarr++;
	subdisks++;
	if (subdisks < PROMISE_MAX_SUBDISKS)
		goto next;

	return (subdisks);
}

static int
promise_meta_write(struct g_consumer *cp,
    struct promise_raid_conf **metaarr, int nsd)
{
	struct g_provider *pp;
	struct promise_raid_conf *meta;
	char *buf;
	int error, i, subdisk;
	uint32_t checksum, *ptr;

	pp = cp->provider;
	subdisk = 0;
next:
	buf = malloc(pp->sectorsize * 4, M_MD_PROMISE, M_WAITOK | M_ZERO);
	if (subdisk < nsd) {
		meta = metaarr[subdisk];
		/* Recalculate checksum for case if metadata were changed. */
		meta->checksum = 0;
		for (checksum = 0, ptr = (uint32_t *)meta, i = 0; i < 511; i++)
			checksum += *ptr++;
		meta->checksum = checksum;
		memcpy(buf, meta, MIN(pp->sectorsize * 4, sizeof(*meta)));
	}
	error = g_write_data(cp, pp->mediasize - pp->sectorsize *
	    (63 - subdisk * PROMISE_META_OFFSET),
	    buf, pp->sectorsize * 4);
	if (error != 0) {
		G_RAID_DEBUG(1, "Cannot write metadata to %s (error=%d).",
		    pp->name, error);
	}
	free(buf, M_MD_PROMISE);

	subdisk++;
	if (subdisk < PROMISE_MAX_SUBDISKS)
		goto next;

	return (error);
}

static int
promise_meta_erase(struct g_consumer *cp)
{
	struct g_provider *pp;
	char *buf;
	int error, subdisk;

	pp = cp->provider;
	buf = malloc(4 * pp->sectorsize, M_MD_PROMISE, M_WAITOK | M_ZERO);
	for (subdisk = 0; subdisk < PROMISE_MAX_SUBDISKS; subdisk++) {
		error = g_write_data(cp, pp->mediasize - pp->sectorsize *
		    (63 - subdisk * PROMISE_META_OFFSET),
		    buf, 4 * pp->sectorsize);
		if (error != 0) {
			G_RAID_DEBUG(1, "Cannot erase metadata on %s (error=%d).",
			    pp->name, error);
		}
	}
	free(buf, M_MD_PROMISE);
	return (error);
}

#if 0
static int
promise_meta_write_spare(struct g_consumer *cp, struct promise_raid_disk *d)
{
	struct promise_raid_conf *meta;
	int error;

	/* Fill anchor and single disk. */
	meta = malloc(sizeof(*meta), M_MD_PROMISE, M_WAITOK | M_ZERO);
	memcpy(&meta->promise_id[0], PROMISE_MAGIC, sizeof(PROMISE_MAGIC));
	memcpy(&meta->version[0], PROMISE_VERSION_1000,
	    sizeof(PROMISE_VERSION_1000));
	meta->generation = 1;
	meta->total_disks = 1;
	meta->disk[0] = *d;
	error = promise_meta_write(cp, meta);
	free(meta, M_MD_PROMISE);
	return (error);
}
#endif

static struct g_raid_volume *
g_raid_md_promise_get_volume(struct g_raid_softc *sc, uint64_t id)
{
	struct g_raid_volume	*vol;
	struct g_raid_md_promise_pervolume *pv;

	TAILQ_FOREACH(vol, &sc->sc_volumes, v_next) {
		pv = vol->v_md_data;
		if (pv->pv_id == id)
			break;
	}
	return (vol);
}

static int
g_raid_md_promise_supported(int level, int qual, int disks, int force)
{

	if (disks > PROMISE_MAX_DISKS)
		return (0);
	switch (level) {
	case G_RAID_VOLUME_RL_RAID0:
		if (disks < 1)
			return (0);
		if (!force && disks < 2)
			return (0);
		break;
	case G_RAID_VOLUME_RL_RAID1:
		if (disks < 1)
			return (0);
		if (!force && (disks != 2))
			return (0);
		break;
	case G_RAID_VOLUME_RL_RAID1E:
		if (disks < 2)
			return (0);
		if (disks % 2 != 0)
			return (0);
		if (!force && (disks != 4))
			return (0);
		break;
	case G_RAID_VOLUME_RL_SINGLE:
		if (disks != 1)
			return (0);
		if (!force)
			return (0);
		break;
	case G_RAID_VOLUME_RL_CONCAT:
		if (disks < 2)
			return (0);
		break;
	case G_RAID_VOLUME_RL_RAID5:
		if (disks < 3)
			return (0);
		break;
	default:
		return (0);
	}
	if (qual != G_RAID_VOLUME_RLQ_NONE)
		return (0);
	return (1);
}

static int
g_raid_md_promise_start_disk(struct g_raid_disk *disk, int sdn)
{
	struct g_raid_softc *sc;
	struct g_raid_volume *vol;
	struct g_raid_subdisk *sd;
	struct g_raid_disk *olddisk;
	struct g_raid_md_object *md;
	struct g_raid_md_promise_object *mdi;
	struct g_raid_md_promise_perdisk *pd, *oldpd;
	struct g_raid_md_promise_pervolume *pv;
	struct promise_raid_conf *meta;
	int disk_pos, md_disk_pos, resurrection = 0;

	sc = disk->d_softc;
	md = sc->sc_md;
	mdi = (struct g_raid_md_promise_object *)md;
	pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;
	olddisk = NULL;

	vol = g_raid_md_promise_get_volume(sc, pd->pd_meta[sdn]->volume_id);
	KASSERT(vol != NULL, ("No Promise volume with ID %16jx",
	    pd->pd_meta[sdn]->volume_id));
	pv = vol->v_md_data;
	meta = pv->pv_meta;

	/* Find disk position in metadata by it's serial. */
	md_disk_pos = promise_meta_find_disk(meta, pd->pd_meta[sdn]->disk.id);
	/* For RAID10 we need to translate order. */
	disk_pos = promise_meta_translate_disk(vol, md_disk_pos);
	if (disk_pos < 0) {
		G_RAID_DEBUG1(1, sc, "Unknown, probably new or stale disk");
		/* Failed stale disk is useless for us. */
		if (meta->disks[md_disk_pos].flags & PROMISE_F_DOWN) {
			g_raid_change_disk_state(disk, G_RAID_DISK_S_STALE_FAILED);
			return (0);
		}
		/* If we are in the start process, that's all for now. */
		if (!pv->pv_started)
			goto nofit;
		/*
		 * If we have already started - try to get use of the disk.
		 * Try to replace OFFLINE disks first, then FAILED.
		 */
#if 0
		TAILQ_FOREACH(tmpdisk, &sc->sc_disks, d_next) {
			if (tmpdisk->d_state != G_RAID_DISK_S_OFFLINE &&
			    tmpdisk->d_state != G_RAID_DISK_S_FAILED)
				continue;
			/* Make sure this disk is big enough. */
			TAILQ_FOREACH(sd, &tmpdisk->d_subdisks, sd_next) {
				if (sd->sd_offset + sd->sd_size + 4096 >
				    (off_t)pd->pd_disk_meta.sectors * 512) {
					G_RAID_DEBUG1(1, sc,
					    "Disk too small (%llu < %llu)",
					    ((unsigned long long)
					    pd->pd_disk_meta.sectors) * 512,
					    (unsigned long long)
					    sd->sd_offset + sd->sd_size + 4096);
					break;
				}
			}
			if (sd != NULL)
				continue;
			if (tmpdisk->d_state == G_RAID_DISK_S_OFFLINE) {
				olddisk = tmpdisk;
				break;
			} else if (olddisk == NULL)
				olddisk = tmpdisk;
		}
#endif
		if (olddisk == NULL) {
nofit:
			if (pd->pd_meta[sdn]->disk.flags & PROMISE_F_SPARE) {
				g_raid_change_disk_state(disk,
				    G_RAID_DISK_S_SPARE);
				return (1);
			} else {
				g_raid_change_disk_state(disk,
				    G_RAID_DISK_S_STALE);
				return (0);
			}
		}
		oldpd = (struct g_raid_md_promise_perdisk *)olddisk->d_md_data;
//		disk_pos = oldpd->pd_disk_pos;
		resurrection = 1;
	}

	sd = &vol->v_subdisks[disk_pos];

	if (olddisk == NULL) {
		/* Look for disk at position. */
		olddisk = sd->sd_disk;
		if (olddisk != NULL) {
			G_RAID_DEBUG1(1, sc, "More then one disk for pos %d",
			    disk_pos);
			g_raid_change_disk_state(disk, G_RAID_DISK_S_STALE);
			return (0);
		}
		oldpd = (struct g_raid_md_promise_perdisk *)olddisk->d_md_data;
	}

#if 0
	/* Replace failed disk or placeholder with new disk. */
	TAILQ_FOREACH_SAFE(sd, &olddisk->d_subdisks, sd_next, tmpsd) {
		TAILQ_REMOVE(&olddisk->d_subdisks, sd, sd_next);
		TAILQ_INSERT_TAIL(&disk->d_subdisks, sd, sd_next);
		sd->sd_disk = disk;
	}
	oldpd->pd_disk_pos = -2;
	pd->pd_disk_pos = disk_pos;

	/* If it was placeholder -- destroy it. */
	if (olddisk != NULL) {
		/* Otherwise, make it STALE_FAILED. */
		g_raid_change_disk_state(olddisk, G_RAID_DISK_S_STALE_FAILED);
		/* Update global metadata just in case. */
		memcpy(&meta->disk[disk_pos], &pd->pd_disk_meta,
		    sizeof(struct promise_raid_disk));
	}
#endif

	vol->v_subdisks[disk_pos].sd_disk = disk;
	TAILQ_INSERT_TAIL(&disk->d_subdisks, sd, sd_next);

	/* Welcome the new disk. */
	if (resurrection)
		g_raid_change_disk_state(disk, G_RAID_DISK_S_ACTIVE);
	else if (meta->disks[md_disk_pos].flags & PROMISE_F_DOWN)
		g_raid_change_disk_state(disk, G_RAID_DISK_S_FAILED);
	else
		g_raid_change_disk_state(disk, G_RAID_DISK_S_ACTIVE);

	sd->sd_offset = (off_t)pd->pd_meta[sdn]->disk_offset * 512;
	sd->sd_size = (off_t)pd->pd_meta[sdn]->disk_sectors * 512;

	if (resurrection) {
		/* Stale disk, almost same as new. */
		g_raid_change_subdisk_state(sd,
		    G_RAID_SUBDISK_S_NEW);
	} else if (meta->disks[md_disk_pos].flags & PROMISE_F_DOWN) {
		/* Failed disk. */
		g_raid_change_subdisk_state(sd,
		    G_RAID_SUBDISK_S_FAILED);
	} else if (meta->disks[md_disk_pos].flags & PROMISE_F_REDIR) {
		/* Rebuilding disk. */
		g_raid_change_subdisk_state(sd,
		    G_RAID_SUBDISK_S_REBUILD);
		if (pd->pd_meta[sdn]->generation != meta->generation)
			sd->sd_rebuild_pos = 0;
		else {
			sd->sd_rebuild_pos =
			    (off_t)pd->pd_meta[sdn]->rebuild_lba * 512;
		}
	} else if (!(meta->disks[md_disk_pos].flags & PROMISE_F_ONLINE)) {
		/* Rebuilding disk. */
		g_raid_change_subdisk_state(sd,
		    G_RAID_SUBDISK_S_NEW);
	} else if (pd->pd_meta[sdn]->generation != meta->generation ||
	    (meta->status & PROMISE_S_MARKED)) {
		/* Stale disk or dirty volume (unclean shutdown). */
		g_raid_change_subdisk_state(sd,
		    G_RAID_SUBDISK_S_STALE);
	} else {
		/* Up to date disk. */
		g_raid_change_subdisk_state(sd,
		    G_RAID_SUBDISK_S_ACTIVE);
	}
	g_raid_event_send(sd, G_RAID_SUBDISK_E_NEW,
	    G_RAID_EVENT_SUBDISK);

#if 0
	/* Update status of our need for spare. */
	if (mdi->mdio_started) {
		mdi->mdio_incomplete =
		    (g_raid_ndisks(sc, G_RAID_DISK_S_ACTIVE) <
		     meta->total_disks);
	}
#endif

	return (resurrection);
}

#if 0
static void
g_disk_md_promise_retaste(void *arg, int pending)
{

	G_RAID_DEBUG(1, "Array is not complete, trying to retaste.");
	g_retaste(&g_raid_class);
	free(arg, M_MD_PROMISE);
}
#endif

static void
g_raid_md_promise_refill(struct g_raid_softc *sc)
{
#if 0
	struct g_raid_md_object *md;
	struct g_raid_md_promise_object *mdi;
	struct promise_raid_conf *meta;
	struct g_raid_disk *disk;
	struct task *task;
	int update, na;

	md = sc->sc_md;
	mdi = (struct g_raid_md_promise_object *)md;
	meta = mdi->mdio_meta;
	update = 0;
	do {
		/* Make sure we miss anything. */
		na = g_raid_ndisks(sc, G_RAID_DISK_S_ACTIVE);
		if (na == meta->total_disks)
			break;

		G_RAID_DEBUG1(1, md->mdo_softc,
		    "Array is not complete (%d of %d), "
		    "trying to refill.", na, meta->total_disks);

		/* Try to get use some of STALE disks. */
		TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
			if (disk->d_state == G_RAID_DISK_S_STALE) {
				update += g_raid_md_promise_start_disk(disk);
				if (disk->d_state == G_RAID_DISK_S_ACTIVE)
					break;
			}
		}
		if (disk != NULL)
			continue;

		/* Try to get use some of SPARE disks. */
		TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
			if (disk->d_state == G_RAID_DISK_S_SPARE) {
				update += g_raid_md_promise_start_disk(disk);
				if (disk->d_state == G_RAID_DISK_S_ACTIVE)
					break;
			}
		}
	} while (disk != NULL);

	/* Write new metadata if we changed something. */
	if (update) {
		g_raid_md_write_promise(md, NULL, NULL, NULL);
		meta = mdi->mdio_meta;
	}

	/* Update status of our need for spare. */
	mdi->mdio_incomplete = (g_raid_ndisks(sc, G_RAID_DISK_S_ACTIVE) <
	    meta->total_disks);

	/* Request retaste hoping to find spare. */
	if (mdi->mdio_incomplete) {
		task = malloc(sizeof(struct task),
		    M_MD_PROMISE, M_WAITOK | M_ZERO);
		TASK_INIT(task, 0, g_disk_md_promise_retaste, task);
		taskqueue_enqueue(taskqueue_swi, task);
	}
#endif
}

static void
g_raid_md_promise_start(struct g_raid_volume *vol)
{
	struct g_raid_softc *sc;
	struct g_raid_subdisk *sd;
	struct g_raid_disk *disk;
	struct g_raid_md_object *md;
	struct g_raid_md_promise_object *mdi;
	struct g_raid_md_promise_perdisk *pd;
	struct g_raid_md_promise_pervolume *pv;
	struct promise_raid_conf *meta;
	int i;

	sc = vol->v_softc;
	md = sc->sc_md;
	mdi = (struct g_raid_md_promise_object *)md;
	pv = vol->v_md_data;
	meta = pv->pv_meta;

	if (meta->type == PROMISE_T_RAID0)
		vol->v_raid_level = G_RAID_VOLUME_RL_RAID0;
	else if (meta->type == PROMISE_T_RAID1) {
		if (meta->array_width == 1)
			vol->v_raid_level = G_RAID_VOLUME_RL_RAID1;
		else
			vol->v_raid_level = G_RAID_VOLUME_RL_RAID1E;
	} else if (meta->type == PROMISE_T_RAID3)
		vol->v_raid_level = G_RAID_VOLUME_RL_RAID3;
	else if (meta->type == PROMISE_T_RAID5)
		vol->v_raid_level = G_RAID_VOLUME_RL_RAID5;
	else if (meta->type == PROMISE_T_SPAN)
		vol->v_raid_level = G_RAID_VOLUME_RL_CONCAT;
	else if (meta->type == PROMISE_T_JBOD)
		vol->v_raid_level = G_RAID_VOLUME_RL_SINGLE;
	else
		vol->v_raid_level = G_RAID_VOLUME_RL_UNKNOWN;
	vol->v_raid_level_qualifier = G_RAID_VOLUME_RLQ_NONE;
	vol->v_strip_size = 512 << meta->stripe_shift; //ZZZ
	vol->v_disks_count = meta->total_disks;
	vol->v_mediasize = (off_t)meta->total_sectors * 512; //ZZZ
	vol->v_sectorsize = 512; //ZZZ
	for (i = 0; i < vol->v_disks_count; i++) {
		sd = &vol->v_subdisks[i];
		sd->sd_offset = (off_t)meta->disk_offset * 512; //ZZZ
		sd->sd_size = (off_t)meta->disk_sectors * 512; //ZZZ
	}
	g_raid_start_volume(vol);

	/* Make all disks found till the moment take their places. */
	TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
		pd = disk->d_md_data;
		for (i = 0; i < pd->pd_subdisks; i++) {
			if (pd->pd_meta[i]->volume_id == meta->volume_id)
				g_raid_md_promise_start_disk(disk, i);
		}
	}

	pv->pv_started = 1;
	G_RAID_DEBUG1(0, sc, "Volume started.");
	g_raid_md_write_promise(md, vol, NULL, NULL);

	/* Pickup any STALE/SPARE disks to refill array if needed. */
//	g_raid_md_promise_refill(sc);

	g_raid_event_send(vol, G_RAID_VOLUME_E_START, G_RAID_EVENT_VOLUME);

	callout_stop(&pv->pv_start_co);
	G_RAID_DEBUG1(1, sc, "root_mount_rel %p", pv->pv_rootmount);
	root_mount_rel(pv->pv_rootmount);
	pv->pv_rootmount = NULL;
}

static void
g_raid_promise_go(void *arg)
{
	struct g_raid_volume *vol;
	struct g_raid_softc *sc;
	struct g_raid_md_promise_pervolume *pv;

	vol = arg;
	pv = vol->v_md_data;
	sc = vol->v_softc;
	if (!pv->pv_started) {
		G_RAID_DEBUG1(0, sc, "Force volume start due to timeout.");
		g_raid_event_send(vol, G_RAID_VOLUME_E_STARTMD,
		    G_RAID_EVENT_VOLUME);
	}
}

static void
g_raid_md_promise_new_disk(struct g_raid_disk *disk)
{
	struct g_raid_softc *sc;
	struct g_raid_md_object *md;
	struct g_raid_md_promise_object *mdi;
	struct promise_raid_conf *pdmeta;
	struct g_raid_md_promise_perdisk *pd;
	struct g_raid_md_promise_pervolume *pv;
	struct g_raid_volume *vol;
	int i;
	char buf[33];

	sc = disk->d_softc;
	md = sc->sc_md;
	mdi = (struct g_raid_md_promise_object *)md;
	pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;

	for (i = 0; i < pd->pd_subdisks; i++) {
		pdmeta = pd->pd_meta[i];

		if (pdmeta->disk.number == 0xff) {
			if (pdmeta->disk.flags & PROMISE_F_SPARE) {
				g_raid_change_disk_state(disk,
				    G_RAID_DISK_S_SPARE);
			}
			continue;
		}

		/* Look for volume with matching ID. */
		vol = g_raid_md_promise_get_volume(sc, pdmeta->volume_id);
		if (vol == NULL) {
			promise_meta_get_name(pdmeta, buf);
			vol = g_raid_create_volume(sc, buf);
			pv = malloc(sizeof(*pv), M_MD_PROMISE, M_WAITOK | M_ZERO);
			pv->pv_id = pdmeta->volume_id;
			vol->v_md_data = pv;
			callout_init(&pv->pv_start_co, 1);
			callout_reset(&pv->pv_start_co,
			    g_raid_start_timeout * hz,
			    g_raid_promise_go, vol);
			pv->pv_rootmount = root_mount_hold("GRAID-Promise");
			G_RAID_DEBUG1(1, sc, "root_mount_hold %p", pv->pv_rootmount);
		} else
			pv = vol->v_md_data;

		/* If we haven't started yet - check metadata freshness. */
		if (pv->pv_meta == NULL || !pv->pv_started) {
			if (pv->pv_meta == NULL ||
			    ((int16_t)(pdmeta->generation - pv->pv_generation)) > 0) {
				G_RAID_DEBUG1(1, sc, "Newer disk");
				if (pv->pv_meta != NULL)
					free(pv->pv_meta, M_MD_PROMISE);
				pv->pv_meta = promise_meta_copy(pdmeta);
				pv->pv_generation = pv->pv_meta->generation;
				pv->pv_disks_present = 1;
			} else if (pdmeta->generation == pv->pv_generation) {
				pv->pv_disks_present++;
				G_RAID_DEBUG1(1, sc, "Matching disk (%d of %d up)",
				    pv->pv_disks_present,
				    pv->pv_meta->total_disks);
			} else {
				G_RAID_DEBUG1(1, sc, "Older disk");
			}
		}
	}

	for (i = 0; i < pd->pd_subdisks; i++) {
		pdmeta = pd->pd_meta[i];

		/* Look for volume with matching ID. */
		vol = g_raid_md_promise_get_volume(sc, pdmeta->volume_id);
		if (vol == NULL)
			continue;
		pv = vol->v_md_data;

		if (pv->pv_started) {
			if (g_raid_md_promise_start_disk(disk, i))
				g_raid_md_write_promise(md, vol, NULL, NULL);
		} else {
			/* If we collected all needed disks - start array. */
			if (pv->pv_disks_present == pv->pv_meta->total_disks)
				g_raid_md_promise_start(vol);
		}
	}
}

static int
g_raid_md_create_promise(struct g_raid_md_object *md, struct g_class *mp,
    struct g_geom **gp)
{
	struct g_geom *geom;
	struct g_raid_softc *sc;

	/* Search for existing node. */
	LIST_FOREACH(geom, &mp->geom, geom) {
		sc = geom->softc;
		if (sc == NULL)
			continue;
		if (sc->sc_stopping != 0)
			continue;
		if (sc->sc_md->mdo_class != md->mdo_class)
			continue;
		break;
	}
	if (geom != NULL) {
		*gp = geom;
		return (G_RAID_MD_TASTE_EXISTING);
	}

	/* Create new one if not found. */
	sc = g_raid_create_node(mp, "Promise", md);
	if (sc == NULL)
		return (G_RAID_MD_TASTE_FAIL);
	md->mdo_softc = sc;
	*gp = sc->sc_geom;
	return (G_RAID_MD_TASTE_NEW);
}

static int
g_raid_md_taste_promise(struct g_raid_md_object *md, struct g_class *mp,
                              struct g_consumer *cp, struct g_geom **gp)
{
	struct g_consumer *rcp;
	struct g_provider *pp;
	struct g_raid_md_promise_object *mdi, *mdi1;
	struct g_raid_softc *sc;
	struct g_raid_disk *disk;
	struct promise_raid_conf *meta, *metaarr[4];
	struct g_raid_md_promise_perdisk *pd;
	struct g_geom *geom;
	int error, i, result, spare, len, subdisks;
	char name[16];
	uint16_t vendor;

	G_RAID_DEBUG(1, "Tasting Promise on %s", cp->provider->name);
	mdi = (struct g_raid_md_promise_object *)md;
	pp = cp->provider;

	/* Read metadata from device. */
	meta = NULL;
	spare = 0;
	vendor = 0xffff;
	if (g_access(cp, 1, 0, 0) != 0)
		return (G_RAID_MD_TASTE_FAIL);
	g_topology_unlock();
	len = 2;
	if (pp->geom->rank == 1)
		g_io_getattr("GEOM::hba_vendor", cp, &len, &vendor);
	subdisks = promise_meta_read(cp, metaarr);
	g_topology_lock();
	g_access(cp, -1, 0, 0);
	if (subdisks == 0) {
		if (g_raid_aggressive_spare) {
			if (vendor == 0x105a || vendor == 0x1002) {
				G_RAID_DEBUG(1,
				    "No Promise metadata, forcing spare.");
				spare = 2;
				goto search;
			} else {
				G_RAID_DEBUG(1,
				    "Promise/ATI vendor mismatch "
				    "0x%04x != 0x105a/0x1002",
				    vendor);
			}
		}
		return (G_RAID_MD_TASTE_FAIL);
	}

	/* Metadata valid. Print it. */
	for (i = 0; i < subdisks; i++)
		g_raid_md_promise_print(metaarr[i]);
	spare = 0;//meta->disks[disk_pos].flags & PROMISE_F_SPARE;

search:
	/* Search for matching node. */
	sc = NULL;
	mdi1 = NULL;
	LIST_FOREACH(geom, &mp->geom, geom) {
		sc = geom->softc;
		if (sc == NULL)
			continue;
		if (sc->sc_stopping != 0)
			continue;
		if (sc->sc_md->mdo_class != md->mdo_class)
			continue;
		mdi1 = (struct g_raid_md_promise_object *)sc->sc_md;
		break;
	}

	/* Found matching node. */
	if (geom != NULL) {
		G_RAID_DEBUG(1, "Found matching array %s", sc->sc_name);
		result = G_RAID_MD_TASTE_EXISTING;

	} else { /* Not found matching node -- create one. */
		result = G_RAID_MD_TASTE_NEW;
		snprintf(name, sizeof(name), "Promise");
		sc = g_raid_create_node(mp, name, md);
		md->mdo_softc = sc;
		geom = sc->sc_geom;
	}

	rcp = g_new_consumer(geom);
	g_attach(rcp, pp);
	if (g_access(rcp, 1, 1, 1) != 0)
		; //goto fail1;

	g_topology_unlock();
	sx_xlock(&sc->sc_lock);

	pd = malloc(sizeof(*pd), M_MD_PROMISE, M_WAITOK | M_ZERO);
	pd->pd_subdisks = subdisks;
	for (i = 0; i < subdisks; i++)
		pd->pd_meta[i] = metaarr[i];
#if 0
	pd->pd_disk_pos = -1;
	if (spare == 2) {
//		pd->pd_disk_meta.sectors = pp->mediasize / pp->sectorsize;
		pd->pd_disk_meta.id = 0;
		pd->pd_disk_meta.flags = PROMISE_F_SPARE;
	} else {
		pd->pd_disk_meta = meta->disks[disk_pos];
	}
#endif
	disk = g_raid_create_disk(sc);
	disk->d_md_data = (void *)pd;
	disk->d_consumer = rcp;
	rcp->private = disk;

	/* Read kernel dumping information. */
	disk->d_kd.offset = 0;
	disk->d_kd.length = OFF_MAX;
	len = sizeof(disk->d_kd);
	error = g_io_getattr("GEOM::kerneldump", rcp, &len, &disk->d_kd);
	if (disk->d_kd.di.dumper == NULL)
		G_RAID_DEBUG1(2, sc, "Dumping not supported by %s: %d.", 
		    rcp->provider->name, error);

	g_raid_md_promise_new_disk(disk);

	sx_xunlock(&sc->sc_lock);
	g_topology_lock();
	*gp = geom;
	return (result);
//fail1:
//	free(meta, M_MD_PROMISE);
//	return (G_RAID_MD_TASTE_FAIL);
}

static int
g_raid_md_event_promise(struct g_raid_md_object *md,
    struct g_raid_disk *disk, u_int event)
{
	struct g_raid_softc *sc;
	struct g_raid_md_promise_object *mdi;
	struct g_raid_md_promise_perdisk *pd;

	sc = md->mdo_softc;
	mdi = (struct g_raid_md_promise_object *)md;
	if (disk == NULL)
		return (-1);
	pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;
	switch (event) {
	case G_RAID_DISK_E_DISCONNECTED:
		/* Delete disk. */
		g_raid_change_disk_state(disk, G_RAID_DISK_S_NONE);
		g_raid_destroy_disk(disk);

		/* Write updated metadata to all disks. */
		g_raid_md_write_promise(md, NULL, NULL, NULL);

		/* Check if anything left except placeholders. */
		if (g_raid_ndisks(sc, -1) ==
		    g_raid_ndisks(sc, G_RAID_DISK_S_OFFLINE))
			g_raid_destroy_node(sc, 0);
		else
			g_raid_md_promise_refill(sc);
		return (0);
	}
	return (-2);
}

static int
g_raid_md_volume_event_promise(struct g_raid_md_object *md,
    struct g_raid_volume *vol, u_int event)
{
	struct g_raid_softc *sc;
	struct g_raid_md_promise_pervolume *pv;

	sc = md->mdo_softc;
	pv = (struct g_raid_md_promise_pervolume *)vol->v_md_data;
	switch (event) {
	case G_RAID_VOLUME_E_STARTMD:
		if (!pv->pv_started)
			g_raid_md_promise_start(vol);
		return (0);
	}
	return (-2);
}

static int
g_raid_md_ctl_promise(struct g_raid_md_object *md,
    struct gctl_req *req)
{
	struct g_raid_softc *sc;
	struct g_raid_volume *vol, *vol1;
	struct g_raid_subdisk *sd;
	struct g_raid_disk *disk, *disks[PROMISE_MAX_DISKS];
	struct g_raid_md_promise_object *mdi;
	struct g_raid_md_promise_perdisk *pd;
	struct g_raid_md_promise_pervolume *pv;
	struct g_consumer *cp;
	struct g_provider *pp;
	char arg[16];
	const char *verb, *volname, *levelname, *diskname;
	char *tmp;
	int *nargs, *force;
	off_t off, size, sectorsize, strip;
	intmax_t *sizearg, *striparg;
	int numdisks, i, len, level, qual, update;
	int error;

	sc = md->mdo_softc;
	mdi = (struct g_raid_md_promise_object *)md;
	verb = gctl_get_param(req, "verb", NULL);
	nargs = gctl_get_paraml(req, "nargs", sizeof(*nargs));
	error = 0;
	if (strcmp(verb, "label") == 0) {

		if (*nargs < 4) {
			gctl_error(req, "Invalid number of arguments.");
			return (-1);
		}
		volname = gctl_get_asciiparam(req, "arg1");
		if (volname == NULL) {
			gctl_error(req, "No volume name.");
			return (-2);
		}
		levelname = gctl_get_asciiparam(req, "arg2");
		if (levelname == NULL) {
			gctl_error(req, "No RAID level.");
			return (-3);
		}
		if (g_raid_volume_str2level(levelname, &level, &qual)) {
			gctl_error(req, "Unknown RAID level '%s'.", levelname);
			return (-4);
		}
		numdisks = *nargs - 3;
		force = gctl_get_paraml(req, "force", sizeof(*force));
		if (!g_raid_md_promise_supported(level, qual, numdisks,
		    force ? *force : 0)) {
			gctl_error(req, "Unsupported RAID level "
			    "(0x%02x/0x%02x), or number of disks (%d).",
			    level, qual, numdisks);
			return (-5);
		}

		/* Search for disks, connect them and probe. */
		size = 0x7fffffffffffffffllu;
		sectorsize = 0;
		bzero(disks, sizeof(disks));
		for (i = 0; i < numdisks; i++) {
			snprintf(arg, sizeof(arg), "arg%d", i + 3);
			diskname = gctl_get_asciiparam(req, arg);
			if (diskname == NULL) {
				gctl_error(req, "No disk name (%s).", arg);
				error = -6;
				break;
			}
			if (strcmp(diskname, "NONE") == 0)
				continue;
			g_topology_lock();
			cp = g_raid_open_consumer(sc, diskname);
			if (cp == NULL) {
				gctl_error(req, "Can't open disk '%s'.",
				    diskname);
				g_topology_unlock();
				error = -4;
				break;
			}
			pp = cp->provider;
			pd = malloc(sizeof(*pd), M_MD_PROMISE, M_WAITOK | M_ZERO);
			disk = g_raid_create_disk(sc);
			disk->d_md_data = (void *)pd;
			disk->d_consumer = cp;
			disks[i] = disk;
			cp->private = disk;
			g_topology_unlock();

			/* Read kernel dumping information. */
			disk->d_kd.offset = 0;
			disk->d_kd.length = OFF_MAX;
			len = sizeof(disk->d_kd);
			g_io_getattr("GEOM::kerneldump", cp, &len, &disk->d_kd);
			if (disk->d_kd.di.dumper == NULL)
				G_RAID_DEBUG1(2, sc,
				    "Dumping not supported by %s.",
				    cp->provider->name);

			if (size > pp->mediasize)
				size = pp->mediasize;
			if (sectorsize < pp->sectorsize)
				sectorsize = pp->sectorsize;
		}
		if (error != 0)
			return (error);

		/* Reserve some space for metadata. */
		size -= size % (63 * sectorsize);
		size -= 63 * sectorsize;

		/* Handle size argument. */
		len = sizeof(*sizearg);
		sizearg = gctl_get_param(req, "size", &len);
		if (sizearg != NULL && len == sizeof(*sizearg) &&
		    *sizearg > 0) {
			if (*sizearg > size) {
				gctl_error(req, "Size too big %lld > %lld.",
				    (long long)*sizearg, (long long)size);
				return (-9);
			}
			size = *sizearg;
		}

		/* Handle strip argument. */
		strip = 131072;
		len = sizeof(*striparg);
		striparg = gctl_get_param(req, "strip", &len);
		if (striparg != NULL && len == sizeof(*striparg) &&
		    *striparg > 0) {
			if (*striparg < sectorsize) {
				gctl_error(req, "Strip size too small.");
				return (-10);
			}
			if (*striparg % sectorsize != 0) {
				gctl_error(req, "Incorrect strip size.");
				return (-11);
			}
			strip = *striparg;
		}

		/* Round size down to strip or sector. */
		if (level == G_RAID_VOLUME_RL_RAID1)
			size -= (size % sectorsize);
		else if (level == G_RAID_VOLUME_RL_RAID1E &&
		    (numdisks & 1) != 0)
			size -= (size % (2 * strip));
		else
			size -= (size % strip);
		if (size <= 0) {
			gctl_error(req, "Size too small.");
			return (-13);
		}
		if (size > 0xffffffffllu * sectorsize) {
			gctl_error(req, "Size too big.");
			return (-14);
		}

		/* We have all we need, create things: volume, ... */
		pv = malloc(sizeof(*pv), M_MD_PROMISE, M_WAITOK | M_ZERO);
		arc4rand(&pv->pv_id, sizeof(pv->pv_id), 0);
		pv->pv_generation = 0;
		pv->pv_started = 1;
		vol = g_raid_create_volume(sc, volname);
		vol->v_md_data = pv;
		vol->v_raid_level = level;
		vol->v_raid_level_qualifier = G_RAID_VOLUME_RLQ_NONE;
		vol->v_strip_size = strip;
		vol->v_disks_count = numdisks;
		if (level == G_RAID_VOLUME_RL_RAID0 ||
		    level == G_RAID_VOLUME_RL_CONCAT ||
		    level == G_RAID_VOLUME_RL_SINGLE)
			vol->v_mediasize = size * numdisks;
		else if (level == G_RAID_VOLUME_RL_RAID1)
			vol->v_mediasize = size;
		else if (level == G_RAID_VOLUME_RL_RAID3 ||
		    level == G_RAID_VOLUME_RL_RAID5)
			vol->v_mediasize = size * (numdisks - 1);
		else { /* RAID1E */
			vol->v_mediasize = ((size * numdisks) / strip / 2) *
			    strip;
		}
		vol->v_sectorsize = sectorsize;
		g_raid_start_volume(vol);

		/* , and subdisks. */
		for (i = 0; i < numdisks; i++) {
			disk = disks[i];
			pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;
			sd = &vol->v_subdisks[i];
			sd->sd_disk = disk;
			sd->sd_offset = 0;
			sd->sd_size = size;
			TAILQ_INSERT_TAIL(&disk->d_subdisks, sd, sd_next);
			if (sd->sd_disk->d_consumer != NULL) {
				g_raid_change_disk_state(disk,
				    G_RAID_DISK_S_ACTIVE);
				g_raid_change_subdisk_state(sd,
				    G_RAID_SUBDISK_S_ACTIVE);
				g_raid_event_send(sd, G_RAID_SUBDISK_E_NEW,
				    G_RAID_EVENT_SUBDISK);
			} else {
				g_raid_change_disk_state(disk, G_RAID_DISK_S_OFFLINE);
			}
		}

		/* Write metadata based on created entities. */
		G_RAID_DEBUG1(0, sc, "Array started.");
		g_raid_md_write_promise(md, vol, NULL, NULL);

		/* Pickup any STALE/SPARE disks to refill array if needed. */
		g_raid_md_promise_refill(sc);

		g_raid_event_send(vol, G_RAID_VOLUME_E_START,
		    G_RAID_EVENT_VOLUME);
		return (0);
	}
	if (strcmp(verb, "add") == 0) {

		if (*nargs != 3) {
			gctl_error(req, "Invalid number of arguments.");
			return (-1);
		}
		volname = gctl_get_asciiparam(req, "arg1");
		if (volname == NULL) {
			gctl_error(req, "No volume name.");
			return (-2);
		}
		levelname = gctl_get_asciiparam(req, "arg2");
		if (levelname == NULL) {
			gctl_error(req, "No RAID level.");
			return (-3);
		}
		if (g_raid_volume_str2level(levelname, &level, &qual)) {
			gctl_error(req, "Unknown RAID level '%s'.", levelname);
			return (-4);
		}

		/* Look for existing volumes. */
		i = 0;
		vol1 = NULL;
		TAILQ_FOREACH(vol, &sc->sc_volumes, v_next) {
			vol1 = vol;
			i++;
		}
		if (i > 1) {
			gctl_error(req, "Maximum two volumes supported.");
			return (-6);
		}
		if (vol1 == NULL) {
			gctl_error(req, "At least one volume must exist.");
			return (-7);
		}

		numdisks = vol1->v_disks_count;
		force = gctl_get_paraml(req, "force", sizeof(*force));
		if (!g_raid_md_promise_supported(level, qual, numdisks,
		    force ? *force : 0)) {
			gctl_error(req, "Unsupported RAID level "
			    "(0x%02x/0x%02x), or number of disks (%d).",
			    level, qual, numdisks);
			return (-5);
		}

		/* Collect info about present disks. */
		size = 0x7fffffffffffffffllu;
		sectorsize = 512;
		for (i = 0; i < numdisks; i++) {
			disk = vol1->v_subdisks[i].sd_disk;
			pd = (struct g_raid_md_promise_perdisk *)
			    disk->d_md_data;
//			if ((off_t)pd->pd_disk_meta.sectors * 512 < size)
//				size = (off_t)pd->pd_disk_meta.sectors * 512;
			if (disk->d_consumer != NULL &&
			    disk->d_consumer->provider != NULL &&
			    disk->d_consumer->provider->sectorsize >
			     sectorsize) {
				sectorsize =
				    disk->d_consumer->provider->sectorsize;
			}
		}

		/* Reserve some space for metadata. */
		size -= ((4096 + sectorsize - 1) / sectorsize) * sectorsize;

		/* Decide insert before or after. */
		sd = &vol1->v_subdisks[0];
		if (sd->sd_offset >
		    size - (sd->sd_offset + sd->sd_size)) {
			off = 0;
			size = sd->sd_offset;
		} else {
			off = sd->sd_offset + sd->sd_size;
			size = size - (sd->sd_offset + sd->sd_size);
		}

		/* Handle strip argument. */
		strip = 131072;
		len = sizeof(*striparg);
		striparg = gctl_get_param(req, "strip", &len);
		if (striparg != NULL && len == sizeof(*striparg) &&
		    *striparg > 0) {
			if (*striparg < sectorsize) {
				gctl_error(req, "Strip size too small.");
				return (-10);
			}
			if (*striparg % sectorsize != 0) {
				gctl_error(req, "Incorrect strip size.");
				return (-11);
			}
			if (strip > 65535 * sectorsize) {
				gctl_error(req, "Strip size too big.");
				return (-12);
			}
			strip = *striparg;
		}

		/* Round offset up to strip. */
		if (off % strip != 0) {
			size -= strip - off % strip;
			off += strip - off % strip;
		}

		/* Handle size argument. */
		len = sizeof(*sizearg);
		sizearg = gctl_get_param(req, "size", &len);
		if (sizearg != NULL && len == sizeof(*sizearg) &&
		    *sizearg > 0) {
			if (*sizearg > size) {
				gctl_error(req, "Size too big %lld > %lld.",
				    (long long)*sizearg, (long long)size);
				return (-9);
			}
			size = *sizearg;
		}

		/* Round size down to strip or sector. */
		if (level == G_RAID_VOLUME_RL_RAID1)
			size -= (size % sectorsize);
		else
			size -= (size % strip);
		if (size <= 0) {
			gctl_error(req, "Size too small.");
			return (-13);
		}
		if (size > 0xffffffffllu * sectorsize) {
			gctl_error(req, "Size too big.");
			return (-14);
		}

		/* We have all we need, create things: volume, ... */
		vol = g_raid_create_volume(sc, volname);
		vol->v_md_data = (void *)(intptr_t)i;
		vol->v_raid_level = level;
		vol->v_raid_level_qualifier = G_RAID_VOLUME_RLQ_NONE;
		vol->v_strip_size = strip;
		vol->v_disks_count = numdisks;
		if (level == G_RAID_VOLUME_RL_RAID0)
			vol->v_mediasize = size * numdisks;
		else if (level == G_RAID_VOLUME_RL_RAID1)
			vol->v_mediasize = size;
		else if (level == G_RAID_VOLUME_RL_RAID5)
			vol->v_mediasize = size * (numdisks - 1);
		else { /* RAID1E */
			vol->v_mediasize = ((size * numdisks) / strip / 2) *
			    strip;
		}
		vol->v_sectorsize = sectorsize;
		g_raid_start_volume(vol);

		/* , and subdisks. */
		for (i = 0; i < numdisks; i++) {
			disk = vol1->v_subdisks[i].sd_disk;
			sd = &vol->v_subdisks[i];
			sd->sd_disk = disk;
			sd->sd_offset = off;
			sd->sd_size = size;
			TAILQ_INSERT_TAIL(&disk->d_subdisks, sd, sd_next);
			if (disk->d_state == G_RAID_DISK_S_ACTIVE) {
				g_raid_change_subdisk_state(sd,
				    G_RAID_SUBDISK_S_ACTIVE);
				g_raid_event_send(sd, G_RAID_SUBDISK_E_NEW,
				    G_RAID_EVENT_SUBDISK);
			}
		}

		/* Write metadata based on created entities. */
		g_raid_md_write_promise(md, vol, NULL, NULL);

		g_raid_event_send(vol, G_RAID_VOLUME_E_START,
		    G_RAID_EVENT_VOLUME);
		return (0);
	}
	if (strcmp(verb, "delete") == 0) {

		/* Full node destruction. */
		if (*nargs == 1) {
			/* Check if some volume is still open. */
			force = gctl_get_paraml(req, "force", sizeof(*force));
			if (force != NULL && *force == 0 &&
			    g_raid_nopens(sc) != 0) {
				gctl_error(req, "Some volume is still open.");
				return (-4);
			}

			TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
				if (disk->d_consumer)
					promise_meta_erase(disk->d_consumer);
			}
			g_raid_destroy_node(sc, 0);
			return (0);
		}

		/* Destroy specified volume. If it was last - all node. */
		if (*nargs != 2) {
			gctl_error(req, "Invalid number of arguments.");
			return (-1);
		}
		volname = gctl_get_asciiparam(req, "arg1");
		if (volname == NULL) {
			gctl_error(req, "No volume name.");
			return (-2);
		}

		/* Search for volume. */
		TAILQ_FOREACH(vol, &sc->sc_volumes, v_next) {
			if (strcmp(vol->v_name, volname) == 0)
				break;
		}
		if (vol == NULL) {
			i = strtol(volname, &tmp, 10);
			if (verb != volname && tmp[0] == 0) {
				TAILQ_FOREACH(vol, &sc->sc_volumes, v_next) {
					if (vol->v_global_id == i)
						break;
				}
			}
		}
		if (vol == NULL) {
			gctl_error(req, "Volume '%s' not found.", volname);
			return (-3);
		}

		/* Check if volume is still open. */
		force = gctl_get_paraml(req, "force", sizeof(*force));
		if (force != NULL && *force == 0 &&
		    vol->v_provider_open != 0) {
			gctl_error(req, "Volume is still open.");
			return (-4);
		}

		/* Destroy volume and potentially node. */
		i = 0;
		TAILQ_FOREACH(vol1, &sc->sc_volumes, v_next)
			i++;
		if (i >= 2) {
			g_raid_destroy_volume(vol);
			g_raid_md_write_promise(md, NULL, NULL, NULL);
		} else {
			TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
				if (disk->d_consumer)
					promise_meta_erase(disk->d_consumer);
			}
			g_raid_destroy_node(sc, 0);
		}
		return (0);
	}
	if (strcmp(verb, "remove") == 0 ||
	    strcmp(verb, "fail") == 0) {
		if (*nargs < 2) {
			gctl_error(req, "Invalid number of arguments.");
			return (-1);
		}
		for (i = 1; i < *nargs; i++) {
			snprintf(arg, sizeof(arg), "arg%d", i);
			diskname = gctl_get_asciiparam(req, arg);
			if (diskname == NULL) {
				gctl_error(req, "No disk name (%s).", arg);
				error = -2;
				break;
			}
			if (strncmp(diskname, "/dev/", 5) == 0)
				diskname += 5;

			TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
				if (disk->d_consumer != NULL && 
				    disk->d_consumer->provider != NULL &&
				    strcmp(disk->d_consumer->provider->name,
				     diskname) == 0)
					break;
			}
			if (disk == NULL) {
				gctl_error(req, "Disk '%s' not found.",
				    diskname);
				error = -3;
				break;
			}

			if (strcmp(verb, "fail") == 0) {
				g_raid_md_fail_disk_promise(md, NULL, disk);
				continue;
			}

			pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;

			/* Erase metadata on deleting disk. */
			promise_meta_erase(disk->d_consumer);

			/* If disk was assigned, just update statuses. */
			if (pd->pd_subdisks >= 0) {
				g_raid_change_disk_state(disk, G_RAID_DISK_S_OFFLINE);
				if (disk->d_consumer) {
					g_raid_kill_consumer(sc, disk->d_consumer);
					disk->d_consumer = NULL;
				}
				TAILQ_FOREACH(sd, &disk->d_subdisks, sd_next) {
					g_raid_change_subdisk_state(sd,
					    G_RAID_SUBDISK_S_NONE);
					g_raid_event_send(sd, G_RAID_SUBDISK_E_DISCONNECTED,
					    G_RAID_EVENT_SUBDISK);
				}
			} else {
				/* Otherwise -- delete. */
				g_raid_change_disk_state(disk, G_RAID_DISK_S_NONE);
				g_raid_destroy_disk(disk);
			}
		}

		/* Write updated metadata to remaining disks. */
		g_raid_md_write_promise(md, NULL, NULL, NULL);

		/* Check if anything left except placeholders. */
		if (g_raid_ndisks(sc, -1) ==
		    g_raid_ndisks(sc, G_RAID_DISK_S_OFFLINE))
			g_raid_destroy_node(sc, 0);
		else
			g_raid_md_promise_refill(sc);
		return (error);
	}
	if (strcmp(verb, "insert") == 0) {
		if (*nargs < 2) {
			gctl_error(req, "Invalid number of arguments.");
			return (-1);
		}
		update = 0;
		for (i = 1; i < *nargs; i++) {
			/* Get disk name. */
			snprintf(arg, sizeof(arg), "arg%d", i);
			diskname = gctl_get_asciiparam(req, arg);
			if (diskname == NULL) {
				gctl_error(req, "No disk name (%s).", arg);
				error = -3;
				break;
			}

			/* Try to find provider with specified name. */
			g_topology_lock();
			cp = g_raid_open_consumer(sc, diskname);
			if (cp == NULL) {
				gctl_error(req, "Can't open disk '%s'.",
				    diskname);
				g_topology_unlock();
				error = -4;
				break;
			}
			pp = cp->provider;
			g_topology_unlock();

			pd = malloc(sizeof(*pd), M_MD_PROMISE, M_WAITOK | M_ZERO);
//			pd->pd_disk_pos = -1;

			disk = g_raid_create_disk(sc);
			disk->d_consumer = cp;
			disk->d_consumer->private = disk;
			disk->d_md_data = (void *)pd;
			cp->private = disk;

			/* Read kernel dumping information. */
			disk->d_kd.offset = 0;
			disk->d_kd.length = OFF_MAX;
			len = sizeof(disk->d_kd);
			g_io_getattr("GEOM::kerneldump", cp, &len, &disk->d_kd);
			if (disk->d_kd.di.dumper == NULL)
				G_RAID_DEBUG1(2, sc,
				    "Dumping not supported by %s.",
				    cp->provider->name);

//			pd->pd_disk_meta.sectors = pp->mediasize / pp->sectorsize;
//			pd->pd_disk_meta.id = 0;
//			pd->pd_disk_meta.flags = PROMISE_F_SPARE;

			/* Welcome the "new" disk. */
			update += g_raid_md_promise_start_disk(disk, 0);
			if (disk->d_state == G_RAID_DISK_S_SPARE) {
//				promise_meta_write_spare(cp, &pd->pd_disk_meta);
				g_raid_destroy_disk(disk);
			} else if (disk->d_state != G_RAID_DISK_S_ACTIVE) {
				gctl_error(req, "Disk '%s' doesn't fit.",
				    diskname);
				g_raid_destroy_disk(disk);
				error = -8;
				break;
			}
		}

		/* Write new metadata if we changed something. */
		if (update)
			g_raid_md_write_promise(md, NULL, NULL, NULL);
		return (error);
	}
	return (-100);
}

static int
g_raid_md_write_promise(struct g_raid_md_object *md, struct g_raid_volume *tvol,
    struct g_raid_subdisk *tsd, struct g_raid_disk *tdisk)
{
	struct g_raid_softc *sc;
	struct g_raid_volume *vol;
	struct g_raid_subdisk *sd;
	struct g_raid_disk *disk;
	struct g_raid_md_promise_object *mdi;
	struct g_raid_md_promise_perdisk *pd;
	struct g_raid_md_promise_pervolume *pv;
	struct promise_raid_conf *meta;
	off_t rebuild_lba64;
	int i, j, pos, rebuild;

	sc = md->mdo_softc;
	mdi = (struct g_raid_md_promise_object *)md;

	if (sc->sc_stopping == G_RAID_DESTROY_HARD)
		return (0);

	/* Clear "updated" flags and scan for deleted volumes. */
	TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
		pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;
		pd->pd_updated = 0;

		for (i = 0; i < pd->pd_subdisks; ) {
			vol = g_raid_md_promise_get_volume(sc,
			    pd->pd_meta[i]->volume_id);
			if (vol != NULL && !vol->v_stopping) {
				i++;
				continue;
			}
			free(pd->pd_meta[i], M_MD_PROMISE);
			for (j = i; j < pd->pd_subdisks - 1; j++)
				pd->pd_meta[j] = pd->pd_meta[j + 1];
			pd->pd_meta[PROMISE_MAX_SUBDISKS - 1] = NULL;
			pd->pd_subdisks--;
			pd->pd_updated = 1;
		}
	}

	/* Generate new per-volume metadata for affected volumes. */
	TAILQ_FOREACH(vol, &sc->sc_volumes, v_next) {
		if (vol->v_stopping)
			continue;

		/* Skip volumes not related to specified targets. */
		if (tvol != NULL && vol != tvol)
			continue;
		if (tsd != NULL && vol != tsd->sd_volume)
			continue;
		if (tdisk != NULL) {
			for (i = 0; i < vol->v_disks_count; i++) {
				if (vol->v_subdisks[i].sd_disk == tdisk)
					break;
			}
			if (i >= vol->v_disks_count)
				continue;
		}

		pv = (struct g_raid_md_promise_pervolume *)vol->v_md_data;
		pv->pv_generation++;

		meta = malloc(sizeof(*meta), M_MD_PROMISE, M_WAITOK | M_ZERO);
		if (pv->pv_meta != NULL)
			memcpy(meta, pv->pv_meta, sizeof(*meta));
		memcpy(meta->promise_id, PROMISE_MAGIC, sizeof(PROMISE_MAGIC));
		meta->dummy_0 = 0x00020000;
		meta->integrity = PROMISE_I_VALID;

		meta->generation = pv->pv_generation;
		meta->status = PROMISE_S_VALID | PROMISE_S_ONLINE |
		    PROMISE_S_INITED | PROMISE_S_READY;
		if (vol->v_state <= G_RAID_VOLUME_S_DEGRADED)
			meta->status |= PROMISE_S_DEGRADED;
		if (vol->v_dirty)
			meta->status |= PROMISE_S_MARKED; /* XXX: INVENTED! */
		if (vol->v_raid_level == G_RAID_VOLUME_RL_RAID0 ||
		    vol->v_raid_level == G_RAID_VOLUME_RL_SINGLE)
			meta->type = PROMISE_T_RAID0;
		else if (vol->v_raid_level == G_RAID_VOLUME_RL_RAID1 ||
		    vol->v_raid_level == G_RAID_VOLUME_RL_RAID1E)
			meta->type = PROMISE_T_RAID1;
		else if (vol->v_raid_level == G_RAID_VOLUME_RL_RAID3)
			meta->type = PROMISE_T_RAID3;
		else if (vol->v_raid_level == G_RAID_VOLUME_RL_RAID5)
			meta->type = PROMISE_T_RAID5;
		else if (vol->v_raid_level == G_RAID_VOLUME_RL_CONCAT)
			meta->type = PROMISE_T_SPAN;
		else
			meta->type = PROMISE_T_JBOD;
		meta->total_disks = vol->v_disks_count;
		meta->stripe_shift = ffs(vol->v_strip_size / 1024);
		meta->array_width = vol->v_disks_count;
		if (vol->v_raid_level == G_RAID_VOLUME_RL_RAID1 ||
		    vol->v_raid_level == G_RAID_VOLUME_RL_RAID1E)
			meta->array_width /= 2;
		if (pv->pv_meta != NULL)
			meta->array_number = pv->pv_meta->array_number;
		meta->total_sectors = vol->v_mediasize / vol->v_sectorsize;
		meta->cylinders = meta->total_sectors / (255 * 63) - 1;
		meta->heads = 254;
		meta->sectors = 63;
		meta->volume_id = pv->pv_id;
		rebuild_lba64 = UINT64_MAX;
		rebuild = 0;
		for (i = 0; i < vol->v_disks_count; i++) {
			sd = &vol->v_subdisks[i];
			/* For RAID10 we need to translate order. */
			pos = promise_meta_translate_disk(vol, i);
			meta->disks[pos].flags = PROMISE_F_VALID |
			    PROMISE_F_ASSIGNED;
			if (sd->sd_state == G_RAID_SUBDISK_S_NONE) {
				meta->disks[pos].flags |= 0;
			} else if (sd->sd_state == G_RAID_SUBDISK_S_FAILED) {
				meta->disks[pos].flags |=
				    PROMISE_F_DOWN | PROMISE_F_REDIR;
			} else if (sd->sd_state <= G_RAID_SUBDISK_S_REBUILD) {
				meta->disks[pos].flags |=
				    PROMISE_F_ONLINE | PROMISE_F_REDIR;
				if (sd->sd_state == G_RAID_SUBDISK_S_REBUILD) {
					rebuild_lba64 = min(rebuild_lba64,
					    sd->sd_rebuild_pos / 512);
				} else
					rebuild_lba64 = 0;
				rebuild = 1;
			} else {
				meta->disks[pos].flags |= PROMISE_F_ONLINE;
				if (sd->sd_state < G_RAID_SUBDISK_S_ACTIVE) {
					meta->status |= PROMISE_S_MARKED;
					if (sd->sd_state == G_RAID_SUBDISK_S_RESYNC) {
						rebuild_lba64 = min(rebuild_lba64,
						    sd->sd_rebuild_pos / 512);
					} else
						rebuild_lba64 = 0;
				}
			}
			if (pv->pv_meta != NULL) {
				meta->disks[pos].id = pv->pv_meta->disks[pos].id;
			} else {
				meta->disks[pos].number = i * 2;
				arc4rand(&meta->disks[pos].id,
				    sizeof(meta->disks[pos].id), 0);
			}
		}
		promise_meta_put_name(meta, vol->v_name);

		/* Try to mimic AMD BIOS rebuild/resync behavior. */
		if (rebuild_lba64 != UINT64_MAX) {
			if (rebuild)
				meta->magic_3 = 0x03040010UL; /* Rebuild? */
			else
				meta->magic_3 = 0x03040008UL; /* Resync? */
			/* Translate from per-disk to per-volume LBA. */
			if (vol->v_raid_level == G_RAID_VOLUME_RL_RAID1 ||
			    vol->v_raid_level == G_RAID_VOLUME_RL_RAID1E) {
				rebuild_lba64 *= meta->array_width;
			} else if (vol->v_raid_level == G_RAID_VOLUME_RL_RAID3 ||
			    vol->v_raid_level == G_RAID_VOLUME_RL_RAID5) {
				rebuild_lba64 *= meta->array_width - 1;
			} else
				rebuild_lba64 = 0;
		} else
			meta->magic_3 = 0x03000000UL;
		meta->rebuild_lba64 = rebuild_lba64;
		meta->magic_4 = 0x04010101UL;

		/* Replace per-volume metadata with new. */
		if (pv->pv_meta != NULL)
			free(pv->pv_meta, M_MD_PROMISE);
		pv->pv_meta = meta;

		/* Copy new metadata to the disks, adding or replacing old. */
		for (i = 0; i < vol->v_disks_count; i++) {
			sd = &vol->v_subdisks[i];
			disk = sd->sd_disk;
			if (disk == NULL)
				continue;
			/* For RAID10 we need to translate order. */
			pos = promise_meta_translate_disk(vol, i);
			pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;
			for (j = 0; j < pd->pd_subdisks; j++) {
				if (pd->pd_meta[j]->volume_id == meta->volume_id)
					break;
			}
			if (j == pd->pd_subdisks)
				pd->pd_subdisks++;
			if (pd->pd_meta[j] != NULL)
				free(pd->pd_meta[j], M_MD_PROMISE);
			pd->pd_meta[j] = promise_meta_copy(meta);
			pd->pd_meta[j]->disk = meta->disks[pos];
			pd->pd_meta[j]->disk.number = pos;
			pd->pd_meta[j]->disk_offset = sd->sd_offset / 512;
			pd->pd_meta[j]->disk_sectors = sd->sd_size / 512;
			if (sd->sd_state == G_RAID_SUBDISK_S_REBUILD) {
				pd->pd_meta[j]->rebuild_lba =
				    sd->sd_rebuild_pos / 512;
			} else if (sd->sd_state < G_RAID_SUBDISK_S_REBUILD)
				pd->pd_meta[j]->rebuild_lba = 0;
			else
				pd->pd_meta[j]->rebuild_lba = UINT32_MAX;
			pd->pd_updated = 1;
		}
	}

	TAILQ_FOREACH(disk, &sc->sc_disks, d_next) {
		pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;
		if (disk->d_state != G_RAID_DISK_S_ACTIVE)
			continue;
		if (!pd->pd_updated)
			continue;
		G_RAID_DEBUG(1, "Writing Promise metadata to %s",
		    g_raid_get_diskname(disk));
		for (i = 0; i < pd->pd_subdisks; i++)
			g_raid_md_promise_print(pd->pd_meta[i]);
		promise_meta_write(disk->d_consumer,
		    pd->pd_meta, pd->pd_subdisks);
	}

	return (0);
}

static int
g_raid_md_fail_disk_promise(struct g_raid_md_object *md,
    struct g_raid_subdisk *tsd, struct g_raid_disk *tdisk)
{
	struct g_raid_softc *sc;
	struct g_raid_md_promise_object *mdi;
	struct g_raid_md_promise_perdisk *pd;
	struct g_raid_subdisk *sd;
	int i, pos;

	sc = md->mdo_softc;
	mdi = (struct g_raid_md_promise_object *)md;
	pd = (struct g_raid_md_promise_perdisk *)tdisk->d_md_data;

	/* We can't fail disk that is not a part of array now. */
	if (tdisk->d_state != G_RAID_DISK_S_ACTIVE)
		return (-1);

	/*
	 * Mark disk as failed in metadata and try to write that metadata
	 * to the disk itself to prevent it's later resurrection as STALE.
	 */
	if (pd->pd_subdisks > 0 && tdisk->d_consumer != NULL)
		G_RAID_DEBUG(1, "Writing Promise metadata to %s",
		    g_raid_get_diskname(tdisk));
	for (i = 0; i < pd->pd_subdisks; i++) {
		pd->pd_meta[i]->disk.flags |=
		    PROMISE_F_DOWN | PROMISE_F_REDIR;
		pos = pd->pd_meta[i]->disk.number;
		if (pos >= 0 && pos < PROMISE_MAX_DISKS) {
			pd->pd_meta[i]->disks[pos].flags |=
			    PROMISE_F_DOWN | PROMISE_F_REDIR;
		}
		g_raid_md_promise_print(pd->pd_meta[i]);
	}
	if (tdisk->d_consumer != NULL)
		promise_meta_write(tdisk->d_consumer,
		    pd->pd_meta, pd->pd_subdisks);

	/* Change states. */
	g_raid_change_disk_state(tdisk, G_RAID_DISK_S_FAILED);
	TAILQ_FOREACH(sd, &tdisk->d_subdisks, sd_next) {
		g_raid_change_subdisk_state(sd,
		    G_RAID_SUBDISK_S_FAILED);
		g_raid_event_send(sd, G_RAID_SUBDISK_E_FAILED,
		    G_RAID_EVENT_SUBDISK);
	}

	/* Write updated metadata to remaining disks. */
	g_raid_md_write_promise(md, NULL, NULL, tdisk);

	/* Check if anything left except placeholders. */
	if (g_raid_ndisks(sc, -1) ==
	    g_raid_ndisks(sc, G_RAID_DISK_S_OFFLINE))
		g_raid_destroy_node(sc, 0);
	else
		g_raid_md_promise_refill(sc);
	return (0);
}

static int
g_raid_md_free_disk_promise(struct g_raid_md_object *md,
    struct g_raid_disk *disk)
{
	struct g_raid_md_promise_perdisk *pd;
	int i;

	pd = (struct g_raid_md_promise_perdisk *)disk->d_md_data;
	for (i = 0; i < pd->pd_subdisks; i++) {
		if (pd->pd_meta[i] != NULL) {
			free(pd->pd_meta[i], M_MD_PROMISE);
			pd->pd_meta[i] = NULL;
		}
	}
	free(pd, M_MD_PROMISE);
	disk->d_md_data = NULL;
	return (0);
}

static int
g_raid_md_free_volume_promise(struct g_raid_md_object *md,
    struct g_raid_volume *vol)
{
	struct g_raid_md_promise_pervolume *pv;

	pv = (struct g_raid_md_promise_pervolume *)vol->v_md_data;
	if (pv && pv->pv_meta != NULL) {
		free(pv->pv_meta, M_MD_PROMISE);
		pv->pv_meta = NULL;
	}
	if (pv && !pv->pv_started) {
		pv->pv_started = 1;
		callout_stop(&pv->pv_start_co);
		G_RAID_DEBUG1(1, md->mdo_softc,
		    "root_mount_rel %p", pv->pv_rootmount);
		root_mount_rel(pv->pv_rootmount);
		pv->pv_rootmount = NULL;
	}
	return (0);
}

static int
g_raid_md_free_promise(struct g_raid_md_object *md)
{
	struct g_raid_md_promise_object *mdi;

	mdi = (struct g_raid_md_promise_object *)md;
	return (0);
}

G_RAID_MD_DECLARE(g_raid_md_promise);
