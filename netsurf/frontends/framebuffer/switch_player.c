/*
 * Minimal ffmpeg/SDL2 media player for the NetSurf Switch port.
 *
 * v1 scope: direct http:// and file/romfs URLs, no seeking, no https
 * (the switch-ffmpeg portlib is built without TLS). Video is converted
 * to YUV420 and presented through the SDL renderer owned by libnsfb's
 * sdl2 surface; audio is resampled to S16 stereo and queued to SDL.
 * Sync is driven by the audio clock, or the wall clock without audio.
 */

#include <string.h>
#include <strings.h>

#include "framebuffer/switch_player.h"

int switch_player_is_media(const char *mime, const char *url)
{
	static const char *exts[] = {
		".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".ts",
		".mp3", ".m4a", ".ogg", ".oga", ".opus", ".flac", ".wav",
		NULL
	};
	int i;

	if (mime != NULL &&
	    (strncasecmp(mime, "video/", 6) == 0 ||
	     strncasecmp(mime, "audio/", 6) == 0))
		return 1;

	if (url != NULL) {
		size_t ulen = strlen(url);
		for (i = 0; exts[i] != NULL; i++) {
			size_t elen = strlen(exts[i]);
			if (ulen > elen &&
			    strncasecmp(url + ulen - elen, exts[i], elen) == 0)
				return 1;
		}
	}

	return 0;
}

#ifdef __SWITCH__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include "libnsfb.h"

#define AUDIO_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_BYTES_PER_SEC (AUDIO_RATE * AUDIO_CHANNELS * 2)
#define AUDIO_QUEUE_MAX (AUDIO_BYTES_PER_SEC * 2) /* cap ~2s of audio */

struct player_state {
	AVFormatContext *fmt;
	AVCodecContext *vctx;
	AVCodecContext *actx;
	int vstream;
	int astream;

	struct SwsContext *sws;
	SwrContext *swr;

	SDL_Renderer *renderer;
	SDL_Texture *tex;
	int tex_w;
	int tex_h;

	SDL_AudioDeviceID adev;
	uint64_t audio_bytes_queued;

	double start_wall; /* wall clock at playback start, seconds */
};

static double player_now(struct player_state *ps)
{
	if (ps->adev != 0 && ps->audio_bytes_queued > 0) {
		uint64_t unplayed = SDL_GetQueuedAudioSize(ps->adev);
		return ((double)ps->audio_bytes_queued - (double)unplayed) /
			AUDIO_BYTES_PER_SEC;
	}
	return (av_gettime_relative() / 1000000.0) - ps->start_wall;
}

/* poll for the user backing out of playback */
static int player_wants_exit(void)
{
	SDL_Event ev;

	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_QUIT:
			return 1;
		case SDL_KEYDOWN:
			if (ev.key.keysym.sym == SDLK_ESCAPE ||
			    ev.key.keysym.sym == SDLK_q)
				return 1;
			break;
		case SDL_CONTROLLERBUTTONDOWN:
			if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
			    ev.cbutton.button == SDL_CONTROLLER_BUTTON_START)
				return 1;
			break;
		case SDL_JOYBUTTONDOWN:
			if (ev.jbutton.button == 1 /* B */ ||
			    ev.jbutton.button == 10 /* Plus */)
				return 1;
			break;
		default:
			break;
		}
	}
	return 0;
}

static int player_open_video(struct player_state *ps)
{
	const AVCodec *dec;
	AVStream *st;

	ps->vstream = av_find_best_stream(ps->fmt, AVMEDIA_TYPE_VIDEO,
					  -1, -1, NULL, 0);
	if (ps->vstream < 0)
		return -1;

	st = ps->fmt->streams[ps->vstream];
	dec = avcodec_find_decoder(st->codecpar->codec_id);
	if (dec == NULL)
		return -1;

	ps->vctx = avcodec_alloc_context3(dec);
	if (ps->vctx == NULL)
		return -1;
	avcodec_parameters_to_context(ps->vctx, st->codecpar);
	ps->vctx->thread_count = 4;

	if (avcodec_open2(ps->vctx, dec, NULL) < 0)
		return -1;

	return 0;
}

