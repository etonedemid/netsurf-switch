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
 *
 * Element state lives with the DOM node (not the box) so playback
 * survives box tree rebuilds. The frontend's gui_media_table does the
 * decoding; this module polls it, fires the HTMLMediaElement events,
 * lays the element out as a replaced box, draws frames, the poster and
 * the built-in controls, and handles clicks on those controls.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dom/dom.h>

#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/utils.h"
#include "netsurf/content.h"
#include "netsurf/media.h"
#include "netsurf/misc.h"
#include "netsurf/plotters.h"
#include "netsurf/mouse.h"
#include "netsurf/layout.h"
#include "content/content_protected.h"
#include "content/hlcache.h"
#include "desktop/gui_internal.h"
#include "css/utils.h"

#include "html/html.h"
#include "html/private.h"
#include "html/box.h"
#include "html/box_inspect.h"
#include "html/box_construct.h"
#include "html/media.h"

#define CONTROLS_H 36
#define TIMEUPDATE_S 0.25

struct html_media {
	struct html_media *next;
	html_content *html;
	dom_node *node;
	bool video;

	struct gui_media *gm;
	char *src;                 /**< resolved source URL */
	hlcache_handle *poster;

	/* element state as seen by scripts */
	bool paused;
	bool ended;
	bool muted;
	bool loop;
	float volume;
	bool error;
	bool autoplay_done;

	struct gui_media_status st;  /**< last polled status */
	double last_timeupdate;
	uint32_t shown_seq;
	int intrinsic_w, intrinsic_h; /**< natural size (300x150 default) */
	int attr_w, attr_h;           /**< width/height attributes, or 0 */
	bool metadata_fired;
};

static void media_tick(void *p);

static struct gui_media_table *media_table(void)
{
	return guit->media;
}

static bool node_has_attr(dom_node *n, const char *name)
{
	dom_string *s = NULL;
	bool has = false;

	if (dom_string_create((const uint8_t *)name, strlen(name), &s) !=
			DOM_NO_ERR)
		return false;
	dom_element_has_attribute(n, s, &has);
	dom_string_unref(s);
	return has;
}

static char *node_attr(dom_node *n, const char *name)
{
	dom_string *s = NULL, *v = NULL;
	char *r = NULL;

	if (dom_string_create((const uint8_t *)name, strlen(name), &s) !=
			DOM_NO_ERR)
		return NULL;
	if (dom_element_get_attribute(n, s, &v) == DOM_NO_ERR && v != NULL) {
		r = strndup(dom_string_data(v), dom_string_byte_length(v));
		dom_string_unref(v);
	}
	dom_string_unref(s);
	return r;
}

static char *resolve(html_content *c, const char *href)
{
	nsurl *u = NULL;
	char *r = NULL;

	if (href == NULL || href[0] == '\0' || c->base_url == NULL)
		return NULL;
	if (nsurl_join(c->base_url, href, &u) == NSERROR_OK) {
		r = strdup(nsurl_access(u));
		nsurl_unref(u);
	}
	return r;
}

/** the element's source: src, or the first usable <source> child */
static char *media_source(html_content *c, dom_node *n)
{
	char *src = node_attr(n, "src"), *r;
	dom_node *child = NULL, *next;

	if (src != NULL && src[0] != '\0') {
		r = resolve(c, src);
		free(src);
		return r;
	}
	free(src);

	if (dom_node_get_first_child(n, &child) != DOM_NO_ERR)
		return NULL;
	while (child != NULL) {
		dom_html_element_type tag;
		dom_node_type type;
		if (dom_node_get_node_type(child, &type) == DOM_NO_ERR &&
		    type == DOM_ELEMENT_NODE &&
		    dom_html_element_get_tag_type(child, &tag) == DOM_NO_ERR &&
		    tag == DOM_HTML_ELEMENT_TYPE_SOURCE) {
			char *s = node_attr(child, "src");
			char *t = node_attr(child, "type");
			bool ok = s != NULL && s[0] != '\0';
			/* adaptive streaming playlists need MSE/HLS */
			if (t != NULL && (strcasestr(t, "mpegurl") != NULL ||
					strcasestr(t, "dash") != NULL))
				ok = false;
			free(t);
			if (ok) {
				r = resolve(c, s);
				free(s);
				dom_node_unref(child);
				return r;
			}
			free(s);
		}
		if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR)
			next = NULL;
		dom_node_unref(child);
		child = next;
	}
	return NULL;
}

