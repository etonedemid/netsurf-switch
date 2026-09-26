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
 * HTML <canvas> 2D drawing: a software implementation of the
 * CanvasRenderingContext2D drawing model.
 *
 * Paths are flattened to polylines in device space as they are built
 * (so any affine transform works) and filled with an anti-aliased
 * signed-area coverage rasteriser; strokes are built from per-segment
 * quads with round joins/caps. Paints are colours or linear/radial
 * gradients, composited with the common composite operations.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <dom/dom.h>
#include <libcss/libcss.h>

#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/utils.h"
#include "netsurf/bitmap.h"
#include "netsurf/canvas.h"
#include "netsurf/content.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "netsurf/plotters.h"
#include "content/content_protected.h"
#include "content/hlcache.h"
#include "desktop/bitmap.h"
#include "desktop/gui_internal.h"

#include "html/html.h"
#include "html/private.h"
#include "html/box.h"
#include "html/box_inspect.h"
#include "html/box_construct.h"
#include "html/canvas.h"

#define CANVAS_MAX_DIM 4096

struct cv_path {
	float *pt;          /* device space x,y pairs */
	int npt, cap;
	int *start;         /* subpath start point indices */
	bool *closed;
	int nsub, subcap;
	bool have_current;
	float cx, cy;       /* current point (device) */
	float sx, sy;       /* subpath start (device) */
};

struct html_canvas {
	struct html_canvas *next;
	html_content *html;
	dom_node *node;

	int w, h;
	uint8_t *px;        /* RGBA, straight alpha, stride w*4 */

	struct bitmap *bmp; /* page copy */
	bool dirty;
	bool redraw_pending;

	struct canvas_state st;
	struct canvas_state *stack;
	int nstack, stackcap;

	struct cv_path path;
};

static void cv_redraw_cb(void *p);

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static void cv_state_init(struct html_canvas *cv)
{
	struct canvas_state *s = &cv->st;

	memset(s, 0, sizeof(*s));
	s->m[0] = s->m[3] = 1;
	s->fill.type = CANVAS_PAINT_COLOUR;
	s->fill.colour = 0xff000000;
	s->stroke = s->fill;
	s->line_width = 1;
	s->global_alpha = 1;
	s->op = CANVAS_OP_SOURCE_OVER;
	s->clip[0] = 0;
	s->clip[1] = 0;
	s->clip[2] = cv->w;
	s->clip[3] = cv->h;
	strcpy(s->font_family, "sans-serif");
	s->font_px = 10;
	s->font_weight = 400;
	s->smoothing = true;
}

static void cv_path_reset(struct cv_path *p)
{
	p->npt = 0;
	p->nsub = 0;
	p->have_current = false;
}

static void cv_path_free(struct cv_path *p)
{
	free(p->pt);
	free(p->start);
	free(p->closed);
	memset(p, 0, sizeof(*p));
}

static void cv_mark_dirty(struct html_canvas *cv)
{
	cv->dirty = true;
	if (!cv->redraw_pending) {
		cv->redraw_pending = true;
		guit->misc->schedule(10, cv_redraw_cb, cv);
	}
}

/* exported interface documented in html/canvas.h */
struct canvas_state *html_canvas_state(struct html_canvas *cv)
{
	return &cv->st;
}

/* exported interface documented in html/canvas.h */
void html_canvas_save(struct html_canvas *cv)
{
	if (cv->nstack == cv->stackcap) {
		int cap = cv->stackcap ? cv->stackcap * 2 : 8;
		struct canvas_state *n;
		if (cap > 512)
			return;
		n = realloc(cv->stack, cap * sizeof(*n));
		if (n == NULL)
			return;
		cv->stack = n;
		cv->stackcap = cap;
	}
	cv->stack[cv->nstack++] = cv->st;
}

/* exported interface documented in html/canvas.h */
void html_canvas_restore(struct html_canvas *cv)
{
	if (cv->nstack > 0)
		cv->st = cv->stack[--cv->nstack];
}

/* exported interface documented in html/canvas.h */
void html_canvas_reset(struct html_canvas *cv)
{
	cv->nstack = 0;
	cv_state_init(cv);
	cv_path_reset(&cv->path);
}

/* exported interface documented in html/canvas.h */
void html_canvas_set_size(struct html_canvas *cv, int w, int h)
{
	uint8_t *px;

	if (w < 0)
		w = 0;
	if (h < 0)
		h = 0;
	if (w > CANVAS_MAX_DIM)
		w = CANVAS_MAX_DIM;
	if (h > CANVAS_MAX_DIM)
		h = CANVAS_MAX_DIM;

	px = calloc((size_t)(w ? w : 1) * (h ? h : 1), 4);
	if (px == NULL)
		return;
	free(cv->px);
	cv->px = px;
	cv->w = w;
	cv->h = h;
	if (cv->bmp != NULL) {
		guit->bitmap->destroy(cv->bmp);
		cv->bmp = NULL;
	}
	html_canvas_reset(cv);
	cv_mark_dirty(cv);
}

/* exported interface documented in html/canvas.h */
void html_canvas_get_size(struct html_canvas *cv, int *w, int *h)
{
	*w = cv->w;
	*h = cv->h;
}

/* ------------------------------------------------------------------ */
/* Element lifecycle                                                  */
/* ------------------------------------------------------------------ */

static int attr_int(dom_node *n, dom_string *name, int dflt)
{
	dom_string *v = NULL;
	int r = dflt;

	if (dom_element_get_attribute(n, name, &v) == DOM_NO_ERR && v != NULL) {
		const char *s = dom_string_data(v);
		char *end;
		long l = strtol(s, &end, 10);
		if (end != s && l >= 0)
			r = (int)l;
		dom_string_unref(v);
	}
	return r;
}

/* exported interface documented in html/canvas.h */
struct html_canvas *html_canvas_for_node(html_content *c, dom_node *n,
		bool create)
{
	struct html_canvas *cv;
	dom_html_element_type tag;

	for (cv = c->canvases; cv != NULL; cv = cv->next)
		if (cv->node == n)
			return cv;
	if (!create)
		return NULL;
	if (dom_html_element_get_tag_type(n, &tag) != DOM_NO_ERR ||
	    tag != DOM_HTML_ELEMENT_TYPE_CANVAS)
		return NULL;

	cv = calloc(1, sizeof(*cv));
	if (cv == NULL)
		return NULL;
	cv->html = c;
	cv->node = dom_node_ref(n);
	cv->next = c->canvases;
	c->canvases = cv;
	html_canvas_set_size(cv, attr_int(n, corestring_dom_width, 300),
			attr_int(n, corestring_dom_height, 150));
	return cv;
}

/* exported interface documented in html/canvas.h */
bool html_canvas_box(html_content *c, dom_node *n, struct box *box,
		bool *convert_children)
{
	struct html_canvas *cv;
	int w, h;

	/* without scripting the fallback content is shown */
	if (!c->enable_scripting)
		return true;
	*convert_children = false;

	cv = html_canvas_for_node(c, n, true);
	if (cv == NULL)
		return true;

	/* the width/height attributes size the bitmap */
	w = attr_int(n, corestring_dom_width, 300);
	h = attr_int(n, corestring_dom_height, 150);
	if (w != cv->w || h != cv->h)
		html_canvas_set_size(cv, w, h);

	box->canvas = cv;
	box->flags |= IS_REPLACED;
	return true;
}

/* exported interface documented in html/canvas.h */
void html_canvas_intrinsic(struct html_canvas *cv, int *w, int *h)
{
	*w = cv->w;
	*h = cv->h;
}

