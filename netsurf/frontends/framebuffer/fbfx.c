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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Framebuffer implementation of the extended plotter operations:
 * anti-aliased rounded shapes, gradients, box shadows and compositing
 * layers (rounded clipping, opacity and filters).
 *
 * These write directly to the 32bpp screen surface and honour the
 * current libnsfb clip rectangle.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include <libnsfb.h>
#include <libnsfb_plot.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "netsurf/types.h"
#include "netsurf/plotters.h"

#include "framebuffer/fbfx.h"

/* from framebuffer.c: the current plot target */
nsfb_t *framebuffer_current_surface(void);

/* NetSurf colour: 0xTTBBGGRR where TT is 255 - alpha */
#define NS_A(c) (255 - (int)(((c) >> 24) & 0xff))
#define NS_R(c) ((int)((c) & 0xff))
#define NS_G(c) ((int)(((c) >> 8) & 0xff))
#define NS_B(c) ((int)(((c) >> 16) & 0xff))

struct fx_surface {
	uint32_t *ptr;
	int stride; /* in pixels */
	int w, h;
	bool bgr; /* pixel layout 0xXXBBGGRR (else 0xXXRRGGBB) */
	int cx0, cy0, cx1, cy1; /* effective clip */
};

static bool fx_get_surface(struct fx_surface *s)
{
	uint8_t *ptr;
	int stride;
	enum nsfb_format_e fmt;
	nsfb_bbox_t clip;
	nsfb_t *fx_nsfb = framebuffer_current_surface();

	if (fx_nsfb == NULL)
		return false;
	if (nsfb_get_geometry(fx_nsfb, &s->w, &s->h, &fmt) != 0)
		return false;
	if (fmt != NSFB_FMT_XBGR8888 && fmt != NSFB_FMT_ABGR8888 &&
	    fmt != NSFB_FMT_XRGB8888 && fmt != NSFB_FMT_ARGB8888)
		return false;
	if (nsfb_get_buffer(fx_nsfb, &ptr, &stride) != 0 || ptr == NULL)
		return false;
	s->ptr = (uint32_t *)ptr;
	s->stride = stride / 4;
	s->bgr = (fmt == NSFB_FMT_XBGR8888 || fmt == NSFB_FMT_ABGR8888);

	nsfb_plot_get_clip(fx_nsfb, &clip);
	s->cx0 = clip.x0 < 0 ? 0 : clip.x0;
	s->cy0 = clip.y0 < 0 ? 0 : clip.y0;
	s->cx1 = clip.x1 > s->w ? s->w : clip.x1;
	s->cy1 = clip.y1 > s->h ? s->h : clip.y1;
	return s->cx0 < s->cx1 && s->cy0 < s->cy1;
}

static inline void fx_unpack(const struct fx_surface *s, uint32_t p,
		int *r, int *g, int *b)
{
	if (s->bgr) {
		*r = p & 0xff; *g = (p >> 8) & 0xff; *b = (p >> 16) & 0xff;
	} else {
		*b = p & 0xff; *g = (p >> 8) & 0xff; *r = (p >> 16) & 0xff;
	}
}