static void fire(struct html_media *m, const char *type)
{
	dom_string *t = NULL;

	if (dom_string_create((const uint8_t *)type, strlen(type), &t) ==
			DOM_NO_ERR) {
		fire_generic_dom_event(t, m->node, false, false);
		dom_string_unref(t);
	}
}

static void schedule_tick(html_content *c, int ms)
{
	guit->misc->schedule(ms, media_tick, c);
}

static void redraw_media(struct html_media *m)
{
	struct box *box = box_for_node(m->node);
	int x, y;

	if (box == NULL || m->html->layout == NULL ||
	    m->html->base.status == CONTENT_STATUS_LOADING)
		return;
	box_coords(box, &x, &y);
	content__request_redraw(&m->html->base, x - box->border[LEFT].width,
			y - box->border[TOP].width,
			box->padding[LEFT] + box->width + box->padding[RIGHT] +
			box->border[LEFT].width + box->border[RIGHT].width,
			box->padding[TOP] + box->height + box->padding[BOTTOM] +
			box->border[TOP].width + box->border[BOTTOM].width);
}

/** create the frontend player for the current source */
static void media_open(struct html_media *m)
{
	struct gui_media_table *mt = media_table();

	if (m->gm != NULL || m->src == NULL || mt == NULL)
		return;
	if (strncmp(m->src, "blob:", 5) == 0 ||
	    strncmp(m->src, "data:", 5) == 0) {
		m->error = true;
		return;
	}
	m->gm = mt->create(m->src, m->html->base_url ?
			nsurl_access(m->html->base_url) : NULL);
	if (m->gm == NULL) {
		m->error = true;
		return;
	}
	mt->set_volume(m->gm, m->volume, m->muted);
	mt->set_loop(m->gm, m->loop);
	memset(&m->st, 0, sizeof(m->st));
	m->st.state = GUI_MEDIA_LOADING;
	fire(m, "loadstart");
	schedule_tick(m->html, 50);
}

static void media_close(struct html_media *m)
{
	if (m->gm != NULL) {
		media_table()->destroy(m->gm);
		m->gm = NULL;
	}
}

static void media_free(struct html_media *m)
{
	media_close(m);
	if (m->poster != NULL)
		hlcache_handle_release(m->poster);
	dom_node_unref(m->node);
	free(m->src);
	free(m);
}

static nserror poster_cb(hlcache_handle *h, const hlcache_event *ev, void *pw)
{
	struct html_media *m = pw;

	if (ev->type == CONTENT_MSG_DONE)
		redraw_media(m);
	return NSERROR_OK;
}

/* exported interface documented in html/media.h */
struct html_media *html_media_for_node(html_content *c, dom_node *n,
		bool create)
{
	struct html_media *m;
	dom_html_element_type tag;

	for (m = c->media_elements; m != NULL; m = m->next)
		if (m->node == n)
			return m;
	if (!create)
		return NULL;
	if (dom_html_element_get_tag_type(n, &tag) != DOM_NO_ERR ||
	    (tag != DOM_HTML_ELEMENT_TYPE_VIDEO &&
	     tag != DOM_HTML_ELEMENT_TYPE_AUDIO))
		return NULL;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return NULL;
	m->html = c;
	m->node = dom_node_ref(n);
	m->video = (tag == DOM_HTML_ELEMENT_TYPE_VIDEO);
	m->paused = true;
	m->volume = 1.0f;
	m->muted = node_has_attr(n, "muted");
	m->loop = node_has_attr(n, "loop");
	m->intrinsic_w = m->video ? 300 : 300;
	m->intrinsic_h = m->video ? 150 : CONTROLS_H;
	m->src = media_source(c, n);
	m->next = c->media_elements;
	c->media_elements = m;

	if (m->video) {
		char *poster = node_attr(n, "poster");
		char *url = resolve(c, poster);
		nsurl *nu;
		if (url != NULL && nsurl_create(url, &nu) == NSERROR_OK) {
			hlcache_handle_retrieve(nu, 0, c->base_url, NULL,
					poster_cb, m, NULL, CONTENT_IMAGE,
					&m->poster);
			nsurl_unref(nu);
		}
		free(poster);
		free(url);
	}
	return m;
}