/* exported interface documented in html/canvas.h */
void html_canvas_destroy_all(html_content *c)
{
	while (c->canvases != NULL) {
		struct html_canvas *cv = c->canvases;
		c->canvases = cv->next;
		guit->misc->schedule(-1, cv_redraw_cb, cv);
		if (cv->bmp != NULL)
			guit->bitmap->destroy(cv->bmp);
		cv_path_free(&cv->path);
		free(cv->stack);
		free(cv->px);
		dom_node_unref(cv->node);
		free(cv);
	}
}

static void cv_redraw_cb(void *p)
{
	struct html_canvas *cv = p;
	struct box *box;
	int x, y;

	cv->redraw_pending = false;
	box = box_for_node(cv->node);
	if (box == NULL || cv->html->layout == NULL ||
	    cv->html->base.status == CONTENT_STATUS_LOADING)
		return;
	box_coords(box, &x, &y);
	content__request_redraw(&cv->html->base, x + box->padding[LEFT],
			y + box->padding[TOP], box->width, box->height);
}

/* exported interface documented in html/canvas.h */
bool html_canvas_redraw(struct box *box, int x, int y, int w, int h,
		const struct rect *clip, const struct redraw_context *ctx)
{
	struct html_canvas *cv = box->canvas;
	uint8_t *dst;
	size_t stride;
	int row;

	if (cv == NULL || cv->w == 0 || cv->h == 0 || w <= 0 || h <= 0)
		return true;

	if (cv->bmp == NULL) {
		cv->bmp = guit->bitmap->create(cv->w, cv->h, BITMAP_NONE);
		if (cv->bmp == NULL)
			return true;
		cv->dirty = true;
	}
	if (cv->dirty) {
		bitmap_fmt_t rgba = {
			.layout = BITMAP_LAYOUT_R8G8B8A8,
			.pma = false,
		};
		dst = guit->bitmap->get_buffer(cv->bmp);
		stride = guit->bitmap->get_rowstride(cv->bmp);
		if (dst == NULL)
			return true;
		for (row = 0; row < cv->h; row++)
			memcpy(dst + row * stride, cv->px + (size_t)row * cv->w * 4,
					(size_t)cv->w * 4);
		bitmap_format_to_client(cv->bmp, &rgba);
		guit->bitmap->modified(cv->bmp);
		cv->dirty = false;
	}

	ctx->plot->clip(ctx, clip);
	ctx->plot->bitmap(ctx, cv->bmp, x, y, w, h, 0xffffff, BITMAPF_NONE);
	return true;
}

/* ------------------------------------------------------------------ */
/* Transforms and paths                                               */
/* ------------------------------------------------------------------ */

static inline void cv_tx(const double m[6], double x, double y,
		float *ox, float *oy)
{
	*ox = (float)(m[0] * x + m[2] * y + m[4]);
	*oy = (float)(m[1] * x + m[3] * y + m[5]);
}

static bool cv_inverse(const double m[6], double inv[6])
{
	double det = m[0] * m[3] - m[1] * m[2];

	if (fabs(det) < 1e-12)
		return false;
	inv[0] = m[3] / det;
	inv[1] = -m[1] / det;
	inv[2] = -m[2] / det;
	inv[3] = m[0] / det;
	inv[4] = (m[2] * m[5] - m[3] * m[4]) / det;
	inv[5] = (m[1] * m[4] - m[0] * m[5]) / det;
	return true;
}

static double cv_scale(const double m[6])
{
	return sqrt(fabs(m[0] * m[3] - m[1] * m[2]));
}

static bool cv_point(struct cv_path *p, float x, float y)
{
	if (p->npt == p->cap) {
		int cap = p->cap ? p->cap * 2 : 256;
		float *n;
		if (cap > 4 * 1024 * 1024)
			return false;
		n = realloc(p->pt, (size_t)cap * 2 * sizeof(float));
		if (n == NULL)
			return false;
		p->pt = n;
		p->cap = cap;
	}
	p->pt[p->npt * 2] = x;
	p->pt[p->npt * 2 + 1] = y;
	p->npt++;
	p->cx = x;
	p->cy = y;
	return true;
}

static bool cv_subpath(struct cv_path *p, float x, float y)
{
	if (p->nsub == p->subcap) {
		int cap = p->subcap ? p->subcap * 2 : 16;
		int *ns;
		bool *nc;
		ns = realloc(p->start, cap * sizeof(int));
		if (ns == NULL)
			return false;
		p->start = ns;
		nc = realloc(p->closed, cap * sizeof(bool));
		if (nc == NULL)
			return false;
		p->closed = nc;
		p->subcap = cap;
	}
	p->start[p->nsub] = p->npt;
	p->closed[p->nsub] = false;
	p->nsub++;
	p->sx = x;
	p->sy = y;
	p->have_current = true;
	return cv_point(p, x, y);
}

/* exported interface documented in html/canvas.h */
void html_canvas_begin_path(struct html_canvas *cv)
{
	cv_path_reset(&cv->path);
}

/* exported interface documented in html/canvas.h */
void html_canvas_close_path(struct html_canvas *cv)
{
	struct cv_path *p = &cv->path;

	if (p->nsub > 0 && p->have_current) {
		p->closed[p->nsub - 1] = true;
		/* a new subpath starts at the same point */
		cv_subpath(p, p->sx, p->sy);
	}
}

static void cv_move_dev(struct cv_path *p, float x, float y)
{
	/* drop an empty trailing subpath */
	if (p->nsub > 0 && p->start[p->nsub - 1] == p->npt - 1 &&
			!p->closed[p->nsub - 1]) {
		p->npt--;
		p->nsub--;
	}
	cv_subpath(p, x, y);
}

/* exported interface documented in html/canvas.h */
void html_canvas_move_to(struct html_canvas *cv, double x, double y)
{
	float dx, dy;

	if (!isfinite(x) || !isfinite(y))
		return;
	cv_tx(cv->st.m, x, y, &dx, &dy);
	cv_move_dev(&cv->path, dx, dy);
}

static void cv_line_dev(struct cv_path *p, float x, float y)
{
	if (!p->have_current)
		cv_subpath(p, x, y);
	else
		cv_point(p, x, y);
}

/* exported interface documented in html/canvas.h */
void html_canvas_line_to(struct html_canvas *cv, double x, double y)
{
	float dx, dy;

	if (!isfinite(x) || !isfinite(y))
		return;
	cv_tx(cv->st.m, x, y, &dx, &dy);
	cv_line_dev(&cv->path, dx, dy);
}

static void cv_bezier_dev(struct cv_path *p, float x1, float y1, float x2,
		float y2, float x3, float y3)
{
	float x0, y0, len;
	int k, steps;

	if (!p->have_current)
		cv_subpath(p, x1, y1);
	x0 = p->cx;
	y0 = p->cy;
	len = hypotf(x1 - x0, y1 - y0) + hypotf(x2 - x1, y2 - y1) +
			hypotf(x3 - x2, y3 - y2);
	steps = (int)(sqrtf(len) * 1.6f) + 2;
	if (steps > 100)
		steps = 100;
	for (k = 1; k <= steps; k++) {
		float u = (float)k / steps, v = 1 - u;
		cv_point(p, v * v * v * x0 + 3 * v * v * u * x1 +
				3 * v * u * u * x2 + u * u * u * x3,
			 v * v * v * y0 + 3 * v * v * u * y1 +
				3 * v * u * u * y2 + u * u * u * y3);
	}
}

