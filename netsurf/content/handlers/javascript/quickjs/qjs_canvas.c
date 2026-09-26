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
 * CanvasRenderingContext2D for the QuickJS backend.
 *
 * A native class whose methods call the software canvas in
 * html/canvas.c. Each context object references its <canvas> node; the
 * drawing state lives with the canvas. Gradients, ImageData and a few
 * conveniences are completed in runtime.js.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dom/dom.h>

#include "utils/config.h"
#include "utils/log.h"
#include "html/private.h"
#include "html/canvas.h"

#include "javascript/quickjs/qjs_private.h"

static JSClassID ctx2d_class;

struct qjs_ctx2d {
	dom_node *node;
};

static void ctx2d_finalizer(JSRuntime *rt, JSValue val)
{
	struct qjs_ctx2d *c = JS_GetOpaque(val, ctx2d_class);

	(void)rt;
	if (c != NULL) {
		dom_node_unref(c->node);
		free(c);
	}
}

static JSClassDef ctx2d_def = {
	.class_name = "CanvasRenderingContext2D",
	.finalizer = ctx2d_finalizer,
};

/** the canvas behind a context object */
static struct html_canvas *cv_of(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	struct qjs_ctx2d *c = JS_GetOpaque(this_val, ctx2d_class);

	if (t == NULL || c == NULL || t->closed || t->htmlc == NULL)
		return NULL;
	return html_canvas_for_node(t->htmlc, c->node, true);
}

static double arg_num(JSContext *ctx, int argc, JSValueConst *argv, int i,
		double dflt)
{
	double d;

	if (i >= argc || JS_ToFloat64(ctx, &d, argv[i]) != 0)
		return dflt;
	return d;
}

#define CV_BEGIN							\
	struct html_canvas *cv = cv_of(ctx, this_val);			\
	if (cv == NULL)							\
		return JS_UNDEFINED;

#define A(i) arg_num(ctx, argc, argv, (i), 0)

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static JSValue c_save(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	CV_BEGIN
	html_canvas_save(cv);
	return JS_UNDEFINED;
}

static JSValue c_restore(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	CV_BEGIN
	html_canvas_restore(cv);
	return JS_UNDEFINED;
}

static JSValue c_reset(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	int w, h;
	CV_BEGIN
	html_canvas_get_size(cv, &w, &h);
	html_canvas_set_size(cv, w, h);
	return JS_UNDEFINED;
}

static void mat_mul(double m[6], const double n[6])
{
	double r[6];

	r[0] = m[0] * n[0] + m[2] * n[1];
	r[1] = m[1] * n[0] + m[3] * n[1];
	r[2] = m[0] * n[2] + m[2] * n[3];
	r[3] = m[1] * n[2] + m[3] * n[3];
	r[4] = m[0] * n[4] + m[2] * n[5] + m[4];
	r[5] = m[1] * n[4] + m[3] * n[5] + m[5];
	memcpy(m, r, sizeof(r));
}

static JSValue c_transform(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	double n[6];
	int i;
	CV_BEGIN
	for (i = 0; i < 6; i++)
		n[i] = A(i);
	for (i = 0; i < 6; i++)
		if (!isfinite(n[i]))
			return JS_UNDEFINED;
	mat_mul(html_canvas_state(cv)->m, n);
	return JS_UNDEFINED;
}

static JSValue c_set_transform(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	double *m;
	int i;
	CV_BEGIN
	m = html_canvas_state(cv)->m;
	if (argc == 0) {
		m[0] = m[3] = 1;
		m[1] = m[2] = m[4] = m[5] = 0;
		return JS_UNDEFINED;
	}
	if (argc == 1 && JS_IsObject(argv[0])) {
		/* DOMMatrix-like */
		static const char *k[6] = { "a", "b", "c", "d", "e", "f" };
		for (i = 0; i < 6; i++) {
			JSValue v = JS_GetPropertyStr(ctx, argv[0], k[i]);
			double d = i == 0 || i == 3 ? 1 : 0;
			JS_ToFloat64(ctx, &d, v);
			JS_FreeValue(ctx, v);
			m[i] = d;
		}
		return JS_UNDEFINED;
	}
	for (i = 0; i < 6; i++)
		m[i] = A(i);
	return JS_UNDEFINED;
}

