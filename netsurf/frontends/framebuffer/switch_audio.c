/*
 * Non-blocking audio playback for the NetSurf Switch port.
 *
 * The modal switch_player blocks the browser for the whole track. This
 * plays audio in slices instead: each pump() decodes just enough to keep
 * roughly a second queued in SDL, then returns, so the browser's event
 * loop keeps running and the page stays usable. Position is taken from
 * the audio device drain (bytes queued minus bytes still unplayed),
 * which stays correct across pauses.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framebuffer/switch_audio.h"

#ifdef __SWITCH__

#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include "utils/log.h"

#define AUDIO_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_BYTES_PER_SEC (AUDIO_RATE * AUDIO_CHANNELS * 2)

/* keep about a second buffered; each pump tops back up to this */
#define AUDIO_TARGET_QUEUE (AUDIO_BYTES_PER_SEC)

/* never decode forever in one slice, however starved the queue is */
#define AUDIO_MAX_PACKETS_PER_PUMP 64

static struct {
	bool active;
	bool paused;
	bool eof;
	bool own_file;

	AVFormatContext *fmt;
	AVCodecContext *actx;
	SwrContext *swr;
	AVPacket *pkt;
	AVFrame *frame;
	int astream;

	SDL_AudioDeviceID adev;

	/* position accounting: base is the last seek target, and bytes
	 * are counted from that point so pausing does not drift */
	double base;
	uint64_t queued_bytes;
	double duration;

	char *path;
	char *title;
} au;

static void audio_release(void)
{
	if (au.adev != 0) {
		SDL_CloseAudioDevice(au.adev);
		au.adev = 0;
	}
	if (au.swr != NULL)
		swr_free(&au.swr);
	if (au.actx != NULL)
		avcodec_free_context(&au.actx);
	if (au.fmt != NULL)
		avformat_close_input(&au.fmt);
	if (au.pkt != NULL)
		av_packet_free(&au.pkt);
	if (au.frame != NULL)
		av_frame_free(&au.frame);

	if (au.path != NULL) {
		if (au.own_file)
			remove(au.path);
		free(au.path);
		au.path = NULL;
	}
	free(au.title);
	au.title = NULL;

	au.active = false;
	au.paused = false;
	au.eof = false;
	au.base = 0.0;
	au.queued_bytes = 0;
	au.duration = 0.0;
}

/* exported interface documented in switch_audio.h */
bool switch_audio_start(const char *path, const char *title, bool own_file)
{
	const AVCodec *dec;
	AVStream *st;
	SDL_AudioSpec want, have;
	char urlbuf[512];
	const char *openurl = path;

	switch_audio_stop();

	memset(&au, 0, sizeof(au));

	/* ffmpeg would read the "sdmc:" of a devoptab path as a protocol
	 * name, so local files are opened as file: URLs instead */
	if (strncmp(path, "http:", 5) != 0 &&
	    strncmp(path, "https:", 6) != 0 &&
	    strncmp(path, "file:", 5) != 0) {
		snprintf(urlbuf, sizeof(urlbuf), "file:%s", path);
		openurl = urlbuf;
	}

	if (avformat_open_input(&au.fmt, openurl, NULL, NULL) < 0) {
		NSLOG(netsurf, INFO, "audio: cannot open %s", openurl);
		return false;
	}
	if (avformat_find_stream_info(au.fmt, NULL) < 0)
		goto fail;

	au.astream = av_find_best_stream(au.fmt, AVMEDIA_TYPE_AUDIO,
					 -1, -1, NULL, 0);
	if (au.astream < 0)
		goto fail;

	st = au.fmt->streams[au.astream];
	dec = avcodec_find_decoder(st->codecpar->codec_id);
	if (dec == NULL)
		goto fail;

	au.actx = avcodec_alloc_context3(dec);
	if (au.actx == NULL)
		goto fail;
	avcodec_parameters_to_context(au.actx, st->codecpar);
	if (avcodec_open2(au.actx, dec, NULL) < 0)
		goto fail;

	/* the sdl2 surface only inits video; audio is brought up here */
	if (SDL_WasInit(SDL_INIT_AUDIO) == 0 &&
	    SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		NSLOG(netsurf, INFO, "audio: init failed: %s", SDL_GetError());
		goto fail;
	}

	SDL_memset(&want, 0, sizeof(want));
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = AUDIO_CHANNELS;
	want.samples = 2048;

	au.adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (au.adev == 0) {
		NSLOG(netsurf, INFO, "audio: open device failed: %s",
		      SDL_GetError());
		goto fail;
	}

	au.pkt = av_packet_alloc();
	au.frame = av_frame_alloc();
	if (au.pkt == NULL || au.frame == NULL)
		goto fail;

	if (au.fmt->duration > 0)
		au.duration = (double)au.fmt->duration / AV_TIME_BASE;

	au.path = strdup(path);
	au.title = strdup(title != NULL ? title : "");
	au.own_file = own_file;
	au.active = true;
	au.paused = false;

	SDL_PauseAudioDevice(au.adev, 0);

	NSLOG(netsurf, INFO, "audio: playing %s (%.1fs)", path, au.duration);
	return true;

fail:
	audio_release();
	return false;
}

/* exported interface documented in switch_audio.h */
void switch_audio_stop(void)
{
	if (au.adev != 0)
		SDL_ClearQueuedAudio(au.adev);
	if (au.active || au.fmt != NULL)
		audio_release();
}