/* exported interface documented in html/canvas.h */
void html_canvas_bezier_to(struct html_canvas *cv, double c1x, double c1y,
		double c2x, double c2y, double x, double y)
{
	float a, b, c, d, e, f;

	cv_tx(cv->st.m, c1x, c1y, &a, &b);
	cv_tx(cv->st.m, c2x, c2y, &c, &d);
	cv_tx(cv->st.m, x, y, &e, &f);
	cv_bezier_dev(&cv->path, a, b, c, d, e, f);
}

/* exported interface documented in html/canvas.h */
void html_canvas_quad_to(struct html_canvas *cv, double cx, double cy,
		double x, double y)
{
	struct cv_path *p = &cv->path;
	float qx, qy, ex, ey, x0, y0;

	cv_tx(cv->st.m, cx, cy, &qx, &qy);
	cv_tx(cv->st.m, x, y, &ex, &ey);
	if (!p->have_current)
		cv_subpath(p, qx, qy);
	x0 = p->cx;
	y0 = p->cy;
	cv_bezier_dev(p, x0 + 2.0f / 3 * (qx - x0), y0 + 2.0f / 3 * (qy - y0),
			ex + 2.0f / 3 * (qx - ex), ey + 2.0f / 3 * (qy - ey),
			ex, ey);
}

/* exported interface documented in html/canvas.h */
void html_canvas_ellipse(struct html_canvas *cv, double x, double y,
		double rx, double ry, double rot, double a0, double a1,
		bool ccw)
{
	struct cv_path *p = &cv->path;
	double sweep, a, cr = cos(rot), sr = sin(rot);
	int k, steps;
	float px, py;

	if (rx < 0 || ry < 0 || !isfinite(a0) || !isfinite(a1))
		return;
	sweep = a1 - a0;
	if (!ccw) {
		if (sweep >= 2 * M_PI)
			sweep = 2 * M_PI;
		else if (sweep < 0)
			sweep = fmod(sweep, 2 * M_PI) + 2 * M_PI;
	} else {
		if (sweep <= -2 * M_PI)
			sweep = -2 * M_PI;
		else if (sweep > 0)
			sweep = fmod(sweep, 2 * M_PI) - 2 * M_PI;
	}
	steps = (int)(fabs(sweep) * sqrt(fmax(rx, ry) *
			cv_scale(cv->st.m)) * 1.2) + 4;
	if (steps > 400)
		steps = 400;
	for (k = 0; k <= steps; k++) {
		double ex, ey;
		a = a0 + sweep * k / steps;
		ex = rx * cos(a);
		ey = ry * sin(a);
		cv_tx(cv->st.m, x + ex * cr - ey * sr, y + ex * sr + ey * cr,
				&px, &py);
		if (k == 0)
			cv_line_dev(p, px, py);
		else
			cv_point(p, px, py);
	}
}

/* exported interface documented in html/canvas.h */
void html_canvas_arc(struct html_canvas *cv, double x, double y, double r,
		double a0, double a1, bool ccw)
{
	html_canvas_ellipse(cv, x, y, r, r, 0, a0, a1, ccw);
}

/* exported interface documented in html/canvas.h */
void html_canvas_arc_to(struct html_canvas *cv, double x1, double y1,
		double x2, double y2, double r)
{
	struct cv_path *p = &cv->path;
	double inv[6], x0, y0, d0x, d0y, d2x, d2y, l0, l2, cosang, ang, t;
	double cx, cy, a0, a1, nx, ny;
	bool ccw;

	if (!p->have_current) {
		html_canvas_move_to(cv, x1, y1);
		return;
	}
	if (!cv_inverse(cv->st.m, inv))
		return;
	x0 = inv[0] * p->cx + inv[2] * p->cy + inv[4];
	y0 = inv[1] * p->cx + inv[3] * p->cy + inv[5];
	d0x = x0 - x1;
	d0y = y0 - y1;
	d2x = x2 - x1;
	d2y = y2 - y1;
	l0 = hypot(d0x, d0y);
	l2 = hypot(d2x, d2y);
	if (r <= 0 || l0 < 1e-9 || l2 < 1e-9) {
		html_canvas_line_to(cv, x1, y1);
		return;
	}
	cosang = (d0x * d2x + d0y * d2y) / (l0 * l2);
	if (fabs(cosang) > 0.99999) {
		html_canvas_line_to(cv, x1, y1);
		return;
	}
	ang = acos(cosang);
	t = r / tan(ang / 2);
	d0x /= l0; d0y /= l0;
	d2x /= l2; d2y /= l2;
	/* centre along the bisector */
	nx = d0x + d2x;
	ny = d0y + d2y;
	{
		double nl = hypot(nx, ny);
		double dist = r / sin(ang / 2);
		cx = x1 + nx / nl * dist;
		cy = y1 + ny / nl * dist;
	}
	html_canvas_line_to(cv, x1 + d0x * t, y1 + d0y * t);
	a0 = atan2(y1 + d0y * t - cy, x1 + d0x * t - cx);
	a1 = atan2(y1 + d2y * t - cy, x1 + d2x * t - cx);
	ccw = (d0x * d2y - d0y * d2x) > 0;
	html_canvas_arc(cv, cx, cy, r, a0, a1, ccw);
}

/* exported interface documented in html/canvas.h */
void html_canvas_rect(struct html_canvas *cv, double x, double y,
		double w, double h)
{
	html_canvas_move_to(cv, x, y);
	html_canvas_line_to(cv, x + w, y);
	html_canvas_line_to(cv, x + w, y + h);
	html_canvas_line_to(cv, x, y + h);
	html_canvas_close_path(cv);
}

/* exported interface documented in html/canvas.h */
void html_canvas_round_rect(struct html_canvas *cv, double x, double y,
		double w, double h, double r)
{
	if (r <= 0) {
		html_canvas_rect(cv, x, y, w, h);
		return;
	}
	r = fmin(r, fmin(fabs(w), fabs(h)) / 2);
	html_canvas_move_to(cv, x + r, y);
	html_canvas_arc(cv, x + w - r, y + r, r, -M_PI / 2, 0, false);
	html_canvas_arc(cv, x + w - r, y + h - r, r, 0, M_PI / 2, false);
	html_canvas_arc(cv, x + r, y + h - r, r, M_PI / 2, M_PI, false);
	html_canvas_arc(cv, x + r, y + r, r, M_PI, M_PI * 1.5, false);
	html_canvas_close_path(cv);
}

/* ------------------------------------------------------------------ */
/* Rasterising                                                        */
/* ------------------------------------------------------------------ */

struct cv_raster {
	float *a;
	int x0, y0, w, h;   /* rows are w + 3 wide */
};

