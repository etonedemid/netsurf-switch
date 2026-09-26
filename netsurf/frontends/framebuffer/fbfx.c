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