/** check the element still has the source it was opened with */
static void media_sync_source(struct html_media *m)
{
	char *src = media_source(m->html, m->node);

	if ((src == NULL) != (m->src == NULL) ||
	    (src != NULL && strcmp(src, m->src) != 0)) {
		bool was_playing = !m->paused;
		media_close(m);
		free(m->src);
		m->src = src;
		m->ended = false;
		m->error = false;
		m->metadata_fired = false;
		m->paused = true;
		if (was_playing || node_has_attr(m->node, "autoplay")) {
			m->autoplay_done = false;
		}
		fire(m, "emptied");
	} else {
		free(src);
	}
}

/* exported interface documented in html/media.h */
bool html_media_box(html_content *c, dom_node *n, struct box *box,
		bool *convert_children)
{
	struct html_media *m = html_media_for_node(c, n, true);
	char *preload;
	bool controls;

	*convert_children = false;
	if (m == NULL)
		return true;

	media_sync_source(m);
	controls = node_has_attr(n, "controls");

	/* <audio> without controls has no rendering */
	if (!m->video && !controls) {
		box->type = BOX_NONE;
	}

	box->media = m;
	box->flags |= IS_REPLACED;

	if (box->style != NULL) {
		css_fixed value = 0;
		css_unit wunit = CSS_UNIT_PX, hunit = CSS_UNIT_PX;
		if (css_computed_width(box->style, &value, &wunit) ==
				CSS_WIDTH_SET && wunit != CSS_UNIT_PCT &&
		    css_computed_height(box->style, &value, &hunit) ==
				CSS_HEIGHT_SET && hunit != CSS_UNIT_PCT)
			box->flags |= REPLACE_DIM;
	}

	/* width/height attributes size the element */
	if (m->video) {
		char *w = node_attr(n, "width"), *h = node_attr(n, "height");
		m->attr_w = w != NULL ? atoi(w) : 0;
		m->attr_h = h != NULL ? atoi(h) : 0;
		free(w);
		free(h);
	}

	preload = node_attr(n, "preload");
	if (m->gm == NULL && !m->error &&
	    (node_has_attr(n, "autoplay") ||
	     (m->video && (preload == NULL || strcasecmp(preload, "none") != 0))))
		media_open(m);
	free(preload);

	if (node_has_attr(n, "autoplay") && !m->autoplay_done && m->gm) {
		m->autoplay_done = true;
		html_media_play(m);
	}
	return true;
}

/* exported interface documented in html/media.h */
void html_media_intrinsic(struct html_media *m, int *w, int *h)
{
	int nw = m->intrinsic_w, nh = m->intrinsic_h;

	if (m->attr_w > 0 && m->attr_h > 0) {
		*w = m->attr_w;
		*h = m->attr_h;
	} else if (m->attr_w > 0) {
		*w = m->attr_w;
		*h = nw > 0 ? m->attr_w * nh / nw : nh;
	} else if (m->attr_h > 0) {
		*h = m->attr_h;
		*w = nh > 0 ? m->attr_h * nw / nh : nw;
	} else {
		*w = nw;
		*h = nh;
	}
}

/* ------------------------------------------------------------------ */
/* Playback control                                                   */
/* ------------------------------------------------------------------ */

/* exported interface documented in html/media.h */
void html_media_play(struct html_media *m)
{
	if (m->gm == NULL)
		media_open(m);
	if (m->gm == NULL)
		return;
	if (m->ended) {
		m->ended = false;
		media_table()->seek(m->gm, 0);
	}
	media_table()->play(m->gm);
	if (m->paused) {
		m->paused = false;
		fire(m, "play");
	}
	schedule_tick(m->html, 20);
	redraw_media(m);
}

