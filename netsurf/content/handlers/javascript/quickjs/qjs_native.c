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
 * Native primitives for the QuickJS web runtime.
 *
 * The browser-facing APIs (fetch, XMLHttpRequest, layout geometry,
 * getComputedStyle, storage, matchMedia, DOMParser, ...) are written in
 * JavaScript (runtime.js) on top of the small set of primitives exposed
 * here as the hidden global object "__ns".
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>
#include <libcss/libcss.h>
#include <nsutils/time.h>

#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "utils/utils.h"
#include "netsurf/content.h"
#include "netsurf/misc.h"
#include "netsurf/browser_window.h"
#include "content/fetch.h"
#include "content/urldb.h"
#include "desktop/gui_internal.h"
#include "desktop/browser_private.h"
#include "desktop/scrollbar.h"
#include "css/utils.h"
#include "html/private.h"
#include "html/box.h"
#include "html/box_inspect.h"
#include "html/box_construct.h"
#include "html/rebuild.h"
#include "html/layout.h"
#include <limits.h>
#include "html/layout_internal.h"
#include "html/form_internal.h"

#include "javascript/quickjs/qjs_private.h"

static struct jsthread *nat_thread(JSContext *ctx)
{
	return JS_GetContextOpaque(ctx);
}

static dom_node *nat_node(struct jsthread *t, JSValueConst v)
{
	return JS_GetOpaque(v, t->heap->node_class);
}

/** make sure the layout reflects the DOM */
static void nat_flush_layout(struct jsthread *t)
{
	if (t->htmlc != NULL)
		html_rebuild_flush(t->htmlc);
}

/* ------------------------------------------------------------------ */
/* Layout geometry */

/**
 * __ns.rect(node) -> [x, y, w, h, scrollW, scrollH, clientW, clientH,
 *                     borderLeft, borderTop] in document coordinates of
 * the border box, or null if the node is not rendered.
 */
static JSValue nat_rect(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	struct box *box;
	int x, y, w, h, i;
	JSValue arr;
	int v[10];

	if (argc < 1 || t == NULL)
		return JS_NULL;
	node = nat_node(t, argv[0]);
	if (node == NULL)
		return JS_NULL;
	nat_flush_layout(t);
	box = box_for_node(node);
	if (box == NULL || t->htmlc == NULL || t->htmlc->layout == NULL)
		return JS_NULL;

	/* the box may belong to a tree that is not the current layout if
	 * the node is detached */
	box_coords(box, &x, &y);
	w = box->padding[LEFT] + box->width + box->padding[RIGHT];
	h = box->padding[TOP] + box->height + box->padding[BOTTOM];

	if (box->type == BOX_INLINE && box->inline_end != NULL) {
		/* span the inline's fragments on its first line */
		int ex, ey;
		box_coords(box->inline_end, &ex, &ey);
		if (ey == y && ex > x)
			w = ex - x;
		else if (ey > y) {
			w = box->parent ? box->parent->width : w;
			h = ey - y + box->inline_end->height;
		}
	}

	v[0] = x - box->border[LEFT].width;
	v[1] = y - box->border[TOP].width;
	v[2] = w + box->border[LEFT].width + box->border[RIGHT].width;
	v[3] = h + box->border[TOP].width + box->border[BOTTOM].width;
	v[4] = box->descendant_x1 > w ? box->descendant_x1 : w;
	v[5] = box->descendant_y1 > h ? box->descendant_y1 : h;
	v[6] = w;
	v[7] = h;
	v[8] = box->border[LEFT].width;
	v[9] = box->border[TOP].width;

	arr = JS_NewArray(ctx);
	for (i = 0; i < 10; i++)
		JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, v[i]));
	return arr;
}

/** __ns.viewport() -> [scrollX, scrollY, width, height, docW, docH] */
static JSValue nat_viewport(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	int sx = 0, sy = 0, w = 1280, h = 720, dw = 0, dh = 0, i;
	JSValue arr;
	int v[6];

	if (t != NULL && t->bw != NULL) {
		browser_window_script_get_scroll(t->bw, &sx, &sy);
		browser_window_get_dimensions(t->bw, &w, &h);
	}
	if (t != NULL && t->htmlc != NULL) {
		nat_flush_layout(t);
		dw = t->htmlc->base.width;
		dh = t->htmlc->base.height;
	}
	v[0] = sx; v[1] = sy; v[2] = w; v[3] = h; v[4] = dw; v[5] = dh;
	arr = JS_NewArray(ctx);
	for (i = 0; i < 6; i++)
		JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, v[i]));
	return arr;
}

/** __ns.scrollTo(x, y) */
static JSValue nat_scroll_to(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	int32_t x = 0, y = 0;

	if (t == NULL || t->bw == NULL || argc < 2)
		return JS_UNDEFINED;
	JS_ToInt32(ctx, &x, argv[0]);
	JS_ToInt32(ctx, &y, argv[1]);
	nat_flush_layout(t);
	browser_window_script_set_scroll(t->bw, x, y);
	return JS_UNDEFINED;
}