static JSValue c_translate(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	double n[6] = { 1, 0, 0, 1, 0, 0 };
	CV_BEGIN
	n[4] = A(0);
	n[5] = A(1);
	if (isfinite(n[4]) && isfinite(n[5]))
		mat_mul(html_canvas_state(cv)->m, n);
	return JS_UNDEFINED;
}

static JSValue c_scale(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	double n[6] = { 1, 0, 0, 1, 0, 0 };
	CV_BEGIN
	n[0] = A(0);
	n[3] = arg_num(ctx, argc, argv, 1, n[0]);
	if (isfinite(n[0]) && isfinite(n[3]))
		mat_mul(html_canvas_state(cv)->m, n);
	return JS_UNDEFINED;
}

static JSValue c_rotate(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	double a, n[6] = { 1, 0, 0, 1, 0, 0 };
	CV_BEGIN
	a = A(0);
	if (!isfinite(a))
		return JS_UNDEFINED;
	n[0] = cos(a);
	n[1] = sin(a);
	n[2] = -sin(a);
	n[3] = cos(a);
	mat_mul(html_canvas_state(cv)->m, n);
	return JS_UNDEFINED;
}

static JSValue c_get_transform(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	static const char *k[6] = { "a", "b", "c", "d", "e", "f" };
	JSValue o;
	double *m;
	int i;
	CV_BEGIN
	m = html_canvas_state(cv)->m;
	o = JS_NewObject(ctx);
	for (i = 0; i < 6; i++)
		JS_SetPropertyStr(ctx, o, k[i], JS_NewFloat64(ctx, m[i]));
	return o;
}

/* ------------------------------------------------------------------ */
/* Paint styles                                                       */
/* ------------------------------------------------------------------ */

/** build a paint from a colour string or CanvasGradient object */
static bool paint_from_js(JSContext *ctx, JSValueConst v,
		struct canvas_paint *p)
{
	if (JS_IsString(v)) {
		const char *s = JS_ToCString(ctx, v);
		uint32_t c;
		bool ok = s != NULL && html_canvas_parse_colour(s, &c);
		if (s != NULL)
			JS_FreeCString(ctx, s);
		if (!ok)
			return false;
		p->type = CANVAS_PAINT_COLOUR;
		p->colour = c;
		p->nstops = 0;
		return true;
	}
	if (JS_IsObject(v)) {
		JSValue t = JS_GetPropertyStr(ctx, v, "_t");
		JSValue a = JS_GetPropertyStr(ctx, v, "_a");
		JSValue st = JS_GetPropertyStr(ctx, v, "_s");
		const char *ts = JS_ToCString(ctx, t);
		double c[6] = { 0, 0, 0, 0, 0, 0 };
		int64_t n = 0, i;
		bool ok = false;

		if (ts != NULL && (strcmp(ts, "linear") == 0 ||
				strcmp(ts, "radial") == 0) &&
		    JS_IsArray(a) && JS_IsArray(st)) {
			for (i = 0; i < 6; i++) {
				JSValue e = JS_GetPropertyUint32(ctx, a, i);
				JS_ToFloat64(ctx, &c[i], e);
				JS_FreeValue(ctx, e);
			}
			p->type = strcmp(ts, "linear") == 0 ?
					CANVAS_PAINT_LINEAR : CANVAS_PAINT_RADIAL;
			if (p->type == CANVAS_PAINT_LINEAR) {
				p->x0 = c[0]; p->y0 = c[1];
				p->x1 = c[2]; p->y1 = c[3];
			} else {
				p->x0 = c[0]; p->y0 = c[1]; p->r0 = c[2];
				p->x1 = c[3]; p->y1 = c[4]; p->r1 = c[5];
			}
			JS_GetLength(ctx, st, &n);
			p->nstops = 0;
			for (i = 0; i < n && p->nstops < CANVAS_MAX_STOPS; i++) {
				JSValue e = JS_GetPropertyUint32(ctx, st, i);
				JSValue pos = JS_GetPropertyUint32(ctx, e, 0);
				JSValue col = JS_GetPropertyUint32(ctx, e, 1);
				double pd = 0;
				const char *cs = JS_ToCString(ctx, col);
				uint32_t cc;
				JS_ToFloat64(ctx, &pd, pos);
				if (cs != NULL && html_canvas_parse_colour(cs,
						&cc)) {
					p->stop_pos[p->nstops] = (float)pd;
					p->stop_colour[p->nstops] = cc;
					p->nstops++;
				}
				if (cs != NULL)
					JS_FreeCString(ctx, cs);
				JS_FreeValue(ctx, pos);
				JS_FreeValue(ctx, col);
				JS_FreeValue(ctx, e);
			}
			if (p->nstops == 0) {
				p->type = CANVAS_PAINT_COLOUR;
				p->colour = 0;
			}
			ok = true;
		} else if (ts != NULL && strcmp(ts, "pattern") == 0) {
			/* patterns: use their average colour */
			JSValue col = JS_GetPropertyStr(ctx, v, "_c");
			const char *cs = JS_ToCString(ctx, col);
			uint32_t cc;
			if (cs != NULL && html_canvas_parse_colour(cs, &cc)) {
				p->type = CANVAS_PAINT_COLOUR;
				p->colour = cc;
				ok = true;
			}
			if (cs != NULL)
				JS_FreeCString(ctx, cs);
			JS_FreeValue(ctx, col);
		}
		if (ts != NULL)
			JS_FreeCString(ctx, ts);
		JS_FreeValue(ctx, t);
		JS_FreeValue(ctx, a);
		JS_FreeValue(ctx, st);
		return ok;
	}
	return false;
}

