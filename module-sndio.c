/* $OpenBSD: module-sndio.c,v 1.9 2016/02/06 07:48:37 ajacoutot Exp $ */
/*
 * Copyright (c) 2012 Eric Faurot <eric@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <sndio.h>

#include "config.h"

#include <pulse/util.h>

#include <pulsecore/core-error.h>
#include <pulsecore/thread.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>

#include "module-sndio-symdef.h"

/*
 * TODO
 *
 * - handle latency correctly
 * - make recording work correctly with playback
 */

PA_MODULE_AUTHOR("Eric Faurot");
PA_MODULE_DESCRIPTION("OpenBSD sndio sink");
PA_MODULE_VERSION("0.0");
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
	"sink_name=<name for the sink> "
	"sink_properties=<properties for the sink> "
	"device=<sndio device> "
	"format=<sample format> "
	"rate=<sample rate> "
	"channels=<number of channels> "
	"channel_map=<channel map> ");

static const char* const modargs[] = {
	"sink_name",
	"sink_properties",
	"device",
	"record",
	"playback",
	"format",
	"rate",
	"channels",
	"channel_map",
	NULL
};

struct userdata {
	pa_core		*core;
	pa_module	*module;
	pa_sink		*sink;

	pa_thread	*thread;
	pa_thread_mq	 thread_mq;
	pa_rtpoll	*rtpoll;
	pa_rtpoll_item	*rtpoll_item;

	pa_memchunk	 memchunk;

	struct sio_hdl	*hdl;
	struct sio_par	 par;
	size_t		 bufsz;

	int		 sink_running;
	unsigned int	 volume;

	int		 set_master;		/* master we're writing */
	int		 last_master;		/* last master we wrote */
	int		 feedback_master;	/* actual master */
};

static void
sndio_on_volume(void *arg, unsigned int vol)
{
	struct userdata *u = arg;

	u->volume = vol;
}

static void
sndio_get_volume(pa_sink *s)
{
	struct userdata *u = s->userdata;
	int		 i;
	uint32_t	 v;

	if (u->feedback_master >= SIO_MAXVOL)
		v = PA_VOLUME_NORM;
	else
		v = PA_CLAMP_VOLUME((u->volume * PA_VOLUME_NORM) / SIO_MAXVOL);

	for (i = 0; i < s->real_volume.channels; i++)
		s->real_volume.values[i] = v;
}

static void
sndio_set_volume(pa_sink *s)
{
	struct userdata *u = s->userdata;

	if (s->real_volume.values[0] >= PA_VOLUME_NORM)
		u->set_master = SIO_MAXVOL;
	else
		u->set_master = (s->real_volume.values[0] * SIO_MAXVOL) / PA_VOLUME_NORM;
}

static int
sndio_sink_message(pa_msgobject *o, int code, void *data, int64_t offset,
    pa_memchunk *chunk)
{
	struct userdata	*u = PA_SINK(o)->userdata;
	pa_sink_state_t	 state;
	int		 ret;

	pa_log_debug(
	    "sndio_sink_msg: obj=%p code=%i data=%p offset=%li chunk=%p",
	    o, code, data, offset, chunk);
	switch (code) {
	case PA_SINK_MESSAGE_GET_LATENCY:
		pa_log_debug("sink:PA_SINK_MESSAGE_GET_LATENCY");
		*(pa_usec_t*)data = pa_bytes_to_usec(u->par.bufsz,
		    &u->sink->sample_spec);
		return (0);
	case PA_SINK_MESSAGE_SET_STATE:
		pa_log_debug("sink:PA_SINK_MESSAGE_SET_STATE ");
		state = (pa_sink_state_t)(data);
		switch (state) {
		case PA_SINK_SUSPENDED:
			pa_log_debug("SUSPEND");
			if (u->sink_running == 1)
				sio_stop(u->hdl);
			u->sink_running = 0;
			break;
		case PA_SINK_IDLE:
		case PA_SINK_RUNNING:
			pa_log_debug((code == PA_SINK_IDLE) ? "IDLE":"RUNNING");
			if (u->sink_running == 0)
				sio_start(u->hdl);
			u->sink_running = 1;
			break;
		case PA_SINK_INVALID_STATE:
			pa_log_debug("INVALID_STATE");
			break;
		case PA_SINK_UNLINKED:
			pa_log_debug("UNLINKED");
			break;
		case PA_SINK_INIT:
			pa_log_debug("INIT");
			break;
		}
		break;
	default:
		pa_log_debug("sink:PA_SINK_???");
	}

	ret = pa_sink_process_msg(o, code, data, offset, chunk);

	return (ret);
}