/** __ns.elementScroll(node[, x, y]) -> [x, y] of a scrolling box */
static JSValue nat_element_scroll(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	struct box *box;
	JSValue arr;

	if (t == NULL || argc < 1)
		return JS_NULL;
	node = nat_node(t, argv[0]);
	if (node == NULL)
		return JS_NULL;
	nat_flush_layout(t);
	box = box_for_node(node);
	if (box == NULL)
		return JS_NULL;
	if (argc >= 3) {
		int32_t x = 0, y = 0;
		JS_ToInt32(ctx, &x, argv[1]);
		JS_ToInt32(ctx, &y, argv[2]);
		if (box->scroll_x != NULL)
			scrollbar_set(box->scroll_x, x, false);
		if (box->scroll_y != NULL)
			scrollbar_set(box->scroll_y, y, false);
		if (t->htmlc != NULL)
			html__redraw_a_box(t->htmlc, box);
	}
	arr = JS_NewArray(ctx);
	JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx,
			box->scroll_x ? scrollbar_get_offset(box->scroll_x) : 0));
	JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx,
			box->scroll_y ? scrollbar_get_offset(box->scroll_y) : 0));
	return arr;
}

/* ------------------------------------------------------------------ */
/* Computed style */

static void nat_set_str(JSContext *ctx, JSValue obj, const char *k,
		const char *v)
{
	JS_SetPropertyStr(ctx, obj, k, JS_NewString(ctx, v));
}

static void nat_set_px(JSContext *ctx, JSValue obj, const char *k, int v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%dpx", v);
	nat_set_str(ctx, obj, k, buf);
}

static void nat_set_colour(JSContext *ctx, JSValue obj, const char *k,
		css_color c)
{
	char buf[64];
	unsigned a = (c >> 24) & 0xff;
	if (a == 255)
		snprintf(buf, sizeof(buf), "rgb(%u, %u, %u)",
			 (unsigned)((c >> 16) & 0xff),
			 (unsigned)((c >> 8) & 0xff), (unsigned)(c & 0xff));
	else
		snprintf(buf, sizeof(buf), "rgba(%u, %u, %u, %g)",
			 (unsigned)((c >> 16) & 0xff),
			 (unsigned)((c >> 8) & 0xff), (unsigned)(c & 0xff),
			 a / 255.0);
	nat_set_str(ctx, obj, k, buf);
}

static const char *nat_display_name(uint8_t d)
{
	switch (d) {
	case CSS_DISPLAY_INLINE: return "inline";
	case CSS_DISPLAY_BLOCK: return "block";
	case CSS_DISPLAY_LIST_ITEM: return "list-item";
	case CSS_DISPLAY_INLINE_BLOCK: return "inline-block";
	case CSS_DISPLAY_TABLE: return "table";
	case CSS_DISPLAY_INLINE_TABLE: return "inline-table";
	case CSS_DISPLAY_TABLE_ROW: return "table-row";
	case CSS_DISPLAY_TABLE_CELL: return "table-cell";
	case CSS_DISPLAY_NONE: return "none";
	case CSS_DISPLAY_FLEX: return "flex";
	case CSS_DISPLAY_INLINE_FLEX: return "inline-flex";
	case CSS_DISPLAY_GRID: return "grid";
	case CSS_DISPLAY_INLINE_GRID: return "inline-grid";
	default: return "block";
	}
}

static const char *nat_position_name(uint8_t p)
{
	switch (p) {
	case CSS_POSITION_RELATIVE: return "relative";
	case CSS_POSITION_ABSOLUTE: return "absolute";
	case CSS_POSITION_FIXED: return "fixed";
	case CSS_POSITION_STICKY: return "sticky";
	default: return "static";
	}
}

/**
 * __ns.computed(node) -> object of computed property strings, or null
 * if the element is not rendered.
 */