static void cv_raster_line(struct cv_raster *r, float px0, float py0,
		float px1, float py1)
{
	float dir, dxdy, x, fx0, fy0, fx1, fy1;
	int y, ystart, yend, stride = r->w + 3;

	fx0 = px0 - r->x0; fy0 = py0 - r->y0;
	fx1 = px1 - r->x0; fy1 = py1 - r->y0;
	if (fabsf(fy0 - fy1) <= 1e-6f || !isfinite(fx0 + fx1 + fy0 + fy1))
		return;
	if (fy0 < fy1) {
		dir = 1.0f;
	} else {
		float t;
		dir = -1.0f;
		t = fx0; fx0 = fx1; fx1 = t;
		t = fy0; fy0 = fy1; fy1 = t;
	}
	if (fy1 <= 0 || fy0 >= r->h)
		return;
	dxdy = (fx1 - fx0) / (fy1 - fy0);
	x = fx0;
	if (fy0 < 0) {
		x -= fy0 * dxdy;
		ystart = 0;
	} else {
		ystart = (int)fy0;
	}
	yend = (int)ceilf(fy1);
	if (yend > r->h)
		yend = r->h;

	for (y = ystart; y < yend; y++) {
		float *row = r->a + (size_t)y * stride;
		float dy = fminf((float)(y + 1), fy1) - fmaxf((float)y, fy0);
		float xnext = x + dxdy * dy;
		float d = dy * dir;
		float xa = x < xnext ? x : xnext;
		float xb = x < xnext ? xnext : x;
		float xa_c = fminf(fmaxf(xa, 0.0f), (float)r->w + 1);
		float xb_c = fminf(fmaxf(xb, 0.0f), (float)r->w + 1);
		float xaf = floorf(xa_c);
		int xai = (int)xaf, xbi = (int)ceilf(xb_c);

		if (xbi <= xai + 1) {
			float xmf = 0.5f * (xa_c + xb_c) - xaf;
			row[xai] += d - d * xmf;
			row[xai + 1] += d * xmf;
		} else {
			float s = 1.0f / (xb_c - xa_c);
			float x0f = xa_c - xaf;
			float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
			float x1f = xb_c - (float)xbi + 1.0f;
			float am = 0.5f * s * x1f * x1f;
			int xi;

			row[xai] += d * a0;
			if (xbi == xai + 2) {
				row[xai + 1] += d * (1.0f - a0 - am);
			} else {
				float a1 = s * (1.5f - x0f), a2;
				row[xai + 1] += d * (a1 - a0);
				for (xi = xai + 2; xi < xbi - 1; xi++)
					row[xi] += d * s;
				a2 = a1 + (float)(xbi - xai - 3) * s;
				row[xbi - 1] += d * (1.0f - a2 - am);
			}
			row[xbi] += d * am;
		}
		x = xnext;
	}
}

/** add a polygon with positive orientation (for unions) */
static void cv_raster_poly(struct cv_raster *r, const float *pt, int n)
{
	float area = 0;
	int k;

	for (k = 0; k < n; k++) {
		int j = (k + 1) % n;
		area += pt[k * 2] * pt[j * 2 + 1] - pt[j * 2] * pt[k * 2 + 1];
	}
	for (k = 0; k < n; k++) {
		int j = (k + 1) % n;
		if (area >= 0)
			cv_raster_line(r, pt[k * 2], pt[k * 2 + 1],
					pt[j * 2], pt[j * 2 + 1]);
		else
			cv_raster_line(r, pt[j * 2], pt[j * 2 + 1],
					pt[k * 2], pt[k * 2 + 1]);
	}
}

static void cv_raster_disc(struct cv_raster *r, float cx, float cy, float rad)
{
	float pt[48];
	int k, n = rad < 3 ? 12 : 24;

	for (k = 0; k < n; k++) {
		float a = (float)k * 6.2831853f / n;
		pt[k * 2] = cx + rad * cosf(a);
		pt[k * 2 + 1] = cy + rad * sinf(a);
	}
	cv_raster_poly(r, pt, n);
}

/** paint colour at a device pixel (straight RGBA) */
static uint32_t cv_paint_at(const struct canvas_paint *p, const double inv[6],
		bool inv_ok, int x, int y)
{
	double ux, uy, t;
	int k;

	if (p->type == CANVAS_PAINT_COLOUR || p->nstops == 0 || !inv_ok)
		return p->nstops > 0 && p->type != CANVAS_PAINT_COLOUR ?
			p->stop_colour[0] : p->colour;

	ux = inv[0] * (x + 0.5) + inv[2] * (y + 0.5) + inv[4];
	uy = inv[1] * (x + 0.5) + inv[3] * (y + 0.5) + inv[5];

	if (p->type == CANVAS_PAINT_LINEAR) {
		double dx = p->x1 - p->x0, dy = p->y1 - p->y0;
		double l2 = dx * dx + dy * dy;
		t = l2 > 0 ? ((ux - p->x0) * dx + (uy - p->y0) * dy) / l2 : 0;
	} else {
		double d = hypot(ux - p->x1, uy - p->y1);
		t = (p->r1 - p->r0) != 0 ? (d - p->r0) / (p->r1 - p->r0) : 1;
	}
	if (t <= p->stop_pos[0])
		return p->stop_colour[0];
	if (t >= p->stop_pos[p->nstops - 1])
		return p->stop_colour[p->nstops - 1];
	for (k = 1; k < p->nstops; k++) {
		if (t <= p->stop_pos[k]) {
			float span = p->stop_pos[k] - p->stop_pos[k - 1];
			float f = span > 0 ? (float)(t - p->stop_pos[k - 1]) /
					span : 1;
			uint32_t a = p->stop_colour[k - 1], b = p->stop_colour[k];
			uint32_t o = 0;
			int sh;
			for (sh = 0; sh < 32; sh += 8) {
				float ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
				o |= (uint32_t)(ca + (cb - ca) * f + 0.5f) << sh;
			}
			return o;
		}
	}
	return p->stop_colour[p->nstops - 1];
}

/** composite one source pixel (straight RGBA) with coverage onto dst */
static inline void cv_composite(uint8_t *d, uint32_t src, float cov,
		enum canvas_composite op)
{
	float sa = ((src >> 24) & 0xff) / 255.0f * cov;
	float sr = src & 0xff, sg = (src >> 8) & 0xff, sb = (src >> 16) & 0xff;
	float da = d[3] / 255.0f;
	float oa, orr, og, ob;

	switch (op) {
	case CANVAS_OP_COPY:
		/* within the shape the source replaces the destination */
		oa = sa + da * (1 - cov);
		if (oa <= 0) {
			d[0] = d[1] = d[2] = d[3] = 0;
			return;
		}
		orr = (sr * sa + d[0] * da * (1 - cov)) / oa;
		og = (sg * sa + d[1] * da * (1 - cov)) / oa;
		ob = (sb * sa + d[2] * da * (1 - cov)) / oa;
		break;
	case CANVAS_OP_DESTINATION_OUT:
		oa = da * (1 - sa);
		d[3] = (uint8_t)(oa * 255 + 0.5f);
		return;
	case CANVAS_OP_DESTINATION_OVER:
		oa = da + sa * (1 - da);
		if (oa <= 0)
			return;
		orr = (d[0] * da + sr * sa * (1 - da)) / oa;
		og = (d[1] * da + sg * sa * (1 - da)) / oa;
		ob = (d[2] * da + sb * sa * (1 - da)) / oa;
		break;
	case CANVAS_OP_LIGHTER:
		oa = fminf(1, da + sa);
		if (oa <= 0)
			return;
		orr = fminf(255, (d[0] * da + sr * sa) / oa);
		og = fminf(255, (d[1] * da + sg * sa) / oa);
		ob = fminf(255, (d[2] * da + sb * sa) / oa);
		break;
	case CANVAS_OP_SOURCE_ATOP:
		if (da <= 0)
			return;
		oa = da;
		orr = sr * sa + d[0] * (1 - sa);
		og = sg * sa + d[1] * (1 - sa);
		ob = sb * sa + d[2] * (1 - sa);
		break;
	default:
		if (sa <= 0)
			return;
		oa = sa + da * (1 - sa);
		orr = (sr * sa + d[0] * da * (1 - sa)) / oa;
		og = (sg * sa + d[1] * da * (1 - sa)) / oa;
		ob = (sb * sa + d[2] * da * (1 - sa)) / oa;
		break;
	}
	d[0] = (uint8_t)(orr + 0.5f);
	d[1] = (uint8_t)(og + 0.5f);
	d[2] = (uint8_t)(ob + 0.5f);
	d[3] = (uint8_t)(oa * 255 + 0.5f);
}