static JSValue get_style(JSContext *ctx, JSValueConst this_val,
		const char *key)
{
	JSValue v = JS_GetPropertyStr(ctx, this_val, key);

	if (JS_IsUndefined(v))
		return JS_NewString(ctx, "#000000");
	return v;
}

static JSValue c_get_fill(JSContext *ctx, JSValueConst this_val)
{
	return get_style(ctx, this_val, "__fill");
}

static JSValue c_get_stroke(JSContext *ctx, JSValueConst this_val)
{
	return get_style(ctx, this_val, "__stroke");
}

static JSValue set_style(JSContext *ctx, JSValueConst this_val,
		JSValueConst v, bool fill)
{
	struct canvas_paint p;
	CV_BEGIN
	memset(&p, 0, sizeof(p));
	if (!paint_from_js(ctx, v, &p))
		return JS_UNDEFINED;   /* invalid values are ignored */
	if (fill)
		html_canvas_state(cv)->fill = p;
	else
		html_canvas_state(cv)->stroke = p;
	JS_SetPropertyStr(ctx, this_val, fill ? "__fill" : "__stroke",
			JS_DupValue(ctx, v));
	return JS_UNDEFINED;
}

static JSValue c_set_fill(JSContext *ctx, JSValueConst this_val,
		JSValueConst v)
{
	return set_style(ctx, this_val, v, true);
}

static JSValue c_set_stroke(JSContext *ctx, JSValueConst this_val,
		JSValueConst v)
{
	return set_style(ctx, this_val, v, false);
}

#define NUM_PROP(gname, sname, field, check)				\
static JSValue gname(JSContext *ctx, JSValueConst this_val)		\
{									\
	CV_BEGIN							\
	return JS_NewFloat64(ctx, html_canvas_state(cv)->field);	\
}									\
static JSValue sname(JSContext *ctx, JSValueConst this_val,		\
		JSValueConst v)						\
{									\
	double d;							\
	CV_BEGIN							\
	if (JS_ToFloat64(ctx, &d, v) == 0 && isfinite(d) && (check))	\
		html_canvas_state(cv)->field = d;			\
	return JS_UNDEFINED;						\
}

NUM_PROP(c_get_lw, c_set_lw, line_width, d > 0)
NUM_PROP(c_get_ga, c_set_ga, global_alpha, d >= 0 && d <= 1)