static void
sndio_thread(void *arg)
{
	struct userdata	*u = arg;
	int		 ret;
	short		 revents, events;
	struct pollfd	*fds_sio;
	size_t		 w, r, l;
	char		*p;
	struct pa_memchunk memchunk;

	pa_log_debug("sndio thread starting up");

	pa_thread_mq_install(&u->thread_mq);

	fds_sio = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

	revents = 0;
	for (;;) {
		pa_log_debug("sndio_thread: loop");

		/* ??? oss does that. */
		if (u->sink
		    && PA_SINK_IS_OPENED(u->sink->thread_info.state)
		    && u->sink->thread_info.rewind_requested)
			pa_sink_process_rewind(u->sink, 0);

		if (u->sink &&
		    PA_SINK_IS_OPENED(u->sink->thread_info.state)
		    && (revents & POLLOUT)) {
			if (u->memchunk.length <= 0)
				pa_sink_render(u->sink, u->bufsz, &u->memchunk);
			p = pa_memblock_acquire(u->memchunk.memblock);
			w = sio_write(u->hdl, p + u->memchunk.index,
			    u->memchunk.length);
			pa_memblock_release(u->memchunk.memblock);
			pa_log_debug("wrote %zu bytes of %zu", w,
			    u->memchunk.length);
			u->memchunk.index += w;
			u->memchunk.length -= w;
			if (u->memchunk.length <= 0) {
				pa_memblock_unref(u->memchunk.memblock);
				pa_memchunk_reset(&u->memchunk);
			}
		}

		events = 0;
		if (u->sink &&
		    PA_SINK_IS_OPENED(u->sink->thread_info.state))
			events |= POLLOUT;

		/*
		 * XXX: {sio,mio}_pollfd() return the number
		 * of descriptors to poll(). It's not correct
		 * to assume only 1 descriptor is used
		 */

		sio_pollfd(u->hdl, fds_sio, events);

		if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
	    		goto fail;
		if (ret == 0)
	    		goto finish;

		revents = sio_revents(u->hdl, fds_sio);

		pa_log_debug("sndio_thread: loop ret=%i, revents=%x", ret,
		    (int)revents);

		if (revents & POLLHUP) {
			pa_log("POLLHUP!");
			break;
		}
	}

    fail:
	pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core),
	    PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
	pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);
    finish:
	pa_log_debug("sndio thread shutting down");
}