/** set up a raster over the bounding box of points, clipped */
static bool cv_raster_begin(struct html_canvas *cv, struct cv_raster *r,
		float minx, float miny, float maxx, float maxy)
{
	const int *c = cv->st.clip;
	int x0 = (int)floorf(minx) - 1, y0 = (int)floorf(miny) - 1;
	int x1 = (int)ceilf(maxx) + 1, y1 = (int)ceilf(maxy) + 1;

	if (x0 < c[0]) x0 = c[0];
	if (y0 < c[1]) y0 = c[1];
	if (x1 > c[2]) x1 = c[2];
	if (y1 > c[3]) y1 = c[3];
	if (x0 >= x1 || y0 >= y1)
		return false;
	r->x0 = x0;
	r->y0 = y0;
	r->w = x1 - x0;
	r->h = y1 - y0;
	r->a = calloc((size_t)(r->w + 3) * r->h, sizeof(float));
	return r->a != NULL;
}

/** composite accumulated coverage with a paint */
static void cv_raster_paint(struct html_canvas *cv, struct cv_raster *r,
		const struct canvas_paint *paint, bool evenodd)
{
	double inv[6];
	bool inv_ok = cv_inverse(cv->st.m, inv);
	float ga = (float)cv->st.global_alpha;
	int x, y, stride = r->w + 3;

	for (y = 0; y < r->h; y++) {
		float acc = 0;
		const float *row = r->a + (size_t)y * stride;
		uint8_t *d = cv->px + ((size_t)(r->y0 + y) * cv->w + r->x0) * 4;
		for (x = 0; x < r->w; x++, d += 4) {
			float v;
			acc += row[x];
			v = fabsf(acc);
			if (evenodd) {
				v = fmodf(v, 2.0f);
				if (v > 1)
					v = 2 - v;
			} else if (v > 1) {
				v = 1;
			}
			if (v < 0.002f) {
				if (cv->st.op == CANVAS_OP_COPY)
					d[0] = d[1] = d[2] = d[3] = 0;
				continue;
			}
			cv_composite(d, cv_paint_at(paint, inv, inv_ok,
					r->x0 + x, r->y0 + y), v * ga, cv->st.op);
		}
	}
	free(r->a);
	r->a = NULL;
	cv_mark_dirty(cv);
}

static void cv_path_bbox(const struct cv_path *p, float pad, float *x0,
		float *y0, float *x1, float *y1)
{
	int k;

	*x0 = *y0 = 1e30f;
	*x1 = *y1 = -1e30f;
	for (k = 0; k < p->npt; k++) {
		*x0 = fminf(*x0, p->pt[k * 2]);
		*x1 = fmaxf(*x1, p->pt[k * 2]);
		*y0 = fminf(*y0, p->pt[k * 2 + 1]);
		*y1 = fmaxf(*y1, p->pt[k * 2 + 1]);
	}
	*x0 -= pad;
	*y0 -= pad;
	*x1 += pad;
	*y1 += pad;
}

/* exported interface documented in html/canvas.h */
void html_canvas_fill(struct html_canvas *cv, bool evenodd)
{
	struct cv_path *p = &cv->path;
	struct cv_raster r;
	float x0, y0, x1, y1;
	int s, k;

	if (p->npt < 3 || cv->px == NULL)
		return;
	cv_path_bbox(p, 0, &x0, &y0, &x1, &y1);
	if (!cv_raster_begin(cv, &r, x0, y0, x1, y1))
		return;
	for (s = 0; s < p->nsub; s++) {
		int a = p->start[s];
		int b = s + 1 < p->nsub ? p->start[s + 1] : p->npt;
		for (k = a; k < b; k++) {
			int j = k + 1 < b ? k + 1 : a;
			cv_raster_line(&r, p->pt[k * 2], p->pt[k * 2 + 1],
					p->pt[j * 2], p->pt[j * 2 + 1]);
		}
	}
	cv_raster_paint(cv, &r, &cv->st.fill, evenodd);
}

/* exported interface documented in html/canvas.h */
void html_canvas_stroke(struct html_canvas *cv)
{
	struct cv_path *p = &cv->path;
	struct cv_raster r;
	float x0, y0, x1, y1, hw;
	int s;

	if (p->npt < 1 || cv->px == NULL)
		return;
	hw = (float)(cv->st.line_width * cv_scale(cv->st.m) / 2);
	if (!(hw > 0))
		return;
	cv_path_bbox(p, hw * 1.5f + 2, &x0, &y0, &x1, &y1);
	if (!cv_raster_begin(cv, &r, x0, y0, x1, y1))
		return;

	for (s = 0; s < p->nsub; s++) {
		int a = p->start[s];
		int b = s + 1 < p->nsub ? p->start[s + 1] : p->npt;
		int last = b - 1, k;
		bool closed = p->closed[s];

		if (b - a < 2)
			continue;
		for (k = a; k < b; k++) {
			float ax = p->pt[k * 2], ay = p->pt[k * 2 + 1];
			float bx, by, dx, dy, l, q[8];
			int j = k + 1;
			if (j > last) {
				if (!closed || b - a < 3)
					break;
				j = a;
			}
			bx = p->pt[j * 2];
			by = p->pt[j * 2 + 1];
			dx = bx - ax;
			dy = by - ay;
			l = hypotf(dx, dy);
			if (l < 1e-5f)
				continue;
			dx = dx / l * hw;
			dy = dy / l * hw;
			if (cv->st.line_cap == 2 && !closed) {
				/* square caps extend the end segments */
				if (k == a) {
					ax -= dx;
					ay -= dy;
				}
				if (j == last) {
					bx += dx;
					by += dy;
				}
			}
			q[0] = ax - dy; q[1] = ay + dx;
			q[2] = bx - dy; q[3] = by + dx;
			q[4] = bx + dy; q[5] = by - dx;
			q[6] = ax + dy; q[7] = ay - dx;
			cv_raster_poly(&r, q, 4);
		}
		/* joins (round/miter approximated by round) and caps */
		for (k = a; k <= last; k++) {
			bool end = !closed && (k == a || k == last);
			if (end && cv->st.line_cap != 1)
				continue;
			if (!end && cv->st.line_join == 2)
				continue;
			if (hw >= 0.6f)
				cv_raster_disc(&r, p->pt[k * 2],
						p->pt[k * 2 + 1], hw);
		}
	}
	cv_raster_paint(cv, &r, &cv->st.stroke, false);
}

/* exported interface documented in html/canvas.h */
void html_canvas_clip(struct html_canvas *cv)
{
	struct cv_path *p = &cv->path;
	float x0, y0, x1, y1;
	int *c = cv->st.clip;

	if (p->npt == 0) {
		c[2] = c[0];
		return;
	}
	/* clip to the path's bounding box */
	cv_path_bbox(p, 0, &x0, &y0, &x1, &y1);
	if ((int)floorf(x0) > c[0]) c[0] = (int)floorf(x0);
	if ((int)floorf(y0) > c[1]) c[1] = (int)floorf(y0);
	if ((int)ceilf(x1) < c[2]) c[2] = (int)ceilf(x1);
	if ((int)ceilf(y1) < c[3]) c[3] = (int)ceilf(y1);
	if (c[2] < c[0]) c[2] = c[0];
	if (c[3] < c[1]) c[3] = c[1];
}