static int keyword(const char *s, const char *const *words)
{
	int i;

	for (i = 0; words[i] != NULL; i++)
		if (strcasecmp(s, words[i]) == 0)
			return i;
	return -1;
}

#define ENUM_PROP(gname, sname, field, words, ...)			\
static const char *const words[] = { __VA_ARGS__, NULL };		\
static JSValue gname(JSContext *ctx, JSValueConst this_val)		\
{									\
	CV_BEGIN							\
	return JS_NewString(ctx, words[html_canvas_state(cv)->field]);	\
}									\
static JSValue sname(JSContext *ctx, JSValueConst this_val,		\
		JSValueConst v)						\
{									\
	const char *s = JS_ToCString(ctx, v);				\
	int k;								\
	struct html_canvas *cv = cv_of(ctx, this_val);			\
	if (s == NULL)							\
		return JS_UNDEFINED;					\
	k = keyword(s, words);						\
	JS_FreeCString(ctx, s);						\
	if (cv != NULL && k >= 0)					\
		html_canvas_state(cv)->field = k;			\
	return JS_UNDEFINED;						\
}

ENUM_PROP(c_get_cap, c_set_cap, line_cap, cap_words,
		"butt", "round", "square")
ENUM_PROP(c_get_join, c_set_join, line_join, join_words,
		"miter", "round", "bevel")
ENUM_PROP(c_get_base, c_set_base, text_baseline, base_words,
		"alphabetic", "top", "middle", "bottom")

static const char *const align_words[] = {
	"start", "center", "end", "left", "right", NULL
};

static JSValue c_get_align(JSContext *ctx, JSValueConst this_val)
{
	JSValue v = JS_GetPropertyStr(ctx, this_val, "__align");
	if (JS_IsUndefined(v))
		return JS_NewString(ctx, "start");
	return v;
}

static JSValue c_set_align(JSContext *ctx, JSValueConst this_val,
		JSValueConst v)
{
	const char *s = JS_ToCString(ctx, v);
	int k;
	CV_BEGIN
	if (s == NULL)
		return JS_UNDEFINED;
	k = keyword(s, align_words);
	JS_FreeCString(ctx, s);
	if (k < 0)
		return JS_UNDEFINED;
	html_canvas_state(cv)->text_align = (k == 0 || k == 3) ? 0 :
			(k == 1 ? 1 : 2);
	JS_SetPropertyStr(ctx, this_val, "__align", JS_DupValue(ctx, v));
	return JS_UNDEFINED;
}

static const char *const op_words[] = {
	"source-over", "copy", "destination-out", "destination-over",
	"lighter", "source-atop", NULL
};

static JSValue c_get_op(JSContext *ctx, JSValueConst this_val)
{
	CV_BEGIN
	return JS_NewString(ctx, op_words[html_canvas_state(cv)->op]);
}

static JSValue c_set_op(JSContext *ctx, JSValueConst this_val,
		JSValueConst v)
{
	const char *s = JS_ToCString(ctx, v);
	int k;
	CV_BEGIN
	if (s == NULL)
		return JS_UNDEFINED;
	k = keyword(s, op_words);
	JS_FreeCString(ctx, s);
	/* unsupported modes draw source-over */
	html_canvas_state(cv)->op = k >= 0 ? (enum canvas_composite)k :
			CANVAS_OP_SOURCE_OVER;
	return JS_UNDEFINED;
}

static JSValue c_get_font(JSContext *ctx, JSValueConst this_val)
{
	JSValue v = JS_GetPropertyStr(ctx, this_val, "__font");
	if (JS_IsUndefined(v))
		return JS_NewString(ctx, "10px sans-serif");
	return v;
}

static JSValue c_set_font(JSContext *ctx, JSValueConst this_val,
		JSValueConst v)
{
	const char *s = JS_ToCString(ctx, v);
	CV_BEGIN
	if (s == NULL)
		return JS_UNDEFINED;
	html_canvas_set_font(cv, s);
	JS_FreeCString(ctx, s);
	JS_SetPropertyStr(ctx, this_val, "__font", JS_DupValue(ctx, v));
	return JS_UNDEFINED;
}

