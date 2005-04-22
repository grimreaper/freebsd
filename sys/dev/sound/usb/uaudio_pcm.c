/* $FreeBSD$ */

/*-
 * Copyright (c) 2000-2002 Hiroyuki Aizu <aizu@navi.org>
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


#include <sys/soundcard.h>
#include <dev/sound/pcm/sound.h>
#include <dev/sound/chip.h>

#include <dev/sound/usb/uaudio.h>

#include "mixer_if.h"

struct ua_info;

struct ua_chinfo {
	struct ua_info *parent;
	struct pcm_channel *channel;
	struct snd_dbuf *buffer;
	u_char *buf;
	int dir, hwch;
	u_int32_t fmt, spd, blksz;	/* XXXXX */
};

struct ua_info {
	device_t sc_dev;
	u_int32_t bufsz;
	struct ua_chinfo pch, rch;
};

static u_int32_t ua_playfmt[8*2+1]; /* 8 format * (stereo or mono) + endptr */

static struct pcmchan_caps ua_playcaps = {8000, 48000, ua_playfmt, 0};

static u_int32_t ua_recfmt[8*2+1]; /* 8 format * (stereo or mono) + endptr */

static struct pcmchan_caps ua_reccaps = {8000, 48000, ua_recfmt, 0};

#define UAUDIO_DEFAULT_BUFSZ		16*1024

/************************************************************/
static void *
ua_chan_init(kobj_t obj, void *devinfo, struct snd_dbuf *b, struct pcm_channel *c, int dir)
{
	device_t pa_dev;

	struct ua_info *sc = devinfo;
	struct ua_chinfo *ch = (dir == PCMDIR_PLAY)? &sc->pch : &sc->rch;

	ch->parent = sc;
	ch->channel = c;
	ch->buffer = b;
	ch->dir = dir;

	pa_dev = device_get_parent(sc->sc_dev);
     	/* Create ua_playfmt[] & ua_recfmt[] */
	uaudio_query_formats(pa_dev, (u_int32_t *)&ua_playfmt, (u_int32_t *)&ua_recfmt);
	if (dir == PCMDIR_PLAY) {
		if (ua_playfmt[0] == 0) {
			printf("play channel supported format list invalid\n");
			return NULL;
		}
	} else {
		if (ua_recfmt[0] == 0) {
			printf("record channel supported format list invalid\n");
			return NULL;
		}

	}

	ch->buf = malloc(sc->bufsz, M_DEVBUF, M_NOWAIT);
        if (ch->buf == NULL)
		return NULL;
	if (sndbuf_setup(b, ch->buf, sc->bufsz) != 0) {
		free(ch->buf, M_DEVBUF);
		return NULL;
	}
	uaudio_chan_set_param_pcm_dma_buff(pa_dev, ch->buf, ch->buf+sc->bufsz, ch->channel, dir);
	if (bootverbose)
		device_printf(pa_dev, "%s buf %p\n", (dir == PCMDIR_PLAY)?
			      "play" : "rec", sndbuf_getbuf(ch->buffer));

	ch->dir = dir;
#ifndef NO_RECORDING
	ch->hwch = 1;
	if (dir == PCMDIR_PLAY)
		ch->hwch = 2;
#else
	ch->hwch = 2;
#endif

	return ch;
}

static int
ua_chan_free(kobj_t obj, void *data)
{
	struct ua_chinfo *ua = data;

	if (ua->buf != NULL)
		free(ua->buf, M_DEVBUF);
	return 0;
}

static int
ua_chan_setformat(kobj_t obj, void *data, u_int32_t format)
{
	device_t pa_dev;
	struct ua_info *ua;

	struct ua_chinfo *ch = data;

	ua = ch->parent;
	pa_dev = device_get_parent(ua->sc_dev);
	uaudio_chan_set_param_format(pa_dev, format, ch->dir);

	ch->fmt = format;
	return 0;
}

static int
ua_chan_setspeed(kobj_t obj, void *data, u_int32_t speed)
{
	device_t pa_dev;
	struct ua_info *ua;

	struct ua_chinfo *ch = data;
	ch->spd = speed;

	ua = ch->parent;
	pa_dev = device_get_parent(ua->sc_dev);
	uaudio_chan_set_param_speed(pa_dev, speed, ch->dir);

	return ch->spd;
}

static int
ua_chan_setblocksize(kobj_t obj, void *data, u_int32_t blocksize)
{
	device_t pa_dev;
	struct ua_chinfo *ch = data;
	struct ua_info *ua = ch->parent;

	if (blocksize) {
		RANGE(blocksize, 128, ua->bufsz / 2);
		if (sndbuf_resize(ch->buffer, ua->bufsz/blocksize, blocksize) != 0) {
			ch->blksz = blocksize;
		}
	}
	pa_dev = device_get_parent(ua->sc_dev);
	uaudio_chan_set_param_blocksize(pa_dev, blocksize, ch->dir);

	return ch->blksz;
}

static int
ua_chan_trigger(kobj_t obj, void *data, int go)
{
	device_t pa_dev;
	struct ua_info *ua;
	struct ua_chinfo *ch = data;

	if (go == PCMTRIG_EMLDMAWR || go == PCMTRIG_EMLDMARD)
		return 0;

	ua = ch->parent;
	pa_dev = device_get_parent(ua->sc_dev);

	/* XXXXX */
	if (ch->dir == PCMDIR_PLAY) {
		if (go == PCMTRIG_START) {
			uaudio_trigger_output(pa_dev);
		} else {
			uaudio_halt_out_dma(pa_dev);
		}
	} else {
#ifndef NO_RECORDING
		if (go == PCMTRIG_START)
			uaudio_trigger_input(pa_dev);
		else
			uaudio_halt_in_dma(pa_dev);
#endif
	}

	return 0;
}