/* exported interface documented in html/media.h */
void html_media_pause(struct html_media *m)
{
	if (m->gm != NULL)
		media_table()->pause(m->gm);
	if (!m->paused) {
		m->paused = true;
		fire(m, "pause");
	}
	redraw_media(m);
}

/* exported interface documented in html/media.h */
void html_media_seek(struct html_media *m, double t)
{
	if (m->gm == NULL)
		media_open(m);
	if (m->gm == NULL)
		return;
	fire(m, "seeking");
	media_table()->seek(m->gm, t);
	m->st.position = t;
	m->ended = false;
	fire(m, "timeupdate");
	fire(m, "seeked");
	schedule_tick(m->html, 20);
}

/* exported interface documented in html/media.h */
void html_media_set_volume(struct html_media *m, float volume, bool muted)
{
	if (volume != m->volume || muted != m->muted) {
		m->volume = volume;
		m->muted = muted;
		if (m->gm != NULL)
			media_table()->set_volume(m->gm, volume, muted);
		fire(m, "volumechange");
	}
}

/* exported interface documented in html/media.h */
void html_media_set_loop(struct html_media *m, bool loop)
{
	m->loop = loop;
	if (m->gm != NULL)
		media_table()->set_loop(m->gm, loop);
}

/* exported interface documented in html/media.h */
void html_media_load(struct html_media *m)
{
	media_close(m);
	free(m->src);
	m->src = media_source(m->html, m->node);
	m->ended = false;
	m->error = false;
	m->metadata_fired = false;
	if (!m->paused) {
		m->paused = true;
		fire(m, "pause");
	}
	fire(m, "emptied");
	media_open(m);
}

/* exported interface documented in html/media.h */
void html_media_get_state(struct html_media *m, struct html_media_state *s)
{
	s->paused = m->paused;
	s->ended = m->ended;
	s->muted = m->muted;
	s->loop = m->loop;
	s->volume = m->volume;
	s->current_time = m->st.position;
	s->duration = m->metadata_fired ?
			(m->st.duration > 0 ? m->st.duration : INFINITY) : NAN;
	s->video_width = m->st.width;
	s->video_height = m->st.height;
	s->error = m->error;
	s->src = m->src;
	if (m->error)
		s->ready_state = 0;
	else if (!m->metadata_fired)
		s->ready_state = 0;
	else
		s->ready_state = 4;
	s->network_state = m->src == NULL ? 3 : (m->gm ? 1 : 1);
}

/* ------------------------------------------------------------------ */
/* Polling                                                            */
/* ------------------------------------------------------------------ */

static void media_tick(void *p)
{
	html_content *c = p;
	struct html_media *m;
	struct gui_media_table *mt = media_table();
	int next = -1;

	if (mt == NULL)
		return;

	for (m = c->media_elements; m != NULL; m = m->next) {
		struct gui_media_status st;
		enum gui_media_state prev;

		if (m->gm == NULL)
			continue;
		prev = m->st.state;
		mt->status(m->gm, &st);
		m->st = st;

		if (st.state == GUI_MEDIA_ERROR && prev != GUI_MEDIA_ERROR) {
			m->error = true;
			m->paused = true;
			fire(m, "error");
			redraw_media(m);
			continue;
		}

		if (!m->metadata_fired && st.state != GUI_MEDIA_LOADING &&
				st.state != GUI_MEDIA_ERROR) {
			m->metadata_fired = true;
			if (m->video && st.width > 0 && st.height > 0 &&
			    (st.width != m->intrinsic_w ||
			     st.height != m->intrinsic_h) &&
			    !(m->attr_w > 0 && m->attr_h > 0)) {
				m->intrinsic_w = st.width;
				m->intrinsic_h = st.height;
				/* lay out again with the real size */
				if (c->layout != NULL)
					content__reformat(&c->base, false,
						c->base.available_width,
						c->base.available_height);
			}
			fire(m, "durationchange");
			fire(m, "loadedmetadata");
			fire(m, "loadeddata");
			fire(m, "canplay");
			fire(m, "canplaythrough");
			redraw_media(m);
		}

		if (st.state == GUI_MEDIA_PLAYING && prev != GUI_MEDIA_PLAYING)
			fire(m, "playing");

		if (st.state == GUI_MEDIA_ENDED && prev != GUI_MEDIA_ENDED &&
				!m->ended) {
			m->ended = true;
			m->paused = true;
			fire(m, "timeupdate");
			fire(m, "pause");
			fire(m, "ended");
			redraw_media(m);
		}

		if (st.state == GUI_MEDIA_PLAYING) {
			if (fabs(st.position - m->last_timeupdate) >=
					TIMEUPDATE_S) {
				m->last_timeupdate = st.position;
				fire(m, "timeupdate");
				if (node_has_attr(m->node, "controls"))
					redraw_media(m);
			}
			next = (next < 0 || next > 200) ? 200 : next;
			if (m->video && st.width > 0)
				next = 15;
		}
		if (st.frame_seq != m->shown_seq) {
			m->shown_seq = st.frame_seq;
			redraw_media(m);
		}
		if (st.state == GUI_MEDIA_LOADING || st.frame_seq == 0 ||
				m->shown_seq == 0)
			next = (next < 0 || next > 60) ? 60 : next;
		if (st.state == GUI_MEDIA_LOADING &&
		    m->metadata_fired == false && next < 0)
			next = 60;
	}

	if (next >= 0)
		schedule_tick(c, next);
}

