/*
 * Copyright 2026 NetSurf Switch port contributors
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/**
 * \file
 * Inline media playback for the framebuffer frontend (HTML <video> and
 * <audio>), built on ffmpeg.
 *
 * Each player runs a decode thread. Network resources are read through
 * libcurl (so https works even though the Switch ffmpeg build has no
 * TLS) with a single sequential connection that is restarted with a
 * Range request on seeks. On the Switch, video is decoded by the
 * nvtegra hardware decoder when the codec allows, falling back to
 * software. Audio is resampled to 48kHz stereo and queued to SDL; its
 * playback position is the master clock (the wall clock is used for
 * silent media). Decoded frames are converted to the size they are
 * shown at and published for the page redraw to blit.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include "utils/log.h"
#include "utils/nsoption.h"
#include "utils/useragent.h"
#include "netsurf/types.h"
#include "netsurf/plotters.h"
#include "netsurf/media.h"

#include "framebuffer/fbfx.h"
#include "framebuffer/fbmedia.h"

#define AUDIO_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_BPS (AUDIO_RATE * AUDIO_CHANNELS * 2)
/* keep this much audio queued ahead of the device */
#define AUDIO_AHEAD 0.35
/* largest frame kept for display */
#define MAX_FRAME_W 1920
#define MAX_FRAME_H 1080

/* ------------------------------------------------------------------ */
/* Streaming input over libcurl                                       */
/* ------------------------------------------------------------------ */

#define STREAM_HIGH_WATER (4 * 1024 * 1024)

struct mstream {
	char *url;
	char *referer;
	CURLM *multi;
	CURL *easy;
	struct curl_slist *hdrs;

	uint8_t *buf;        /* buffered bytes, starting at buf_pos */
	size_t len, cap;
	int64_t buf_pos;     /* file offset of buf[0] */
	int64_t pos;         /* read position */
	int64_t size;        /* total size, -1 if unknown */
	int64_t skip;        /* bytes to discard (server ignored Range) */
	bool done;           /* transfer finished */
	bool failed;
	bool paused;
	bool body_seen;
	long status;
	volatile bool *abort;
};

static size_t ms_header(char *b, size_t sz, size_t n, void *p)
{
	struct mstream *s = p;
	size_t len = sz * n;
	long long a, e, total;

	if (len > 14 && strncasecmp(b, "Content-Range:", 14) == 0) {
		const char *slash = memchr(b, '/', len);
		if (sscanf(b + 14, " bytes %lld-%lld/%lld", &a, &e,
				&total) == 3)
			s->size = total;
		else if (slash != NULL && slash[1] != '*')
			s->size = strtoll(slash + 1, NULL, 10);
	} else if (len > 15 && strncasecmp(b, "Content-Length:", 15) == 0 &&
			s->size < 0 && s->buf_pos == 0 && s->len == 0) {
		s->size = strtoll(b + 15, NULL, 10);
	} else if (len > 5 && strncmp(b, "HTTP/", 5) == 0) {
		const char *sp = memchr(b, ' ', len);
		if (sp != NULL)
			s->status = strtol(sp + 1, NULL, 10);
	}
	return len;
}

static size_t ms_write(char *d, size_t sz, size_t n, void *p)
{
	struct mstream *s = p;
	size_t len = sz * n, off = 0;

	if (s->status >= 400)
		return 0;
	if (!s->body_seen) {
		s->body_seen = true;
		/* a 200 in reply to a Range request carries the whole file */
		if (s->status == 200 && s->buf_pos > 0)
			s->skip = s->buf_pos;
	}
	if (s->skip > 0) {
		/* the server sent the whole resource: drop the prefix */
		size_t k = (int64_t)len < s->skip ? len : (size_t)s->skip;
		s->skip -= k;
		off = k;
		if (off == len)
			return len;
	}
	if (s->len + (len - off) > STREAM_HIGH_WATER && s->len > 0) {
		s->paused = true;
		return CURL_WRITEFUNC_PAUSE;
	}
	if (s->len + (len - off) > s->cap) {
		size_t cap = s->cap ? s->cap : 65536;
		uint8_t *nb;
		while (cap < s->len + (len - off))
			cap *= 2;
		nb = realloc(s->buf, cap);
		if (nb == NULL)
			return 0;
		s->buf = nb;
		s->cap = cap;
	}
	memcpy(s->buf + s->len, d + off, len - off);
	s->len += len - off;
	return len;
}

static void ms_stop(struct mstream *s)
{
	if (s->easy != NULL) {
		curl_multi_remove_handle(s->multi, s->easy);
		curl_easy_cleanup(s->easy);
		s->easy = NULL;
	}
}

