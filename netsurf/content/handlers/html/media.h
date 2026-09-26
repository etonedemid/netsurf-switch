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
 * HTML media elements (<video> and <audio>).
 */

#ifndef NETSURF_HTML_MEDIA_H
#define NETSURF_HTML_MEDIA_H

#include <stdbool.h>

#include "netsurf/mouse.h"

struct html_content;
struct html_media;
struct dom_node;
struct box;
struct rect;
struct redraw_context;

/** script-visible state of a media element */
struct html_media_state {
	bool paused, ended, muted, loop, error;
	float volume;
	double current_time;
	double duration;  /**< NaN before metadata, Inf when unbounded */
	int video_width, video_height;
	int ready_state, network_state;
	const char *src;
};

/**
 * Find (or create) the media state of a <video>/<audio> node.
 */
struct html_media *html_media_for_node(struct html_content *c,
		struct dom_node *n, bool create);

/** Box construction for <video> and <audio>. */
bool html_media_box(struct html_content *c, struct dom_node *n,
		struct box *box, bool *convert_children);

/** Intrinsic size of a media box. */
void html_media_intrinsic(struct html_media *m, int *w, int *h);

/** Draw a media box's content area. */
bool html_media_redraw(struct box *box, int x, int y, int w, int h,
		const struct rect *clip, float scale,
		const struct redraw_context *ctx);

/**
 * Mouse action on a media box's built-in controls.
 *
 * \param bx, by  position relative to the content box
 * \return true if the controls handled it
 */
bool html_media_mouse(struct box *box, int bx, int by,
		browser_mouse_state mouse);

void html_media_play(struct html_media *m);
void html_media_pause(struct html_media *m);
void html_media_seek(struct html_media *m, double t);
void html_media_load(struct html_media *m);
void html_media_set_volume(struct html_media *m, float volume, bool muted);
void html_media_set_loop(struct html_media *m, bool loop);
void html_media_get_state(struct html_media *m, struct html_media_state *s);

/** Pause every media element of a document (it is being closed). */
void html_media_pause_all(struct html_content *c);

/** Release every media element of a document. */
void html_media_destroy_all(struct html_content *c);

#endif