/* exported interface documented in html/media.h */
void html_media_pause_all(html_content *c)
{
	struct html_media *m;

	guit->misc->schedule(-1, media_tick, c);
	for (m = c->media_elements; m != NULL; m = m->next) {
		/* release the decoder; play() reopens it */
		media_close(m);
		m->paused = true;
		m->st.frame_seq = 0;
		m->shown_seq = 0;
	}
}

/* exported interface documented in html/media.h */
void html_media_destroy_all(html_content *c)
{
	guit->misc->schedule(-1, media_tick, c);
	while (c->media_elements != NULL) {
		struct html_media *m = c->media_elements;
		c->media_elements = m->next;
		media_free(m);
	}
}

/* ------------------------------------------------------------------ */
/* Rendering and controls                                             */
/* ------------------------------------------------------------------ */

static void fmt_time(char *buf, size_t n, double t)
{
	int s;

	if (!(t >= 0) || isinf(t))
		t = 0;
	s = (int)t;
	if (s >= 3600)
		snprintf(buf, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60,
				s % 60);
	else
		snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

static void fill(const struct redraw_context *ctx, int x0, int y0, int x1,
		int y1, colour c)
{
	struct rect r = { x0, y0, x1, y1 };
	plot_style_t ps = {
		.fill_type = PLOT_OP_TYPE_SOLID,
		.fill_colour = c,
	};

	if (ctx->plot->rounded_fill != NULL) {
		struct plot_radii zero;
		memset(&zero, 0, sizeof(zero));
		ctx->plot->rounded_fill(ctx, &r, &zero, NULL, NULL, c);
	} else {
		ctx->plot->rectangle(ctx, &ps, &r);
	}
}

static void polygon(const struct redraw_context *ctx, const float *pts,
		int n, colour c)
{
	float path[64];
	float ident[6] = { 1, 0, 0, 1, 0, 0 };
	plot_style_t ps;
	int i, k = 0;

	if (n > 10)
		return;
	memset(&ps, 0, sizeof(ps));
	ps.fill_type = PLOT_OP_TYPE_SOLID;
	ps.fill_colour = c;
	ps.stroke_type = PLOT_OP_TYPE_NONE;
	for (i = 0; i < n; i++) {
		path[k++] = i == 0 ? PLOTTER_PATH_MOVE : PLOTTER_PATH_LINE;
		path[k++] = pts[i * 2];
		path[k++] = pts[i * 2 + 1];
	}
	path[k++] = PLOTTER_PATH_CLOSE;
	ctx->plot->path(ctx, &ps, path, k, ident);
}

/* play triangle or pause bars centred on (cx, cy) with size s */
static void play_glyph(const struct redraw_context *ctx, float cx, float cy,
		float s, bool pause, colour c)
{
	if (pause) {
		fill(ctx, cx - s * 0.45f, cy - s * 0.5f, cx - s * 0.12f,
				cy + s * 0.5f, c);
		fill(ctx, cx + s * 0.12f, cy - s * 0.5f, cx + s * 0.45f,
				cy + s * 0.5f, c);
	} else {
		float t[6] = { cx - s * 0.4f, cy - s * 0.5f,
			       cx + s * 0.5f, cy,
			       cx - s * 0.4f, cy + s * 0.5f };
		polygon(ctx, t, 3, c);
	}
}

static void draw_controls(struct html_media *m,
		const struct redraw_context *ctx, int x, int y, int w, int h,
		float scale)
{
	int bh = CONTROLS_H * scale;
	int by = y + h - bh;
	int pad = 8 * scale;
	int bx0 = x + bh + pad, bx1;
	double pos = m->st.position, dur = m->st.duration;
	char tbuf[48], a[16], b[16];
	int tw = 0;
	plot_font_style_t fs = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 11 * PLOT_STYLE_SCALE * scale,
		.weight = 400,
		.flags = FONTF_NONE,
		.background = 0x000000,
		.foreground = 0xffffff,
	};

	if (bh > h)
		bh = h, by = y;
	fill(ctx, x, by, x + w, y + h, m->video ? 0x60000000 : 0x00303030);
	play_glyph(ctx, x + bh / 2.0f, by + bh / 2.0f, bh * 0.42f,
			!m->paused, 0xffffff);

	fmt_time(a, sizeof(a), pos);
	fmt_time(b, sizeof(b), dur);
	if (dur > 0)
		snprintf(tbuf, sizeof(tbuf), "%s / %s", a, b);
	else
		snprintf(tbuf, sizeof(tbuf), "%s", a);
	guit->layout->width(&fs, tbuf, strlen(tbuf), &tw);
	ctx->plot->text(ctx, &fs, x + w - pad - tw, by + bh / 2 + 4 * scale,
			tbuf, strlen(tbuf));

	/* progress track */
	bx1 = x + w - pad * 2 - tw;
	if (bx1 > bx0 + 10) {
		int ty = by + bh / 2;
		int t0 = ty - 2 * scale, t1 = ty + 2 * scale;
		int filled = bx0;
		if (dur > 0)
			filled = bx0 + (int)((bx1 - bx0) * fmin(pos / dur, 1));
		fill(ctx, bx0, t0, bx1, t1, 0x00808080);
		fill(ctx, bx0, t0, filled, t1, 0x00ffffff);
		fill(ctx, filled - 4 * scale, ty - 6 * scale,
				filled + 4 * scale, ty + 6 * scale, 0x00ffffff);
	}
}