/** (re)start the transfer at offset */
static bool ms_start(struct mstream *s, int64_t offset)
{
	char range[64];
	const char *ca;

	ms_stop(s);
	s->len = 0;
	s->buf_pos = offset;
	s->done = false;
	s->failed = false;
	s->paused = false;
	s->body_seen = false;
	s->status = 0;
	s->skip = 0;

	s->easy = curl_easy_init();
	if (s->easy == NULL)
		return false;
	curl_easy_setopt(s->easy, CURLOPT_URL, s->url);
	curl_easy_setopt(s->easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(s->easy, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(s->easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(s->easy, CURLOPT_USERAGENT, user_agent_string());
	curl_easy_setopt(s->easy, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(s->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(s->easy, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(s->easy, CURLOPT_ACCEPT_ENCODING, "identity");
	curl_easy_setopt(s->easy, CURLOPT_HEADERFUNCTION, ms_header);
	curl_easy_setopt(s->easy, CURLOPT_HEADERDATA, s);
	curl_easy_setopt(s->easy, CURLOPT_WRITEFUNCTION, ms_write);
	curl_easy_setopt(s->easy, CURLOPT_WRITEDATA, s);
	if (s->referer != NULL)
		curl_easy_setopt(s->easy, CURLOPT_REFERER, s->referer);
	ca = nsoption_charp(ca_bundle);
	if (ca != NULL && ca[0] != '\0')
		curl_easy_setopt(s->easy, CURLOPT_CAINFO, ca);
	if (offset > 0) {
		snprintf(range, sizeof(range), "%lld-", (long long)offset);
		curl_easy_setopt(s->easy, CURLOPT_RANGE, range);
	}
	curl_multi_add_handle(s->multi, s->easy);
	return true;
}

/** run the transfer until more data, completion or ~timeout_ms */
static void ms_pump(struct mstream *s, int timeout_ms)
{
	int running = 0, q;
	CURLMsg *msg;

	if (s->easy == NULL || s->done)
		return;
	if (s->paused && s->len < STREAM_HIGH_WATER / 2) {
		s->paused = false;
		curl_easy_pause(s->easy, CURLPAUSE_CONT);
	}
	curl_multi_perform(s->multi, &running);
	if (running > 0 && timeout_ms > 0)
		curl_multi_poll(s->multi, NULL, 0, timeout_ms, NULL);
	curl_multi_perform(s->multi, &running);
	while ((msg = curl_multi_info_read(s->multi, &q)) != NULL) {
		if (msg->msg == CURLMSG_DONE) {
			s->done = true;
			if (msg->data.result != CURLE_OK || s->status >= 400) {
				s->failed = true;
				NSLOG(netsurf, WARNING, "media fetch %s: %s (%ld)",
					s->url,
					curl_easy_strerror(msg->data.result),
					s->status);
			}
		}
	}
}

static int ms_read(void *opaque, uint8_t *out, int size)
{
	struct mstream *s = opaque;
	size_t avail, n;

	for (;;) {
		if (*s->abort)
			return AVERROR_EXIT;
		if (s->pos >= s->buf_pos && s->pos < s->buf_pos + (int64_t)s->len)
			break;
		if (s->pos < s->buf_pos ||
		    s->pos > s->buf_pos + (int64_t)s->len + 256 * 1024) {
			/* outside the stream window: reconnect */
			if (!ms_start(s, s->pos))
				return AVERROR(EIO);
			continue;
		}
		if (s->done) {
			if (s->failed && s->len == 0)
				return AVERROR(EIO);
			if (s->pos >= s->buf_pos + (int64_t)s->len)
				return AVERROR_EOF;
		}
		ms_pump(s, 50);
	}

	avail = s->buf_pos + s->len - s->pos;
	n = avail < (size_t)size ? avail : (size_t)size;
	memcpy(out, s->buf + (s->pos - s->buf_pos), n);
	s->pos += n;

	/* drop consumed data, keeping some behind for small seeks */
	if (s->pos - s->buf_pos > 1024 * 1024) {
		size_t drop = (s->pos - s->buf_pos) - 256 * 1024;
		memmove(s->buf, s->buf + drop, s->len - drop);
		s->len -= drop;
		s->buf_pos += drop;
	}
	return (int)n;
}

static int64_t ms_seek(void *opaque, int64_t offset, int whence)
{
	struct mstream *s = opaque;

	switch (whence & ~AVSEEK_FORCE) {
	case AVSEEK_SIZE:
		while (s->size < 0 && !s->done && s->status == 0 &&
				!*s->abort)
			ms_pump(s, 50);
		return s->size;
	case SEEK_SET:
		s->pos = offset;
		break;
	case SEEK_CUR:
		s->pos += offset;
		break;
	case SEEK_END:
		if (s->size < 0)
			return -1;
		s->pos = s->size + offset;
		break;
	default:
		return -1;
	}
	return s->pos;
}

static struct mstream *ms_open(const char *url, const char *referer,
		volatile bool *abort)
{
	struct mstream *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	s->url = strdup(url);
	s->referer = referer ? strdup(referer) : NULL;
	s->size = -1;
	s->abort = abort;
	s->multi = curl_multi_init();
	if (s->url == NULL || s->multi == NULL || !ms_start(s, 0)) {
		if (s->multi)
			curl_multi_cleanup(s->multi);
		free(s->url);
		free(s->referer);
		free(s);
		return NULL;
	}
	return s;
}

static void ms_close(struct mstream *s)
{
	if (s == NULL)
		return;
	ms_stop(s);
	curl_multi_cleanup(s->multi);
	free(s->buf);
	free(s->url);
	free(s->referer);
	free(s);
}

/* ------------------------------------------------------------------ */
/* Shared audio output: one SDL device, a software mixer and a ring    */
/* buffer per player                                                  */
/* ------------------------------------------------------------------ */

#define TRACK_BYTES (AUDIO_BPS)   /* one second */

struct atrack {
	struct atrack *next;
	uint8_t ring[TRACK_BYTES];
	size_t head, count;   /* read position, bytes buffered */
	bool paused;
};

static SDL_AudioDeviceID mix_dev;
static struct atrack *mix_tracks;

static void mix_callback(void *ud, Uint8 *stream, int len)
{
	struct atrack *t;
	int16_t *out = (int16_t *)stream;
	int n = len / 2, i;

	(void)ud;
	memset(stream, 0, len);
	for (t = mix_tracks; t != NULL; t = t->next) {
		size_t take, done = 0;
		if (t->paused || t->count == 0)
			continue;
		take = t->count < (size_t)len ? t->count : (size_t)len;
		while (done < take) {
			size_t chunk = TRACK_BYTES - t->head;
			const int16_t *src;
			if (chunk > take - done)
				chunk = take - done;
			src = (const int16_t *)(t->ring + t->head);
			for (i = 0; i < (int)(chunk / 2); i++) {
				int v = out[done / 2 + i] + src[i];
				out[done / 2 + i] = v > 32767 ? 32767 :
					(v < -32768 ? -32768 : v);
			}
			t->head = (t->head + chunk) % TRACK_BYTES;
			t->count -= chunk;
			done += chunk;
		}
	}
	(void)n;
}

static struct atrack *trk_open(void)
{
	struct atrack *t;

	if (mix_dev == 0) {
		SDL_AudioSpec want, have;
		if (!SDL_WasInit(SDL_INIT_AUDIO) &&
		    SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
			NSLOG(netsurf, INFO, "media: no audio: %s",
					SDL_GetError());
			return NULL;
		}
		memset(&want, 0, sizeof(want));
		want.freq = AUDIO_RATE;
		want.format = AUDIO_S16SYS;
		want.channels = AUDIO_CHANNELS;
		want.samples = 1024;
		want.callback = mix_callback;
		mix_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (mix_dev == 0) {
			NSLOG(netsurf, INFO, "media: audio open failed: %s",
					SDL_GetError());
			return NULL;
		}
		SDL_PauseAudioDevice(mix_dev, 0);
	}
	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->paused = true;
	SDL_LockAudioDevice(mix_dev);
	t->next = mix_tracks;
	mix_tracks = t;
	SDL_UnlockAudioDevice(mix_dev);
	return t;
}

static void trk_close(struct atrack *t)
{
	struct atrack **pp;

	SDL_LockAudioDevice(mix_dev);
	for (pp = &mix_tracks; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	}
	SDL_UnlockAudioDevice(mix_dev);
	free(t);
}

static uint32_t trk_queued(struct atrack *t)
{
	uint32_t n;

	SDL_LockAudioDevice(mix_dev);
	n = t->count;
	SDL_UnlockAudioDevice(mix_dev);
	return n;
}

static void trk_clear(struct atrack *t)
{
	SDL_LockAudioDevice(mix_dev);
	t->count = 0;
	SDL_UnlockAudioDevice(mix_dev);
}

static void trk_pause(struct atrack *t, int pause)
{
	SDL_LockAudioDevice(mix_dev);
	t->paused = pause != 0;
	SDL_UnlockAudioDevice(mix_dev);
}

/** buffer audio; returns bytes accepted */
static size_t trk_push(struct atrack *t, const uint8_t *buf, size_t len)
{
	size_t tail, n, done = 0;

	SDL_LockAudioDevice(mix_dev);
	n = TRACK_BYTES - t->count;
	if (len > n)
		len = n & ~(size_t)3;
	tail = (t->head + t->count) % TRACK_BYTES;
	while (done < len) {
		size_t chunk = TRACK_BYTES - tail;
		if (chunk > len - done)
			chunk = len - done;
		memcpy(t->ring + tail, buf + done, chunk);
		tail = (tail + chunk) % TRACK_BYTES;
		done += chunk;
	}
	t->count += len;
	SDL_UnlockAudioDevice(mix_dev);
	return len;
}

/* ------------------------------------------------------------------ */
/* Player                                                             */
/* ------------------------------------------------------------------ */

struct gui_media {
	char *url;
	char *referer;

	pthread_t thread;
	bool thread_ok;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	/* requests from the main thread (lock) */
	volatile bool quit;
	bool want_play;
	bool loop;
	double seek_to;      /* < 0: none */
	float volume;
	bool muted;
	int req_w, req_h;    /* size frames are shown at */

	/* status (lock) */
	enum gui_media_state state;
	double duration;
	int vw, vh;
	bool has_audio;
	bool buffering;
	double shown_pos;    /* position when not playing */

	/* published frame (lock) */
	uint32_t *frame;
	int fw, fh;
	uint32_t frame_seq;

	/* clock (lock) */
	struct atrack *adev;   /* audio output track, or NULL */
	double audio_base;    /* media time of the first queued sample */
	uint64_t audio_queued; /* bytes queued since audio_base */
	double wall_base;     /* media time at wall_start */
	double wall_start;    /* wall clock when (re)started, <0 if stopped */
};

static double now_s(void)
{
	return av_gettime_relative() / 1000000.0;
}

/** media time of what is being heard/seen now (lock held) */
static double media_clock(struct gui_media *m)
{
	if (m->adev != 0 && m->has_audio) {
		uint32_t q = trk_queued(m->adev);
		double played = m->audio_queued > q ?
				(double)(m->audio_queued - q) / AUDIO_BPS : 0;
		return m->audio_base + played;
	}
	if (m->wall_start >= 0)
		return m->wall_base + (now_s() - m->wall_start);
	return m->wall_base;
}

static void clock_reset(struct gui_media *m, double t)
{
	m->audio_base = t;
	m->audio_queued = 0;
	if (m->adev != 0)
		trk_clear(m->adev);
	m->wall_base = t;
	m->wall_start = (m->state == GUI_MEDIA_PLAYING) ? now_s() : -1;
}

static void set_state(struct gui_media *m, enum gui_media_state st)
{
	if (m->state == GUI_MEDIA_PLAYING && st != GUI_MEDIA_PLAYING) {
		/* freeze the wall clock */
		m->wall_base = media_clock(m);
		m->wall_start = -1;
		if (m->adev != 0)
			trk_pause(m->adev, 1);
	} else if (st == GUI_MEDIA_PLAYING && m->state != GUI_MEDIA_PLAYING) {
		m->wall_start = now_s();
		if (m->adev != 0)
			trk_pause(m->adev, 0);
	}
	m->state = st;
}

static bool audio_open(struct gui_media *m)
{
	m->adev = trk_open();
	if (m->adev != NULL && m->state == GUI_MEDIA_PLAYING)
		trk_pause(m->adev, 0);
	return m->adev != NULL;
}

struct decoder {
	struct gui_media *m;
	AVFormatContext *fmt;
	AVIOContext *avio;
	struct mstream *io;
	AVCodecContext *vctx, *actx;
	AVBufferRef *hwdev;
	int vs, as;
	struct SwsContext *sws;
	SwrContext *swr;
	uint32_t *back;
	int back_w, back_h;
	bool hw_failed;
	int frames_decoded;
};

#ifdef __SWITCH__
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
		const enum AVPixelFormat *fmts)
{
	const enum AVPixelFormat *p;

	for (p = fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == AV_PIX_FMT_NVTEGRA)
			return *p;
	return fmts[0];
}
#endif

static bool open_video(struct decoder *d, bool try_hw)
{
	const AVCodec *codec = NULL;
	AVStream *st;
	int idx;

	idx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
			&codec, 0);
	if (idx < 0 || codec == NULL)
		return false;
	st = d->fmt->streams[idx];
	if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
		return false;
	d->vctx = avcodec_alloc_context3(codec);
	if (d->vctx == NULL)
		return false;
	avcodec_parameters_to_context(d->vctx, st->codecpar);
	d->vctx->pkt_timebase = st->time_base;
	d->vctx->thread_count = 3;
	d->vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

#ifdef __SWITCH__
	if (try_hw && av_hwdevice_ctx_create(&d->hwdev,
			AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0) == 0) {
		d->vctx->hw_device_ctx = av_buffer_ref(d->hwdev);
		d->vctx->get_format = get_hw_format;
		d->vctx->thread_count = 1;
	}
#else
	(void)try_hw;
#endif

	if (avcodec_open2(d->vctx, codec, NULL) < 0) {
		avcodec_free_context(&d->vctx);
		return false;
	}
	d->vs = idx;
	return true;
}

static bool open_audio(struct decoder *d)
{
	const AVCodec *codec = NULL;
	AVStream *st;
	AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
	int idx;

	idx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1,
			&codec, 0);
	if (idx < 0 || codec == NULL)
		return false;
	st = d->fmt->streams[idx];
	d->actx = avcodec_alloc_context3(codec);
	if (d->actx == NULL)
		return false;
	avcodec_parameters_to_context(d->actx, st->codecpar);
	d->actx->pkt_timebase = st->time_base;
	if (avcodec_open2(d->actx, codec, NULL) < 0) {
		avcodec_free_context(&d->actx);
		return false;
	}
	if (swr_alloc_set_opts2(&d->swr, &out, AV_SAMPLE_FMT_S16, AUDIO_RATE,
			&d->actx->ch_layout, d->actx->sample_fmt,
			d->actx->sample_rate, 0, NULL) < 0 ||
	    swr_init(d->swr) < 0) {
		avcodec_free_context(&d->actx);
		swr_free(&d->swr);
		return false;
	}
	d->as = idx;
	return true;
}

/** wait while paused; handle seeks. Returns false on quit. */
static bool wait_ready(struct decoder *d, bool *seeked)
{
	struct gui_media *m = d->m;
	double target = -1;

	pthread_mutex_lock(&m->lock);
	while (!m->quit && !m->want_play && m->seek_to < 0)
		pthread_cond_wait(&m->cond, &m->lock);
	if (m->quit) {
		pthread_mutex_unlock(&m->lock);
		return false;
	}
	if (m->seek_to >= 0) {
		target = m->seek_to;
		m->seek_to = -1;
	}
	if (m->want_play && m->state != GUI_MEDIA_PLAYING)
		set_state(m, GUI_MEDIA_PLAYING);
	pthread_mutex_unlock(&m->lock);

	if (target >= 0) {
		int64_t ts = (int64_t)(target * AV_TIME_BASE);
		avformat_seek_file(d->fmt, -1, INT64_MIN, ts, ts, 0);
		if (d->vctx)
			avcodec_flush_buffers(d->vctx);
		if (d->actx)
			avcodec_flush_buffers(d->actx);
		pthread_mutex_lock(&m->lock);
		clock_reset(m, target);
		m->shown_pos = target;
		if (m->state == GUI_MEDIA_ENDED)
			set_state(m, m->want_play ? GUI_MEDIA_PLAYING :
					GUI_MEDIA_PAUSED);
		pthread_mutex_unlock(&m->lock);
		*seeked = true;
	}
	return true;
}

static void queue_audio(struct decoder *d, AVFrame *f)
{
	struct gui_media *m = d->m;
	int max_out = swr_get_out_samples(d->swr, f->nb_samples);
	uint8_t *buf;
	int n, i;
	float vol;
	bool first;

	if (max_out <= 0)
		return;
	buf = malloc((size_t)max_out * AUDIO_CHANNELS * 2);
	if (buf == NULL)
		return;
	n = swr_convert(d->swr, &buf, max_out,
			(const uint8_t **)f->extended_data, f->nb_samples);
	if (n > 0) {
		pthread_mutex_lock(&m->lock);
		vol = m->muted ? 0.0f : m->volume;
		if (vol < 0.999f) {
			int16_t *s = (int16_t *)buf;
			for (i = 0; i < n * AUDIO_CHANNELS; i++)
				s[i] = (int16_t)(s[i] * vol);
		}
		first = (m->audio_queued == 0);
		if (first && f->best_effort_timestamp != AV_NOPTS_VALUE)
			m->audio_base = f->best_effort_timestamp *
				av_q2d(d->fmt->streams[d->as]->time_base);
		if (m->adev != 0)
			trk_push(m->adev, buf, n * AUDIO_CHANNELS * 2);
		m->audio_queued += (uint64_t)n * AUDIO_CHANNELS * 2;
		pthread_mutex_unlock(&m->lock);
	}
	free(buf);
}

/** convert and publish a frame (called at its presentation time) */
static void show_frame(struct decoder *d, AVFrame *f)
{
	struct gui_media *m = d->m;
	AVFrame *sw = f, *tmp = NULL;
	int ow, oh;
	uint8_t *dst[4];
	int dst_ls[4];
	uint32_t *t;
	int tw, th;

	if (f->hw_frames_ctx != NULL) {
		tmp = av_frame_alloc();
		if (tmp == NULL || av_hwframe_transfer_data(tmp, f, 0) < 0) {
			av_frame_free(&tmp);
			return;
		}
		sw = tmp;
	}

	pthread_mutex_lock(&m->lock);
	ow = m->req_w;
	oh = m->req_h;
	pthread_mutex_unlock(&m->lock);
	if (ow <= 0 || oh <= 0 || ow > sw->width || oh > sw->height) {
		/* never upscale here; the blit scales up */
		ow = sw->width;
		oh = sw->height;
	}
	if (ow > MAX_FRAME_W || oh > MAX_FRAME_H) {
		double k = fmin((double)MAX_FRAME_W / ow,
				(double)MAX_FRAME_H / oh);
		ow = ow * k;
		oh = oh * k;
	}
	if (ow < 1 || oh < 1)
		goto out;

	if (d->back == NULL || d->back_w != ow || d->back_h != oh) {
		free(d->back);
		d->back = malloc((size_t)ow * oh * 4);
		d->back_w = ow;
		d->back_h = oh;
		if (d->back == NULL)
			goto out;
	}
	d->sws = sws_getCachedContext(d->sws, sw->width, sw->height,
			sw->format, ow, oh, AV_PIX_FMT_RGB0,
			SWS_BILINEAR, NULL, NULL, NULL);
	if (d->sws == NULL)
		goto out;
	dst[0] = (uint8_t *)d->back;
	dst[1] = dst[2] = dst[3] = NULL;
	dst_ls[0] = ow * 4;
	dst_ls[1] = dst_ls[2] = dst_ls[3] = 0;
	sws_scale(d->sws, (const uint8_t * const *)sw->data, sw->linesize,
			0, sw->height, dst, dst_ls);

	/* swap buffers; the old front buffer becomes the back buffer */
	pthread_mutex_lock(&m->lock);
	t = m->frame;
	tw = m->fw;
	th = m->fh;
	m->frame = d->back;
	m->fw = ow;
	m->fh = oh;
	m->frame_seq++;
	pthread_mutex_unlock(&m->lock);
	d->back = t;
	d->back_w = tw;
	d->back_h = th;
out:
	av_frame_free(&tmp);
}

/**
 * Wait until a video frame is due. Returns 1 to show it, 0 to drop it,
 * -1 to abandon it (seek/quit).
 */
static int wait_frame(struct decoder *d, double pts)
{
	struct gui_media *m = d->m;

	for (;;) {
		double now;
		bool playing;

		pthread_mutex_lock(&m->lock);
		if (m->quit || m->seek_to >= 0) {
			pthread_mutex_unlock(&m->lock);
			return -1;
		}
		playing = (m->state == GUI_MEDIA_PLAYING);
		now = media_clock(m);
		pthread_mutex_unlock(&m->lock);

		if (!playing) {
			bool seeked = false;
			/* show the first frame even while paused */
			if (d->frames_decoded == 1)
				return 1;
			if (!wait_ready(d, &seeked))
				return -1;
			if (seeked)
				return -1;
			continue;
		}
		if (pts <= now + 0.005)
			return (pts < now - 0.12) ? 0 : 1;
		av_usleep((unsigned)fmin((pts - now) * 1e6, 10000));
	}
}

static void decode_video(struct decoder *d, AVPacket *pkt, AVFrame *f)
{
	AVStream *st = d->fmt->streams[d->vs];
	int r = avcodec_send_packet(d->vctx, pkt);

	if (r < 0 && r != AVERROR(EAGAIN) && d->hwdev != NULL &&
			d->frames_decoded == 0 && !d->hw_failed) {
		/* hardware decode unavailable for this stream */
		NSLOG(netsurf, INFO, "media: hw decode failed, using sw");
		d->hw_failed = true;
		avcodec_free_context(&d->vctx);
		av_buffer_unref(&d->hwdev);
		if (!open_video(d, false)) {
			d->vs = -1;
			return;
		}
		avcodec_send_packet(d->vctx, pkt);
	}
	while (avcodec_receive_frame(d->vctx, f) == 0) {
		double pts = (f->best_effort_timestamp == AV_NOPTS_VALUE) ? 0 :
			f->best_effort_timestamp * av_q2d(st->time_base);
		int w;
		d->frames_decoded++;
		w = wait_frame(d, pts);
		if (w > 0)
			show_frame(d, f);
		av_frame_unref(f);
		if (w < 0)
			break;
	}
}

static void decode_audio(struct decoder *d, AVPacket *pkt, AVFrame *f)
{
	struct gui_media *m = d->m;

	if (avcodec_send_packet(d->actx, pkt) < 0)
		return;
	while (avcodec_receive_frame(d->actx, f) == 0) {
		queue_audio(d, f);
		av_frame_unref(f);
	}
	/* don't run too far ahead of the device */
	for (;;) {
		double ahead;
		bool go;
		pthread_mutex_lock(&m->lock);
		ahead = (m->adev != 0) ?
			(double)trk_queued(m->adev) / AUDIO_BPS : 0;
		go = m->quit || m->seek_to >= 0 ||
			m->state != GUI_MEDIA_PLAYING || ahead < AUDIO_AHEAD;
		pthread_mutex_unlock(&m->lock);
		if (go)
			break;
		av_usleep(10000);
	}
}

static void *media_thread(void *p)
{
	struct gui_media *m = p;
	struct decoder d;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	uint8_t *iobuf;
	bool is_file = strncmp(m->url, "file://", 7) == 0;

	memset(&d, 0, sizeof(d));
	d.m = m;
	d.vs = d.as = -1;

	d.fmt = avformat_alloc_context();
	if (d.fmt == NULL)
		goto fail;
	if (!is_file) {
		d.io = ms_open(m->url, m->referer, &m->quit);
		iobuf = av_malloc(64 * 1024);
		if (d.io == NULL || iobuf == NULL)
			goto fail;
		d.avio = avio_alloc_context(iobuf, 64 * 1024, 0, d.io,
				ms_read, NULL, ms_seek);
		if (d.avio == NULL)
			goto fail;
		d.fmt->pb = d.avio;
		d.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	}
	if (avformat_open_input(&d.fmt, is_file ? m->url + 7 : m->url,
			NULL, NULL) < 0) {
		d.fmt = NULL;
		goto fail;
	}
	if (avformat_find_stream_info(d.fmt, NULL) < 0)
		goto fail;

	open_video(&d, true);
	if (open_audio(&d)) {
		pthread_mutex_lock(&m->lock);
		m->has_audio = audio_open(m);
		if (!m->has_audio)
			d.as = -1;
		pthread_mutex_unlock(&m->lock);
	}
	if (d.vs < 0 && d.as < 0)
		goto fail;

	pthread_mutex_lock(&m->lock);
	if (d.fmt->duration > 0)
		m->duration = d.fmt->duration / (double)AV_TIME_BASE;
	if (d.vctx != NULL) {
		m->vw = d.vctx->width;
		m->vh = d.vctx->height;
	}
	if (m->state == GUI_MEDIA_LOADING)
		set_state(m, GUI_MEDIA_READY);
	pthread_mutex_unlock(&m->lock);
	NSLOG(netsurf, INFO, "media: opened %s (%dx%d, %.1fs, audio %d)",
			m->url, m->vw, m->vh, m->duration, m->has_audio);

	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	if (pkt == NULL || frame == NULL)
		goto fail;

	for (;;) {
		bool seeked = false;
		int r;

		/* decode the first frame for display before waiting */
		if (d.frames_decoded > 0 || d.vs < 0) {
			if (!wait_ready(&d, &seeked))
				break;
		} else {
			pthread_mutex_lock(&m->lock);
			if (m->quit) {
				pthread_mutex_unlock(&m->lock);
				break;
			}
			pthread_mutex_unlock(&m->lock);
		}

		r = av_read_frame(d.fmt, pkt);
		if (r == AVERROR_EXIT)
			break;
		if (r < 0) {
			bool loop;
			/* end of stream: let audio drain, then finish */
			pthread_mutex_lock(&m->lock);
			loop = m->loop;
			pthread_mutex_unlock(&m->lock);
			for (;;) {
				bool stop;
				pthread_mutex_lock(&m->lock);
				stop = m->quit || m->seek_to >= 0 ||
					m->adev == 0 || !m->has_audio ||
					trk_queued(m->adev) == 0 ||
					m->state != GUI_MEDIA_PLAYING;
				pthread_mutex_unlock(&m->lock);
				if (stop)
					break;
				av_usleep(20000);
			}
			pthread_mutex_lock(&m->lock);
			if (m->seek_to < 0 && !m->quit) {
				if (loop) {
					m->seek_to = 0;
				} else {
					m->shown_pos = m->duration > 0 ?
						m->duration : media_clock(m);
					set_state(m, GUI_MEDIA_ENDED);
					m->want_play = false;
				}
			}
			pthread_mutex_unlock(&m->lock);
			continue;
		}

		if (pkt->stream_index == d.vs && d.vctx != NULL)
			decode_video(&d, pkt, frame);
		else if (pkt->stream_index == d.as && d.actx != NULL)
			decode_audio(&d, pkt, frame);
		av_packet_unref(pkt);
	}
	goto out;

fail:
	NSLOG(netsurf, WARNING, "media: cannot play %s", m->url);
	pthread_mutex_lock(&m->lock);
	set_state(m, GUI_MEDIA_ERROR);
	pthread_mutex_unlock(&m->lock);
out:
	av_packet_free(&pkt);
	av_frame_free(&frame);
	if (d.vctx)
		avcodec_free_context(&d.vctx);
	if (d.actx)
		avcodec_free_context(&d.actx);
	av_buffer_unref(&d.hwdev);
	if (d.sws)
		sws_freeContext(d.sws);
	if (d.swr)
		swr_free(&d.swr);
	if (d.fmt)
		avformat_close_input(&d.fmt);
	if (d.avio) {
		av_freep(&d.avio->buffer);
		avio_context_free(&d.avio);
	}
	ms_close(d.io);
	free(d.back);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* gui_media_table                                                    */
/* ------------------------------------------------------------------ */

static struct gui_media *fbmedia_create(const char *url, const char *referer)
{
	struct gui_media *m = calloc(1, sizeof(*m));

	if (m == NULL)
		return NULL;
	m->url = strdup(url);
	m->referer = referer ? strdup(referer) : NULL;
	m->seek_to = -1;
	m->volume = 1.0f;
	m->state = GUI_MEDIA_LOADING;
	m->wall_start = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->cond, NULL);
	if (m->url == NULL ||
	    pthread_create(&m->thread, NULL, media_thread, m) != 0) {
		m->state = GUI_MEDIA_ERROR;
		return m;
	}
	m->thread_ok = true;
	return m;
}

static void fbmedia_destroy(struct gui_media *m)
{
	if (m == NULL)
		return;
	pthread_mutex_lock(&m->lock);
	m->quit = true;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->lock);
	if (m->thread_ok)
		pthread_join(m->thread, NULL);
	if (m->adev != 0)
		trk_close(m->adev);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->cond);
	free(m->frame);
	free(m->url);
	free(m->referer);
	free(m);
}

static void fbmedia_play(struct gui_media *m)
{
	pthread_mutex_lock(&m->lock);
	m->want_play = true;
	if (m->state == GUI_MEDIA_ENDED) {
		m->seek_to = 0;
	} else if (m->state == GUI_MEDIA_READY ||
			m->state == GUI_MEDIA_PAUSED) {
		set_state(m, GUI_MEDIA_PLAYING);
	}
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->lock);
}