/* exported interface documented in html/canvas.h */
bool html_canvas_point_in_path(struct html_canvas *cv, double x, double y)
{
	struct cv_path *p = &cv->path;
	int s, k, wn = 0;

	/* point in device space (spec: the point is not transformed) */
	for (s = 0; s < p->nsub; s++) {
		int a = p->start[s];
		int b = s + 1 < p->nsub ? p->start[s + 1] : p->npt;
		for (k = a; k < b; k++) {
			int j = k + 1 < b ? k + 1 : a;
			float ax = p->pt[k * 2], ay = p->pt[k * 2 + 1];
			float bx = p->pt[j * 2], by = p->pt[j * 2 + 1];
			if (ay <= y) {
				if (by > y && (bx - ax) * (y - ay) -
						(x - ax) * (by - ay) > 0)
					wn++;
			} else if (by <= y && (bx - ax) * (y - ay) -
					(x - ax) * (by - ay) < 0) {
				wn--;
			}
		}
	}
	return wn != 0;
}

/* exported interface documented in html/canvas.h */
void html_canvas_fill_rect(struct html_canvas *cv, double x, double y,
		double w, double h)
{
	struct cv_path saved = cv->path;

	memset(&cv->path, 0, sizeof(cv->path));
	html_canvas_rect(cv, x, y, w, h);
	html_canvas_fill(cv, false);
	cv_path_free(&cv->path);
	cv->path = saved;
}

/* exported interface documented in html/canvas.h */
void html_canvas_stroke_rect(struct html_canvas *cv, double x, double y,
		double w, double h)
{
	struct cv_path saved = cv->path;

	memset(&cv->path, 0, sizeof(cv->path));
	html_canvas_rect(cv, x, y, w, h);
	html_canvas_stroke(cv);
	cv_path_free(&cv->path);
	cv->path = saved;
}

/* exported interface documented in html/canvas.h */
void html_canvas_clear_rect(struct html_canvas *cv, double x, double y,
		double w, double h)
{
	const double *m = cv->st.m;

	if (cv->px == NULL)
		return;
	if (m[1] == 0 && m[2] == 0) {
		/* axis aligned: clear whole pixels */
		float fx0, fy0, fx1, fy1;
		int x0, y0, x1, y1, row;
		const int *c = cv->st.clip;
		cv_tx(m, x, y, &fx0, &fy0);
		cv_tx(m, x + w, y + h, &fx1, &fy1);
		x0 = (int)floorf(fminf(fx0, fx1) + 0.5f);
		x1 = (int)floorf(fmaxf(fx0, fx1) + 0.5f);
		y0 = (int)floorf(fminf(fy0, fy1) + 0.5f);
		y1 = (int)floorf(fmaxf(fy0, fy1) + 0.5f);
		if (x0 < c[0]) x0 = c[0];
		if (y0 < c[1]) y0 = c[1];
		if (x1 > c[2]) x1 = c[2];
		if (y1 > c[3]) y1 = c[3];
		if (x0 >= x1 || y0 >= y1)
			return;
		for (row = y0; row < y1; row++)
			memset(cv->px + ((size_t)row * cv->w + x0) * 4, 0,
					(size_t)(x1 - x0) * 4);
		cv_mark_dirty(cv);
	} else {
		struct cv_path saved = cv->path;
		enum canvas_composite op = cv->st.op;
		struct canvas_paint fill = cv->st.fill;
		double ga = cv->st.global_alpha;

		cv->st.op = CANVAS_OP_DESTINATION_OUT;
		cv->st.fill.type = CANVAS_PAINT_COLOUR;
		cv->st.fill.colour = 0xff000000;
		cv->st.global_alpha = 1;
		memset(&cv->path, 0, sizeof(cv->path));
		html_canvas_rect(cv, x, y, w, h);
		html_canvas_fill(cv, false);
		cv_path_free(&cv->path);
		cv->path = saved;
		cv->st.op = op;
		cv->st.fill = fill;
		cv->st.global_alpha = ga;
	}
}

/* ------------------------------------------------------------------ */
/* Images                                                             */
/* ------------------------------------------------------------------ */

/* exported interface documented in html/canvas.h */
void html_canvas_draw_pixels(struct html_canvas *cv, const uint8_t *px,
		int pw, int ph, size_t stride, double sx, double sy,
		double sw, double sh, double dx, double dy, double dw,
		double dh)
{
	double m[6], inv[6];
	float cx[4], cy[4], x0, y0, x1, y1;
	const int *c = cv->st.clip;
	int ix0, iy0, ix1, iy1, x, y, k;
	float ga = (float)cv->st.global_alpha;
	bool smooth = cv->st.smoothing;

	if (cv->px == NULL || px == NULL || pw <= 0 || ph <= 0 ||
	    sw == 0 || sh == 0 || dw == 0 || dh == 0)
		return;

	/* device -> source mapping: combine the dest rect with the CTM */
	memcpy(m, cv->st.m, sizeof(m));
	cv_tx(m, dx, dy, &cx[0], &cy[0]);
	cv_tx(m, dx + dw, dy, &cx[1], &cy[1]);
	cv_tx(m, dx + dw, dy + dh, &cx[2], &cy[2]);
	cv_tx(m, dx, dy + dh, &cx[3], &cy[3]);
	x0 = x1 = cx[0];
	y0 = y1 = cy[0];
	for (k = 1; k < 4; k++) {
		x0 = fminf(x0, cx[k]); x1 = fmaxf(x1, cx[k]);
		y0 = fminf(y0, cy[k]); y1 = fmaxf(y1, cy[k]);
	}
	if (!cv_inverse(m, inv))
		return;
	ix0 = (int)floorf(x0); iy0 = (int)floorf(y0);
	ix1 = (int)ceilf(x1); iy1 = (int)ceilf(y1);
	if (ix0 < c[0]) ix0 = c[0];
	if (iy0 < c[1]) iy0 = c[1];
	if (ix1 > c[2]) ix1 = c[2];
	if (iy1 > c[3]) iy1 = c[3];

	for (y = iy0; y < iy1; y++) {
		uint8_t *d = cv->px + ((size_t)y * cv->w + ix0) * 4;
		for (x = ix0; x < ix1; x++, d += 4) {
			double ux = inv[0] * (x + 0.5) + inv[2] * (y + 0.5) +
					inv[4];
			double uy = inv[1] * (x + 0.5) + inv[3] * (y + 0.5) +
					inv[5];
			double fu = (ux - dx) / dw, fv = (uy - dy) / dh;
			double spx, spy;
			uint32_t src;

			if (fu < 0 || fu >= 1 || fv < 0 || fv >= 1)
				continue;
			spx = sx + fu * sw;
			spy = sy + fv * sh;
			if (smooth && (fabs(sw - dw * cv_scale(m)) > 0.5 ||
					fabs(sh - dh * cv_scale(m)) > 0.5)) {
				/* bilinear */
				double fx = spx - 0.5, fy = spy - 0.5;
				int ax = (int)floor(fx), ay = (int)floor(fy);
				float tx = (float)(fx - ax), ty = (float)(fy - ay);
				int bx = ax + 1, by = ay + 1, ch;
				const uint8_t *p00, *p10, *p01, *p11;
				float acc[4] = { 0, 0, 0, 0 }, wsum;
				if (ax < 0) ax = 0;
				if (ay < 0) ay = 0;
				if (bx >= pw) bx = pw - 1;
				if (by >= ph) by = ph - 1;
				if (ax >= pw) ax = pw - 1;
				if (ay >= ph) ay = ph - 1;
				p00 = px + ay * stride + ax * 4;
				p10 = px + ay * stride + bx * 4;
				p01 = px + by * stride + ax * 4;
				p11 = px + by * stride + bx * 4;
				/* weight colour by alpha to avoid dark fringes */
				{
					float w00 = (1 - tx) * (1 - ty) * p00[3];
					float w10 = tx * (1 - ty) * p10[3];
					float w01 = (1 - tx) * ty * p01[3];
					float w11 = tx * ty * p11[3];
					wsum = w00 + w10 + w01 + w11;
					for (ch = 0; ch < 3; ch++)
						acc[ch] = wsum > 0 ? (p00[ch] * w00 +
							p10[ch] * w10 + p01[ch] * w01 +
							p11[ch] * w11) / wsum : 0;
					acc[3] = wsum;
				}
				src = (uint32_t)(acc[0] + 0.5f) |
					((uint32_t)(acc[1] + 0.5f) << 8) |
					((uint32_t)(acc[2] + 0.5f) << 16) |
					((uint32_t)(acc[3] + 0.5f) << 24);
			} else {
				int ixs = (int)spx, iys = (int)spy;
				const uint8_t *s;
				if (ixs < 0) ixs = 0;
				if (iys < 0) iys = 0;
				if (ixs >= pw) ixs = pw - 1;
				if (iys >= ph) iys = ph - 1;
				s = px + iys * stride + ixs * 4;
				src = s[0] | (s[1] << 8) | (s[2] << 16) |
					((uint32_t)s[3] << 24);
			}
			cv_composite(d, src, ga, cv->st.op);
		}
	}
	cv_mark_dirty(cv);
}