static JSValue nat_computed(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	struct box *box;
	const css_computed_style *s;
	JSValue o;
	css_color col;
	css_fixed len = 0;
	css_unit unit = CSS_UNIT_PX;
	static const char *sides[4] = { "top", "right", "bottom", "left" };
	char key[40];
	int i;

	if (t == NULL || argc < 1)
		return JS_NULL;
	node = nat_node(t, argv[0]);
	if (node == NULL)
		return JS_NULL;
	nat_flush_layout(t);
	box = box_for_node(node);
	if (box == NULL || box->style == NULL)
		return JS_NULL;
	s = box->style;

	o = JS_NewObject(ctx);
	nat_set_str(ctx, o, "display",
		    nat_display_name(css_computed_display(s, false)));
	nat_set_str(ctx, o, "position",
		    nat_position_name(css_computed_position(s)));
	nat_set_str(ctx, o, "visibility",
		    css_computed_visibility(s) == CSS_VISIBILITY_HIDDEN ?
		    "hidden" : "visible");
	nat_set_str(ctx, o, "float",
		    css_computed_float(s) == CSS_FLOAT_LEFT ? "left" :
		    css_computed_float(s) == CSS_FLOAT_RIGHT ? "right" :
		    "none");
	css_computed_color(s, &col);
	nat_set_colour(ctx, o, "color", col);
	css_computed_background_color(s, &col);
	nat_set_colour(ctx, o, "backgroundColor", col);

	if (css_computed_font_size(s, &len, &unit) == CSS_FONT_SIZE_DIMENSION)
		nat_set_px(ctx, o, "fontSize", FIXTOINT(css_unit_len2device_px(
				s, &t->htmlc->unit_len_ctx, len, unit)));
	switch (css_computed_font_weight(s)) {
	case CSS_FONT_WEIGHT_BOLD:
	case CSS_FONT_WEIGHT_700:
		nat_set_str(ctx, o, "fontWeight", "700");
		break;
	default:
		nat_set_str(ctx, o, "fontWeight", "400");
		break;
	}
	nat_set_str(ctx, o, "fontStyle",
		    css_computed_font_style(s) == CSS_FONT_STYLE_ITALIC ?
		    "italic" : "normal");

	nat_set_px(ctx, o, "width", box->width == AUTO ? 0 : box->width);
	nat_set_px(ctx, o, "height", box->height == AUTO ? 0 : box->height);
	for (i = 0; i < 4; i++) {
		snprintf(key, sizeof(key), "margin-%s", sides[i]);
		nat_set_px(ctx, o, key,
			   box->margin[i] == AUTO ? 0 : box->margin[i]);
		snprintf(key, sizeof(key), "padding-%s", sides[i]);
		nat_set_px(ctx, o, key, box->padding[i]);
		snprintf(key, sizeof(key), "border-%s-width", sides[i]);
		nat_set_px(ctx, o, key, box->border[i].width);
	}

	if (css_computed_opacity(s, &len) == CSS_OPACITY_SET) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%g", FIXTOFLT(len));
		nat_set_str(ctx, o, "opacity", buf);
	}
	{
		uint8_t ov = css_computed_overflow_y(s);
		nat_set_str(ctx, o, "overflowY",
			    ov == CSS_OVERFLOW_HIDDEN ? "hidden" :
			    ov == CSS_OVERFLOW_SCROLL ? "scroll" :
			    ov == CSS_OVERFLOW_AUTO ? "auto" : "visible");
		ov = css_computed_overflow_x(s);
		nat_set_str(ctx, o, "overflowX",
			    ov == CSS_OVERFLOW_HIDDEN ? "hidden" :
			    ov == CSS_OVERFLOW_SCROLL ? "scroll" :
			    ov == CSS_OVERFLOW_AUTO ? "auto" : "visible");
	}
	{
		int32_t z = 0;
		if (css_computed_z_index(s, &z) == CSS_Z_INDEX_SET) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", z);
			nat_set_str(ctx, o, "zIndex", buf);
		} else {
			nat_set_str(ctx, o, "zIndex", "auto");
		}
	}
	{
		lwc_string *raw = NULL;
		if (css_computed_raw(s, CSS_PROP_TRANSFORM, &raw) ==
				CSS_RAW_SET && raw != NULL)
			nat_set_str(ctx, o, "transform", lwc_string_data(raw));
		else
			nat_set_str(ctx, o, "transform", "none");
		raw = NULL;
		if (css_computed_raw(s, CSS_PROP_POINTER_EVENTS, &raw) ==
				CSS_RAW_SET && raw != NULL)
			nat_set_str(ctx, o, "pointerEvents",
				    lwc_string_data(raw));
		else
			nat_set_str(ctx, o, "pointerEvents", "auto");
	}
	return o;
}

/* ------------------------------------------------------------------ */
/* Network */

struct nat_buf {
	char *d;
	size_t len, cap;
};

static bool nat_buf_add(struct nat_buf *b, const void *d, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		char *nd;
		while (cap < b->len + n + 1)
			cap *= 2;
		nd = realloc(b->d, cap);
		if (nd == NULL)
			return false;
		b->d = nd;
		b->cap = cap;
	}
	memcpy(b->d + b->len, d, n);
	b->len += n;
	b->d[b->len] = '\0';
	return true;
}

struct nat_fetch {
	struct nat_fetch *next;
	struct jsthread *t;
	struct fetch *fetch;
	int32_t id;
	nsurl *url;
	char *body;
	char **headers; /* NULL terminated "Name: value" */
	struct nat_buf hdrs;
	struct nat_buf data;
	long status;
	int redirects;
	bool done;
	char *error;
};

static struct nat_fetch *nat_fetches;

static void nat_fetch_callback(const fetch_msg *msg, void *p);