static void fbmedia_pause(struct gui_media *m)
{
	pthread_mutex_lock(&m->lock);
	m->want_play = false;
	if (m->state == GUI_MEDIA_PLAYING) {
		m->shown_pos = media_clock(m);
		set_state(m, GUI_MEDIA_PAUSED);
	}
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->lock);
}

static void fbmedia_seek(struct gui_media *m, double t)
{
	pthread_mutex_lock(&m->lock);
	if (t < 0)
		t = 0;
	if (m->duration > 0 && t > m->duration)
		t = m->duration;
	m->seek_to = t;
	m->shown_pos = t;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->lock);
}

static void fbmedia_set_volume(struct gui_media *m, float v, bool muted)
{
	pthread_mutex_lock(&m->lock);
	m->volume = v < 0 ? 0 : (v > 1 ? 1 : v);
	m->muted = muted;
	pthread_mutex_unlock(&m->lock);
}

static void fbmedia_set_loop(struct gui_media *m, bool loop)
{
	pthread_mutex_lock(&m->lock);
	m->loop = loop;
	pthread_mutex_unlock(&m->lock);
}

static void fbmedia_status(struct gui_media *m, struct gui_media_status *st)
{
	pthread_mutex_lock(&m->lock);
	st->state = m->state;
	st->duration = m->duration;
	st->width = m->vw;
	st->height = m->vh;
	st->frame_seq = m->frame_seq;
	st->has_audio = m->has_audio;
	st->buffering = false;
	if (m->state == GUI_MEDIA_PLAYING && m->seek_to < 0)
		st->position = media_clock(m);
	else
		st->position = m->shown_pos;
	if (st->position < 0)
		st->position = 0;
	if (m->duration > 0 && st->position > m->duration)
		st->position = m->duration;
	if (m->state == GUI_MEDIA_PLAYING)
		m->shown_pos = st->position;
	pthread_mutex_unlock(&m->lock);
}

static bool fbmedia_redraw(struct gui_media *m,
		const struct redraw_context *ctx, const struct rect *dst,
		const struct rect *clip)
{
	bool ok = false;

	pthread_mutex_lock(&m->lock);
	m->req_w = dst->x1 - dst->x0;
	m->req_h = dst->y1 - dst->y0;
	if (m->frame != NULL) {
		fbfx_blit_rgbx(ctx, m->frame, m->fw, m->fh, dst, clip);
		ok = true;
	}
	pthread_mutex_unlock(&m->lock);
	return ok;
}

static struct gui_media_table media_table = {
	.create = fbmedia_create,
	.destroy = fbmedia_destroy,
	.play = fbmedia_play,
	.pause = fbmedia_pause,
	.seek = fbmedia_seek,
	.set_volume = fbmedia_set_volume,
	.set_loop = fbmedia_set_loop,
	.status = fbmedia_status,
	.redraw = fbmedia_redraw,
};

struct gui_media_table *framebuffer_media_table = &media_table;