/** find the decoded image for an <img> node */
static hlcache_handle *cv_image_handle(html_content *c, dom_node *n)
{
	struct box *b = box_for_node(n);
	struct content_html_object *o;
	dom_string *src = NULL;
	nsurl *url = NULL;
	hlcache_handle *h = NULL;

	if (b != NULL && b->object != NULL)
		return b->object;

	/* not laid out (e.g. new Image()): look for its fetch */
	if (dom_element_get_attribute(n, corestring_dom_src, &src) !=
			DOM_NO_ERR || src == NULL)
		return NULL;
	if (nsurl_join(c->base_url, dom_string_data(src), &url) !=
			NSERROR_OK) {
		dom_string_unref(src);
		return NULL;
	}
	dom_string_unref(src);
	for (o = c->object_list; o != NULL; o = o->next) {
		if (o->content != NULL && nsurl_compare(
				hlcache_handle_get_url(o->content), url,
				NSURL_COMPLETE)) {
			h = o->content;
			break;
		}
	}
	nsurl_unref(url);
	return h;
}

/** pixels of a decoded image as straight RGBA, caller frees */
static uint8_t *cv_image_pixels(hlcache_handle *h, int *w, int *h_out)
{
	struct bitmap *bmp;
	const uint8_t *buf;
	uint8_t *out;
	size_t stride;
	int bw, bh, row;
	bitmap_fmt_t rgba = { .layout = BITMAP_LAYOUT_R8G8B8A8, .pma = false };

	if (content_get_status(h) != CONTENT_STATUS_DONE)
		return NULL;
	bmp = content_get_bitmap(h);
	if (bmp == NULL)
		return NULL;
	buf = guit->bitmap->get_buffer(bmp);
	stride = guit->bitmap->get_rowstride(bmp);
	bw = guit->bitmap->get_width(bmp);
	bh = guit->bitmap->get_height(bmp);
	if (buf == NULL || bw <= 0 || bh <= 0)
		return NULL;
	out = malloc((size_t)bw * bh * 4);
	if (out == NULL)
		return NULL;
	for (row = 0; row < bh; row++)
		memcpy(out + (size_t)row * bw * 4, buf + row * stride,
				(size_t)bw * 4);
	/* convert from the client's layout */
	if (bitmap_fmt.layout != rgba.layout || bitmap_fmt.pma) {
		bitmap_fmt_t from = bitmap_fmt;
		size_t i, n = (size_t)bw * bh;
		for (i = 0; i < n; i++) {
			uint8_t *p = out + i * 4, t[4];
			memcpy(t, p, 4);
			switch (from.layout) {
			case BITMAP_LAYOUT_B8G8R8A8:
				p[0] = t[2]; p[1] = t[1]; p[2] = t[0]; p[3] = t[3];
				break;
			case BITMAP_LAYOUT_A8R8G8B8:
				p[0] = t[1]; p[1] = t[2]; p[2] = t[3]; p[3] = t[0];
				break;
			case BITMAP_LAYOUT_A8B8G8R8:
				p[0] = t[3]; p[1] = t[2]; p[2] = t[1]; p[3] = t[0];
				break;
			default:
				break;
			}
			if (from.pma && p[3] > 0 && p[3] < 255) {
				p[0] = p[0] * 255 / p[3];
				p[1] = p[1] * 255 / p[3];
				p[2] = p[2] * 255 / p[3];
			}
		}
	}
	*w = bw;
	*h_out = bh;
	return out;
}

/* exported interface documented in html/canvas.h */
bool html_canvas_element_size(html_content *c, dom_node *src, int *w, int *h)
{
	dom_html_element_type tag;
	hlcache_handle *hh;

	if (dom_html_element_get_tag_type(src, &tag) != DOM_NO_ERR)
		return false;
	if (tag == DOM_HTML_ELEMENT_TYPE_CANVAS) {
		struct html_canvas *o = html_canvas_for_node(c, src, true);
		if (o == NULL)
			return false;
		*w = o->w;
		*h = o->h;
		return true;
	}
	if (tag != DOM_HTML_ELEMENT_TYPE_IMG)
		return false;
	hh = cv_image_handle(c, src);
	if (hh == NULL || content_get_status(hh) != CONTENT_STATUS_DONE)
		return false;
	*w = content_get_width(hh);
	*h = content_get_height(hh);
	return *w > 0 && *h > 0;
}

/* exported interface documented in html/canvas.h */
bool html_canvas_draw_element(struct html_canvas *cv, dom_node *src,
		double sx, double sy, double sw, double sh, double dx,
		double dy, double dw, double dh, bool have_src_rect)
{
	dom_html_element_type tag;
	int w, h;

	if (dom_html_element_get_tag_type(src, &tag) != DOM_NO_ERR)
		return false;

	if (tag == DOM_HTML_ELEMENT_TYPE_CANVAS) {
		struct html_canvas *o = html_canvas_for_node(cv->html, src,
				true);
		uint8_t *copy;
		if (o == NULL || o->px == NULL || o->w == 0 || o->h == 0)
			return false;
		if (!have_src_rect) {
			sx = sy = 0;
			sw = o->w;
			sh = o->h;
		}
		if (o == cv) {
			/* drawing a canvas onto itself: use a snapshot */
			copy = malloc((size_t)o->w * o->h * 4);
			if (copy == NULL)
				return false;
			memcpy(copy, o->px, (size_t)o->w * o->h * 4);
			html_canvas_draw_pixels(cv, copy, o->w, o->h,
					(size_t)o->w * 4, sx, sy, sw, sh,
					dx, dy, dw, dh);
			free(copy);
		} else {
			html_canvas_draw_pixels(cv, o->px, o->w, o->h,
					(size_t)o->w * 4, sx, sy, sw, sh,
					dx, dy, dw, dh);
		}
		return true;
	}

	if (tag == DOM_HTML_ELEMENT_TYPE_IMG) {
		hlcache_handle *hh = cv_image_handle(cv->html, src);
		uint8_t *px;
		if (hh == NULL)
			return false;
		px = cv_image_pixels(hh, &w, &h);
		if (px == NULL)
			return false;
		if (!have_src_rect) {
			sx = sy = 0;
			sw = w;
			sh = h;
		}
		html_canvas_draw_pixels(cv, px, w, h, (size_t)w * 4, sx, sy,
				sw, sh, dx, dy, dw, dh);
		free(px);
		return true;
	}
	return false;
}