static JSValue c_get_smooth(JSContext *ctx, JSValueConst this_val)
{
	CV_BEGIN
	return JS_NewBool(ctx, html_canvas_state(cv)->smoothing);
}

static JSValue c_set_smooth(JSContext *ctx, JSValueConst this_val,
		JSValueConst v)
{
	CV_BEGIN
	html_canvas_state(cv)->smoothing = JS_ToBool(ctx, v);
	return JS_UNDEFINED;
}

static JSValue c_get_canvas(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	struct qjs_ctx2d *c = JS_GetOpaque(this_val, ctx2d_class);

	if (t == NULL || c == NULL)
		return JS_NULL;
	return qjs_dom_wrap_node(t, c->node);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

#define SIMPLE(fname, call)						\
static JSValue fname(JSContext *ctx, JSValueConst this_val, int argc,	\
		JSValueConst *argv)					\
{									\
	CV_BEGIN							\
	call;								\
	return JS_UNDEFINED;						\
}

SIMPLE(c_begin_path, html_canvas_begin_path(cv))
SIMPLE(c_close_path, html_canvas_close_path(cv))
SIMPLE(c_move_to, html_canvas_move_to(cv, A(0), A(1)))
SIMPLE(c_line_to, html_canvas_line_to(cv, A(0), A(1)))
SIMPLE(c_bezier_to, html_canvas_bezier_to(cv, A(0), A(1), A(2), A(3),
		A(4), A(5)))
SIMPLE(c_quad_to, html_canvas_quad_to(cv, A(0), A(1), A(2), A(3)))
SIMPLE(c_arc, html_canvas_arc(cv, A(0), A(1), A(2), A(3), A(4),
		argc > 5 && JS_ToBool(ctx, argv[5])))
SIMPLE(c_arc_to, html_canvas_arc_to(cv, A(0), A(1), A(2), A(3), A(4)))
SIMPLE(c_ellipse, html_canvas_ellipse(cv, A(0), A(1), A(2), A(3), A(4),
		A(5), A(6), argc > 7 && JS_ToBool(ctx, argv[7])))
SIMPLE(c_rect, html_canvas_rect(cv, A(0), A(1), A(2), A(3)))
SIMPLE(c_fill_rect, html_canvas_fill_rect(cv, A(0), A(1), A(2), A(3)))
SIMPLE(c_stroke_rect, html_canvas_stroke_rect(cv, A(0), A(1), A(2), A(3)))
SIMPLE(c_clear_rect, html_canvas_clear_rect(cv, A(0), A(1), A(2), A(3)))
SIMPLE(c_stroke, html_canvas_stroke(cv))
SIMPLE(c_clip, html_canvas_clip(cv))

static JSValue c_round_rect(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	double r = 0;
	CV_BEGIN
	if (argc > 4) {
		if (JS_IsArray(argv[4])) {
			JSValue e = JS_GetPropertyUint32(ctx, argv[4], 0);
			JS_ToFloat64(ctx, &r, e);
			JS_FreeValue(ctx, e);
		} else {
			JS_ToFloat64(ctx, &r, argv[4]);
		}
	}
	html_canvas_round_rect(cv, A(0), A(1), A(2), A(3), r);
	return JS_UNDEFINED;
}

static bool is_evenodd(JSContext *ctx, int argc, JSValueConst *argv)
{
	int i;

	for (i = 0; i < argc; i++) {
		if (JS_IsString(argv[i])) {
			const char *s = JS_ToCString(ctx, argv[i]);
			bool eo = s != NULL && strcmp(s, "evenodd") == 0;
			if (s != NULL)
				JS_FreeCString(ctx, s);
			return eo;
		}
	}
	return false;
}

static JSValue c_fill(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	CV_BEGIN
	html_canvas_fill(cv, is_evenodd(ctx, argc, argv));
	return JS_UNDEFINED;
}

static JSValue c_point_in_path(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct html_canvas *cv = cv_of(ctx, this_val);

	if (cv == NULL)
		return JS_FALSE;
	return JS_NewBool(ctx, html_canvas_point_in_path(cv, A(0), A(1)));
}

static JSValue c_text(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv, bool stroke)
{
	const char *s;
	CV_BEGIN
	if (argc < 3)
		return JS_UNDEFINED;
	s = JS_ToCString(ctx, argv[0]);
	if (s == NULL)
		return JS_UNDEFINED;
	html_canvas_fill_text(cv, s, A(1), A(2), stroke);
	JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
}

static JSValue c_fill_text(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	return c_text(ctx, this_val, argc, argv, false);
}

static JSValue c_stroke_text(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	return c_text(ctx, this_val, argc, argv, true);
}

static JSValue c_measure_text(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct html_canvas *cv = cv_of(ctx, this_val);
	const char *s;
	double w = 0, px = 10;
	JSValue o;

	s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
	if (cv != NULL && s != NULL) {
		w = html_canvas_measure_text(cv, s);
		px = html_canvas_state(cv)->font_px;
	}
	if (s != NULL)
		JS_FreeCString(ctx, s);
	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, w));
	JS_SetPropertyStr(ctx, o, "actualBoundingBoxLeft", JS_NewFloat64(ctx, 0));
	JS_SetPropertyStr(ctx, o, "actualBoundingBoxRight", JS_NewFloat64(ctx, w));
	JS_SetPropertyStr(ctx, o, "actualBoundingBoxAscent",
			JS_NewFloat64(ctx, px * 0.75));
	JS_SetPropertyStr(ctx, o, "actualBoundingBoxDescent",
			JS_NewFloat64(ctx, px * 0.22));
	JS_SetPropertyStr(ctx, o, "fontBoundingBoxAscent",
			JS_NewFloat64(ctx, px * 0.9));
	JS_SetPropertyStr(ctx, o, "fontBoundingBoxDescent",
			JS_NewFloat64(ctx, px * 0.25));
	return o;
}