/* exported interface documented in html/media.h */
bool html_media_redraw(struct box *box, int x, int y, int w, int h,
		const struct rect *clip, float scale,
		const struct redraw_context *ctx)
{
	struct html_media *m = box->media;
	struct gui_media_table *mt = media_table();
	bool controls, drawn = false;
	struct rect area = { x, y, x + w, y + h };
	struct rect r;

	if (m == NULL || w <= 0 || h <= 0)
		return true;
	r.x0 = clip->x0 > area.x0 ? clip->x0 : area.x0;
	r.y0 = clip->y0 > area.y0 ? clip->y0 : area.y0;
	r.x1 = clip->x1 < area.x1 ? clip->x1 : area.x1;
	r.y1 = clip->y1 < area.y1 ? clip->y1 : area.y1;
	if (r.x0 >= r.x1 || r.y0 >= r.y1)
		return true;
	ctx->plot->clip(ctx, &r);

	controls = node_has_attr(m->node, "controls");

	if (m->video) {
		/* letterboxed frame (object-fit: contain) */
		int vw = m->st.width > 0 ? m->st.width : m->intrinsic_w;
		int vh = m->st.height > 0 ? m->st.height : m->intrinsic_h;
		struct rect dst = area;
		bool playing_started = m->st.frame_seq > 0 &&
			(!m->paused || m->st.position > 0.05 || !m->poster);

		if (vw > 0 && vh > 0) {
			if ((double)w / h > (double)vw / vh) {
				int dw = (int)((double)h * vw / vh);
				dst.x0 = x + (w - dw) / 2;
				dst.x1 = dst.x0 + dw;
			} else {
				int dh = (int)((double)w * vh / vw);
				dst.y0 = y + (h - dh) / 2;
				dst.y1 = dst.y0 + dh;
			}
		}
		if (m->gm != NULL && mt != NULL && playing_started) {
			fill(ctx, x, y, x + w, y + h, 0x00000000);
			drawn = mt->redraw(m->gm, ctx, &dst, &r);
		}
		if (!drawn && m->poster != NULL &&
		    content_get_status(m->poster) == CONTENT_STATUS_DONE) {
			struct content_redraw_data d;
			int pw = content_get_width(m->poster);
			int ph = content_get_height(m->poster);
			struct rect pd = area;
			if (pw > 0 && ph > 0) {
				if ((double)w / h > (double)pw / ph) {
					int dw = (int)((double)h * pw / ph);
					pd.x0 = x + (w - dw) / 2;
					pd.x1 = pd.x0 + dw;
				} else {
					int dh = (int)((double)w * ph / pw);
					pd.y0 = y + (h - dh) / 2;
					pd.y1 = pd.y0 + dh;
				}
			}
			fill(ctx, x, y, x + w, y + h, 0x00000000);
			d.x = pd.x0;
			d.y = pd.y0;
			d.width = pd.x1 - pd.x0;
			d.height = pd.y1 - pd.y0;
			d.background_colour = 0;
			d.scale = scale;
			d.repeat_x = d.repeat_y = false;
			drawn = content_redraw(m->poster, &d, &r, ctx);
		}
		if (!drawn && m->gm != NULL && mt != NULL) {
			fill(ctx, x, y, x + w, y + h, 0x00000000);
			drawn = mt->redraw(m->gm, ctx, &dst, &r);
		}
		if (!drawn)
			fill(ctx, x, y, x + w, y + h, 0x00000000);

		/* big play button while paused */
		if (m->paused && (controls || !drawn) && w > 60 && h > 60) {
			float cx = x + w / 2.0f, cy = y + h / 2.0f;
			float s = fminf(w, h) * 0.18f;
			if (s > 40 * scale)
				s = 40 * scale;
			fill(ctx, cx - s, cy - s * 0.7f, cx + s, cy + s * 0.7f,
					0x40000000);
			play_glyph(ctx, cx + s * 0.08f, cy, s * 0.8f, false,
					0xffffff);
		}
		if (m->error && w > 60) {
			plot_font_style_t fs = {
				.family = PLOT_FONT_FAMILY_SANS_SERIF,
				.size = 11 * PLOT_STYLE_SCALE * scale,
				.weight = 400,
				.background = 0,
				.foreground = 0xc0c0c0,
			};
			ctx->plot->text(ctx, &fs, x + 8 * scale,
					y + 18 * scale, "Media unavailable", 17);
		}
	}

	if (controls)
		draw_controls(m, ctx, x, y, w, h, scale);

	ctx->plot->clip(ctx, clip);
	return true;
}

/* exported interface documented in html/media.h */
bool html_media_mouse(struct box *box, int bx, int by,
		browser_mouse_state mouse)
{
	struct html_media *m = box->media;
	int w = box->width, h = box->height;
	int bh = CONTROLS_H;

	if (m == NULL || !(mouse & BROWSER_MOUSE_CLICK_1))
		return false;
	if (!node_has_attr(m->node, "controls"))
		return false;
	if (bx < 0 || by < 0 || bx >= w || by >= h)
		return false;

	if (by >= h - bh) {
		/* control bar */
		int bx0 = bh + 8, bx1 = w - 8 * 2 - 70;
		if (bx < bh) {
			if (m->paused)
				html_media_play(m);
			else
				html_media_pause(m);
		} else if (bx >= bx0 && bx <= bx1 && m->st.duration > 0) {
			html_media_seek(m, m->st.duration *
					(bx - bx0) / (double)(bx1 - bx0));
			redraw_media(m);
		}
		return true;
	}
	if (m->paused)
		html_media_play(m);
	else
		html_media_pause(m);
	return true;
}