static int player_open_audio(struct player_state *ps)
{
	const AVCodec *dec;
	AVStream *st;
	SDL_AudioSpec want, have;

	ps->astream = av_find_best_stream(ps->fmt, AVMEDIA_TYPE_AUDIO,
					  -1, -1, NULL, 0);
	if (ps->astream < 0)
		return -1;

	st = ps->fmt->streams[ps->astream];
	dec = avcodec_find_decoder(st->codecpar->codec_id);
	if (dec == NULL)
		return -1;

	ps->actx = avcodec_alloc_context3(dec);
	if (ps->actx == NULL)
		return -1;
	avcodec_parameters_to_context(ps->actx, st->codecpar);

	if (avcodec_open2(ps->actx, dec, NULL) < 0)
		return -1;

	/* the framebuffer surface only inits SDL video; bring up the audio
	 * subsystem on demand (idempotent, refcounted) or SDL_OpenAudioDevice
	 * fails with "Audio subsystem is not initialized" */
	if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
			fprintf(stderr, "player: audio init failed: %s\n",
				SDL_GetError());
			return -1;
		}
	}

	SDL_memset(&want, 0, sizeof(want));
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = AUDIO_CHANNELS;
	want.samples = 2048;

	ps->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (ps->adev == 0) {
		fprintf(stderr, "player: audio open failed: %s\n",
			SDL_GetError());
		return -1;
	}
	SDL_PauseAudioDevice(ps->adev, 0);

	return 0;
}

static void player_queue_audio(struct player_state *ps, AVFrame *frame)
{
	uint8_t *out = NULL;
	int out_samples;
	int out_size;

	if (ps->swr == NULL) {
		AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;

		if (swr_alloc_set_opts2(&ps->swr,
				&out_layout, AV_SAMPLE_FMT_S16, AUDIO_RATE,
				&frame->ch_layout, frame->format,
				frame->sample_rate, 0, NULL) < 0)
			return;
		if (swr_init(ps->swr) < 0)
			return;
	}

	out_samples = av_rescale_rnd(
		swr_get_delay(ps->swr, frame->sample_rate) + frame->nb_samples,
		AUDIO_RATE, frame->sample_rate, AV_ROUND_UP);

	if (av_samples_alloc(&out, NULL, AUDIO_CHANNELS, out_samples,
			     AV_SAMPLE_FMT_S16, 0) < 0)
		return;

	out_samples = swr_convert(ps->swr, &out, out_samples,
				  (const uint8_t **)frame->extended_data,
				  frame->nb_samples);
	if (out_samples > 0) {
		out_size = out_samples * AUDIO_CHANNELS * 2;

		/* keep the queue bounded */
		while (SDL_GetQueuedAudioSize(ps->adev) > AUDIO_QUEUE_MAX) {
			if (player_wants_exit()) {
				av_freep(&out);
				return;
			}
			SDL_Delay(10);
		}

		SDL_QueueAudio(ps->adev, out, out_size);
		ps->audio_bytes_queued += out_size;
	}

	av_freep(&out);
}

static void player_show_video(struct player_state *ps, AVFrame *frame)
{
	AVFrame *use = frame;
	static AVFrame *conv;
	SDL_Rect dst;
	int rw, rh;

	if (ps->tex == NULL || ps->tex_w != frame->width ||
	    ps->tex_h != frame->height) {
		if (ps->tex != NULL)
			SDL_DestroyTexture(ps->tex);
		ps->tex = SDL_CreateTexture(ps->renderer,
					    SDL_PIXELFORMAT_IYUV,
					    SDL_TEXTUREACCESS_STREAMING,
					    frame->width, frame->height);
		ps->tex_w = frame->width;
		ps->tex_h = frame->height;
	}
	if (ps->tex == NULL)
		return;

	if (frame->format != AV_PIX_FMT_YUV420P) {
		if (conv == NULL) {
			conv = av_frame_alloc();
			conv->format = AV_PIX_FMT_YUV420P;
			conv->width = frame->width;
			conv->height = frame->height;
			av_frame_get_buffer(conv, 32);
		}
		ps->sws = sws_getCachedContext(ps->sws,
				frame->width, frame->height, frame->format,
				frame->width, frame->height, AV_PIX_FMT_YUV420P,
				SWS_BILINEAR, NULL, NULL, NULL);
		if (ps->sws == NULL)
			return;
		sws_scale(ps->sws, (const uint8_t * const *)frame->data,
			  frame->linesize, 0, frame->height,
			  conv->data, conv->linesize);
		use = conv;
	}

	SDL_UpdateYUVTexture(ps->tex, NULL,
			     use->data[0], use->linesize[0],
			     use->data[1], use->linesize[1],
			     use->data[2], use->linesize[2]);

	/* letterbox into the render output */
	SDL_GetRendererOutputSize(ps->renderer, &rw, &rh);
	if (ps->tex_w * rh > ps->tex_h * rw) {
		dst.w = rw;
		dst.h = ps->tex_h * rw / ps->tex_w;
	} else {
		dst.h = rh;
		dst.w = ps->tex_w * rh / ps->tex_h;
	}
	dst.x = (rw - dst.w) / 2;
	dst.y = (rh - dst.h) / 2;

	SDL_SetRenderDrawColor(ps->renderer, 0, 0, 0, 255);
	SDL_RenderClear(ps->renderer);
	SDL_RenderCopy(ps->renderer, ps->tex, NULL, &dst);
	SDL_RenderPresent(ps->renderer);
}