static JSValue c_draw_image(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	dom_node *src;
	int w = 0, h = 0;
	CV_BEGIN
	if (argc < 3)
		return JS_UNDEFINED;
	src = qjs_dom_node_of(ctx, argv[0]);
	if (src == NULL || !html_canvas_element_size(t->htmlc, src, &w, &h))
		return JS_UNDEFINED;
	if (argc >= 9)
		html_canvas_draw_element(cv, src, A(1), A(2), A(3), A(4),
				A(5), A(6), A(7), A(8), true);
	else if (argc >= 5)
		html_canvas_draw_element(cv, src, 0, 0, w, h, A(1), A(2),
				A(3), A(4), false);
	else
		html_canvas_draw_element(cv, src, 0, 0, w, h, A(1), A(2),
				w, h, false);
	return JS_UNDEFINED;
}

/* __getImageData(x, y, w, h) -> ArrayBuffer */
static JSValue c_get_image_data(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	int x = (int)A(0), y = (int)A(1), w = (int)A(2), h = (int)A(3);
	uint8_t *buf;
	JSValue ab;
	CV_BEGIN
	if (w <= 0 || h <= 0 || (int64_t)w * h > 16 * 1024 * 1024)
		return JS_ThrowRangeError(ctx, "invalid image data size");
	buf = malloc((size_t)w * h * 4);
	if (buf == NULL)
		return JS_ThrowOutOfMemory(ctx);
	html_canvas_get_image_data(cv, x, y, w, h, buf);
	ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)w * h * 4);
	free(buf);
	return ab;
}

/* __putImageData(u8array, iw, ih, dx, dy, sx, sy, sw, sh) */
static JSValue c_put_image_data(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	size_t off = 0, len = 0, bpe = 0, abl = 0;
	JSValue ab;
	uint8_t *p;
	int iw = (int)A(1), ih = (int)A(2);
	CV_BEGIN
	if (argc < 9)
		return JS_UNDEFINED;
	ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
	if (JS_IsException(ab))
		return ab;
	p = JS_GetArrayBuffer(ctx, &abl, ab);
	if (p != NULL && iw > 0 && ih > 0 &&
	    off + (size_t)iw * ih * 4 <= abl)
		html_canvas_put_image_data(cv, p + off, iw, ih, (int)A(3),
				(int)A(4), (int)A(5), (int)A(6), (int)A(7),
				(int)A(8));
	JS_FreeValue(ctx, ab);
	return JS_UNDEFINED;
}