static void nat_fetch_free(struct nat_fetch *f)
{
	struct nat_fetch **pp;
	int i;

	for (pp = &nat_fetches; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == f) {
			*pp = f->next;
			break;
		}
	}
	if (f->url != NULL)
		nsurl_unref(f->url);
	free(f->body);
	if (f->headers != NULL) {
		for (i = 0; f->headers[i] != NULL; i++)
			free(f->headers[i]);
		free(f->headers);
	}
	free(f->hdrs.d);
	free(f->data.d);
	free(f->error);
	free(f);
}

/** deliver a finished fetch to script (scheduled, not re-entrant) */
static void nat_fetch_deliver(void *p)
{
	struct nat_fetch *f = p;
	struct jsthread *t = f->t;
	JSContext *ctx;
	JSValue global, fn, args[6], ret;

	if (t == NULL || t->closed) {
		nat_fetch_free(f);
		return;
	}
	ctx = t->ctx;
	global = JS_GetGlobalObject(ctx);
	fn = JS_GetPropertyStr(ctx, global, "__ns_fetch_done");
	if (JS_IsFunction(ctx, fn)) {
		args[0] = JS_NewInt32(ctx, f->id);
		args[1] = JS_NewInt32(ctx, f->error ? 0 : (int)f->status);
		args[2] = JS_NewString(ctx, f->url ? nsurl_access(f->url) : "");
		args[3] = JS_NewStringLen(ctx, f->hdrs.d ? f->hdrs.d : "",
				f->hdrs.len);
		args[4] = JS_NewArrayBufferCopy(ctx,
				(const uint8_t *)(f->data.d ? f->data.d : ""),
				f->data.len);
		args[5] = f->error ? JS_NewString(ctx, f->error) : JS_NULL;
		qjs_deadline_start(t);
		ret = JS_Call(ctx, fn, global, 6, args);
		qjs_deadline_stop(t);
		if (JS_IsException(ret))
			qjs_dump_error(ctx);
		JS_FreeValue(ctx, ret);
		for (int i = 0; i < 6; i++)
			JS_FreeValue(ctx, args[i]);
		qjs_run_jobs(ctx);
	}
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, global);
	nat_fetch_free(f);
}

static bool nat_fetch_begin(struct nat_fetch *f)
{
	nsurl *referer = NULL;
	nserror err;

	if (f->t != NULL && f->t->htmlc != NULL)
		referer = f->t->htmlc->base_url;

	free(f->hdrs.d);
	memset(&f->hdrs, 0, sizeof(f->hdrs));
	free(f->data.d);
	memset(&f->data, 0, sizeof(f->data));
	f->status = 0;

	err = fetch_start(f->url, referer, nat_fetch_callback, f, false,
			f->body, NULL, false, false,
			(const char **)f->headers, &f->fetch);
	return err == NSERROR_OK;
}

static void nat_fetch_restart(void *p)
{
	struct nat_fetch *f = p;

	if (f->t == NULL || f->t->closed || !nat_fetch_begin(f)) {
		if (f->error == NULL)
			f->error = strdup("network error");
		nat_fetch_deliver(f);
	}
}

static void nat_fetch_callback(const fetch_msg *msg, void *p)
{
	struct nat_fetch *f = p;

	switch (msg->type) {
	case FETCH_HEADER:
		if (f->status == 0 && f->fetch != NULL)
			f->status = fetch_http_code(f->fetch);
		nat_buf_add(&f->hdrs, msg->data.header_or_data.buf,
				msg->data.header_or_data.len);
		if (msg->data.header_or_data.len > 11 &&
		    strncasecmp((const char *)msg->data.header_or_data.buf,
				"Set-Cookie:", 11) == 0) {
			char *h = strndup((const char *)
					msg->data.header_or_data.buf + 11,
					msg->data.header_or_data.len - 11);
			if (h != NULL) {
				urldb_set_cookie(h, f->url, f->t && f->t->htmlc ?
						f->t->htmlc->base_url : NULL);
				free(h);
			}
		}
		break;

	case FETCH_DATA:
		if (f->status == 0 && f->fetch != NULL)
			f->status = fetch_http_code(f->fetch);
		nat_buf_add(&f->data, msg->data.header_or_data.buf,
				msg->data.header_or_data.len);
		break;

	case FETCH_FINISHED:
	case FETCH_NOTMODIFIED:
		if (f->fetch != NULL && f->status == 0)
			f->status = fetch_http_code(f->fetch);
		if (f->status == 0)
			f->status = 200;
		f->fetch = NULL;
		f->done = true;
		guit->misc->schedule(0, nat_fetch_deliver, f);
		break;

	case FETCH_REDIRECT: {
		nsurl *next = NULL;
		f->fetch = NULL;
		if (f->redirects++ < 20 && msg->data.redirect != NULL &&
		    nsurl_join(f->url, msg->data.redirect, &next) ==
				NSERROR_OK) {
			nsurl_unref(f->url);
			f->url = next;
			/* 301/302/303 after POST become GET */
			free(f->body);
			f->body = NULL;
			guit->misc->schedule(0, nat_fetch_restart, f);
		} else {
			f->error = strdup("too many redirects");
			f->done = true;
			guit->misc->schedule(0, nat_fetch_deliver, f);
		}
		break;
	}

	case FETCH_ERROR:
	case FETCH_TIMEDOUT:
	case FETCH_AUTH:
	case FETCH_CERT_ERR:
	case FETCH_SSL_ERR:
		f->fetch = NULL;
		f->done = true;
		f->error = strdup(msg->type == FETCH_ERROR &&
				msg->data.error != NULL ?
				msg->data.error : "network error");
		guit->misc->schedule(0, nat_fetch_deliver, f);
		break;

	default:
		break;
	}
}

