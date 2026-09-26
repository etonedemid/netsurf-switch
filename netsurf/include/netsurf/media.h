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
 * Interface to frontend media playback (HTML <video> and <audio>).
 *
 * The frontend decodes and plays a media resource; the core owns the
 * element state, polls the player for progress and asks it to draw the
 * current video frame into the page.
 */

#ifndef NETSURF_MEDIA_H_
#define NETSURF_MEDIA_H_

#include <stdbool.h>
#include <stdint.h>

struct gui_media;
struct redraw_context;
struct rect;

enum gui_media_state {
	GUI_MEDIA_LOADING,  /**< opening / probing the resource */
	GUI_MEDIA_READY,    /**< metadata known, not playing */
	GUI_MEDIA_PLAYING,
	GUI_MEDIA_PAUSED,
	GUI_MEDIA_ENDED,
	GUI_MEDIA_ERROR,
};

struct gui_media_status {
	enum gui_media_state state;
	double position;       /**< seconds */
	double duration;       /**< seconds, 0 if unknown/live */
	int width, height;     /**< video size, 0 for audio only */
	uint32_t frame_seq;    /**< changes whenever a new frame is ready */
	bool buffering;        /**< playing but starved of data */
	bool has_audio;
};

struct gui_media_table {
	/**
	 * Open a media resource. Playback does not start until play().
	 *
	 * \param url      absolute URL
	 * \param referer  document URL (may be NULL)
	 */
	struct gui_media *(*create)(const char *url, const char *referer);

	/** Stop and release a player. */
	void (*destroy)(struct gui_media *m);

	void (*play)(struct gui_media *m);
	void (*pause)(struct gui_media *m);
	void (*seek)(struct gui_media *m, double seconds);
	void (*set_volume)(struct gui_media *m, float volume, bool muted);
	void (*set_loop)(struct gui_media *m, bool loop);

	/** Snapshot of the player's state (cheap, main thread). */
	void (*status)(struct gui_media *m, struct gui_media_status *st);

	/**
	 * Draw the current video frame scaled into dst.
	 *
	 * \return false if no frame is available yet
	 */
	bool (*redraw)(struct gui_media *m, const struct redraw_context *ctx,
			const struct rect *dst, const struct rect *clip);
};

#endif