static JSValue c_noop(JSContext *ctx, JSValueConst this_val, int argc,
		JSValueConst *argv)
{
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry ctx2d_funcs[] = {
	JS_CFUNC_DEF("save", 0, c_save),
	JS_CFUNC_DEF("restore", 0, c_restore),
	JS_CFUNC_DEF("reset", 0, c_reset),
	JS_CFUNC_DEF("transform", 6, c_transform),
	JS_CFUNC_DEF("setTransform", 6, c_set_transform),
	JS_CFUNC_DEF("resetTransform", 0, c_set_transform),
	JS_CFUNC_DEF("getTransform", 0, c_get_transform),
	JS_CFUNC_DEF("translate", 2, c_translate),
	JS_CFUNC_DEF("scale", 2, c_scale),
	JS_CFUNC_DEF("rotate", 1, c_rotate),
	JS_CFUNC_DEF("beginPath", 0, c_begin_path),
	JS_CFUNC_DEF("closePath", 0, c_close_path),
	JS_CFUNC_DEF("moveTo", 2, c_move_to),
	JS_CFUNC_DEF("lineTo", 2, c_line_to),
	JS_CFUNC_DEF("bezierCurveTo", 6, c_bezier_to),
	JS_CFUNC_DEF("quadraticCurveTo", 4, c_quad_to),
	JS_CFUNC_DEF("arc", 6, c_arc),
	JS_CFUNC_DEF("arcTo", 5, c_arc_to),
	JS_CFUNC_DEF("ellipse", 8, c_ellipse),
	JS_CFUNC_DEF("rect", 4, c_rect),
	JS_CFUNC_DEF("roundRect", 5, c_round_rect),
	JS_CFUNC_DEF("fill", 0, c_fill),
	JS_CFUNC_DEF("stroke", 0, c_stroke),
	JS_CFUNC_DEF("clip", 0, c_clip),
	JS_CFUNC_DEF("isPointInPath", 2, c_point_in_path),
	JS_CFUNC_DEF("fillRect", 4, c_fill_rect),
	JS_CFUNC_DEF("strokeRect", 4, c_stroke_rect),
	JS_CFUNC_DEF("clearRect", 4, c_clear_rect),
	JS_CFUNC_DEF("fillText", 3, c_fill_text),
	JS_CFUNC_DEF("strokeText", 3, c_stroke_text),
	JS_CFUNC_DEF("measureText", 1, c_measure_text),
	JS_CFUNC_DEF("drawImage", 3, c_draw_image),
	JS_CFUNC_DEF("__getImageData", 4, c_get_image_data),
	JS_CFUNC_DEF("__putImageData", 9, c_put_image_data),
	JS_CFUNC_DEF("setLineDash", 1, c_noop),
	JS_CFUNC_DEF("drawFocusIfNeeded", 1, c_noop),
	JS_CGETSET_DEF("fillStyle", c_get_fill, c_set_fill),
	JS_CGETSET_DEF("strokeStyle", c_get_stroke, c_set_stroke),
	JS_CGETSET_DEF("lineWidth", c_get_lw, c_set_lw),
	JS_CGETSET_DEF("globalAlpha", c_get_ga, c_set_ga),
	JS_CGETSET_DEF("lineCap", c_get_cap, c_set_cap),
	JS_CGETSET_DEF("lineJoin", c_get_join, c_set_join),
	JS_CGETSET_DEF("textBaseline", c_get_base, c_set_base),
	JS_CGETSET_DEF("textAlign", c_get_align, c_set_align),
	JS_CGETSET_DEF("globalCompositeOperation", c_get_op, c_set_op),
	JS_CGETSET_DEF("font", c_get_font, c_set_font),
	JS_CGETSET_DEF("imageSmoothingEnabled", c_get_smooth, c_set_smooth),
	JS_CGETSET_DEF("canvas", c_get_canvas, NULL),
};

static JSValue c_illegal(JSContext *ctx, JSValueConst new_target, int argc,
		JSValueConst *argv)
{
	return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* __ns.canvasContext(el) -> CanvasRenderingContext2D */
static JSValue n_canvas_context(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	dom_node *node = argc > 0 ? qjs_dom_node_of(ctx, argv[0]) : NULL;
	struct qjs_ctx2d *c;
	JSValue obj;

	if (t == NULL || node == NULL || t->htmlc == NULL ||
	    html_canvas_for_node(t->htmlc, node, true) == NULL)
		return JS_NULL;
	obj = JS_NewObjectClass(ctx, ctx2d_class);
	if (JS_IsException(obj))
		return obj;
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		JS_FreeValue(ctx, obj);
		return JS_ThrowOutOfMemory(ctx);
	}
	c->node = dom_node_ref(node);
	JS_SetOpaque(obj, c);
	return obj;
}

/* __ns.canvasSize(el) -> [w, h] */
static JSValue n_canvas_size(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	dom_node *node = argc > 0 ? qjs_dom_node_of(ctx, argv[0]) : NULL;
	struct html_canvas *cv;
	JSValue a;
	int w = 300, h = 150;

	if (t != NULL && node != NULL && t->htmlc != NULL &&
	    (cv = html_canvas_for_node(t->htmlc, node, true)) != NULL)
		html_canvas_get_size(cv, &w, &h);
	a = JS_NewArray(ctx);
	JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, w));
	JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, h));
	return a;
}