/**
 * __ns.fetch(id, method, url, [headers...], body|null) -> bool
 *
 * The result is delivered to __ns_fetch_done(id, status, url, headers,
 * body ArrayBuffer, error).
 */
static JSValue nat_fetch(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	struct nat_fetch *f;
	const char *s;
	uint32_t nh = 0, i;
	nsurl *url;

	if (t == NULL || argc < 5)
		return JS_FALSE;

	s = JS_ToCString(ctx, argv[2]);
	if (s == NULL)
		return JS_FALSE;
	if (nsurl_create(s, &url) != NSERROR_OK) {
		JS_FreeCString(ctx, s);
		return JS_FALSE;
	}
	JS_FreeCString(ctx, s);
	if (!fetch_can_fetch(url)) {
		nsurl_unref(url);
		return JS_FALSE;
	}

	f = calloc(1, sizeof(*f));
	if (f == NULL) {
		nsurl_unref(url);
		return JS_FALSE;
	}
	f->t = t;
	f->url = url;
	JS_ToInt32(ctx, &f->id, argv[0]);

	if (JS_IsArray(argv[3])) {
		JSValue lenv = JS_GetPropertyStr(ctx, argv[3], "length");
		JS_ToUint32(ctx, &nh, lenv);
		JS_FreeValue(ctx, lenv);
	}
	f->headers = calloc(nh + 1, sizeof(char *));
	for (i = 0; f->headers != NULL && i < nh; i++) {
		JSValue hv = JS_GetPropertyUint32(ctx, argv[3], i);
		const char *hs = JS_ToCString(ctx, hv);
		if (hs != NULL) {
			f->headers[i] = strdup(hs);
			JS_FreeCString(ctx, hs);
		}
		JS_FreeValue(ctx, hv);
	}

	if (!JS_IsNull(argv[4]) && !JS_IsUndefined(argv[4])) {
		size_t blen;
		s = JS_ToCStringLen(ctx, &blen, argv[4]);
		if (s != NULL) {
			f->body = strndup(s, blen);
			JS_FreeCString(ctx, s);
		}
	}

	f->next = nat_fetches;
	nat_fetches = f;

	if (!nat_fetch_begin(f)) {
		f->error = strdup("network error");
		f->done = true;
		guit->misc->schedule(0, nat_fetch_deliver, f);
	}
	return JS_TRUE;
}

/** __ns.fetchAbort(id) */
static JSValue nat_fetch_abort(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	struct nat_fetch *f;
	int32_t id = -1;

	if (argc < 1)
		return JS_UNDEFINED;
	JS_ToInt32(ctx, &id, argv[0]);
	for (f = nat_fetches; f != NULL; f = f->next) {
		if (f->t == t && f->id == id && !f->done) {
			if (f->fetch != NULL)
				fetch_abort(f->fetch);
			f->fetch = NULL;
			guit->misc->schedule(-1, nat_fetch_restart, f);
			nat_fetch_free(f);
			break;
		}
	}
	return JS_UNDEFINED;
}

/* exported interface documented in qjs_private.h */
void qjs_native_closethread(struct jsthread *t)
{
	struct nat_fetch *f, *next;

	for (f = nat_fetches; f != NULL; f = next) {
		next = f->next;
		if (f->t != t)
			continue;
		if (f->fetch != NULL)
			fetch_abort(f->fetch);
		f->fetch = NULL;
		guit->misc->schedule(-1, nat_fetch_restart, f);
		guit->misc->schedule(-1, nat_fetch_deliver, f);
		nat_fetch_free(f);
	}
}

/* ------------------------------------------------------------------ */
/* Storage */