int
pa__init(pa_module *m)
{
	pa_modargs		*ma = NULL;
	pa_sample_spec		 ss;
	pa_channel_map		 map;
	pa_sink_new_data	 sink;
	pa_source_new_data	 source;

	struct sio_par		 par;
	char			 buf[256];
	const char		*name, *dev;
	struct userdata		*u = NULL;
	int			 nfds;
	struct			 pollfd;

	if ((u = calloc(1, sizeof(struct userdata))) == NULL) {
		pa_log("Failed to allocate userdata");
		goto fail;
	}
	m->userdata = u;
	u->core = m->core;
	u->module = m;
	u->rtpoll = pa_rtpoll_new();
	pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

	if (!(ma = pa_modargs_new(m->argument, modargs))) {
		pa_log("Failed to parse module arguments.");
		goto fail;
	}

	dev = pa_modargs_get_value(ma, "device", NULL);
	if ((u->hdl = sio_open(dev, SIO_PLAY, 1)) == NULL) {
		pa_log("Cannot open sndio device.");
		goto fail;
	}

	ss = m->core->default_sample_spec;
	map = m->core->default_channel_map;

	if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map,
	    PA_CHANNEL_MAP_OSS) < 0) {
		pa_log("Failed to parse sample specification or channel map");
		goto fail;
	}

	sio_initpar(&par);
	par.rate = ss.rate;
	par.pchan = ss.channels;
	par.sig = 1;

	switch (ss.format) {
	case PA_SAMPLE_U8:
		par.bits = 8;
		par.bps = 1;
		par.sig = 0;
		break;
	case PA_SAMPLE_S16LE:
	case PA_SAMPLE_S16BE:
		par.bits = 16;
		par.bps = 2;
		par.le = (ss.format == PA_SAMPLE_S16LE) ? 1 : 0;
		break;
	case PA_SAMPLE_S32LE:
	case PA_SAMPLE_S32BE:
		par.bits = 32;
		par.bps = 4;
		par.le = (ss.format == PA_SAMPLE_S32LE) ? 1 : 0;
		break;
	case PA_SAMPLE_S24LE:
	case PA_SAMPLE_S24BE:
		par.bits = 24;
		par.bps = 3;
		par.le = (ss.format == PA_SAMPLE_S24LE) ? 1 : 0;
		break;
	case PA_SAMPLE_S24_32LE:
	case PA_SAMPLE_S24_32BE:
		par.bits = 24;
		par.bps = 4;
		par.le = (ss.format == PA_SAMPLE_S24_32LE) ? 1 : 0;
		par.msb = 0; /* XXX check this */
		break;
	case PA_SAMPLE_ALAW:
	case PA_SAMPLE_ULAW:
	case PA_SAMPLE_FLOAT32LE:
	case PA_SAMPLE_FLOAT32BE:
	default:
		pa_log("Unsupported sample format");
		goto fail;
	}

	/* XXX what to do with channel map? */

	if (sio_setpar(u->hdl, &par) == -1) {
		pa_log("Could not set requested parameters");
		goto fail;
	}
	if (sio_getpar(u->hdl, &u->par) == -1) {
		pa_log("Could not retreive parameters");
		goto fail;
	}
	if (u->par.rate != par.rate)
		pa_log_warn("rate changed: %u -> %u", par.rate, u->par.rate);
	if (u->par.pchan != par.pchan)
		pa_log_warn("playback channels changed: %u -> %u",
		    par.rchan, u->par.rchan);
	if (u->par.rchan != par.rchan)
		pa_log_warn("record channels changed: %u -> %u",
		    par.rchan, u->par.rchan);
	/* XXX check sample format */

	ss.rate = u->par.rate;
	ss.channels = u->par.pchan;
	/* XXX what to do with map? */

	u->bufsz = u->par.bufsz * u->par.bps * u->par.pchan;

	nfds = sio_nfds(u->hdl);
	u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, nfds);
	if (u->rtpoll_item == NULL) {
		pa_log("could not allocate poll item");
		goto fail;
	}

	pa_sink_new_data_init(&sink);
	sink.driver = __FILE__;
	sink.module = m;
	sink.namereg_fail = true;
	name = pa_modargs_get_value(ma, "sink_name", NULL);
	if (name == NULL) {
		sink.namereg_fail = false;
		snprintf(buf, sizeof (buf), "sndio-sink");
		name = buf;
	}
	pa_sink_new_data_set_name(&sink, name);
	pa_sink_new_data_set_sample_spec(&sink, &ss);
	pa_sink_new_data_set_channel_map(&sink, &map);
	pa_proplist_sets(sink.proplist,
			 PA_PROP_DEVICE_STRING, dev ? dev : "default");
	pa_proplist_sets(sink.proplist,
			 PA_PROP_DEVICE_API, "sndio");
	pa_proplist_sets(sink.proplist,
			 PA_PROP_DEVICE_DESCRIPTION, dev ? dev : "default");
	pa_proplist_sets(sink.proplist,
			 PA_PROP_DEVICE_ACCESS_MODE, "serial");

	if (pa_modargs_get_proplist(ma, "sink_properties",
				    sink.proplist, PA_UPDATE_REPLACE) < 0) {
		pa_log("Invalid sink properties");
		pa_sink_new_data_done(&sink);
		goto fail;
	}

	u->sink = pa_sink_new(m->core, &sink, PA_SINK_LATENCY);
	pa_sink_new_data_done(&sink);
	if (u->sink == NULL) {
		pa_log("Failed to create sync");
		goto fail;
	}

	u->sink->userdata = u;
	u->sink->parent.process_msg = sndio_sink_message;
	pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
	pa_sink_set_rtpoll(u->sink, u->rtpoll);
	pa_sink_set_fixed_latency(u->sink,
				  pa_bytes_to_usec(u->bufsz, &u->sink->sample_spec));

	sio_onvol(u->hdl, sndio_on_volume, u);
	pa_sink_set_get_volume_callback(u->sink, sndio_get_volume);
	pa_sink_set_set_volume_callback(u->sink, sndio_set_volume);
	u->sink->n_volume_steps = SIO_MAXVOL + 1;

	pa_log_debug("buffer: frame=%u bytes=%zu msec=%u", u->par.bufsz,
	    u->bufsz, (unsigned int) pa_bytes_to_usec(u->bufsz, &u->sink->sample_spec));

	pa_memchunk_reset(&u->memchunk);

	if ((u->thread = pa_thread_new("sndio", sndio_thread, u)) == NULL) {
		pa_log("Failed to create sndio thread.");
		goto fail;
	}

	if (u->sink)
		pa_sink_put(u->sink);

	pa_modargs_free(ma);

	return (0);
fail:
	if (u)
		pa__done(m);
	if (ma)
		pa_modargs_free(ma);

	return (-1);
}

void
pa__done(pa_module *m)
{
	struct userdata *u;

	if (!(u = m->userdata))
		return;

	if (u->sink)
		pa_sink_unlink(u->sink);
	if (u->thread) {
		pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN,
		    NULL, 0, NULL);
		pa_thread_free(u->thread);
	}
	pa_thread_mq_done(&u->thread_mq);

	if (u->sink)
		pa_sink_unref(u->sink);
	if (u->memchunk.memblock)
		pa_memblock_unref(u->memchunk.memblock);
	if (u->rtpoll_item)
		pa_rtpoll_item_free(u->rtpoll_item);
	if (u->rtpoll)
		pa_rtpoll_free(u->rtpoll);
	if (u->hdl)
		sio_close(u->hdl);
	free(u);
}