/* __ns.canvasResize(el, w, h): clears the canvas */
static JSValue n_canvas_resize(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	dom_node *node = argc > 0 ? qjs_dom_node_of(ctx, argv[0]) : NULL;
	struct html_canvas *cv;
	int32_t w = 300, h = 150;

	if (argc > 2) {
		JS_ToInt32(ctx, &w, argv[1]);
		JS_ToInt32(ctx, &h, argv[2]);
	}
	if (t != NULL && node != NULL && t->htmlc != NULL &&
	    (cv = html_canvas_for_node(t->htmlc, node, true)) != NULL)
		html_canvas_set_size(cv, w, h);
	return JS_UNDEFINED;
}

/* exported interface documented in qjs_private.h */
void qjs_canvas_install(struct jsthread *t, JSValue ns)
{
	JSContext *ctx = t->ctx;
	JSRuntime *rt = JS_GetRuntime(ctx);
	JSValue proto, global;

	if (ctx2d_class == 0)
		JS_NewClassID(rt, &ctx2d_class);
	if (!JS_IsRegisteredClass(rt, ctx2d_class))
		JS_NewClass(rt, ctx2d_class, &ctx2d_def);

	proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, proto, ctx2d_funcs,
			sizeof(ctx2d_funcs) / sizeof(ctx2d_funcs[0]));
	JS_SetClassProto(ctx, ctx2d_class, JS_DupValue(ctx, proto));

	/* expose the prototype for runtime.js and instanceof */
	global = JS_GetGlobalObject(ctx);
	{
		JSValue ctor = JS_NewCFunction2(ctx, c_illegal,
				"CanvasRenderingContext2D", 0,
				JS_CFUNC_constructor, 0);
		JS_SetConstructor(ctx, ctor, proto);
		JS_FreeValue(ctx, proto);
		JS_DefinePropertyValueStr(ctx, global,
				"CanvasRenderingContext2D", ctor,
				JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
	}
	JS_FreeValue(ctx, global);

	JS_SetPropertyStr(ctx, ns, "canvasContext",
			JS_NewCFunction(ctx, n_canvas_context,
					"canvasContext", 1));
	JS_SetPropertyStr(ctx, ns, "canvasSize",
			JS_NewCFunction(ctx, n_canvas_size, "canvasSize", 1));
	JS_SetPropertyStr(ctx, ns, "canvasResize",
			JS_NewCFunction(ctx, n_canvas_resize,
					"canvasResize", 3));
}