/** path of the per-origin storage file */
static bool nat_storage_path(struct jsthread *t, const char *kind,
		char *out, size_t outlen)
{
	const char *jar = nsoption_charp(cookie_jar);
	const char *slash;
	lwc_string *host = NULL;
	char hostbuf[128];
	size_t i, dirlen;

	if (t == NULL || t->htmlc == NULL || jar == NULL)
		return false;
	host = nsurl_get_component(content_get_url(&t->htmlc->base),
			NSURL_HOST);
	if (host == NULL)
		snprintf(hostbuf, sizeof(hostbuf), "local");
	else {
		snprintf(hostbuf, sizeof(hostbuf), "%s", lwc_string_data(host));
		lwc_string_unref(host);
	}
	for (i = 0; hostbuf[i] != '\0'; i++) {
		char c = hostbuf[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '-'))
			hostbuf[i] = '_';
	}
	slash = strrchr(jar, '/');
	dirlen = slash ? (size_t)(slash - jar + 1) : 0;
	snprintf(out, outlen, "%.*s%s-%s.json", (int)dirlen, jar, kind, hostbuf);
	return true;
}

/** __ns.storageLoad(kind) -> string|null */
static JSValue nat_storage_load(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	char path[512];
	const char *kind;
	FILE *fp;
	struct nat_buf b = { 0 };
	char chunk[4096];
	size_t n;
	JSValue ret;

	if (argc < 1)
		return JS_NULL;
	kind = JS_ToCString(ctx, argv[0]);
	if (kind == NULL)
		return JS_NULL;
	if (!nat_storage_path(t, kind, path, sizeof(path))) {
		JS_FreeCString(ctx, kind);
		return JS_NULL;
	}
	JS_FreeCString(ctx, kind);
	fp = fopen(path, "rb");
	if (fp == NULL)
		return JS_NULL;
	while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
		if (!nat_buf_add(&b, chunk, n) || b.len > 5 * 1024 * 1024)
			break;
	}
	fclose(fp);
	ret = JS_NewStringLen(ctx, b.d ? b.d : "", b.len);
	free(b.d);
	return ret;
}

/** __ns.storageSave(kind, text) */
static JSValue nat_storage_save(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	char path[512];
	const char *kind, *text;
	size_t len;
	FILE *fp;

	if (argc < 2)
		return JS_FALSE;
	kind = JS_ToCString(ctx, argv[0]);
	if (kind == NULL)
		return JS_FALSE;
	if (!nat_storage_path(t, kind, path, sizeof(path))) {
		JS_FreeCString(ctx, kind);
		return JS_FALSE;
	}
	JS_FreeCString(ctx, kind);
	text = JS_ToCStringLen(ctx, &len, argv[1]);
	if (text == NULL)
		return JS_FALSE;
	fp = fopen(path, "wb");
	if (fp != NULL) {
		fwrite(text, 1, len, fp);
		fclose(fp);
	}
	JS_FreeCString(ctx, text);
	return fp != NULL ? JS_TRUE : JS_FALSE;
}

/* ------------------------------------------------------------------ */
/* Misc */

/** __ns.mediaMatches(query) -> bool */
static JSValue nat_media_matches(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *q;
	bool m;

	if (argc < 1)
		return JS_FALSE;
	q = JS_ToCString(ctx, argv[0]);
	if (q == NULL)
		return JS_FALSE;
	m = css_media_query_matches(q);
	JS_FreeCString(ctx, q);
	return JS_NewBool(ctx, m);
}

/** __ns.parseDocument(html) -> Document (detached) */
static JSValue nat_parse_document(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_hubbub_parser_params params;
	dom_hubbub_parser *parser = NULL;
	dom_document *doc = NULL;
	const char *s;
	size_t len;
	JSValue ret;

	if (t == NULL || argc < 1)
		return JS_NULL;
	s = JS_ToCStringLen(ctx, &len, argv[0]);
	if (s == NULL)
		return JS_NULL;

	memset(&params, 0, sizeof(params));
	params.enc = "UTF-8";
	params.fix_enc = true;
	params.enable_script = false;
	params.msg = NULL;
	params.script = NULL;
	params.ctx = NULL;
	params.daf = NULL;

	if (dom_hubbub_parser_create(&params, &parser, &doc) !=
			DOM_HUBBUB_OK) {
		JS_FreeCString(ctx, s);
		return JS_NULL;
	}
	dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)s, len);
	dom_hubbub_parser_completed(parser);
	dom_hubbub_parser_destroy(parser);
	JS_FreeCString(ctx, s);

	ret = qjs_dom_wrap_node(t, (dom_node *)doc);
	dom_node_unref(doc);
	return ret;
}

/** __ns.now() -> ms (monotonic, fractional) */
static JSValue nat_now(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	uint64_t ms = 0;
	nsu_getmonotonic_ms(&ms);
	return JS_NewFloat64(ctx, (double)ms);
}

/** __ns.baseURL() -> document base URL */
static JSValue nat_base_url(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	if (t == NULL || t->htmlc == NULL || t->htmlc->base_url == NULL)
		return JS_NewString(ctx, "about:blank");
	return JS_NewString(ctx, nsurl_access(t->htmlc->base_url));
}