/* resample one decoded frame into the SDL queue */
static void audio_queue_frame(AVFrame *frame)
{
	uint8_t *out = NULL;
	int out_samples;

	if (au.swr == NULL) {
		AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;

		if (swr_alloc_set_opts2(&au.swr,
					&out_layout, AV_SAMPLE_FMT_S16,
					AUDIO_RATE, &frame->ch_layout,
					frame->format, frame->sample_rate,
					0, NULL) < 0)
			return;
		if (swr_init(au.swr) < 0)
			return;
	}

	out_samples = av_rescale_rnd(
		swr_get_delay(au.swr, frame->sample_rate) + frame->nb_samples,
		AUDIO_RATE, frame->sample_rate, AV_ROUND_UP);

	if (av_samples_alloc(&out, NULL, AUDIO_CHANNELS, out_samples,
			     AV_SAMPLE_FMT_S16, 0) < 0)
		return;

	out_samples = swr_convert(au.swr, &out, out_samples,
				  (const uint8_t **)frame->extended_data,
				  frame->nb_samples);
	if (out_samples > 0) {
		int out_size = out_samples * AUDIO_CHANNELS * 2;

		SDL_QueueAudio(au.adev, out, out_size);
		au.queued_bytes += out_size;
	}

	av_freep(&out);
}

/* exported interface documented in switch_audio.h */
void switch_audio_pump(void)
{
	int packets = 0;

	if (!au.active || au.paused)
		return;

	while (SDL_GetQueuedAudioSize(au.adev) < AUDIO_TARGET_QUEUE &&
	       packets < AUDIO_MAX_PACKETS_PER_PUMP) {
		int ret;

		if (au.eof) {
			/* stop once the device has drained */
			if (SDL_GetQueuedAudioSize(au.adev) == 0) {
				NSLOG(netsurf, INFO, "audio: finished");
				switch_audio_stop();
			}
			return;
		}

		ret = av_read_frame(au.fmt, au.pkt);
		if (ret < 0) {
			au.eof = true;
			avcodec_send_packet(au.actx, NULL); /* flush */
			while (avcodec_receive_frame(au.actx,
						     au.frame) == 0)
				audio_queue_frame(au.frame);
			continue;
		}
		packets++;

		if (au.pkt->stream_index != au.astream) {
			av_packet_unref(au.pkt);
			continue;
		}

		if (avcodec_send_packet(au.actx, au.pkt) == 0) {
			while (avcodec_receive_frame(au.actx,
						     au.frame) == 0)
				audio_queue_frame(au.frame);
		}
		av_packet_unref(au.pkt);
	}
}

/* exported interface documented in switch_audio.h */
void switch_audio_toggle_pause(void)
{
	if (!au.active)
		return;
	au.paused = !au.paused;
	SDL_PauseAudioDevice(au.adev, au.paused ? 1 : 0);
}

/* exported interface documented in switch_audio.h */
void switch_audio_seek_relative(double delta)
{
	double target;
	int64_t ts;

	if (!au.active)
		return;

	target = switch_audio_position() + delta;
	if (target < 0.0)
		target = 0.0;
	if (au.duration > 0.0 && target > au.duration - 0.5)
		target = au.duration - 0.5;
	if (target < 0.0)
		target = 0.0;

	ts = (int64_t)(target * AV_TIME_BASE);
	if (av_seek_frame(au.fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) {
		NSLOG(netsurf, INFO, "audio: seek to %.1f failed", target);
		return;
	}

	avcodec_flush_buffers(au.actx);
	SDL_ClearQueuedAudio(au.adev);

	/* restart the position accounting from the seek point */
	au.base = target;
	au.queued_bytes = 0;
	au.eof = false;
}

/* exported interface documented in switch_audio.h */
bool switch_audio_active(void)
{
	return au.active;
}

/* exported interface documented in switch_audio.h */
bool switch_audio_paused(void)
{
	return au.paused;
}

/* exported interface documented in switch_audio.h */
double switch_audio_position(void)
{
	uint64_t unplayed;

	if (!au.active)
		return 0.0;

	unplayed = SDL_GetQueuedAudioSize(au.adev);
	if (unplayed > au.queued_bytes)
		return au.base;

	return au.base + ((double)(au.queued_bytes - unplayed) /
			  AUDIO_BYTES_PER_SEC);
}

/* exported interface documented in switch_audio.h */
double switch_audio_duration(void)
{
	return au.active ? au.duration : 0.0;
}

/* exported interface documented in switch_audio.h */
const char *switch_audio_title(void)
{
	return (au.active && au.title != NULL) ? au.title : "";
}

#else /* !__SWITCH__ */

bool switch_audio_start(const char *path, const char *title, bool own_file)
{
	(void)path; (void)title; (void)own_file;
	return false;
}
void switch_audio_stop(void) {}
void switch_audio_pump(void) {}
void switch_audio_toggle_pause(void) {}
void switch_audio_seek_relative(double delta) { (void)delta; }
bool switch_audio_active(void) { return false; }
bool switch_audio_paused(void) { return false; }
double switch_audio_position(void) { return 0.0; }
double switch_audio_duration(void) { return 0.0; }
const char *switch_audio_title(void) { return ""; }

#endif