int switch_player_play_url(const char *url)
{
	struct player_state ps;
	AVPacket *pkt;
	AVFrame *frame;
	int have_video;
	int have_audio;
	int ret = 0;

	memset(&ps, 0, sizeof(ps));
	ps.vstream = -1;
	ps.astream = -1;

	ps.renderer = nsfb_sdl2_get_renderer();
	if (ps.renderer == NULL) {
		fprintf(stderr, "player: no sdl2 renderer available\n");
		return -1;
	}

	avformat_network_init();

	fprintf(stderr, "player: opening %s\n", url);
	if (avformat_open_input(&ps.fmt, url, NULL, NULL) < 0) {
		fprintf(stderr, "player: open failed\n");
		return -1;
	}
	if (avformat_find_stream_info(ps.fmt, NULL) < 0) {
		fprintf(stderr, "player: no stream info\n");
		avformat_close_input(&ps.fmt);
		return -1;
	}

	have_video = (player_open_video(&ps) == 0);
	have_audio = (player_open_audio(&ps) == 0);
	if (!have_video && !have_audio) {
		fprintf(stderr, "player: no playable streams\n");
		ret = -1;
		goto out;
	}

	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	ps.start_wall = av_gettime_relative() / 1000000.0;

	while (av_read_frame(ps.fmt, pkt) >= 0) {
		if (player_wants_exit()) {
			av_packet_unref(pkt);
			break;
		}

		if (have_video && pkt->stream_index == ps.vstream) {
			if (avcodec_send_packet(ps.vctx, pkt) == 0) {
				while (avcodec_receive_frame(ps.vctx,
							     frame) == 0) {
					AVStream *st =
						ps.fmt->streams[ps.vstream];
					double pts = frame->best_effort_timestamp *
						av_q2d(st->time_base);

					/* wait for presentation time */
					while (pts > player_now(&ps) + 0.005) {
						if (player_wants_exit())
							break;
						SDL_Delay(2);
					}
					/* drop frames >150ms late */
					if (pts > player_now(&ps) - 0.15)
						player_show_video(&ps, frame);
				}
			}
		} else if (have_audio && pkt->stream_index == ps.astream) {
			if (avcodec_send_packet(ps.actx, pkt) == 0) {
				while (avcodec_receive_frame(ps.actx,
							     frame) == 0) {
					player_queue_audio(&ps, frame);
				}
			}
		}

		av_packet_unref(pkt);
	}

	/* let queued audio drain briefly */
	while (ps.adev != 0 && SDL_GetQueuedAudioSize(ps.adev) > 0) {
		if (player_wants_exit())
			break;
		SDL_Delay(20);
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);

out:
	if (ps.tex != NULL)
		SDL_DestroyTexture(ps.tex);
	if (ps.sws != NULL)
		sws_freeContext(ps.sws);
	if (ps.swr != NULL)
		swr_free(&ps.swr);
	if (ps.adev != 0)
		SDL_CloseAudioDevice(ps.adev);
	if (ps.vctx != NULL)
		avcodec_free_context(&ps.vctx);
	if (ps.actx != NULL)
		avcodec_free_context(&ps.actx);
	if (ps.fmt != NULL)
		avformat_close_input(&ps.fmt);

	fprintf(stderr, "player: done\n");
	return ret;
}

#else /* !__SWITCH__ */

int switch_player_play_url(const char *url)
{
	(void)url;
	return -1;
}

#endif