/** __ns.resolveURL(url[, base]) -> absolute URL string or null */
static JSValue nat_resolve_url(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	const char *s, *b = NULL;
	nsurl *base = NULL, *joined = NULL;
	JSValue ret = JS_NULL;

	if (argc < 1)
		return JS_NULL;
	s = JS_ToCString(ctx, argv[0]);
	if (s == NULL)
		return JS_NULL;
	if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
		b = JS_ToCString(ctx, argv[1]);
		if (b != NULL && nsurl_create(b, &base) != NSERROR_OK)
			base = NULL;
	} else if (t != NULL && t->htmlc != NULL && t->htmlc->base_url) {
		base = nsurl_ref(t->htmlc->base_url);
	}
	if (base != NULL) {
		if (nsurl_join(base, s, &joined) == NSERROR_OK) {
			ret = JS_NewString(ctx, nsurl_access(joined));
			nsurl_unref(joined);
		}
		nsurl_unref(base);
	} else if (nsurl_create(s, &joined) == NSERROR_OK) {
		ret = JS_NewString(ctx, nsurl_access(joined));
		nsurl_unref(joined);
	}
	if (b != NULL)
		JS_FreeCString(ctx, b);
	JS_FreeCString(ctx, s);
	return ret;
}

/** __ns.cookie() / __ns.setCookie(str): document.cookie */
static JSValue nat_log(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	const char *s;
	if (argc < 1)
		return JS_UNDEFINED;
	s = JS_ToCString(ctx, argv[0]);
	if (s != NULL) {
		NSLOG(netsurf, INFO, "js: %s", s);
		JS_FreeCString(ctx, s);
	}
	return JS_UNDEFINED;
}

/** __ns.elements(root) -> descendant elements in document order */
static JSValue nat_elements(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *root, *n;
	JSValue arr;
	uint32_t count = 0;

	if (t == NULL || argc < 1)
		return JS_NewArray(ctx);
	root = nat_node(t, argv[0]);
	arr = JS_NewArray(ctx);
	if (root == NULL)
		return arr;

	dom_node_get_first_child(root, &n);
	while (n != NULL) {
		dom_node_type type;
		dom_node *next = NULL;

		if (dom_node_get_node_type(n, &type) == DOM_NO_ERR &&
				type == DOM_ELEMENT_NODE) {
			JS_SetPropertyUint32(ctx, arr, count++,
					qjs_dom_wrap_node(t, n));
			dom_node_get_first_child(n, &next);
		}
		if (next == NULL) {
			dom_node *cur = dom_node_ref(n);
			for (;;) {
				dom_node *parent = NULL;
				dom_node_get_next_sibling(cur, &next);
				if (next != NULL)
					break;
				dom_node_get_parent_node(cur, &parent);
				dom_node_unref(cur);
				cur = parent;
				if (cur == NULL || cur == root)
					break;
			}
			if (cur != NULL)
				dom_node_unref(cur);
		}
		dom_node_unref(n);
		n = next;
	}
	return arr;
}

/** __ns.attributes(el) -> [[name, value], ...] */
static JSValue nat_attributes(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	dom_namednodemap *map = NULL;
	dom_ulong len = 0, i;
	JSValue arr = JS_NewArray(ctx);

	if (t == NULL || argc < 1)
		return arr;
	node = nat_node(t, argv[0]);
	if (node == NULL || dom_node_get_attributes(node, &map) != DOM_NO_ERR ||
			map == NULL)
		return arr;
	dom_namednodemap_get_length(map, &len);
	for (i = 0; i < len; i++) {
		dom_attr *attr = NULL;
		dom_string *k = NULL, *v = NULL;
		JSValue pair;
		if (dom_namednodemap_item(map, i, (void *)&attr) != DOM_NO_ERR ||
				attr == NULL)
			continue;
		dom_attr_get_name(attr, &k);
		dom_attr_get_value(attr, &v);
		pair = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, pair, 0, k ?
				JS_NewStringLen(ctx, dom_string_data(k),
					dom_string_byte_length(k)) :
				JS_NewString(ctx, ""));
		JS_SetPropertyUint32(ctx, pair, 1, v ?
				JS_NewStringLen(ctx, dom_string_data(v),
					dom_string_byte_length(v)) :
				JS_NewString(ctx, ""));
		JS_SetPropertyUint32(ctx, arr, i, pair);
		if (k != NULL)
			dom_string_unref(k);
		if (v != NULL)
			dom_string_unref(v);
		dom_node_unref(attr);
	}
	dom_namednodemap_unref(map);
	return arr;
}

static bool nat_is_tag(dom_node *node, dom_html_element_type want)
{
	dom_html_element_type tag;
	return dom_html_element_get_tag_type(node, &tag) == DOM_NO_ERR &&
			tag == want;
}