static inline uint32_t fx_pack(const struct fx_surface *s, int r, int g, int b)
{
	if (s->bgr)
		return 0xff000000u | (b << 16) | (g << 8) | r;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

/** blend colour (r,g,b) with coverage a (0..256) onto pixel */
static inline void fx_blend(const struct fx_surface *s, uint32_t *px,
		int r, int g, int b, int a)
{
	int dr, dg, db;

	if (a <= 0)
		return;
	if (a >= 256) {
		*px = fx_pack(s, r, g, b);
		return;
	}
	fx_unpack(s, *px, &dr, &dg, &db);
	dr += ((r - dr) * a) >> 8;
	dg += ((g - dg) * a) >> 8;
	db += ((b - db) * a) >> 8;
	*px = fx_pack(s, dr, dg, db);
}

/**
 * Rounded rectangle description in float device coordinates.
 */
struct fx_rr {
	float x0, y0, x1, y1;
	float rh[4], rv[4];
};

static void fx_rr_init(struct fx_rr *rr, const struct rect *r,
		const struct plot_radii *radii)
{
	float w, h, f = 1.0f;
	int i;

	rr->x0 = r->x0; rr->y0 = r->y0; rr->x1 = r->x1; rr->y1 = r->y1;
	w = rr->x1 - rr->x0;
	h = rr->y1 - rr->y0;
	for (i = 0; i < 4; i++) {
		rr->rh[i] = radii ? (radii->h[i] > 0 ? radii->h[i] : 0) : 0;
		rr->rv[i] = radii ? (radii->v[i] > 0 ? radii->v[i] : 0) : 0;
	}
	/* scale radii down if they overlap (CSS Backgrounds 5.5) */
	if (rr->rh[0] + rr->rh[1] > w && rr->rh[0] + rr->rh[1] > 0)
		f = fminf(f, w / (rr->rh[0] + rr->rh[1]));
	if (rr->rh[3] + rr->rh[2] > w && rr->rh[3] + rr->rh[2] > 0)
		f = fminf(f, w / (rr->rh[3] + rr->rh[2]));
	if (rr->rv[0] + rr->rv[3] > h && rr->rv[0] + rr->rv[3] > 0)
		f = fminf(f, h / (rr->rv[0] + rr->rv[3]));
	if (rr->rv[1] + rr->rv[2] > h && rr->rv[1] + rr->rv[2] > 0)
		f = fminf(f, h / (rr->rv[1] + rr->rv[2]));
	if (f < 1.0f) {
		for (i = 0; i < 4; i++) {
			rr->rh[i] *= f;
			rr->rv[i] *= f;
		}
	}
}

static inline bool fx_rr_has_radius(const struct fx_rr *rr)
{
	return rr->rh[0] > 0 || rr->rh[1] > 0 || rr->rh[2] > 0 ||
			rr->rh[3] > 0;
}

/**
 * Signed distance from pixel centre (px, py) to the rounded rectangle
 * boundary (negative inside). Elliptical corners are approximated.
 */
static float fx_rr_sdf(const struct fx_rr *rr, float px, float py)
{
	float dx, dy;
	int k;
	float ccx, ccy, rh, rv;
	bool in_corner = false;

	/* select corner by quadrant */
	if (px < (rr->x0 + rr->x1) * 0.5f) {
		k = (py < (rr->y0 + rr->y1) * 0.5f) ? 0 : 3;
	} else {
		k = (py < (rr->y0 + rr->y1) * 0.5f) ? 1 : 2;
	}
	rh = rr->rh[k];
	rv = rr->rv[k];
	if (rh > 0.0f && rv > 0.0f) {
		ccx = (k == 0 || k == 3) ? rr->x0 + rh : rr->x1 - rh;
		ccy = (k == 0 || k == 1) ? rr->y0 + rv : rr->y1 - rv;
		in_corner = ((k == 0 || k == 3) ? px < ccx : px > ccx) &&
				((k == 0 || k == 1) ? py < ccy : py > ccy);
		if (in_corner) {
			float ex = (px - ccx) / rh;
			float ey = (py - ccy) / rv;
			return (sqrtf(ex * ex + ey * ey) - 1.0f) * fminf(rh, rv);
		}
	}

	dx = fmaxf(rr->x0 - px, px - rr->x1);
	dy = fmaxf(rr->y0 - py, py - rr->y1);
	if (dx > 0.0f || dy > 0.0f) {
		float ox = dx > 0.0f ? dx : 0.0f;
		float oy = dy > 0.0f ? dy : 0.0f;
		return sqrtf(ox * ox + oy * oy);
	}
	return fmaxf(dx, dy);
}

/** coverage (0..256) of pixel (x, y) by the rounded rectangle */
static inline int fx_rr_cover(const struct fx_rr *rr, int x, int y)
{
	float d = fx_rr_sdf(rr, x + 0.5f, y + 0.5f);
	if (d <= -0.5f)
		return 256;
	if (d >= 0.5f)
		return 0;
	return (int)((0.5f - d) * 256.0f);
}

/**
 * Is pixel row y / column x inside the straight (non-corner) band where
 * coverage is trivially 0 or 256? Used to skip work.
 */
static inline bool fx_rr_row_simple(const struct fx_rr *rr, int y)
{
	float top = fmaxf(rr->rv[0], rr->rv[1]);
	float bot = fmaxf(rr->rv[2], rr->rv[3]);
	return (y >= rr->y0 + top + 1) && (y + 1 <= rr->y1 - bot - 1);
}

/* exported interface documented in fbfx.h */
nserror fbfx_rounded_fill(const struct redraw_context *ctx,
		const struct rect *outer, const struct plot_radii *oradii,
		const struct rect *inner, const struct plot_radii *iradii,
		colour c)
{
	struct fx_surface s;
	struct fx_rr ro, ri;
	int x, y, x0, y0, x1, y1;
	int r = NS_R(c), g = NS_G(c), b = NS_B(c), a = NS_A(c);
	int alpha = a + (a >> 7); /* 0..256 */

	if (a == 0 || !fx_get_surface(&s))
		return NSERROR_OK;

	fx_rr_init(&ro, outer, oradii);
	if (inner != NULL)
		fx_rr_init(&ri, inner, iradii);

	x0 = outer->x0 > s.cx0 ? outer->x0 : s.cx0;
	y0 = outer->y0 > s.cy0 ? outer->y0 : s.cy0;
	x1 = outer->x1 < s.cx1 ? outer->x1 : s.cx1;
	y1 = outer->y1 < s.cy1 ? outer->y1 : s.cy1;

	for (y = y0; y < y1; y++) {
		uint32_t *row = s.ptr + y * s.stride;
		bool simple = fx_rr_row_simple(&ro, y);
		for (x = x0; x < x1; x++) {
			int cov;
			if (inner != NULL && x >= inner->x0 + 1 &&
					x + 1 < inner->x1 &&
					y >= inner->y0 + 1 && y + 1 < inner->y1) {
				/* possibly deep inside the hole: jump */
				int ci = fx_rr_cover(&ri, x, y);
				if (ci == 256) {
					/* skip to the far side of the hole */
					float rmax = fmaxf(fmaxf(ri.rh[0], ri.rh[1]),
						fmaxf(ri.rh[2], ri.rh[3]));
					int skip_to = inner->x1 - (int)rmax - 2;
					if (skip_to > x)
						x = skip_to;
					continue;
				}
				cov = (simple ? 256 : fx_rr_cover(&ro, x, y)) - ci;
			} else {
				cov = simple && x > ro.x0 + 1 && x + 2 < ro.x1 ?
						256 : fx_rr_cover(&ro, x, y);
				if (inner != NULL)
					cov -= fx_rr_cover(&ri, x, y);
			}
			if (cov > 0)
				fx_blend(&s, row + x, r, g, b,
						(cov * alpha) >> 8);
		}
	}
	return NSERROR_OK;
}

/** interpolate gradient colour at position t */
static void fx_gradient_colour(const struct plot_gradient *g, float t,
		int *r, int *gg, int *b, int *a)
{
	int i;
	colour c0, c1;
	float f;

	if (g->repeating && g->nstops > 1) {
		float first = g->stop_pos[0];
		float period = g->stop_pos[g->nstops - 1] - first;
		if (period > 0.0001f) {
			t = fmodf(t - first, period);
			if (t < 0)
				t += period;
			t += first;
		}
	}

	if (t <= g->stop_pos[0]) {
		c0 = g->stop_colour[0];
		*r = NS_R(c0); *gg = NS_G(c0); *b = NS_B(c0); *a = NS_A(c0);
		return;
	}
	for (i = 1; i < g->nstops; i++) {
		if (t <= g->stop_pos[i])
			break;
	}
	if (i >= g->nstops) {
		c0 = g->stop_colour[g->nstops - 1];
		*r = NS_R(c0); *gg = NS_G(c0); *b = NS_B(c0); *a = NS_A(c0);
		return;
	}
	c0 = g->stop_colour[i - 1];
	c1 = g->stop_colour[i];
	f = g->stop_pos[i] - g->stop_pos[i - 1];
	f = f > 0.00001f ? (t - g->stop_pos[i - 1]) / f : 1.0f;
	*r = NS_R(c0) + (int)((NS_R(c1) - NS_R(c0)) * f);
	*gg = NS_G(c0) + (int)((NS_G(c1) - NS_G(c0)) * f);
	*b = NS_B(c0) + (int)((NS_B(c1) - NS_B(c0)) * f);
	*a = NS_A(c0) + (int)((NS_A(c1) - NS_A(c0)) * f);
}

/* exported interface documented in fbfx.h */
nserror fbfx_gradient(const struct redraw_context *ctx,
		const struct rect *area, const struct plot_gradient *g)
{
	struct fx_surface s;
	int x, y, x0, y0, x1, y1;
	int r, gg, b, a;

	if (g->nstops < 1 || !fx_get_surface(&s))
		return NSERROR_OK;

	x0 = area->x0 > s.cx0 ? area->x0 : s.cx0;
	y0 = area->y0 > s.cy0 ? area->y0 : s.cy0;
	x1 = area->x1 < s.cx1 ? area->x1 : s.cx1;
	y1 = area->y1 < s.cy1 ? area->y1 : s.cy1;

	if (!g->radial && fabsf(g->dy) < 0.0001f) {
		/* horizontal gradient: one colour per column */
		for (x = x0; x < x1; x++) {
			float t = ((x + 0.5f - g->x0) * g->dx) / g->length;
			fx_gradient_colour(g, t, &r, &gg, &b, &a);
			a = a + (a >> 7);
			for (y = y0; y < y1; y++)
				fx_blend(&s, s.ptr + y * s.stride + x,
						r, gg, b, a);
		}
		return NSERROR_OK;
	}

	for (y = y0; y < y1; y++) {
		uint32_t *row = s.ptr + y * s.stride;
		if (!g->radial && fabsf(g->dx) < 0.0001f) {
			/* vertical gradient: one colour per row */
			float t = ((y + 0.5f - g->y0) * g->dy) / g->length;
			fx_gradient_colour(g, t, &r, &gg, &b, &a);
			a = a + (a >> 7);
			for (x = x0; x < x1; x++)
				fx_blend(&s, row + x, r, gg, b, a);
			continue;
		}
		for (x = x0; x < x1; x++) {
			float t;
			if (g->radial) {
				float ex = (x + 0.5f - g->cx) /
						(g->rx > 0.5f ? g->rx : 0.5f);
				float ey = (y + 0.5f - g->cy) /
						(g->ry > 0.5f ? g->ry : 0.5f);
				t = sqrtf(ex * ex + ey * ey);
			} else {
				t = ((x + 0.5f - g->x0) * g->dx +
				     (y + 0.5f - g->y0) * g->dy) / g->length;
			}
			fx_gradient_colour(g, t, &r, &gg, &b, &a);
			fx_blend(&s, row + x, r, gg, b, a + (a >> 7));
		}
	}
	return NSERROR_OK;
}

/** approximation of the Gaussian-blurred edge profile, x in sigmas */
static inline float fx_erfc_half(float x)
{
	/* 0.5 * erfc(x / sqrt(2)), logistic approximation */
	return 1.0f / (1.0f + expf(1.702f * x));
}

/* exported interface documented in fbfx.h */
nserror fbfx_shadow(const struct redraw_context *ctx,
		const struct rect *box, const struct plot_radii *radii,
		const struct plot_shadow *sh)
{
	struct fx_surface s;
	struct fx_rr rb, rs;
	struct rect sr;
	struct plot_radii sradii;
	int x, y, x0, y0, x1, y1, i;
	int r = NS_R(sh->colour), g = NS_G(sh->colour), b = NS_B(sh->colour);
	int a = NS_A(sh->colour);
	float sigma = sh->blur > 0 ? sh->blur / 2.0f : 0.0f;
	int ext = sh->blur + 1;

	if (a == 0 || !fx_get_surface(&s))
		return NSERROR_OK;

	fx_rr_init(&rb, box, radii);

	/* shadow shape: offset box, grown (or shrunk for inset) by spread */
	sr.x0 = box->x0 + sh->offset_x - (sh->inset ? -sh->spread : sh->spread);
	sr.y0 = box->y0 + sh->offset_y - (sh->inset ? -sh->spread : sh->spread);
	sr.x1 = box->x1 + sh->offset_x + (sh->inset ? -sh->spread : sh->spread);
	sr.y1 = box->y1 + sh->offset_y + (sh->inset ? -sh->spread : sh->spread);
	for (i = 0; i < 4; i++) {
		int d = sh->inset ? -sh->spread : sh->spread;
		sradii.h[i] = radii ? radii->h[i] : 0;
		sradii.v[i] = radii ? radii->v[i] : 0;
		if (sradii.h[i] > 0) sradii.h[i] += d;
		if (sradii.v[i] > 0) sradii.v[i] += d;
		if (sradii.h[i] < 0) sradii.h[i] = 0;
		if (sradii.v[i] < 0) sradii.v[i] = 0;
	}
	if (sr.x1 < sr.x0) sr.x1 = sr.x0;
	if (sr.y1 < sr.y0) sr.y1 = sr.y0;
	fx_rr_init(&rs, &sr, &sradii);

	if (sh->inset) {
		x0 = box->x0; y0 = box->y0; x1 = box->x1; y1 = box->y1;
	} else {
		x0 = sr.x0 - ext; y0 = sr.y0 - ext;
		x1 = sr.x1 + ext; y1 = sr.y1 + ext;
	}
	if (x0 < s.cx0) x0 = s.cx0;
	if (y0 < s.cy0) y0 = s.cy0;
	if (x1 > s.cx1) x1 = s.cx1;
	if (y1 > s.cy1) y1 = s.cy1;

	for (y = y0; y < y1; y++) {
		uint32_t *row = s.ptr + y * s.stride;
		for (x = x0; x < x1; x++) {
			float d, v;
			int cb = fx_rr_cover(&rb, x, y);

			if (!sh->inset && cb >= 256) {
				/* shadow is never drawn under the box;
				 * jump across its interior */
				if (fx_rr_row_simple(&rb, y) &&
						x < box->x1 - 2) {
					float rmax = fmaxf(rb.rh[1], rb.rh[2]);
					int skip = box->x1 - (int)rmax - 2;
					if (skip > x)
						x = skip;
				}
				continue;
			}
			if (sh->inset && cb <= 0)
				continue;

			d = fx_rr_sdf(&rs, x + 0.5f, y + 0.5f);
			if (sh->inset)
				d = -d;
			if (sigma > 0.0f)
				v = fx_erfc_half(d / sigma);
			else
				v = d <= -0.5f ? 1.0f : (d >= 0.5f ? 0.0f :
						0.5f - d);
			if (v <= 0.002f)
				continue;
			if (sh->inset)
				v *= cb / 256.0f;
			else
				v *= (256 - cb) / 256.0f;
			fx_blend(&s, row + x, r, g, b, (int)(v * a * 256 / 255));
		}
	}
	return NSERROR_OK;
}

/** a saved layer */
struct fx_layer {
	int x0, y0, x1, y1; /* saved area (clipped to surface) */
	uint32_t *pixels;
};

#define FX_MAX_LAYERS 32
static struct fx_layer fx_layers[FX_MAX_LAYERS];
static int fx_nlayers;

/* exported interface documented in fbfx.h */
nserror fbfx_layer_begin(const struct redraw_context *ctx,
		const struct rect *area)
{
	struct fx_surface s;
	struct fx_layer *l;
	int y, w;

	if (fx_nlayers >= FX_MAX_LAYERS)
		return NSERROR_NOMEM;

	l = &fx_layers[fx_nlayers++];
	l->pixels = NULL;
	l->x0 = l->x1 = l->y0 = l->y1 = 0;

	if (!fx_get_surface(&s))
		return NSERROR_OK;

	l->x0 = area->x0 > 0 ? area->x0 : 0;
	l->y0 = area->y0 > 0 ? area->y0 : 0;
	l->x1 = area->x1 < s.w ? area->x1 : s.w;
	l->y1 = area->y1 < s.h ? area->y1 : s.h;
	/* nothing outside the clip can change, so no need to save it */
	if (l->x0 < s.cx0) l->x0 = s.cx0;
	if (l->y0 < s.cy0) l->y0 = s.cy0;
	if (l->x1 > s.cx1) l->x1 = s.cx1;
	if (l->y1 > s.cy1) l->y1 = s.cy1;
	if (l->x0 >= l->x1 || l->y0 >= l->y1) {
		l->x1 = l->x0;
		return NSERROR_OK;
	}

	w = l->x1 - l->x0;
	l->pixels = malloc((size_t)w * (l->y1 - l->y0) * sizeof(uint32_t));
	if (l->pixels == NULL) {
		l->x1 = l->x0;
		return NSERROR_OK;
	}
	for (y = l->y0; y < l->y1; y++) {
		memcpy(l->pixels + (size_t)(y - l->y0) * w,
				s.ptr + y * s.stride + l->x0,
				w * sizeof(uint32_t));
	}
	return NSERROR_OK;
}

/** apply CSS filter functions to a pixel */
static inline void fx_filter(const struct plot_layer *p, int *r, int *g, int *b)
{
	float fr = *r, fg = *g, fb = *b;

	if (p->grayscale > 0.0f) {
		float lum = 0.2126f * fr + 0.7152f * fg + 0.0722f * fb;
		fr += (lum - fr) * p->grayscale;
		fg += (lum - fg) * p->grayscale;
		fb += (lum - fb) * p->grayscale;
	}
	if (p->sepia > 0.0f) {
		float sr = 0.393f * fr + 0.769f * fg + 0.189f * fb;
		float sg = 0.349f * fr + 0.686f * fg + 0.168f * fb;
		float sb = 0.272f * fr + 0.534f * fg + 0.131f * fb;
		fr += (sr - fr) * p->sepia;
		fg += (sg - fg) * p->sepia;
		fb += (sb - fb) * p->sepia;
	}
	if (p->invert > 0.0f) {
		fr += (255.0f - 2.0f * fr) * p->invert;
		fg += (255.0f - 2.0f * fg) * p->invert;
		fb += (255.0f - 2.0f * fb) * p->invert;
	}
	if (p->brightness != 1.0f) {
		fr *= p->brightness;
		fg *= p->brightness;
		fb *= p->brightness;
	}
	*r = fr < 0 ? 0 : (fr > 255 ? 255 : (int)fr);
	*g = fg < 0 ? 0 : (fg > 255 ? 255 : (int)fg);
	*b = fb < 0 ? 0 : (fb > 255 ? 255 : (int)fb);
}

/* exported interface documented in fbfx.h */
nserror fbfx_layer_end(const struct redraw_context *ctx,
		const struct plot_layer *p)
{
	struct fx_surface s;
	struct fx_layer *l;
	struct fx_rr rr;
	int x, y, w;
	bool rounded, filter;
	int opacity;

	if (fx_nlayers == 0)
		return NSERROR_INVALID;
	l = &fx_layers[--fx_nlayers];
	if (l->pixels == NULL)
		return NSERROR_OK;

	if (!fx_get_surface(&s)) {
		free(l->pixels);
		return NSERROR_OK;
	}

	fx_rr_init(&rr, &p->box, &p->radii);
	rounded = fx_rr_has_radius(&rr);
	filter = p->grayscale > 0.0f || p->sepia > 0.0f ||
			p->invert > 0.0f || p->brightness != 1.0f;
	opacity = (int)(p->opacity * 256.0f);
	if (opacity < 0) opacity = 0;
	if (opacity > 256) opacity = 256;
	w = l->x1 - l->x0;

	for (y = l->y0; y < l->y1; y++) {
		uint32_t *row = s.ptr + y * s.stride;
		uint32_t *saved = l->pixels + (size_t)(y - l->y0) * w - l->x0;
		bool simple_row = !rounded || fx_rr_row_simple(&rr, y);

		if (simple_row && opacity == 256 && !filter) {
			/* only the parts outside the box change */
			int bx0 = p->box.x0 > l->x0 ? p->box.x0 : l->x0;
			int bx1 = p->box.x1 < l->x1 ? p->box.x1 : l->x1;
			if (y < p->box.y0 || y >= p->box.y1) {
				bx0 = bx1 = l->x0;
			}
			for (x = l->x0; x < bx0; x++)
				row[x] = saved[x];
			for (x = bx1 > l->x0 ? bx1 : l->x0; x < l->x1; x++)
				row[x] = saved[x];
			continue;
		}

		for (x = l->x0; x < l->x1; x++) {
			int cov;
			int r, g, b, sr, sg, sb;

			if (rounded && !simple_row) {
				cov = fx_rr_cover(&rr, x, y);
			} else {
				cov = (x >= p->box.x0 && x < p->box.x1 &&
				       y >= p->box.y0 && y < p->box.y1) ?
						256 : 0;
			}
			cov = (cov * opacity) >> 8;
			if (cov >= 256 && !filter)
				continue;
			/* pixels the layer did not paint are backdrop */
			if (cov > 0 && row[x] == saved[x])
				continue;
			if (cov <= 0) {
				row[x] = saved[x];
				continue;
			}
			fx_unpack(&s, row[x], &r, &g, &b);
			if (filter)
				fx_filter(p, &r, &g, &b);
			if (cov >= 256) {
				row[x] = fx_pack(&s, r, g, b);
				continue;
			}
			fx_unpack(&s, saved[x], &sr, &sg, &sb);
			sr += ((r - sr) * cov) >> 8;
			sg += ((g - sg) * cov) >> 8;
			sb += ((b - sb) * cov) >> 8;
			row[x] = fx_pack(&s, sr, sg, sb);
		}
	}

	free(l->pixels);
	l->pixels = NULL;
	return NSERROR_OK;
}

static bool fx_tint_on;
static colour fx_tint_colour;

/* ------------------------------------------------------------------ */
/* Anti-aliased path rendering (SVG)                                  */
/* ------------------------------------------------------------------ */

/**
 * Coverage accumulation raster: each edge adds its signed area
 * contribution to the cells it crosses; a running sum along each row
 * then yields the winding coverage (as in font-rs).
 */
struct fx_raster {
	float *a;
	int x0, y0; /* device origin of the raster */
	int w, h;   /* area in pixels; rows are w + 3 wide */
};

static void fx_raster_line(struct fx_raster *r, float px0, float py0,
		float px1, float py1)
{
	float dir, dxdy, x, fx0, fy0, fx1, fy1;
	int y, ystart, yend, stride = r->w + 3;

	fx0 = px0 - r->x0; fy0 = py0 - r->y0;
	fx1 = px1 - r->x0; fy1 = py1 - r->y0;
	if (fabsf(fy0 - fy1) <= 1e-6f)
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
		float *row = r->a + y * stride;
		float dy = fminf((float)(y + 1), fy1) - fmaxf((float)y, fy0);
		float xnext = x + dxdy * dy;
		float d = dy * dir;
		float xa = x < xnext ? x : xnext;
		float xb = x < xnext ? xnext : x;
		float xa_c, xb_c, xaf;
		int xai, xbi;

		/* clamp horizontally: area left of the raster still
		 * accumulates into column 0 */
		xa_c = fminf(fmaxf(xa, 0.0f), (float)r->w + 1);
		xb_c = fminf(fmaxf(xb, 0.0f), (float)r->w + 1);
		xaf = floorf(xa_c);
		xai = (int)xaf;
		xbi = (int)ceilf(xb_c);

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
				float a1 = s * (1.5f - x0f);
				float a2;
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

/** composite accumulated coverage onto the surface */
static void fx_raster_fill(struct fx_raster *r, const struct fx_surface *s,
		colour c)
{
	int x, y, stride = r->w + 3;
	int cr = NS_R(c), cg = NS_G(c), cb = NS_B(c), ca = NS_A(c);
	int alpha = ca + (ca >> 7);

	for (y = 0; y < r->h; y++) {
		float acc = 0;
		float *row = r->a + y * stride;
		int dy = r->y0 + y;
		uint32_t *px;

		if (dy < s->cy0 || dy >= s->cy1)
			continue;
		px = s->ptr + dy * s->stride;
		for (x = 0; x < r->w; x++) {
			int dx = r->x0 + x, cov;
			float v;
			acc += row[x];
			v = fabsf(acc);
			if (v < 0.002f || dx < s->cx0 || dx >= s->cx1)
				continue;
			cov = v >= 1.0f ? 256 : (int)(v * 256.0f);
			fx_blend(s, px + dx, cr, cg, cb, (cov * alpha) >> 8);
		}
	}
}

/** a flattened path: subpaths of points */
struct fx_poly {
	float *pt;     /* x,y pairs */
	int *start;    /* subpath start indices (in points) */
	bool *closed;  /* per subpath */
	int npt, cap;
	int nsub, subcap;
};

static bool fx_poly_point(struct fx_poly *p, float x, float y)
{
	if (p->npt == p->cap) {
		int cap = p->cap ? p->cap * 2 : 256;
		float *n = realloc(p->pt, cap * 2 * sizeof(float));
		if (n == NULL)
			return false;
		p->pt = n;
		p->cap = cap;
	}
	p->pt[p->npt * 2] = x;
	p->pt[p->npt * 2 + 1] = y;
	p->npt++;
	return true;
}

static bool fx_poly_sub(struct fx_poly *p)
{
	if (p->nsub == p->subcap) {
		int cap = p->subcap ? p->subcap * 2 : 16;
		int *ns = realloc(p->start, cap * sizeof(int));
		bool *nc;
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
	return true;
}

static void fx_poly_free(struct fx_poly *p)
{
	free(p->pt);
	free(p->start);
	free(p->closed);
}

#define FX_TX(t, x, y) ((t)[0] * (x) + (t)[2] * (y) + (t)[4])
#define FX_TY(t, x, y) ((t)[1] * (x) + (t)[3] * (y) + (t)[5])

/** flatten path commands into device-space polylines */
static bool fx_flatten(const float *p, unsigned int n, const float t[6],
		struct fx_poly *out)
{
	unsigned int i = 0;
	float cx = 0, cy = 0, sx = 0, sy = 0;
	bool open = false;

	while (i < n) {
		int cmd = (int)p[i];
		switch (cmd) {
		case PLOTTER_PATH_MOVE:
			if (i + 2 >= n)
				return true;
			cx = sx = FX_TX(t, p[i + 1], p[i + 2]);
			cy = sy = FX_TY(t, p[i + 1], p[i + 2]);
			if (!fx_poly_sub(out) || !fx_poly_point(out, cx, cy))
				return false;
			open = true;
			i += 3;
			break;
		case PLOTTER_PATH_LINE:
			if (i + 2 >= n)
				return true;
			if (!open) {
				if (!fx_poly_sub(out) ||
				    !fx_poly_point(out, cx, cy))
					return false;
				open = true;
			}
			cx = FX_TX(t, p[i + 1], p[i + 2]);
			cy = FX_TY(t, p[i + 1], p[i + 2]);
			if (!fx_poly_point(out, cx, cy))
				return false;
			i += 3;
			break;
		case PLOTTER_PATH_BEZIER: {
			float x1, y1, x2, y2, x3, y3, len;
			int k, steps;
			if (i + 6 >= n)
				return true;
			if (!open) {
				if (!fx_poly_sub(out) ||
				    !fx_poly_point(out, cx, cy))
					return false;
				open = true;
			}
			x1 = FX_TX(t, p[i + 1], p[i + 2]);
			y1 = FX_TY(t, p[i + 1], p[i + 2]);
			x2 = FX_TX(t, p[i + 3], p[i + 4]);
			y2 = FX_TY(t, p[i + 3], p[i + 4]);
			x3 = FX_TX(t, p[i + 5], p[i + 6]);
			y3 = FX_TY(t, p[i + 5], p[i + 6]);
			/* segment count from control polygon length */
			len = hypotf(x1 - cx, y1 - cy) + hypotf(x2 - x1, y2 - y1) +
				hypotf(x3 - x2, y3 - y2);
			steps = (int)(sqrtf(len) * 1.5f) + 2;
			if (steps > 64)
				steps = 64;
			for (k = 1; k <= steps; k++) {
				float u = (float)k / steps, v = 1 - u;
				float bx = v * v * v * cx + 3 * v * v * u * x1 +
					3 * v * u * u * x2 + u * u * u * x3;
				float by = v * v * v * cy + 3 * v * v * u * y1 +
					3 * v * u * u * y2 + u * u * u * y3;
				if (!fx_poly_point(out, bx, by))
					return false;
			}
			cx = x3;
			cy = y3;
			i += 7;
			break;
		}
		case PLOTTER_PATH_CLOSE:
			if (open && out->nsub > 0) {
				out->closed[out->nsub - 1] = true;
				open = false;
			}
			cx = sx;
			cy = sy;
			i += 1;
			break;
		default:
			return true;
		}
	}
	return true;
}

/** add a polygon with consistent (positive area) orientation */
static void fx_raster_poly(struct fx_raster *r, const float *pt, int n)
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
			fx_raster_line(r, pt[k * 2], pt[k * 2 + 1],
					pt[j * 2], pt[j * 2 + 1]);
		else
			fx_raster_line(r, pt[j * 2], pt[j * 2 + 1],
					pt[k * 2], pt[k * 2 + 1]);
	}
}

static void fx_raster_disc(struct fx_raster *r, float cx, float cy, float rad)
{
	float pt[32];
	int k, n = rad < 2 ? 8 : 16;

	for (k = 0; k < n; k++) {
		float a = (float)k * 6.2831853f / n;
		pt[k * 2] = cx + rad * cosf(a);
		pt[k * 2 + 1] = cy + rad * sinf(a);
	}
	fx_raster_poly(r, pt, n);
}

/** stroke outline: a quad per segment plus round joins */
static void fx_raster_stroke(struct fx_raster *r, const struct fx_poly *p,
		float width)
{
	float hw = width * 0.5f;
	int s;

	for (s = 0; s < p->nsub; s++) {
		int a = p->start[s];
		int b = (s + 1 < p->nsub) ? p->start[s + 1] : p->npt;
		int k, last = b - 1;

		for (k = a; k < b; k++) {
			float x0 = p->pt[k * 2], y0 = p->pt[k * 2 + 1];
			float x1, y1, dx, dy, l, q[8];
			int j = k + 1;

			if (j > last) {
				if (!p->closed[s] || b - a < 3)
					break;
				j = a;
			}
			x1 = p->pt[j * 2];
			y1 = p->pt[j * 2 + 1];
			dx = x1 - x0;
			dy = y1 - y0;
			l = hypotf(dx, dy);
			if (l < 1e-4f)
				continue;
			dx = dx / l * hw;
			dy = dy / l * hw;
			q[0] = x0 - dy; q[1] = y0 + dx;
			q[2] = x1 - dy; q[3] = y1 + dx;
			q[4] = x1 + dy; q[5] = y1 - dx;
			q[6] = x0 + dy; q[7] = y0 - dx;
			fx_raster_poly(r, q, 4);
		}
		/* joins (and round caps) where segments meet */
		if (hw >= 0.75f) {
			for (k = a; k < b; k++) {
				bool end = (k == a || k == last);
				if (end && p->closed[s])
					end = false;
				fx_raster_disc(r, p->pt[k * 2], p->pt[k * 2 + 1],
						end ? hw * 0.98f : hw);
			}
		}
	}
}

/* exported interface documented in fbfx.h */
nserror fbfx_path(const struct redraw_context *ctx,
		const plot_style_t *pstyle, const float *p, unsigned int n,
		const float transform[6])
{
	struct fx_surface s;
	struct fx_poly poly;
	struct fx_raster r;
	float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
	float stroke_w = 0, pad;
	bool fill, stroke;
	colour fill_c, stroke_c;
	int k, x0, y0, x1, y1;

	fill = pstyle->fill_type != PLOT_OP_TYPE_NONE &&
			NS_A(pstyle->fill_colour) > 0;
	stroke = pstyle->stroke_type != PLOT_OP_TYPE_NONE &&
			NS_A(pstyle->stroke_colour) > 0;
	fill_c = fx_tint_on ? fx_tint_colour : pstyle->fill_colour;
	stroke_c = fx_tint_on ? fx_tint_colour : pstyle->stroke_colour;
	if (stroke) {
		float sc = sqrtf(fabsf(transform[0] * transform[3] -
				transform[1] * transform[2]));
		stroke_w = plot_style_fixed_to_float(pstyle->stroke_width) * sc;
		if (stroke_w <= 0)
			stroke_w = sc > 0 ? sc : 1;
		/* hairlines keep a visible weight */
		if (stroke_w < 1)
			stroke_w = 1;
	}
	if ((!fill && !stroke) || n == 0 || !fx_get_surface(&s))
		return NSERROR_OK;

	memset(&poly, 0, sizeof(poly));
	if (!fx_flatten(p, n, transform, &poly) || poly.npt == 0) {
		fx_poly_free(&poly);
		return NSERROR_OK;
	}

	for (k = 0; k < poly.npt; k++) {
		minx = fminf(minx, poly.pt[k * 2]);
		maxx = fmaxf(maxx, poly.pt[k * 2]);
		miny = fminf(miny, poly.pt[k * 2 + 1]);
		maxy = fmaxf(maxy, poly.pt[k * 2 + 1]);
	}
	pad = stroke ? stroke_w * 0.5f + 1 : 1;
	x0 = (int)floorf(minx - pad);
	y0 = (int)floorf(miny - pad);
	x1 = (int)ceilf(maxx + pad);
	y1 = (int)ceilf(maxy + pad);
	if (x0 < s.cx0) x0 = s.cx0;
	if (y0 < s.cy0) y0 = s.cy0;
	if (x1 > s.cx1) x1 = s.cx1;
	if (y1 > s.cy1) y1 = s.cy1;
	if (x0 >= x1 || y0 >= y1) {
		fx_poly_free(&poly);
		return NSERROR_OK;
	}

	r.x0 = x0;
	r.y0 = y0;
	r.w = x1 - x0;
	r.h = y1 - y0;
	r.a = calloc((size_t)(r.w + 3) * r.h, sizeof(float));
	if (r.a == NULL) {
		fx_poly_free(&poly);
		return NSERROR_OK;
	}

	if (fill) {
		int sub;
		for (sub = 0; sub < poly.nsub; sub++) {
			int a = poly.start[sub];
			int b = sub + 1 < poly.nsub ? poly.start[sub + 1] :
					poly.npt;
			/* fills are implicitly closed */
			for (k = a; k < b; k++) {
				int j = k + 1 < b ? k + 1 : a;
				fx_raster_line(&r, poly.pt[k * 2],
						poly.pt[k * 2 + 1],
						poly.pt[j * 2], poly.pt[j * 2 + 1]);
			}
		}
		fx_raster_fill(&r, &s, fill_c);
	}

	if (stroke) {
		if (fill)
			memset(r.a, 0, (size_t)(r.w + 3) * r.h * sizeof(float));
		fx_raster_stroke(&r, &poly, stroke_w);
		fx_raster_fill(&r, &s, stroke_c);
	}

	free(r.a);
	fx_poly_free(&poly);
	return NSERROR_OK;
}

/* ------------------------------------------------------------------ */
/* Tint mode (mask-image)                                             */
/* ------------------------------------------------------------------ */

/* exported interface documented in fbfx.h */
nserror fbfx_tint(const struct redraw_context *ctx, bool enable, colour c)
{
	fx_tint_on = enable;
	fx_tint_colour = c;
	return NSERROR_OK;
}

/* exported interface documented in fbfx.h */
bool fbfx_tint_bitmap(struct nsfb_s *bm, int x, int y, int width, int height)
{
	struct fx_surface s;
	int bw, bh, bstride, dx, dy;
	enum nsfb_format_e fmt;
	uint8_t *bptr;
	int r = NS_R(fx_tint_colour), g = NS_G(fx_tint_colour);
	int b = NS_B(fx_tint_colour), ta = NS_A(fx_tint_colour);
	int x0, y0, x1, y1;

	if (!fx_tint_on)
		return false;
	if (width <= 0 || height <= 0 || !fx_get_surface(&s))
		return true;
	if (nsfb_get_geometry(bm, &bw, &bh, &fmt) != 0 ||
	    nsfb_get_buffer(bm, &bptr, &bstride) != 0 || bptr == NULL ||
	    bw <= 0 || bh <= 0)
		return true;

	x0 = x > s.cx0 ? x : s.cx0;
	y0 = y > s.cy0 ? y : s.cy0;
	x1 = x + width < s.cx1 ? x + width : s.cx1;
	y1 = y + height < s.cy1 ? y + height : s.cy1;
	ta = ta + (ta >> 7);

	for (dy = y0; dy < y1; dy++) {
		int sy = (dy - y) * bh / height;
		const uint32_t *srow = (const uint32_t *)(bptr + sy * bstride);
		uint32_t *drow = s.ptr + dy * s.stride;
		for (dx = x0; dx < x1; dx++) {
			int sx = (dx - x) * bw / width;
			int a = (srow[sx] >> 24) & 0xff;
			if (fmt == NSFB_FMT_XBGR8888 || fmt == NSFB_FMT_XRGB8888)
				a = 255;
			a = a + (a >> 7);
			fx_blend(&s, drow + dx, r, g, b, (a * ta) >> 8);
		}
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Video frame blit                                                   */
/* ------------------------------------------------------------------ */

/* exported interface documented in fbfx.h */
void fbfx_blit_rgbx(const struct redraw_context *ctx, const uint32_t *px,
		int w, int h, const struct rect *dst, const struct rect *clip)
{
	struct fx_surface s;
	int dw = dst->x1 - dst->x0, dh = dst->y1 - dst->y0;
	int x0, y0, x1, y1, x, y;
	int *xmap;

	if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0 || !fx_get_surface(&s))
		return;
	x0 = dst->x0 > s.cx0 ? dst->x0 : s.cx0;
	y0 = dst->y0 > s.cy0 ? dst->y0 : s.cy0;
	x1 = dst->x1 < s.cx1 ? dst->x1 : s.cx1;
	y1 = dst->y1 < s.cy1 ? dst->y1 : s.cy1;
	if (clip != NULL) {
		if (clip->x0 > x0) x0 = clip->x0;
		if (clip->y0 > y0) y0 = clip->y0;
		if (clip->x1 < x1) x1 = clip->x1;
		if (clip->y1 < y1) y1 = clip->y1;
	}
	if (x0 >= x1 || y0 >= y1)
		return;

	xmap = malloc((x1 - x0) * sizeof(int));
	if (xmap == NULL)
		return;
	for (x = x0; x < x1; x++)
		xmap[x - x0] = (int)((int64_t)(x - dst->x0) * w / dw);

	for (y = y0; y < y1; y++) {
		const uint32_t *srow = px + (size_t)((int64_t)(y - dst->y0) *
				h / dh) * w;
		uint32_t *drow = s.ptr + y * s.stride + x0;
		int n = x1 - x0;
		if (s.bgr) {
			/* source bytes R,G,B,X match 0xXXBBGGRR */
			if (w == dw) {
				memcpy(drow, srow + xmap[0], n * 4);
			} else {
				for (x = 0; x < n; x++)
					drow[x] = srow[xmap[x]] | 0xff000000u;
			}
		} else {
			for (x = 0; x < n; x++) {
				uint32_t p = srow[xmap[x]];
				drow[x] = 0xff000000u | ((p & 0xff) << 16) |
					(p & 0xff00) | ((p >> 16) & 0xff);
			}
		}
	}
	free(xmap);
}