static int
ua_chan_getptr(kobj_t obj, void *data)
{
	device_t pa_dev;
	struct ua_info *ua;
	struct ua_chinfo *ch = data;

	ua = ch->parent;
	pa_dev = device_get_parent(ua->sc_dev);

	return uaudio_chan_getptr(pa_dev, ch->dir);
}

static struct pcmchan_caps *
ua_chan_getcaps(kobj_t obj, void *data)
{
	struct ua_chinfo *ch = data;

	return (ch->dir == PCMDIR_PLAY) ? &ua_playcaps : & ua_reccaps;
}

static kobj_method_t ua_chan_methods[] = {
	KOBJMETHOD(channel_init,		ua_chan_init),
	KOBJMETHOD(channel_free,                ua_chan_free),
	KOBJMETHOD(channel_setformat,		ua_chan_setformat),
	KOBJMETHOD(channel_setspeed,		ua_chan_setspeed),
	KOBJMETHOD(channel_setblocksize,	ua_chan_setblocksize),
	KOBJMETHOD(channel_trigger,		ua_chan_trigger),
	KOBJMETHOD(channel_getptr,		ua_chan_getptr),
	KOBJMETHOD(channel_getcaps,		ua_chan_getcaps),
	{ 0, 0 }
};

CHANNEL_DECLARE(ua_chan);

/************************************************************/
static int
ua_mixer_init(struct snd_mixer *m)
{
	u_int32_t mask;
	device_t pa_dev;
	struct ua_info *ua = mix_getdevinfo(m);

	pa_dev = device_get_parent(ua->sc_dev);

	mask = uaudio_query_mix_info(pa_dev);
	mix_setdevs(m,	mask);

	mask = uaudio_query_recsrc_info(pa_dev);
	mix_setrecdevs(m, mask);

	return 0;
}

static int
ua_mixer_set(struct snd_mixer *m, unsigned type, unsigned left, unsigned right)
{
	device_t pa_dev;
	struct ua_info *ua = mix_getdevinfo(m);

	pa_dev = device_get_parent(ua->sc_dev);
	uaudio_mixer_set(pa_dev, type, left, right);

	return left | (right << 8);
}

static int
ua_mixer_setrecsrc(struct snd_mixer *m, u_int32_t src)
{
	device_t pa_dev;
	struct ua_info *ua = mix_getdevinfo(m);

	pa_dev = device_get_parent(ua->sc_dev);
	return uaudio_mixer_setrecsrc(pa_dev, src);
}

static kobj_method_t ua_mixer_methods[] = {
	KOBJMETHOD(mixer_init,		ua_mixer_init),
	KOBJMETHOD(mixer_set,		ua_mixer_set),
	KOBJMETHOD(mixer_setrecsrc,	ua_mixer_setrecsrc),

	{ 0, 0 }
};
MIXER_DECLARE(ua_mixer);
/************************************************************/


static int
ua_probe(device_t dev)
{
	char *s;
	struct sndcard_func *func;

	/* The parent device has already been probed. */

	func = device_get_ivars(dev);
	if (func == NULL || func->func != SCF_PCM)
		return (ENXIO);

	s = "USB Audio";

	device_set_desc(dev, s);
	return 0;
}

static int
ua_attach(device_t dev)
{
	struct ua_info *ua;
	char status[SND_STATUSLEN];

	ua = (struct ua_info *)malloc(sizeof *ua, M_DEVBUF, M_NOWAIT);
	if (!ua)
		return ENXIO;
	bzero(ua, sizeof *ua);

	ua->sc_dev = dev;

	ua->bufsz = pcm_getbuffersize(dev, 4096, UAUDIO_DEFAULT_BUFSZ, 65536);
	if (bootverbose)
		printf("uaudio: using a default buffer size of %d\n",
		ua->bufsz);

	if (mixer_init(dev, &ua_mixer_class, ua)) {
		return(ENXIO);
	}

	snprintf(status, SND_STATUSLEN, "at addr ?");

#ifndef NO_RECORDING
	if (pcm_register(dev, ua, 1, 1)) {
#else
	if (pcm_register(dev, ua, 1, 0)) {
#endif
		return(ENXIO);
	}

	pcm_addchan(dev, PCMDIR_PLAY, &ua_chan_class, ua);
#ifndef NO_RECORDING
	pcm_addchan(dev, PCMDIR_REC, &ua_chan_class, ua);
#endif
	pcm_setstatus(dev, status);

	return 0;
}

static int
ua_detach(device_t dev)
{
	int r;
	struct ua_info *sc;

	r = pcm_unregister(dev);
	if (r)
		return r;

	sc = pcm_getdevinfo(dev);
	free(sc, M_DEVBUF);

	return 0;
}

/************************************************************/

static device_method_t ua_pcm_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		ua_probe),
	DEVMETHOD(device_attach,	ua_attach),
	DEVMETHOD(device_detach,	ua_detach),

	{ 0, 0 }
};

static driver_t ua_pcm_driver = {
	"pcm",
	ua_pcm_methods,
	PCM_SOFTC_SIZE,
};


DRIVER_MODULE(ua_pcm, uaudio, ua_pcm_driver, pcm_devclass, 0, 0);
MODULE_DEPEND(ua_pcm, uaudio, 1, 1, 1);
MODULE_DEPEND(ua_pcm, snd_pcm, PCM_MINVER, PCM_PREFVER, PCM_MAXVER);
MODULE_VERSION(ua_pcm, 1);