/** __ns.inputValue(el) -> current value of an input or textarea */
static JSValue nat_input_value(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	dom_string *v = NULL;
	dom_exception err;
	JSValue ret;

	if (t == NULL || argc < 1)
		return JS_NewString(ctx, "");
	node = nat_node(t, argv[0]);
	if (node == NULL)
		return JS_NewString(ctx, "");
	if (nat_is_tag(node, DOM_HTML_ELEMENT_TYPE_TEXTAREA))
		err = dom_html_text_area_element_get_value(
				(dom_html_text_area_element *)node, &v);
	else
		err = dom_html_input_element_get_value(
				(dom_html_input_element *)node, &v);
	if (err != DOM_NO_ERR || v == NULL)
		return JS_NewString(ctx, "");
	ret = JS_NewStringLen(ctx, dom_string_data(v), dom_string_byte_length(v));
	dom_string_unref(v);
	return ret;
}

/** __ns.setInputValue(el, value) */
static JSValue nat_set_input_value(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	dom_string *v = NULL;
	const char *s;
	size_t len;

	if (t == NULL || argc < 2)
		return JS_UNDEFINED;
	node = nat_node(t, argv[0]);
	if (node == NULL)
		return JS_UNDEFINED;
	s = JS_ToCStringLen(ctx, &len, argv[1]);
	if (s == NULL)
		return JS_UNDEFINED;
	if (dom_string_create((const uint8_t *)s, len, &v) == DOM_NO_ERR) {
		/* the form widget is updated directly; no relayout needed */
		html_rebuild_suppress(true);
		if (nat_is_tag(node, DOM_HTML_ELEMENT_TYPE_TEXTAREA))
			dom_html_text_area_element_set_value(
				(dom_html_text_area_element *)node, v);
		else
			dom_html_input_element_set_value(
				(dom_html_input_element *)node, v);
		html_rebuild_suppress(false);
		dom_string_unref(v);
	}
	JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
}

/** __ns.inputChecked(el) -> bool */
static JSValue nat_input_checked(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	bool checked = false;

	if (t == NULL || argc < 1)
		return JS_FALSE;
	node = nat_node(t, argv[0]);
	if (node == NULL)
		return JS_FALSE;
	dom_html_input_element_get_checked((dom_html_input_element *)node,
			&checked);
	return JS_NewBool(ctx, checked);
}

/** __ns.setInputChecked(el, bool) */
static JSValue nat_set_input_checked(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = nat_thread(ctx);
	dom_node *node;
	struct box *box;

	if (t == NULL || argc < 2)
		return JS_UNDEFINED;
	node = nat_node(t, argv[0]);
	if (node == NULL)
		return JS_UNDEFINED;
	dom_html_input_element_set_checked((dom_html_input_element *)node,
			JS_ToBool(ctx, argv[1]));
	box = box_for_node(node);
	if (box != NULL && box->gadget != NULL) {
		box->gadget->selected = JS_ToBool(ctx, argv[1]);
		if (t->htmlc != NULL)
			html__redraw_a_box(t->htmlc, box);
	}
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry nat_funcs[] = {
	JS_CFUNC_DEF("elements", 1, nat_elements),
	JS_CFUNC_DEF("attributes", 1, nat_attributes),
	JS_CFUNC_DEF("inputValue", 1, nat_input_value),
	JS_CFUNC_DEF("setInputValue", 2, nat_set_input_value),
	JS_CFUNC_DEF("inputChecked", 1, nat_input_checked),
	JS_CFUNC_DEF("setInputChecked", 2, nat_set_input_checked),
	JS_CFUNC_DEF("rect", 1, nat_rect),
	JS_CFUNC_DEF("viewport", 0, nat_viewport),
	JS_CFUNC_DEF("scrollTo", 2, nat_scroll_to),
	JS_CFUNC_DEF("elementScroll", 3, nat_element_scroll),
	JS_CFUNC_DEF("computed", 1, nat_computed),
	JS_CFUNC_DEF("fetch", 5, nat_fetch),
	JS_CFUNC_DEF("fetchAbort", 1, nat_fetch_abort),
	JS_CFUNC_DEF("storageLoad", 1, nat_storage_load),
	JS_CFUNC_DEF("storageSave", 2, nat_storage_save),
	JS_CFUNC_DEF("mediaMatches", 1, nat_media_matches),
	JS_CFUNC_DEF("parseDocument", 1, nat_parse_document),
	JS_CFUNC_DEF("now", 0, nat_now),
	JS_CFUNC_DEF("baseURL", 0, nat_base_url),
	JS_CFUNC_DEF("resolveURL", 2, nat_resolve_url),
	JS_CFUNC_DEF("log", 1, nat_log),
};

/* exported interface documented in qjs_private.h */
void qjs_native_setup(struct jsthread *t)
{
	JSContext *ctx = t->ctx;
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue ns = JS_NewObject(ctx);

	JS_SetPropertyFunctionList(ctx, ns, nat_funcs,
			sizeof(nat_funcs) / sizeof(nat_funcs[0]));
	qjs_modules_install(t, ns);
	qjs_canvas_install(t, ns);
	JS_DefinePropertyValueStr(ctx, global, "__ns", ns,
			JS_PROP_CONFIGURABLE);
	JS_FreeValue(ctx, global);
}