/* exported interface documented in html/canvas.h */
void html_canvas_get_image_data(struct html_canvas *cv, int x, int y,
		int w, int h, uint8_t *out)
{
	int row, col;

	for (row = 0; row < h; row++) {
		for (col = 0; col < w; col++) {
			int sx = x + col, sy = y + row;
			uint8_t *o = out + ((size_t)row * w + col) * 4;
			if (cv->px == NULL || sx < 0 || sy < 0 ||
			    sx >= cv->w || sy >= cv->h) {
				o[0] = o[1] = o[2] = o[3] = 0;
			} else {
				memcpy(o, cv->px + ((size_t)sy * cv->w + sx) * 4,
						4);
			}
		}
	}
}

/* exported interface documented in html/canvas.h */
void html_canvas_put_image_data(struct html_canvas *cv, const uint8_t *in,
		int iw, int ih, int dx, int dy, int sx, int sy, int sw,
		int sh)
{
	int row;

	if (cv->px == NULL)
		return;
	if (sx < 0) { sw += sx; sx = 0; }
	if (sy < 0) { sh += sy; sy = 0; }
	if (sx + sw > iw) sw = iw - sx;
	if (sy + sh > ih) sh = ih - sy;
	for (row = 0; row < sh; row++) {
		int ty = dy + sy + row, tx0 = dx + sx, col0 = 0, n = sw;
		if (ty < 0 || ty >= cv->h)
			continue;
		if (tx0 < 0) {
			col0 = -tx0;
			n += tx0;
			tx0 = 0;
		}
		if (tx0 + n > cv->w)
			n = cv->w - tx0;
		if (n <= 0)
			continue;
		memcpy(cv->px + ((size_t)ty * cv->w + tx0) * 4,
				in + ((size_t)(sy + row) * iw + sx + col0) * 4,
				(size_t)n * 4);
	}
	cv_mark_dirty(cv);
}

/* ------------------------------------------------------------------ */
/* Text                                                               */
/* ------------------------------------------------------------------ */

/* exported interface documented in html/canvas.h */
void html_canvas_set_font(struct html_canvas *cv, const char *css)
{
	struct canvas_state *s = &cv->st;
	const char *p = css;
	double size = -1;

	s->font_weight = 400;
	s->font_italic = false;
	while (*p != '\0') {
		const char *e;
		while (*p == ' ')
			p++;
		e = p;
		while (*e != '\0' && *e != ' ')
			e++;
		if (e == p)
			break;
		if (strncasecmp(p, "bold", 4) == 0 && e - p == 4) {
			s->font_weight = 700;
		} else if (strncasecmp(p, "italic", 6) == 0 ||
			   strncasecmp(p, "oblique", 7) == 0) {
			s->font_italic = true;
		} else if (isdigit((unsigned char)*p) && size < 0) {
			char *end;
			double v = strtod(p, &end);
			if (end < e && (end[0] == 'p' || end[0] == 'e' ||
					end[0] == 'r' || end[0] == '%')) {
				if (strncmp(end, "px", 2) == 0)
					size = v;
				else if (strncmp(end, "pt", 2) == 0)
					size = v * 96 / 72;
				else if (strncmp(end, "em", 2) == 0 ||
					 strncmp(end, "rem", 3) == 0)
					size = v * 16;
				else if (end[0] == '%')
					size = v * 16 / 100;
				/* "16px/1.5" line height is ignored */
				p = e;
				/* the rest is the family list */
				while (*p == ' ')
					p++;
				snprintf(s->font_family, sizeof(s->font_family),
						"%s", p);
				break;
			} else if (end == e && v >= 100 && v <= 900) {
				s->font_weight = (int)v;
			}
		}
		p = e;
	}
	if (size > 0)
		s->font_px = size;
}

static void cv_font_style(struct html_canvas *cv, double scale,
		plot_font_style_t *fs)
{
	const struct canvas_state *s = &cv->st;
	const char *f = s->font_family;

	memset(fs, 0, sizeof(*fs));
	fs->family = PLOT_FONT_FAMILY_SANS_SERIF;
	if (strcasestr(f, "mono") != NULL || strcasestr(f, "courier") != NULL ||
	    strcasestr(f, "consol") != NULL)
		fs->family = PLOT_FONT_FAMILY_MONOSPACE;
	else if ((strcasestr(f, "serif") != NULL &&
			strcasestr(f, "sans") == NULL) ||
		 strcasestr(f, "times") != NULL ||
		 strcasestr(f, "georgia") != NULL)
		fs->family = PLOT_FONT_FAMILY_SERIF;
	fs->size = (int)(s->font_px * scale * 72.0 / 96.0 * PLOT_STYLE_SCALE);
	if (fs->size < PLOT_STYLE_SCALE)
		fs->size = PLOT_STYLE_SCALE;
	fs->weight = s->font_weight;
	fs->flags = s->font_italic ? FONTF_ITALIC : FONTF_NONE;
	fs->background = 0xffffff;
	fs->foreground = 0;
}

/* exported interface documented in html/canvas.h */
double html_canvas_measure_text(struct html_canvas *cv, const char *text)
{
	plot_font_style_t fs;
	int w = 0;

	cv_font_style(cv, 1.0, &fs);
	if (guit->layout->width(&fs, text, strlen(text), &w) != NSERROR_OK)
		return strlen(text) * cv->st.font_px * 0.5;
	return w;
}

/* exported interface documented in html/canvas.h */
void html_canvas_fill_text(struct html_canvas *cv, const char *text,
		double x, double y, bool stroke)
{
	struct canvas_state *s = &cv->st;
	const struct canvas_paint *paint = stroke ? &s->stroke : &s->fill;
	double scale = cv_scale(s->m), w;
	plot_font_style_t fs;
	float dx, dy;
	int tw = 0;
	uint32_t c = paint->type == CANVAS_PAINT_COLOUR ? paint->colour :
			(paint->nstops > 0 ? paint->stop_colour[0] : 0xff000000);

	if (cv->px == NULL || guit->canvas == NULL ||
	    guit->canvas->fill_text == NULL || text[0] == '\0')
		return;

	cv_font_style(cv, scale, &fs);
	guit->layout->width(&fs, text, strlen(text), &tw);
	w = tw / (scale > 0 ? scale : 1);
	if (s->text_align == 1)
		x -= w / 2;
	else if (s->text_align == 2)
		x -= w;
	switch (s->text_baseline) {
	case 1: y += s->font_px * 0.8; break;          /* top/hanging */
	case 2: y += s->font_px * 0.35; break;         /* middle */
	case 3: y -= s->font_px * 0.2; break;          /* bottom */
	default: break;                                /* alphabetic */
	}
	cv_tx(s->m, x, y, &dx, &dy);
	guit->canvas->fill_text(cv->px, cv->w, cv->h, (size_t)cv->w * 4, &fs,
			(int)lroundf(dx), (int)lroundf(dy), text, strlen(text),
			c & 0xffffff, ((c >> 24) & 0xff) / 255.0f *
			(float)s->global_alpha, s->clip);
	cv_mark_dirty(cv);
}

/* exported interface documented in html/canvas.h */
bool html_canvas_parse_colour(const char *text, uint32_t *out)
{
	css_color c;
	const char *p = text;

	if (!css_parse_color_text(&p, 0xff000000, &c))
		return false;
	/* css_color is 0xAARRGGBB */
	*out = (c & 0xff000000) | ((c >> 16) & 0xff) | (c & 0xff00) |
			((c & 0xff) << 16);
	return true;
}
