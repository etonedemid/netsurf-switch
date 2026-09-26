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
 * QuickJS DOM bindings over libdom.
 *
 * Hand-written core DOM surface for the QuickJS backend: Window
 * globals, Document/Element/Node/Text wrappers, events, timers and a
 * simple selector engine ("#id", ".class", "tag" forms only).  The
 * duktape .bnd bodies cannot be reused (they are duktape stack C), so
 * this layer implements the interfaces most scripts touch directly
 * against libdom.  It is grown interface by interface; anything not
 * implemented reads as undefined, matching how NetSurf's duktape
 * bindings degrade.
 *
 * Wrapper identity: one JS object per dom_node/dom_event, cached in
 * pointer-keyed maps on the thread.  Each wrapper holds one libdom ref
 * released by the class finalizer.  The maps hold strong references,
 * so wrappers live until the thread is destroyed; acceptable for
 * page-lifetime contexts.
 *
 * Rendering note: NetSurf has no dynamic restyle/relayout, so DOM
 * mutations only affect what the user sees when they happen before
 * layout (parse-time scripts).  Element.style writes are stored but
 * inert.  This matches the duktape backend's behaviour.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dom/dom.h>

#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/useragent.h"
#include "netsurf/browser_window.h"
#include "netsurf/misc.h"
#include "desktop/gui_internal.h"
#include "html/private.h"

#include "javascript/js.h"
#include "javascript/quickjs/qjs_private.h"

/* runtime.js, embedded at build time */
#include "quickjs/runtime.js.inc"

/* property names used for the JS-side bookkeeping */
#define NSPROP_LISTENERS "__ns_listeners"
#define NSPROP_REGISTERED "__ns_reg"
#define NSPROP_STYLE "__ns_style"

struct qjs_timer {
	struct qjs_timer *next;
	struct jsthread *thread;
	JSValue func;   /* JS_UNDEFINED when src is used */
	char *src;      /* string form of setTimeout */
	int32_t ms;
	bool repeat;
	uint32_t id;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static struct jsthread *qjs_thread(JSContext *ctx)
{
	return JS_GetContextOpaque(ctx);
}

static void qjs_navigate(struct jsthread *t, const char *url_s);
static bool qjs_node_is_media(dom_node *node);

static void qjs_ptr_key(char *buf, size_t buflen, const void *p)
{
	snprintf(buf, buflen, "%p", p);
}

/* dom_string to JS string; NULL becomes "" */
static JSValue qjs_ds_to_js(JSContext *ctx, dom_string *ds)
{
	if (ds == NULL)
		return JS_NewStringLen(ctx, "", 0);
	return JS_NewStringLen(ctx, dom_string_data(ds),
			       dom_string_length(ds));
}

/* JS value to new dom_string; caller unrefs. NULL on failure */
static dom_string *qjs_js_to_ds(JSContext *ctx, JSValueConst v)
{
	size_t len;
	const char *s = JS_ToCStringLen(ctx, &len, v);
	dom_string *ds = NULL;

	if (s == NULL)
		return NULL;
	if (dom_string_create((const uint8_t *)s, len, &ds) != DOM_NO_ERR)
		ds = NULL;
	JS_FreeCString(ctx, s);
	return ds;
}

static dom_node *qjs_this_node(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);

	if (t == NULL)
		return NULL;
	return JS_GetOpaque(this_val, t->heap->node_class);
}

static dom_event *qjs_this_event(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);

	if (t == NULL)
		return NULL;
	return JS_GetOpaque(this_val, t->heap->event_class);
}

void qjs_dump_error(JSContext *ctx)
{
	JSValue exc = JS_GetException(ctx);
	const char *msg = JS_ToCString(ctx, exc);

	const char *stack = NULL;
	JSValue sv = JS_UNDEFINED;

	if (JS_IsObject(exc)) {
		sv = JS_GetPropertyStr(ctx, exc, "stack");
		if (JS_IsString(sv))
			stack = JS_ToCString(ctx, sv);
	}

	NSLOG(jserrors, WARNING, "Uncaught error in JS: %s%s%.300s",
	      msg ? msg : "(unprintable)", stack ? " at " : "",
	      stack ? stack : "");

	if (stack != NULL)
		JS_FreeCString(ctx, stack);
	JS_FreeValue(ctx, sv);
	if (msg != NULL)
		JS_FreeCString(ctx, msg);
	JS_FreeValue(ctx, exc);
}

void qjs_run_jobs(JSContext *ctx)
{
	JSContext *jctx;

	while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &jctx) > 0)
		;
}

/* call a helper function defined by the setup script */
static JSValue
qjs_call_helper(JSContext *ctx, const char *name,
		int argc, JSValueConst *argv)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue fn = JS_GetPropertyStr(ctx, global, name);
	JSValue ret = JS_UNDEFINED;

	if (JS_IsFunction(ctx, fn)) {
		ret = JS_Call(ctx, fn, global, argc, argv);
		if (JS_IsException(ret)) {
			qjs_dump_error(ctx);
			ret = JS_UNDEFINED;
		}
	}
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, global);
	return ret;
}

/* ------------------------------------------------------------------ */
/* wrapper caches                                                     */
/* ------------------------------------------------------------------ */

JSValue qjs_dom_wrap_node(struct jsthread *t, struct dom_node *node)
{
	JSContext *ctx = t->ctx;
	char key[32];
	JSValue obj;
	JSValue proto;
	dom_node_type type = 0;

	if (node == NULL)
		return JS_NULL;

	qjs_ptr_key(key, sizeof(key), node);
	obj = JS_GetPropertyStr(ctx, t->node_map, key);
	if (JS_IsObject(obj))
		return obj;
	JS_FreeValue(ctx, obj);

	dom_node_get_node_type(node, &type);
	switch (type) {
	case DOM_DOCUMENT_NODE:
		proto = t->document_proto;
		break;
	case DOM_ELEMENT_NODE:
		proto = qjs_node_is_media(node) ?
			t->media_proto : t->element_proto;
		break;
	case DOM_TEXT_NODE:
	case DOM_CDATA_SECTION_NODE:
	case DOM_COMMENT_NODE:
		proto = t->text_proto;
		break;
	default:
		proto = t->node_proto;
		break;
	}

	obj = JS_NewObjectProtoClass(ctx, proto, t->heap->node_class);
	if (JS_IsException(obj))
		return obj;

	dom_node_ref(node);
	JS_SetOpaque(obj, node);
	JS_SetPropertyStr(ctx, t->node_map, key, JS_DupValue(ctx, obj));
	return obj;
}

JSValue qjs_dom_wrap_event(struct jsthread *t, struct dom_event *evt)
{
	JSContext *ctx = t->ctx;
	char key[32];
	JSValue obj;

	if (evt == NULL)
		return JS_NULL;

	qjs_ptr_key(key, sizeof(key), evt);
	obj = JS_GetPropertyStr(ctx, t->event_map, key);
	if (JS_IsObject(obj))
		return obj;
	JS_FreeValue(ctx, obj);

	obj = JS_NewObjectProtoClass(ctx, t->event_proto,
				     t->heap->event_class);
	if (JS_IsException(obj))
		return obj;

	dom_event_ref(evt);
	JS_SetOpaque(obj, evt);
	JS_SetPropertyStr(ctx, t->event_map, key, JS_DupValue(ctx, obj));
	return obj;
}

static void qjs_node_finalizer(JSRuntime *rt, JSValue val)
{
	struct jsheap *heap = JS_GetRuntimeOpaque(rt);
	dom_node *node;

	if (heap == NULL)
		return;
	node = JS_GetOpaque(val, heap->node_class);
	if (node != NULL)
		dom_node_unref(node);
}

static void qjs_event_finalizer(JSRuntime *rt, JSValue val)
{
	struct jsheap *heap = JS_GetRuntimeOpaque(rt);
	dom_event *evt;

	if (heap == NULL)
		return;
	evt = JS_GetOpaque(val, heap->event_class);
	if (evt != NULL)
		dom_event_unref(evt);
}

/* wrap a getter that produces a referenced node and unref it */
static JSValue qjs_wrap_and_unref(struct jsthread *t, dom_node *n)
{
	JSValue v = qjs_dom_wrap_node(t, n);

	if (n != NULL)
		dom_node_unref(n);
	return v;
}

/* ------------------------------------------------------------------ */
/* event listener plumbing                                            */
/* ------------------------------------------------------------------ */

static void qjs_generic_handler(dom_event *evt, void *pw)
{
	struct jsthread *t = pw;
	JSContext *ctx;
	dom_event_target *targ = NULL;
	dom_string *type = NULL;
	JSValue args[3];
	JSValue ret;

	if (t == NULL || t->closed)
		return;
	ctx = t->ctx;

	if (dom_event_get_current_target(evt, &targ) != DOM_NO_ERR ||
	    targ == NULL)
		return;
	if (dom_event_get_type(evt, &type) != DOM_NO_ERR || type == NULL) {
		dom_node_unref((dom_node *)targ);
		return;
	}

	args[0] = qjs_dom_wrap_node(t, (dom_node *)targ);
	args[1] = qjs_ds_to_js(ctx, type);
	args[2] = qjs_dom_wrap_event(t, evt);

	qjs_deadline_start(t);
	ret = qjs_call_helper(ctx, "__ns_call", 3, args);
	qjs_deadline_stop(t);

	if (JS_IsBool(ret) && !JS_ToBool(ctx, ret))
		dom_event_prevent_default(evt);

	JS_FreeValue(ctx, ret);
	JS_FreeValue(ctx, args[0]);
	JS_FreeValue(ctx, args[1]);
	JS_FreeValue(ctx, args[2]);

	qjs_run_jobs(ctx);

	dom_string_unref(type);
	dom_node_unref((dom_node *)targ);
}

/* ensure ONE libdom listener is registered for (node, type) */
static void
qjs_ensure_dom_listener(struct jsthread *t, JSValueConst node_js,
			dom_node *node, const char *type)
{
	JSContext *ctx = t->ctx;
	JSValue reg;
	JSValue seen;
	dom_string *tds = NULL;
	dom_event_listener *listener = NULL;

	reg = JS_GetPropertyStr(ctx, node_js, NSPROP_REGISTERED);
	if (!JS_IsObject(reg)) {
		JS_FreeValue(ctx, reg);
		reg = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, node_js, NSPROP_REGISTERED,
				  JS_DupValue(ctx, reg));
	}

	seen = JS_GetPropertyStr(ctx, reg, type);
	if (JS_ToBool(ctx, seen)) {
		JS_FreeValue(ctx, seen);
		JS_FreeValue(ctx, reg);
		return;
	}
	JS_FreeValue(ctx, seen);
	JS_SetPropertyStr(ctx, reg, type, JS_TRUE);
	JS_FreeValue(ctx, reg);

	if (dom_string_create((const uint8_t *)type, strlen(type),
			      &tds) != DOM_NO_ERR)
		return;

	if (dom_event_listener_create(qjs_generic_handler, t,
				      &listener) == DOM_NO_ERR) {
		dom_event_target_add_event_listener(
			(dom_event_target *)node, tds, listener, false);
		dom_event_listener_unref(listener);
	}
	dom_string_unref(tds);
}

/* shared by nodes and window; nodes also register with libdom */
static JSValue
qjs_addEventListener(JSContext *ctx, JSValueConst this_val,
		     int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	const char *type;
	JSValue args[3];
	JSValue ret;

	if (t == NULL || argc < 2 || !JS_IsFunction(ctx, argv[1]))
		return JS_UNDEFINED;

	type = JS_ToCString(ctx, argv[0]);
	if (type == NULL)
		return JS_UNDEFINED;

	args[0] = this_val;
	args[1] = argv[0];
	args[2] = argv[1];
	ret = qjs_call_helper(ctx, "__ns_addl", 3, args);
	JS_FreeValue(ctx, ret);

	if (node != NULL)
		qjs_ensure_dom_listener(t, this_val, node, type);

	JS_FreeCString(ctx, type);
	return JS_UNDEFINED;
}

static JSValue
qjs_removeEventListener(JSContext *ctx, JSValueConst this_val,
			int argc, JSValueConst *argv)
{
	JSValue args[3];
	JSValue ret;

	if (argc < 2)
		return JS_UNDEFINED;
	args[0] = this_val;
	args[1] = argv[0];
	args[2] = argv[1];
	ret = qjs_call_helper(ctx, "__ns_reml", 3, args);
	JS_FreeValue(ctx, ret);
	return JS_UNDEFINED;
}

static JSValue
qjs_dispatchEvent(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_event *evt;
	bool success = false;

	if (node == NULL || argc < 1)
		return JS_FALSE;
	evt = qjs_this_event(ctx, argv[0]);
	if (evt == NULL)
		return JS_FALSE;

	dom_event_target_dispatch_event((dom_event_target *)node, evt,
					&success);
	return JS_NewBool(ctx, success);
}

/* ------------------------------------------------------------------ */
/* Node prototype                                                     */
/* ------------------------------------------------------------------ */

static JSValue
node_get_nodeType(JSContext *ctx, JSValueConst this_val)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node_type type = 0;

	if (node == NULL)
		return JS_UNDEFINED;
	dom_node_get_node_type(node, &type);
	return JS_NewInt32(ctx, (int32_t)type);
}

static JSValue
node_get_nodeName(JSContext *ctx, JSValueConst this_val)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *name = NULL;
	JSValue ret;

	if (node == NULL)
		return JS_UNDEFINED;
	if (dom_node_get_node_name(node, &name) != DOM_NO_ERR)
		return JS_UNDEFINED;
	ret = qjs_ds_to_js(ctx, name);
	if (name != NULL)
		dom_string_unref(name);
	return ret;
}

static JSValue
node_get_nodeValue(JSContext *ctx, JSValueConst this_val)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *val = NULL;
	JSValue ret;

	if (node == NULL)
		return JS_UNDEFINED;
	if (dom_node_get_node_value(node, &val) != DOM_NO_ERR)
		return JS_NULL;
	if (val == NULL)
		return JS_NULL;
	ret = qjs_ds_to_js(ctx, val);
	dom_string_unref(val);
	return ret;
}

static JSValue
node_set_nodeValue(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *ds;

	if (node == NULL)
		return JS_UNDEFINED;
	ds = qjs_js_to_ds(ctx, v);
	if (ds != NULL) {
		dom_node_set_node_value(node, ds);
		dom_string_unref(ds);
	}
	return JS_UNDEFINED;
}

static JSValue
node_get_textContent(JSContext *ctx, JSValueConst this_val)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *val = NULL;
	JSValue ret;

	if (node == NULL)
		return JS_UNDEFINED;
	if (dom_node_get_text_content(node, &val) != DOM_NO_ERR)
		return JS_NULL;
	ret = qjs_ds_to_js(ctx, val);
	if (val != NULL)
		dom_string_unref(val);
	return ret;
}

static JSValue
node_set_textContent(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *ds;

	if (node == NULL)
		return JS_UNDEFINED;
	ds = qjs_js_to_ds(ctx, v);
	if (ds != NULL) {
		dom_node_set_text_content(node, ds);
		dom_string_unref(ds);
	}
	return JS_UNDEFINED;
}

#define NODE_NAV_GETTER(fname, domcall)					\
static JSValue								\
fname(JSContext *ctx, JSValueConst this_val)				\
{									\
	struct jsthread *t = qjs_thread(ctx);				\
	dom_node *node = qjs_this_node(ctx, this_val);			\
	dom_node *res = NULL;						\
									\
	if (t == NULL || node == NULL)					\
		return JS_UNDEFINED;					\
	if (domcall(node, &res) != DOM_NO_ERR)				\
		return JS_NULL;						\
	return qjs_wrap_and_unref(t, res);				\
}

NODE_NAV_GETTER(node_get_parentNode, dom_node_get_parent_node)
NODE_NAV_GETTER(node_get_firstChild, dom_node_get_first_child)
NODE_NAV_GETTER(node_get_lastChild, dom_node_get_last_child)
NODE_NAV_GETTER(node_get_nextSibling, dom_node_get_next_sibling)
NODE_NAV_GETTER(node_get_previousSibling, dom_node_get_previous_sibling)

static JSValue
node_get_ownerDocument(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_document *res = NULL;

	if (t == NULL || node == NULL)
		return JS_UNDEFINED;
	if (dom_node_get_owner_document(node, &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
node_get_parentElement(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node *res = NULL;
	dom_node_type type = 0;

	if (t == NULL || node == NULL)
		return JS_UNDEFINED;
	if (dom_node_get_parent_node(node, &res) != DOM_NO_ERR ||
	    res == NULL)
		return JS_NULL;
	dom_node_get_node_type(res, &type);
	if (type != DOM_ELEMENT_NODE) {
		dom_node_unref(res);
		return JS_NULL;
	}
	return qjs_wrap_and_unref(t, res);
}

/* snapshot array of children; elements_only filters to element nodes.
 * Live NodeList semantics are not provided. */
static JSValue
qjs_children_array(JSContext *ctx, dom_node *node, bool elements_only)
{
	struct jsthread *t = qjs_thread(ctx);
	JSValue arr = JS_NewArray(ctx);
	dom_node *child = NULL;
	uint32_t idx = 0;

	if (t == NULL || node == NULL)
		return arr;

	if (dom_node_get_first_child(node, &child) != DOM_NO_ERR)
		return arr;
	while (child != NULL) {
		dom_node *next = NULL;
		dom_node_type type = 0;

		dom_node_get_node_type(child, &type);
		if (!elements_only || type == DOM_ELEMENT_NODE) {
			JS_DefinePropertyValueUint32(
				ctx, arr, idx++,
				qjs_dom_wrap_node(t, child),
				JS_PROP_C_W_E);
		}
		if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR)
			next = NULL;
		dom_node_unref(child);
		child = next;
	}
	return arr;
}

static JSValue
node_get_childNodes(JSContext *ctx, JSValueConst this_val)
{
	return qjs_children_array(ctx, qjs_this_node(ctx, this_val), false);
}

static JSValue
node_appendChild(JSContext *ctx, JSValueConst this_val,
		 int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node *child;
	dom_node *res = NULL;

	if (t == NULL || node == NULL || argc < 1)
		return JS_UNDEFINED;
	child = qjs_this_node(ctx, argv[0]);
	if (child == NULL)
		return JS_UNDEFINED;
	if (dom_node_append_child(node, child, &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, res);
}

static JSValue
node_insertBefore(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node *child;
	dom_node *ref = NULL;
	dom_node *res = NULL;

	if (t == NULL || node == NULL || argc < 1)
		return JS_UNDEFINED;
	child = qjs_this_node(ctx, argv[0]);
	if (child == NULL)
		return JS_UNDEFINED;
	if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
		ref = qjs_this_node(ctx, argv[1]);
	if (dom_node_insert_before(node, child, ref, &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, res);
}

static JSValue
node_removeChild(JSContext *ctx, JSValueConst this_val,
		 int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node *child;
	dom_node *res = NULL;

	if (t == NULL || node == NULL || argc < 1)
		return JS_UNDEFINED;
	child = qjs_this_node(ctx, argv[0]);
	if (child == NULL)
		return JS_UNDEFINED;
	if (dom_node_remove_child(node, child, &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, res);
}

static JSValue
node_replaceChild(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node *newc;
	dom_node *oldc;
	dom_node *res = NULL;

	if (t == NULL || node == NULL || argc < 2)
		return JS_UNDEFINED;
	newc = qjs_this_node(ctx, argv[0]);
	oldc = qjs_this_node(ctx, argv[1]);
	if (newc == NULL || oldc == NULL)
		return JS_UNDEFINED;
	if (dom_node_replace_child(node, newc, oldc, &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, res);
}

static JSValue
node_cloneNode(JSContext *ctx, JSValueConst this_val,
	       int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node *res = NULL;
	bool deep = false;

	if (t == NULL || node == NULL)
		return JS_UNDEFINED;
	if (argc >= 1)
		deep = JS_ToBool(ctx, argv[0]);
	if (dom_node_clone_node(node, deep, &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, res);
}

static JSValue
node_hasChildNodes(JSContext *ctx, JSValueConst this_val,
		   int argc, JSValueConst *argv)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	bool has = false;

	if (node != NULL)
		dom_node_has_child_nodes(node, &has);
	return JS_NewBool(ctx, has);
}

static JSValue
node_contains(JSContext *ctx, JSValueConst this_val,
	      int argc, JSValueConst *argv)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_node *other;
	dom_node *walk;
	bool found = false;

	if (node == NULL || argc < 1)
		return JS_FALSE;
	other = qjs_this_node(ctx, argv[0]);
	if (other == NULL)
		return JS_FALSE;

	dom_node_ref(other);
	walk = other;
	while (walk != NULL) {
		dom_node *parent = NULL;

		if (walk == node) {
			found = true;
			dom_node_unref(walk);
			break;
		}
		if (dom_node_get_parent_node(walk, &parent) != DOM_NO_ERR)
			parent = NULL;
		dom_node_unref(walk);
		walk = parent;
	}
	return JS_NewBool(ctx, found);
}

static const JSCFunctionListEntry qjs_node_funcs[] = {
	JS_CGETSET_DEF("nodeType", node_get_nodeType, NULL),
	JS_CGETSET_DEF("nodeName", node_get_nodeName, NULL),
	JS_CGETSET_DEF("nodeValue", node_get_nodeValue, node_set_nodeValue),
	JS_CGETSET_DEF("textContent", node_get_textContent,
		       node_set_textContent),
	JS_CGETSET_DEF("parentNode", node_get_parentNode, NULL),
	JS_CGETSET_DEF("parentElement", node_get_parentElement, NULL),
	JS_CGETSET_DEF("firstChild", node_get_firstChild, NULL),
	JS_CGETSET_DEF("lastChild", node_get_lastChild, NULL),
	JS_CGETSET_DEF("nextSibling", node_get_nextSibling, NULL),
	JS_CGETSET_DEF("previousSibling", node_get_previousSibling, NULL),
	JS_CGETSET_DEF("ownerDocument", node_get_ownerDocument, NULL),
	JS_CGETSET_DEF("childNodes", node_get_childNodes, NULL),
	JS_CFUNC_DEF("appendChild", 1, node_appendChild),
	JS_CFUNC_DEF("insertBefore", 2, node_insertBefore),
	JS_CFUNC_DEF("removeChild", 1, node_removeChild),
	JS_CFUNC_DEF("replaceChild", 2, node_replaceChild),
	JS_CFUNC_DEF("cloneNode", 1, node_cloneNode),
	JS_CFUNC_DEF("hasChildNodes", 0, node_hasChildNodes),
	JS_CFUNC_DEF("contains", 1, node_contains),
	JS_CFUNC_DEF("addEventListener", 2, qjs_addEventListener),
	JS_CFUNC_DEF("removeEventListener", 2, qjs_removeEventListener),
	JS_CFUNC_DEF("dispatchEvent", 1, qjs_dispatchEvent),
};

/* ------------------------------------------------------------------ */
/* selector walker                                                    */
/* ------------------------------------------------------------------ */

enum qjs_sel_kind {
	QJS_SEL_TAG,
	QJS_SEL_CLASS,
	QJS_SEL_ID,
	QJS_SEL_BAD
};

/* does class attribute value contain token (space separated) */
static bool qjs_class_contains(dom_string *cls, const char *token)
{
	const char *data;
	size_t len, tlen, i;

	if (cls == NULL)
		return false;
	data = dom_string_data(cls);
	len = dom_string_length(cls);
	tlen = strlen(token);

	for (i = 0; i + tlen <= len; i++) {
		if ((i == 0 || data[i - 1] == ' ' || data[i - 1] == '\t' ||
		     data[i - 1] == '\n') &&
		    strncasecmp(data + i, token, tlen) == 0 &&
		    (i + tlen == len || data[i + tlen] == ' ' ||
		     data[i + tlen] == '\t' || data[i + tlen] == '\n'))
			return true;
	}
	return false;
}

static bool
qjs_el_matches(dom_node *el, enum qjs_sel_kind kind, const char *arg)
{
	dom_string *val = NULL;
	bool match = false;

	switch (kind) {
	case QJS_SEL_TAG:
		if (strcmp(arg, "*") == 0)
			return true;
		if (dom_node_get_node_name(el, &val) != DOM_NO_ERR ||
		    val == NULL)
			return false;
		match = (dom_string_length(val) == strlen(arg)) &&
			(strncasecmp(dom_string_data(val), arg,
				     strlen(arg)) == 0);
		dom_string_unref(val);
		return match;

	case QJS_SEL_CLASS:
		if (dom_element_get_attribute((dom_element *)el,
					      corestring_dom_class,
					      &val) != DOM_NO_ERR)
			return false;
		match = qjs_class_contains(val, arg);
		if (val != NULL)
			dom_string_unref(val);
		return match;

	case QJS_SEL_ID:
		if (dom_element_get_attribute((dom_element *)el,
					      corestring_dom_id,
					      &val) != DOM_NO_ERR ||
		    val == NULL)
			return false;
		match = (dom_string_length(val) == strlen(arg)) &&
			(strncmp(dom_string_data(val), arg,
				 strlen(arg)) == 0);
		dom_string_unref(val);
		return match;

	default:
		return false;
	}
}

/* depth first walk collecting matching elements below root */
static void
qjs_walk_collect(struct jsthread *t, dom_node *root,
		 enum qjs_sel_kind kind, const char *arg,
		 JSValue arr, uint32_t *count,
		 bool first_only, dom_node **first)
{
	dom_node *child = NULL;

	if (first_only && *first != NULL)
		return;

	if (dom_node_get_first_child(root, &child) != DOM_NO_ERR)
		return;

	while (child != NULL) {
		dom_node *next = NULL;
		dom_node_type type = 0;

		dom_node_get_node_type(child, &type);
		if (type == DOM_ELEMENT_NODE &&
		    qjs_el_matches(child, kind, arg)) {
			if (first_only) {
				*first = child; /* transfer this ref */
				return;
			}
			JS_DefinePropertyValueUint32(
				t->ctx, arr, (*count)++,
				qjs_dom_wrap_node(t, child),
				JS_PROP_C_W_E);
		}

		if (type == DOM_ELEMENT_NODE) {
			qjs_walk_collect(t, child, kind, arg, arr, count,
					 first_only, first);
			if (first_only && *first != NULL) {
				dom_node_unref(child);
				return;
			}
		}

		if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR)
			next = NULL;
		dom_node_unref(child);
		child = next;
	}
}

/* parse the supported simple selector forms */
static enum qjs_sel_kind qjs_parse_selector(const char *sel, const char **arg)
{
	while (*sel == ' ' || *sel == '\t')
		sel++;
	if (*sel == '\0')
		return QJS_SEL_BAD;
	if (strpbrk(sel, " \t>[:,+~") != NULL)
		return QJS_SEL_BAD; /* combinators etc not supported */
	if (*sel == '#') {
		*arg = sel + 1;
		return QJS_SEL_ID;
	}
	if (*sel == '.') {
		*arg = sel + 1;
		return QJS_SEL_CLASS;
	}
	*arg = sel;
	return QJS_SEL_TAG;
}

static JSValue
qjs_select(JSContext *ctx, JSValueConst this_val,
	   int argc, JSValueConst *argv, bool first_only,
	   enum qjs_sel_kind forced_kind)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *root = qjs_this_node(ctx, this_val);
	const char *sel = NULL;
	const char *arg = NULL;
	enum qjs_sel_kind kind;
	JSValue ret;
	uint32_t count = 0;
	dom_node *first = NULL;

	if (t == NULL || root == NULL || argc < 1)
		return first_only ? JS_NULL : JS_NewArray(ctx);

	sel = JS_ToCString(ctx, argv[0]);
	if (sel == NULL)
		return first_only ? JS_NULL : JS_NewArray(ctx);

	if (forced_kind != QJS_SEL_BAD) {
		kind = forced_kind;
		arg = sel;
	} else {
		kind = qjs_parse_selector(sel, &arg);
	}

	if (kind == QJS_SEL_BAD) {
		NSLOG(netsurf, DEBUG, "unsupported selector '%s'", sel);
		JS_FreeCString(ctx, sel);
		return first_only ? JS_NULL : JS_NewArray(ctx);
	}

	ret = first_only ? JS_NULL : JS_NewArray(ctx);
	qjs_walk_collect(t, root, kind, arg,
			 first_only ? JS_UNDEFINED : ret,
			 &count, first_only, &first);
	if (first_only && first != NULL)
		ret = qjs_wrap_and_unref(t, first);

	JS_FreeCString(ctx, sel);
	return ret;
}

static JSValue
el_querySelector(JSContext *ctx, JSValueConst this_val,
		 int argc, JSValueConst *argv)
{
	return qjs_select(ctx, this_val, argc, argv, true, QJS_SEL_BAD);
}

static JSValue
el_querySelectorAll(JSContext *ctx, JSValueConst this_val,
		    int argc, JSValueConst *argv)
{
	return qjs_select(ctx, this_val, argc, argv, false, QJS_SEL_BAD);
}

static JSValue
el_getElementsByTagName(JSContext *ctx, JSValueConst this_val,
			int argc, JSValueConst *argv)
{
	return qjs_select(ctx, this_val, argc, argv, false, QJS_SEL_TAG);
}

static JSValue
el_getElementsByClassName(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv)
{
	return qjs_select(ctx, this_val, argc, argv, false, QJS_SEL_CLASS);
}

/* ------------------------------------------------------------------ */
/* Element prototype                                                  */
/* ------------------------------------------------------------------ */

static JSValue
el_get_tagName(JSContext *ctx, JSValueConst this_val)
{
	return node_get_nodeName(ctx, this_val);
}

static JSValue
qjs_attr_get(JSContext *ctx, JSValueConst this_val, dom_string *name)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *val = NULL;
	JSValue ret;

	if (node == NULL)
		return JS_UNDEFINED;
	if (dom_element_get_attribute((dom_element *)node, name,
				      &val) != DOM_NO_ERR || val == NULL)
		return JS_NewStringLen(ctx, "", 0);
	ret = qjs_ds_to_js(ctx, val);
	dom_string_unref(val);
	return ret;
}

static JSValue
qjs_attr_set(JSContext *ctx, JSValueConst this_val, dom_string *name,
	     JSValueConst v)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *ds;

	if (node == NULL)
		return JS_UNDEFINED;
	ds = qjs_js_to_ds(ctx, v);
	if (ds != NULL) {
		dom_element_set_attribute((dom_element *)node, name, ds);
		dom_string_unref(ds);
	}
	return JS_UNDEFINED;
}

static JSValue
el_get_id(JSContext *ctx, JSValueConst this_val)
{
	return qjs_attr_get(ctx, this_val, corestring_dom_id);
}

static JSValue
el_set_id(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	return qjs_attr_set(ctx, this_val, corestring_dom_id, v);
}

static JSValue
el_get_className(JSContext *ctx, JSValueConst this_val)
{
	return qjs_attr_get(ctx, this_val, corestring_dom_class);
}

static JSValue
el_set_className(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	return qjs_attr_set(ctx, this_val, corestring_dom_class, v);
}

static JSValue
el_getAttribute(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *name;
	dom_string *val = NULL;
	JSValue ret;

	if (node == NULL || argc < 1)
		return JS_NULL;
	name = qjs_js_to_ds(ctx, argv[0]);
	if (name == NULL)
		return JS_NULL;
	if (dom_element_get_attribute((dom_element *)node, name,
				      &val) != DOM_NO_ERR || val == NULL) {
		dom_string_unref(name);
		return JS_NULL;
	}
	ret = qjs_ds_to_js(ctx, val);
	dom_string_unref(val);
	dom_string_unref(name);
	return ret;
}

static JSValue
el_setAttribute(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *name;
	dom_string *val;

	if (node == NULL || argc < 2)
		return JS_UNDEFINED;
	name = qjs_js_to_ds(ctx, argv[0]);
	val = qjs_js_to_ds(ctx, argv[1]);
	if (name != NULL && val != NULL)
		dom_element_set_attribute((dom_element *)node, name, val);
	if (name != NULL)
		dom_string_unref(name);
	if (val != NULL)
		dom_string_unref(val);
	return JS_UNDEFINED;
}

static JSValue
el_removeAttribute(JSContext *ctx, JSValueConst this_val,
		   int argc, JSValueConst *argv)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *name;

	if (node == NULL || argc < 1)
		return JS_UNDEFINED;
	name = qjs_js_to_ds(ctx, argv[0]);
	if (name != NULL) {
		dom_element_remove_attribute((dom_element *)node, name);
		dom_string_unref(name);
	}
	return JS_UNDEFINED;
}

static JSValue
el_hasAttribute(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *name;
	bool has = false;

	if (node == NULL || argc < 1)
		return JS_FALSE;
	name = qjs_js_to_ds(ctx, argv[0]);
	if (name != NULL) {
		dom_element_has_attribute((dom_element *)node, name, &has);
		dom_string_unref(name);
	}
	return JS_NewBool(ctx, has);
}

static JSValue
el_get_children(JSContext *ctx, JSValueConst this_val)
{
	return qjs_children_array(ctx, qjs_this_node(ctx, this_val), true);
}

static JSValue
el_get_innerHTML(JSContext *ctx, JSValueConst this_val)
{
	/* parity with the duktape backend, which also returns "" */
	return JS_NewStringLen(ctx, "", 0);
}

/* replace this element's children with a parsed HTML fragment;
 * same technique as the duktape Element.bnd setter */
static JSValue
el_set_innerHTML(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	size_t size;
	const char *s;
	dom_hubbub_parser_params parse_params;
	dom_hubbub_error error;
	dom_hubbub_parser *parser = NULL;
	struct dom_document *doc = NULL;
	struct dom_document_fragment *fragment = NULL;
	dom_exception exc;
	struct dom_node *child = NULL, *html = NULL, *body = NULL;
	struct dom_nodelist *bodies = NULL;

	if (node == NULL)
		return JS_UNDEFINED;
	s = JS_ToCStringLen(ctx, &size, v);
	if (s == NULL)
		return JS_UNDEFINED;

	exc = dom_node_get_owner_document(node, &doc);
	if (exc != DOM_NO_ERR)
		goto out;

	parse_params.enc = "UTF-8";
	parse_params.fix_enc = true;
	parse_params.enable_script = false;
	parse_params.msg = NULL;
	parse_params.script = NULL;
	parse_params.ctx = NULL;
	parse_params.daf = NULL;

	error = dom_hubbub_fragment_parser_create(&parse_params, doc,
						  &parser, &fragment);
	if (error != DOM_HUBBUB_OK) {
		NSLOG(netsurf, ERROR, "innerHTML: no fragment parser");
		goto out;
	}
	error = dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)s,
					      size);
	if (error != DOM_HUBBUB_OK)
		goto out;
	error = dom_hubbub_parser_completed(parser);
	if (error != DOM_HUBBUB_OK)
		goto out;

	/* empty this node */
	exc = dom_node_get_first_child(node, &child);
	if (exc != DOM_NO_ERR)
		goto out;
	while (child != NULL) {
		struct dom_node *cref;

		exc = dom_node_remove_child(node, child, &cref);
		if (exc != DOM_NO_ERR)
			goto out;
		dom_node_unref(child);
		child = NULL;
		dom_node_unref(cref);
		exc = dom_node_get_first_child(node, &child);
		if (exc != DOM_NO_ERR)
			goto out;
	}

	/* hubbub wraps the fragment in html/body; migrate body's kids */
	exc = dom_node_get_first_child(fragment, &html);
	if (exc != DOM_NO_ERR)
		goto out;
	exc = dom_element_get_elements_by_tag_name(html,
						   corestring_dom_BODY,
						   &bodies);
	if (exc != DOM_NO_ERR)
		goto out;
	exc = dom_nodelist_item(bodies, 0, &body);
	if (exc != DOM_NO_ERR)
		goto out;

	exc = dom_node_get_first_child(body, &child);
	if (exc != DOM_NO_ERR)
		goto out;
	while (child != NULL) {
		struct dom_node *cref;

		exc = dom_node_remove_child(body, child, &cref);
		if (exc != DOM_NO_ERR)
			goto out;
		dom_node_unref(cref);
		exc = dom_node_append_child(node, child, &cref);
		if (exc != DOM_NO_ERR)
			goto out;
		dom_node_unref(cref);
		dom_node_unref(child);
		child = NULL;
		exc = dom_node_get_first_child(body, &child);
		if (exc != DOM_NO_ERR)
			goto out;
	}

out:
	if (parser != NULL)
		dom_hubbub_parser_destroy(parser);
	if (doc != NULL)
		dom_node_unref(doc);
	if (fragment != NULL)
		dom_node_unref(fragment);
	if (child != NULL)
		dom_node_unref(child);
	if (html != NULL)
		dom_node_unref(html);
	if (bodies != NULL)
		dom_nodelist_unref(bodies);
	if (body != NULL)
		dom_node_unref(body);
	JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
}

/* element.style: inert per-element store with the CSSOM entry points;
 * NetSurf cannot restyle after layout so writes are not rendered */
static JSValue
style_setProperty(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	JSAtom atom;

	if (argc < 2)
		return JS_UNDEFINED;
	atom = JS_ValueToAtom(ctx, argv[0]);
	JS_SetProperty(ctx, this_val, atom, JS_DupValue(ctx, argv[1]));
	JS_FreeAtom(ctx, atom);
	return JS_UNDEFINED;
}

static JSValue
style_getPropertyValue(JSContext *ctx, JSValueConst this_val,
		       int argc, JSValueConst *argv)
{
	JSAtom atom;
	JSValue v;

	if (argc < 1)
		return JS_NewStringLen(ctx, "", 0);
	atom = JS_ValueToAtom(ctx, argv[0]);
	v = JS_GetProperty(ctx, this_val, atom);
	JS_FreeAtom(ctx, atom);
	if (JS_IsUndefined(v)) {
		JS_FreeValue(ctx, v);
		return JS_NewStringLen(ctx, "", 0);
	}
	return v;
}

static JSValue
style_removeProperty(JSContext *ctx, JSValueConst this_val,
		     int argc, JSValueConst *argv)
{
	JSAtom atom;

	if (argc < 1)
		return JS_UNDEFINED;
	atom = JS_ValueToAtom(ctx, argv[0]);
	JS_DeleteProperty(ctx, this_val, atom, 0);
	JS_FreeAtom(ctx, atom);
	return JS_UNDEFINED;
}

static JSValue
el_get_style(JSContext *ctx, JSValueConst this_val)
{
	JSValue style = JS_GetPropertyStr(ctx, this_val, NSPROP_STYLE);

	if (JS_IsObject(style))
		return style;
	JS_FreeValue(ctx, style);

	style = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, style, "setProperty",
			  JS_NewCFunction(ctx, style_setProperty,
					  "setProperty", 2));
	JS_SetPropertyStr(ctx, style, "getPropertyValue",
			  JS_NewCFunction(ctx, style_getPropertyValue,
					  "getPropertyValue", 1));
	JS_SetPropertyStr(ctx, style, "removeProperty",
			  JS_NewCFunction(ctx, style_removeProperty,
					  "removeProperty", 1));
	JS_SetPropertyStr(ctx, this_val, NSPROP_STYLE,
			  JS_DupValue(ctx, style));
	return style;
}

static const JSCFunctionListEntry qjs_element_funcs[] = {
	JS_CGETSET_DEF("tagName", el_get_tagName, NULL),
	JS_CGETSET_DEF("id", el_get_id, el_set_id),
	JS_CGETSET_DEF("className", el_get_className, el_set_className),
	JS_CGETSET_DEF("children", el_get_children, NULL),
	JS_CGETSET_DEF("innerHTML", el_get_innerHTML, el_set_innerHTML),
	JS_CGETSET_DEF("style", el_get_style, NULL),
	JS_CFUNC_DEF("getAttribute", 1, el_getAttribute),
	JS_CFUNC_DEF("setAttribute", 2, el_setAttribute),
	JS_CFUNC_DEF("removeAttribute", 1, el_removeAttribute),
	JS_CFUNC_DEF("hasAttribute", 1, el_hasAttribute),
	JS_CFUNC_DEF("querySelector", 1, el_querySelector),
	JS_CFUNC_DEF("querySelectorAll", 1, el_querySelectorAll),
	JS_CFUNC_DEF("getElementsByTagName", 1, el_getElementsByTagName),
	JS_CFUNC_DEF("getElementsByClassName", 1,
		     el_getElementsByClassName),
};

/* ------------------------------------------------------------------ */
/* HTMLMediaElement (<audio>/<video>) and Audio()                     */
/*                                                                    */
/* NetSurf has no media element rendering, so these are a thin shim:  */
/* play() hands the resolved src to the browser as a navigation,      */
/* which NetSurf routes to its download path, where the Switch        */
/* frontend's media intercept plays it fullscreen (spooling https to  */
/* the SD first).  The other members are enough state for typical     */
/* player scripts (paused/volume/currentTime) not to throw before     */
/* they reach play().  Playback itself is fire-and-forget: NetSurf's  */
/* player is modal, so timeupdate/ended events are not delivered.     */
/* ------------------------------------------------------------------ */

static bool qjs_node_is_media(dom_node *node)
{
	dom_string *name = NULL;
	bool media = false;

	if (dom_node_get_node_name(node, &name) != DOM_NO_ERR ||
	    name == NULL)
		return false;
	if (dom_string_length(name) == 5)
		media = (strncasecmp(dom_string_data(name), "AUDIO",
				     5) == 0) ||
			(strncasecmp(dom_string_data(name), "VIDEO",
				     5) == 0);
	dom_string_unref(name);
	return media;
}

/* resolved absolute src, or NULL; caller frees with nsurl_unref */
static nsurl *qjs_media_src_url(struct jsthread *t, dom_node *node)
{
	dom_string *src = NULL;
	nsurl *base;
	nsurl *abs = NULL;

	if (t->bw == NULL)
		return NULL;
	if (dom_element_get_attribute((dom_element *)node,
				      corestring_dom_src,
				      &src) != DOM_NO_ERR || src == NULL)
		return NULL;

	base = browser_window_access_url(t->bw);
	if (base != NULL)
		nsurl_join(base, dom_string_data(src), &abs);
	dom_string_unref(src);
	return abs;
}

static JSValue qjs_resolved_promise(JSContext *ctx)
{
	JSValue funcs[2];
	JSValue promise = JS_NewPromiseCapability(ctx, funcs);
	JSValue r;

	if (JS_IsException(promise))
		return JS_UNDEFINED;
	r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 0, NULL);
	JS_FreeValue(ctx, r);
	JS_FreeValue(ctx, funcs[0]);
	JS_FreeValue(ctx, funcs[1]);
	return promise;
}

static JSValue
media_play(JSContext *ctx, JSValueConst this_val,
	   int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	nsurl *url;

	if (t == NULL || node == NULL || t->closed)
		return qjs_resolved_promise(ctx);

	url = qjs_media_src_url(t, node);
	if (url != NULL) {
		NSLOG(netsurf, INFO, "media play(): %s", nsurl_access(url));
		/* NetSurf's download intercept plays it; the frontend
		 * defers the actual player launch off this call stack */
		browser_window_navigate(t->bw, url, NULL,
					BW_NAVIGATE_DOWNLOAD, NULL, NULL,
					NULL);
		nsurl_unref(url);
		JS_SetPropertyStr(ctx, this_val, "__ns_paused", JS_FALSE);
	} else {
		NSLOG(netsurf, INFO, "media play(): no src");
	}
	return qjs_resolved_promise(ctx);
}

static JSValue
media_pause(JSContext *ctx, JSValueConst this_val,
	    int argc, JSValueConst *argv)
{
	/* the modal player is exited with B/Plus, not from script */
	JS_SetPropertyStr(ctx, this_val, "__ns_paused", JS_TRUE);
	return JS_UNDEFINED;
}

static JSValue
media_load(JSContext *ctx, JSValueConst this_val,
	   int argc, JSValueConst *argv)
{
	return JS_UNDEFINED;
}

static JSValue
media_canPlayType(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	const char *type;
	JSValue ret;

	if (argc < 1)
		return JS_NewString(ctx, "");
	type = JS_ToCString(ctx, argv[0]);
	if (type == NULL)
		return JS_NewString(ctx, "");
	/* ffmpeg-backed player is broad; claim "maybe" for audio/video */
	ret = JS_NewString(ctx,
			   (strncmp(type, "audio/", 6) == 0 ||
			    strncmp(type, "video/", 6) == 0) ?
				   "maybe" : "");
	JS_FreeCString(ctx, type);
	return ret;
}

static JSValue
media_get_src(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	nsurl *url;
	JSValue ret;

	if (t == NULL || node == NULL)
		return JS_NewString(ctx, "");
	url = qjs_media_src_url(t, node);
	if (url == NULL)
		return JS_NewString(ctx, "");
	ret = JS_NewString(ctx, nsurl_access(url));
	nsurl_unref(url);
	return ret;
}

static JSValue
media_set_src(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	return qjs_attr_set(ctx, this_val, corestring_dom_src, v);
}

static JSValue
media_get_paused(JSContext *ctx, JSValueConst this_val)
{
	JSValue p = JS_GetPropertyStr(ctx, this_val, "__ns_paused");

	if (JS_IsUndefined(p)) {
		JS_FreeValue(ctx, p);
		return JS_TRUE; /* default state is paused */
	}
	return p;
}

/* simple JS-backed scalar property with a default */
#define MEDIA_STORED_PROP(getname, setname, storekey, defexpr)		\
static JSValue								\
getname(JSContext *ctx, JSValueConst this_val)				\
{									\
	JSValue v = JS_GetPropertyStr(ctx, this_val, storekey);		\
	if (JS_IsUndefined(v)) {					\
		JS_FreeValue(ctx, v);					\
		return (defexpr);					\
	}								\
	return v;							\
}									\
static JSValue								\
setname(JSContext *ctx, JSValueConst this_val, JSValueConst v)		\
{									\
	JS_SetPropertyStr(ctx, this_val, storekey, JS_DupValue(ctx, v));	\
	return JS_UNDEFINED;						\
}

MEDIA_STORED_PROP(media_get_volume, media_set_volume, "__ns_volume",
		  JS_NewFloat64(ctx, 1.0))
MEDIA_STORED_PROP(media_get_currentTime, media_set_currentTime,
		  "__ns_curtime", JS_NewFloat64(ctx, 0.0))
MEDIA_STORED_PROP(media_get_muted, media_set_muted, "__ns_muted",
		  JS_FALSE)
MEDIA_STORED_PROP(media_get_loop, media_set_loop, "__ns_loop", JS_FALSE)
MEDIA_STORED_PROP(media_get_autoplay, media_set_autoplay, "__ns_autoplay",
		  JS_FALSE)

static JSValue
media_get_duration(JSContext *ctx, JSValueConst this_val)
{
	return JS_NewFloat64(ctx, 0.0); /* unknown; NaN upsets some scripts */
}

static JSValue
media_get_ended(JSContext *ctx, JSValueConst this_val)
{
	return JS_FALSE;
}

static JSValue
media_get_readyState(JSContext *ctx, JSValueConst this_val)
{
	return JS_NewInt32(ctx, 4); /* HAVE_ENOUGH_DATA */
}

static JSValue
media_get_networkState(JSContext *ctx, JSValueConst this_val)
{
	return JS_NewInt32(ctx, 1); /* NETWORK_IDLE */
}

static const JSCFunctionListEntry qjs_media_funcs[] = {
	JS_CFUNC_DEF("play", 0, media_play),
	JS_CFUNC_DEF("pause", 0, media_pause),
	JS_CFUNC_DEF("load", 0, media_load),
	JS_CFUNC_DEF("canPlayType", 1, media_canPlayType),
	JS_CGETSET_DEF("src", media_get_src, media_set_src),
	JS_CGETSET_DEF("currentSrc", media_get_src, NULL),
	JS_CGETSET_DEF("paused", media_get_paused, NULL),
	JS_CGETSET_DEF("volume", media_get_volume, media_set_volume),
	JS_CGETSET_DEF("currentTime", media_get_currentTime,
		       media_set_currentTime),
	JS_CGETSET_DEF("muted", media_get_muted, media_set_muted),
	JS_CGETSET_DEF("loop", media_get_loop, media_set_loop),
	JS_CGETSET_DEF("autoplay", media_get_autoplay, media_set_autoplay),
	JS_CGETSET_DEF("duration", media_get_duration, NULL),
	JS_CGETSET_DEF("ended", media_get_ended, NULL),
	JS_CGETSET_DEF("readyState", media_get_readyState, NULL),
	JS_CGETSET_DEF("networkState", media_get_networkState, NULL),
};

/* ------------------------------------------------------------------ */
/* Text prototype                                                     */
/* ------------------------------------------------------------------ */

static JSValue
text_get_data(JSContext *ctx, JSValueConst this_val)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *val = NULL;
	JSValue ret;

	if (node == NULL)
		return JS_UNDEFINED;
	if (dom_characterdata_get_data((dom_characterdata *)node,
				       &val) != DOM_NO_ERR)
		return JS_NULL;
	ret = qjs_ds_to_js(ctx, val);
	if (val != NULL)
		dom_string_unref(val);
	return ret;
}

static JSValue
text_set_data(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *ds;

	if (node == NULL)
		return JS_UNDEFINED;
	ds = qjs_js_to_ds(ctx, v);
	if (ds != NULL) {
		dom_characterdata_set_data((dom_characterdata *)node, ds);
		dom_string_unref(ds);
	}
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry qjs_text_funcs[] = {
	JS_CGETSET_DEF("data", text_get_data, text_set_data),
};

/* ------------------------------------------------------------------ */
/* Document prototype                                                 */
/* ------------------------------------------------------------------ */

static JSValue
doc_get_documentElement(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_element *res = NULL;

	if (t == NULL || node == NULL)
		return JS_UNDEFINED;
	if (dom_document_get_document_element((dom_document *)node,
					      &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
doc_get_body(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_html_element *res = NULL;

	if (t == NULL || node == NULL)
		return JS_UNDEFINED;
	if (dom_html_document_get_body((dom_html_document *)node,
				       &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
doc_get_head(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_element *root = NULL;
	dom_node *child = NULL;
	JSValue ret = JS_NULL;

	if (t == NULL || node == NULL)
		return JS_UNDEFINED;
	if (dom_document_get_document_element((dom_document *)node,
					      &root) != DOM_NO_ERR ||
	    root == NULL)
		return JS_NULL;

	if (dom_node_get_first_child(root, &child) == DOM_NO_ERR) {
		while (child != NULL) {
			dom_node *next = NULL;
			dom_string *name = NULL;

			if (dom_node_get_node_name(child, &name) ==
				    DOM_NO_ERR && name != NULL) {
				bool is_head =
					(dom_string_length(name) == 4) &&
					(strncasecmp(dom_string_data(name),
						     "HEAD", 4) == 0);
				dom_string_unref(name);
				if (is_head) {
					ret = qjs_wrap_and_unref(t, child);
					child = NULL;
					break;
				}
			}
			if (dom_node_get_next_sibling(child, &next) !=
			    DOM_NO_ERR)
				next = NULL;
			dom_node_unref(child);
			child = next;
		}
	}
	dom_node_unref(root);
	return ret;
}

static JSValue
doc_get_title(JSContext *ctx, JSValueConst this_val)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *title = NULL;
	JSValue ret;

	if (node == NULL)
		return JS_UNDEFINED;
	if (dom_html_document_get_title((dom_html_document *)node,
					&title) != DOM_NO_ERR)
		return JS_NewStringLen(ctx, "", 0);
	ret = qjs_ds_to_js(ctx, title);
	if (title != NULL)
		dom_string_unref(title);
	return ret;
}

static JSValue
doc_set_title(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *ds;

	if (node == NULL)
		return JS_UNDEFINED;
	ds = qjs_js_to_ds(ctx, v);
	if (ds != NULL) {
		dom_html_document_set_title((dom_html_document *)node, ds);
		dom_string_unref(ds);
	}
	return JS_UNDEFINED;
}

static JSValue
doc_get_URL(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	struct nsurl *url;

	if (t == NULL || t->bw == NULL)
		return JS_NewStringLen(ctx, "", 0);
	url = browser_window_access_url(t->bw);
	return JS_NewString(ctx, url ? nsurl_access(url) : "");
}

static JSValue
doc_get_readyState(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	static const char *states[] = { "loading", "interactive", "complete" };
	return JS_NewString(ctx, states[t != NULL ? t->ready_state % 3 : 2]);
}

static JSValue
doc_get_cookie(JSContext *ctx, JSValueConst this_val)
{
	return JS_NewStringLen(ctx, "", 0);
}

static JSValue
doc_set_cookie(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	NSLOG(netsurf, DEBUG, "document.cookie write ignored");
	return JS_UNDEFINED;
}

static JSValue
doc_get_defaultView(JSContext *ctx, JSValueConst this_val)
{
	return JS_GetGlobalObject(ctx);
}

static JSValue
doc_get_location(JSContext *ctx, JSValueConst this_val)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue loc = JS_GetPropertyStr(ctx, global, "__ns_location");

	JS_FreeValue(ctx, global);
	return loc;
}

/* "location = url" and "document.location = url" must navigate; the
 * DDG result redirect page relies on exactly that idiom */
static JSValue
doc_set_location(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	struct jsthread *t = qjs_thread(ctx);
	const char *s = JS_ToCString(ctx, v);

	if (t != NULL && s != NULL)
		qjs_navigate(t, s);
	if (s != NULL)
		JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
}

static JSValue
doc_getElementById(JSContext *ctx, JSValueConst this_val,
		   int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *id;
	dom_element *res = NULL;

	if (t == NULL || node == NULL || argc < 1)
		return JS_NULL;
	id = qjs_js_to_ds(ctx, argv[0]);
	if (id == NULL)
		return JS_NULL;
	if (dom_document_get_element_by_id((dom_document *)node, id,
					   &res) != DOM_NO_ERR ||
	    res == NULL) {
		dom_string_unref(id);
		return JS_NULL;
	}
	dom_string_unref(id);
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
doc_createElement(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *tag;
	dom_element *res = NULL;

	if (t == NULL || node == NULL || argc < 1)
		return JS_NULL;
	tag = qjs_js_to_ds(ctx, argv[0]);
	if (tag == NULL)
		return JS_NULL;
	if (dom_document_create_element((dom_document *)node, tag,
					&res) != DOM_NO_ERR) {
		dom_string_unref(tag);
		return JS_NULL;
	}
	dom_string_unref(tag);
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
doc_createTextNode(JSContext *ctx, JSValueConst this_val,
		   int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *data;
	dom_text *res = NULL;

	if (t == NULL || node == NULL || argc < 1)
		return JS_NULL;
	data = qjs_js_to_ds(ctx, argv[0]);
	if (data == NULL)
		return JS_NULL;
	if (dom_document_create_text_node((dom_document *)node, data,
					  &res) != DOM_NO_ERR) {
		dom_string_unref(data);
		return JS_NULL;
	}
	dom_string_unref(data);
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
doc_createComment(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_string *data;
	dom_comment *res = NULL;

	if (t == NULL || node == NULL || argc < 1)
		return JS_NULL;
	data = qjs_js_to_ds(ctx, argv[0]);
	if (data == NULL)
		return JS_NULL;
	if (dom_document_create_comment((dom_document *)node, data,
					&res) != DOM_NO_ERR) {
		dom_string_unref(data);
		return JS_NULL;
	}
	dom_string_unref(data);
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
doc_createDocumentFragment(JSContext *ctx, JSValueConst this_val,
			   int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_node *node = qjs_this_node(ctx, this_val);
	dom_document_fragment *res = NULL;

	if (t == NULL || node == NULL)
		return JS_NULL;
	if (dom_document_create_document_fragment((dom_document *)node,
						  &res) != DOM_NO_ERR)
		return JS_NULL;
	return qjs_wrap_and_unref(t, (dom_node *)res);
}

static JSValue
doc_createEvent(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_event *evt = NULL;
	JSValue ret;

	if (t == NULL)
		return JS_NULL;
	if (dom_event_create(&evt) != DOM_NO_ERR)
		return JS_NULL;
	ret = qjs_dom_wrap_event(t, evt);
	dom_event_unref(evt); /* wrapper holds its own ref */
	return ret;
}

static JSValue
doc_write(JSContext *ctx, JSValueConst this_val,
	  int argc, JSValueConst *argv)
{
	/* parse-time document.write needs parser interleaving that this
	 * backend does not support yet; log and drop */
	NSLOG(netsurf, DEBUG, "document.write ignored (quickjs backend)");
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry qjs_document_funcs[] = {
	JS_CGETSET_DEF("documentElement", doc_get_documentElement, NULL),
	JS_CGETSET_DEF("body", doc_get_body, NULL),
	JS_CGETSET_DEF("head", doc_get_head, NULL),
	JS_CGETSET_DEF("title", doc_get_title, doc_set_title),
	JS_CGETSET_DEF("URL", doc_get_URL, NULL),
	JS_CGETSET_DEF("documentURI", doc_get_URL, NULL),
	JS_CGETSET_DEF("readyState", doc_get_readyState, NULL),
	JS_CGETSET_DEF("cookie", doc_get_cookie, doc_set_cookie),
	JS_CGETSET_DEF("defaultView", doc_get_defaultView, NULL),
	JS_CGETSET_DEF("location", doc_get_location, doc_set_location),
	JS_CFUNC_DEF("getElementById", 1, doc_getElementById),
	JS_CFUNC_DEF("createElement", 1, doc_createElement),
	JS_CFUNC_DEF("createTextNode", 1, doc_createTextNode),
	JS_CFUNC_DEF("createComment", 1, doc_createComment),
	JS_CFUNC_DEF("createDocumentFragment", 0,
		     doc_createDocumentFragment),
	JS_CFUNC_DEF("createEvent", 1, doc_createEvent),
	JS_CFUNC_DEF("write", 1, doc_write),
	JS_CFUNC_DEF("writeln", 1, doc_write),
	JS_CFUNC_DEF("querySelector", 1, el_querySelector),
	JS_CFUNC_DEF("querySelectorAll", 1, el_querySelectorAll),
	JS_CFUNC_DEF("getElementsByTagName", 1, el_getElementsByTagName),
	JS_CFUNC_DEF("getElementsByClassName", 1,
		     el_getElementsByClassName),
};

/* ------------------------------------------------------------------ */
/* Event prototype                                                    */
/* ------------------------------------------------------------------ */

static JSValue
event_get_type(JSContext *ctx, JSValueConst this_val)
{
	dom_event *evt = qjs_this_event(ctx, this_val);
	dom_string *type = NULL;
	JSValue ret;

	if (evt == NULL)
		return JS_UNDEFINED;
	if (dom_event_get_type(evt, &type) != DOM_NO_ERR)
		return JS_UNDEFINED;
	ret = qjs_ds_to_js(ctx, type);
	if (type != NULL)
		dom_string_unref(type);
	return ret;
}

#define EVENT_BOOL_GETTER(fname, domfn)					\
static JSValue								\
fname(JSContext *ctx, JSValueConst this_val)				\
{									\
	dom_event *evt = qjs_this_event(ctx, this_val);			\
	bool v = false;							\
	if (evt == NULL)						\
		return JS_FALSE;					\
	domfn(evt, &v);							\
	return JS_NewBool(ctx, v);					\
}
EVENT_BOOL_GETTER(event_get_bubbles, dom_event_get_bubbles)
EVENT_BOOL_GETTER(event_get_cancelable, dom_event_get_cancelable)
EVENT_BOOL_GETTER(event_get_defaultPrevented, dom_event_is_default_prevented)
EVENT_BOOL_GETTER(event_get_isTrusted, dom_event_get_is_trusted)

static JSValue
event_get_eventPhase(JSContext *ctx, JSValueConst this_val)
{
	dom_event *evt = qjs_this_event(ctx, this_val);
	dom_event_flow_phase phase = 0;
	if (evt == NULL)
		return JS_NewInt32(ctx, 0);
	dom_event_get_event_phase(evt, &phase);
	return JS_NewInt32(ctx, (int32_t)phase);
}

static JSValue
event_get_timeStamp(JSContext *ctx, JSValueConst this_val)
{
	dom_event *evt = qjs_this_event(ctx, this_val);
	unsigned int ts = 0;
	if (evt == NULL)
		return JS_NewInt32(ctx, 0);
	dom_event_get_timestamp(evt, &ts);
	return JS_NewFloat64(ctx, (double)ts);
}

static JSValue
event_get_target(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_event *evt = qjs_this_event(ctx, this_val);
	dom_event_target *targ = NULL;

	if (t == NULL || evt == NULL)
		return JS_UNDEFINED;
	if (dom_event_get_target(evt, &targ) != DOM_NO_ERR ||
	    targ == NULL)
		return JS_NULL;
	return qjs_wrap_and_unref(t, (dom_node *)targ);
}

static JSValue
event_get_currentTarget(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_event *evt = qjs_this_event(ctx, this_val);
	dom_event_target *targ = NULL;

	if (t == NULL || evt == NULL)
		return JS_UNDEFINED;
	if (dom_event_get_current_target(evt, &targ) != DOM_NO_ERR ||
	    targ == NULL)
		return JS_NULL;
	return qjs_wrap_and_unref(t, (dom_node *)targ);
}

static JSValue
event_preventDefault(JSContext *ctx, JSValueConst this_val,
		     int argc, JSValueConst *argv)
{
	dom_event *evt = qjs_this_event(ctx, this_val);

	if (evt != NULL)
		dom_event_prevent_default(evt);
	return JS_UNDEFINED;
}

static JSValue
event_stopPropagation(JSContext *ctx, JSValueConst this_val,
		      int argc, JSValueConst *argv)
{
	dom_event *evt = qjs_this_event(ctx, this_val);

	if (evt != NULL)
		dom_event_stop_propagation(evt);
	return JS_UNDEFINED;
}

static JSValue
event_stopImmediatePropagation(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	dom_event *evt = qjs_this_event(ctx, this_val);

	if (evt != NULL)
		dom_event_stop_immediate_propagation(evt);
	return JS_UNDEFINED;
}

static JSValue
event_initEvent(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	dom_event *evt = qjs_this_event(ctx, this_val);
	dom_string *type;
	bool bubbles = false;
	bool cancelable = false;

	if (evt == NULL || argc < 1)
		return JS_UNDEFINED;
	type = qjs_js_to_ds(ctx, argv[0]);
	if (type == NULL)
		return JS_UNDEFINED;
	if (argc >= 2)
		bubbles = JS_ToBool(ctx, argv[1]);
	if (argc >= 3)
		cancelable = JS_ToBool(ctx, argv[2]);
	dom_event_init(evt, type, bubbles, cancelable);
	dom_string_unref(type);
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry qjs_event_funcs[] = {
	JS_CGETSET_DEF("type", event_get_type, NULL),
	JS_CGETSET_DEF("target", event_get_target, NULL),
	JS_CGETSET_DEF("bubbles", event_get_bubbles, NULL),
	JS_CGETSET_DEF("cancelable", event_get_cancelable, NULL),
	JS_CGETSET_DEF("defaultPrevented", event_get_defaultPrevented, NULL),
	JS_CGETSET_DEF("isTrusted", event_get_isTrusted, NULL),
	JS_CGETSET_DEF("eventPhase", event_get_eventPhase, NULL),
	JS_CGETSET_DEF("timeStamp", event_get_timeStamp, NULL),
	JS_CGETSET_DEF("currentTarget", event_get_currentTarget, NULL),
	JS_CFUNC_DEF("preventDefault", 0, event_preventDefault),
	JS_CFUNC_DEF("stopPropagation", 0, event_stopPropagation),
	JS_CFUNC_DEF("stopImmediatePropagation", 0,
		     event_stopImmediatePropagation),
	JS_CFUNC_DEF("initEvent", 3, event_initEvent),
};

/* ------------------------------------------------------------------ */
/* window: location, navigator, timers, misc                          */
/* ------------------------------------------------------------------ */

static JSValue
qjs_url_component(JSContext *ctx, nsurl_component part, const char *prefix)
{
	struct jsthread *t = qjs_thread(ctx);
	struct nsurl *url;
	lwc_string *comp;
	JSValue ret;
	char *buf;
	size_t plen;

	if (t == NULL || t->bw == NULL)
		return JS_NewStringLen(ctx, "", 0);
	url = browser_window_access_url(t->bw);
	if (url == NULL)
		return JS_NewStringLen(ctx, "", 0);
	comp = nsurl_get_component(url, part);
	if (comp == NULL)
		return JS_NewStringLen(ctx, "", 0);

	plen = strlen(prefix);
	buf = malloc(plen + lwc_string_length(comp) + 1);
	if (buf == NULL) {
		lwc_string_unref(comp);
		return JS_NewStringLen(ctx, "", 0);
	}
	memcpy(buf, prefix, plen);
	memcpy(buf + plen, lwc_string_data(comp),
	       lwc_string_length(comp));
	buf[plen + lwc_string_length(comp)] = '\0';
	ret = JS_NewString(ctx, buf);
	free(buf);
	lwc_string_unref(comp);
	return ret;
}

static JSValue
loc_get_href(JSContext *ctx, JSValueConst this_val)
{
	return doc_get_URL(ctx, this_val);
}

static void qjs_navigate(struct jsthread *t, const char *url_s)
{
	nsurl *url;

	if (t->closed || t->bw == NULL)
		return;
	if (nsurl_create(url_s, &url) != NSERROR_OK)
		return;
	browser_window_navigate(t->bw, url, NULL, BW_NAVIGATE_HISTORY,
				NULL, NULL, NULL);
	nsurl_unref(url);
}

static JSValue
loc_set_href(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
	struct jsthread *t = qjs_thread(ctx);
	const char *s = JS_ToCString(ctx, v);

	if (t != NULL && s != NULL)
		qjs_navigate(t, s);
	if (s != NULL)
		JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
}

static JSValue
loc_get_protocol(JSContext *ctx, JSValueConst this_val)
{
	struct jsthread *t = qjs_thread(ctx);
	struct nsurl *url;
	lwc_string *comp;
	char buf[32];

	if (t == NULL || t->bw == NULL)
		return JS_NewStringLen(ctx, "", 0);
	url = browser_window_access_url(t->bw);
	if (url == NULL)
		return JS_NewStringLen(ctx, "", 0);
	comp = nsurl_get_component(url, NSURL_SCHEME);
	if (comp == NULL)
		return JS_NewStringLen(ctx, "", 0);
	/* spec form is "https:" */
	snprintf(buf, sizeof(buf), "%.*s:",
		 (int)lwc_string_length(comp), lwc_string_data(comp));
	lwc_string_unref(comp);
	return JS_NewString(ctx, buf);
}

static JSValue
loc_get_host(JSContext *ctx, JSValueConst this_val)
{
	return qjs_url_component(ctx, NSURL_HOST, "");
}

static JSValue
loc_get_pathname(JSContext *ctx, JSValueConst this_val)
{
	return qjs_url_component(ctx, NSURL_PATH, "");
}

static JSValue
loc_get_search(JSContext *ctx, JSValueConst this_val)
{
	JSValue v = qjs_url_component(ctx, NSURL_QUERY, "");
	return v;
}

static JSValue
loc_get_hash(JSContext *ctx, JSValueConst this_val)
{
	return qjs_url_component(ctx, NSURL_FRAGMENT, "#");
}

static JSValue
loc_assign(JSContext *ctx, JSValueConst this_val,
	   int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	const char *s;

	if (t == NULL || argc < 1)
		return JS_UNDEFINED;
	s = JS_ToCString(ctx, argv[0]);
	if (s != NULL) {
		qjs_navigate(t, s);
		JS_FreeCString(ctx, s);
	}
	return JS_UNDEFINED;
}

static JSValue
loc_reload(JSContext *ctx, JSValueConst this_val,
	   int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);

	if (t != NULL && !t->closed && t->bw != NULL)
		browser_window_reload(t->bw, false);
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry qjs_location_funcs[] = {
	JS_CGETSET_DEF("href", loc_get_href, loc_set_href),
	JS_CGETSET_DEF("protocol", loc_get_protocol, NULL),
	JS_CGETSET_DEF("host", loc_get_host, NULL),
	JS_CGETSET_DEF("hostname", loc_get_host, NULL),
	JS_CGETSET_DEF("pathname", loc_get_pathname, NULL),
	JS_CGETSET_DEF("search", loc_get_search, NULL),
	JS_CGETSET_DEF("hash", loc_get_hash, NULL),
	JS_CFUNC_DEF("assign", 1, loc_assign),
	JS_CFUNC_DEF("replace", 1, loc_assign),
	JS_CFUNC_DEF("reload", 0, loc_reload),
};

/* timers */

static void qjs_timer_unlink(struct qjs_timer *tm)
{
	struct qjs_timer **link;

	for (link = &tm->thread->timers; *link != NULL;
	     link = &(*link)->next) {
		if (*link == tm) {
			*link = tm->next;
			break;
		}
	}
}

static void qjs_timer_free(struct qjs_timer *tm)
{
	qjs_timer_unlink(tm);
	if (!JS_IsUndefined(tm->func))
		JS_FreeValue(tm->thread->ctx, tm->func);
	free(tm->src);
	free(tm);
}

static void qjs_timer_fire(void *p)
{
	struct qjs_timer *tm = p;
	struct jsthread *t = tm->thread;
	JSContext *ctx = t->ctx;

	if (t->closed) {
		qjs_timer_free(tm);
		return;
	}

	qjs_deadline_start(t);
	if (!JS_IsUndefined(tm->func)) {
		JSValue global = JS_GetGlobalObject(ctx);
		JSValue ret = JS_Call(ctx, tm->func, global, 0, NULL);

		if (JS_IsException(ret))
			qjs_dump_error(ctx);
		JS_FreeValue(ctx, ret);
		JS_FreeValue(ctx, global);
	} else if (tm->src != NULL) {
		JSValue ret = JS_Eval(ctx, tm->src, strlen(tm->src),
				      "<timer>", JS_EVAL_TYPE_GLOBAL);

		if (JS_IsException(ret))
			qjs_dump_error(ctx);
		JS_FreeValue(ctx, ret);
	}
	qjs_deadline_stop(t);
	qjs_run_jobs(ctx);

	if (tm->repeat && !t->closed) {
		guit->misc->schedule(tm->ms, qjs_timer_fire, tm);
	} else {
		qjs_timer_free(tm);
	}
}

static JSValue
qjs_settimer(JSContext *ctx, JSValueConst this_val,
	     int argc, JSValueConst *argv, int magic)
{
	struct jsthread *t = qjs_thread(ctx);
	struct qjs_timer *tm;
	int32_t ms = 0;
	bool repeat = (magic != 0);

	if (t == NULL || t->closed || argc < 1)
		return JS_NewInt32(ctx, 0);
	if (argc >= 2)
		JS_ToInt32(ctx, &ms, argv[1]);
	if (ms < 0)
		ms = 0;

	tm = calloc(1, sizeof(*tm));
	if (tm == NULL)
		return JS_NewInt32(ctx, 0);
	tm->thread = t;
	tm->ms = ms;
	tm->repeat = repeat;
	tm->id = ++t->next_timer_id;
	tm->func = JS_UNDEFINED;

	if (JS_IsFunction(ctx, argv[0])) {
		tm->func = JS_DupValue(ctx, argv[0]);
	} else {
		const char *s = JS_ToCString(ctx, argv[0]);

		tm->src = (s != NULL) ? strdup(s) : NULL;
		if (s != NULL)
			JS_FreeCString(ctx, s);
	}

	tm->next = t->timers;
	t->timers = tm;
	guit->misc->schedule(ms, qjs_timer_fire, tm);
	return JS_NewInt32(ctx, (int32_t)tm->id);
}

static JSValue
qjs_cleartimer(JSContext *ctx, JSValueConst this_val,
	       int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	struct qjs_timer *tm;
	int32_t id = 0;

	if (t == NULL || argc < 1)
		return JS_UNDEFINED;
	JS_ToInt32(ctx, &id, argv[0]);

	for (tm = t->timers; tm != NULL; tm = tm->next) {
		if (tm->id == (uint32_t)id) {
			guit->misc->schedule(-1, qjs_timer_fire, tm);
			qjs_timer_free(tm);
			break;
		}
	}
	return JS_UNDEFINED;
}

/* misc window functions */

static JSValue
win_alert(JSContext *ctx, JSValueConst this_val,
	  int argc, JSValueConst *argv)
{
	if (argc >= 1) {
		const char *s = JS_ToCString(ctx, argv[0]);

		if (s != NULL) {
			NSLOG(netsurf, INFO, "js alert: %s", s);
			JS_FreeCString(ctx, s);
		}
	}
	return JS_UNDEFINED;
}

static JSValue
win_getComputedStyle(JSContext *ctx, JSValueConst this_val,
		     int argc, JSValueConst *argv)
{
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_NULL;
	return JS_GetPropertyStr(ctx, argv[0], "style");
}

/* window.dispatchEvent has no dom node; run JS-side listeners only */
static JSValue
win_dispatchEvent(JSContext *ctx, JSValueConst this_val,
		  int argc, JSValueConst *argv)
{
	JSValue type;
	JSValue args[3];
	JSValue ret;

	if (argc < 1)
		return JS_FALSE;
	type = JS_GetPropertyStr(ctx, argv[0], "type");
	args[0] = this_val;
	args[1] = type;
	args[2] = argv[0];
	ret = qjs_call_helper(ctx, "__ns_call", 3, args);
	JS_FreeValue(ctx, type);
	JS_FreeValue(ctx, ret);
	return JS_TRUE;
}

/* Event constructor: new Event(type, {bubbles, cancelable}) */
static JSValue
qjs_event_ctor(JSContext *ctx, JSValueConst new_target,
	       int argc, JSValueConst *argv)
{
	struct jsthread *t = qjs_thread(ctx);
	dom_event *evt = NULL;
	dom_string *type;
	bool bubbles = false;
	bool cancelable = false;
	JSValue ret;

	if (t == NULL || argc < 1)
		return JS_EXCEPTION;
	type = qjs_js_to_ds(ctx, argv[0]);
	if (type == NULL)
		return JS_EXCEPTION;

	if (argc >= 2 && JS_IsObject(argv[1])) {
		JSValue b = JS_GetPropertyStr(ctx, argv[1], "bubbles");
		JSValue c = JS_GetPropertyStr(ctx, argv[1], "cancelable");

		bubbles = JS_ToBool(ctx, b);
		cancelable = JS_ToBool(ctx, c);
		JS_FreeValue(ctx, b);
		JS_FreeValue(ctx, c);
	}

	if (dom_event_create(&evt) != DOM_NO_ERR) {
		dom_string_unref(type);
		return JS_EXCEPTION;
	}
	dom_event_init(evt, type, bubbles, cancelable);
	dom_string_unref(type);

	ret = qjs_dom_wrap_event(t, evt);
	dom_event_unref(evt);
	return ret;
}

/* ------------------------------------------------------------------ */
/* setup script: JS-side helpers                                      */
/* ------------------------------------------------------------------ */

static const char qjs_setup_script[] =
"(function(){\n"
"  globalThis.__ns_addl = function(o, t, f) {\n"
"    if (o == null) o = globalThis;\n"
"    var m = o.__ns_listeners || (o.__ns_listeners = {});\n"
"    var a = m[t] || (m[t] = []);\n"
"    if (a.indexOf(f) < 0) a.push(f);\n"
"  };\n"
"  globalThis.__ns_reml = function(o, t, f) {\n"
"    if (o == null) o = globalThis;\n"
"    var m = o.__ns_listeners; if (!m) return;\n"
"    var a = m[t]; if (!a) return;\n"
"    var i = a.indexOf(f); if (i >= 0) a.splice(i, 1);\n"
"  };\n"
"  globalThis.__ns_call = function(o, t, ev) {\n"
"    var r = true;\n"
"    function fire(f) {\n"
"      try { if (f.call(o, ev) === false) r = false; }\n"
"      catch (e) {\n"
"        try { console.error('event handler error: ' + e +\n"
"          (e && e.stack ? '\\n' + e.stack : '')); } catch (x) {}\n"
"      }\n"
"    }\n"
"    var h = o['on' + t];\n"
"    if (typeof h === 'function') fire(h);\n"
"    var m = o.__ns_listeners; var a = m && m[t];\n"
"    if (a) { a = a.slice();\n"
"      for (var i = 0; i < a.length; i++) fire(a[i]); }\n"
"    return r;\n"
"  };\n"
"  /* classList sugar over className */\n"
"  var ep = globalThis.__ns_Element_proto;\n"
"  if (ep) Object.defineProperty(ep, 'classList', { get: function() {\n"
"    var el = this;\n"
"    function toks() {\n"
"      var c = el.className;\n"
"      return c ? c.split(/\\s+/).filter(function(x){return x;}) : [];\n"
"    }\n"
"    return {\n"
"      add: function() { var a = toks();\n"
"        for (var i = 0; i < arguments.length; i++)\n"
"          if (a.indexOf(arguments[i]) < 0) a.push(arguments[i]);\n"
"        el.className = a.join(' '); },\n"
"      remove: function() { var rm = [].slice.call(arguments);\n"
"        el.className = toks().filter(function(x){\n"
"          return rm.indexOf(x) < 0; }).join(' '); },\n"
"      contains: function(x) { return toks().indexOf(x) >= 0; },\n"
"      toggle: function(x) { var a = toks(); var i = a.indexOf(x);\n"
"        if (i < 0) a.push(x); else a.splice(i, 1);\n"
"        el.className = a.join(' '); return i < 0; }\n"
"    };\n"
"  }, configurable: true });\n"
"  if (globalThis.location) {\n"
"    globalThis.location.toString = function() { return this.href; };\n"
"  }\n"
"  globalThis.Image = function() {\n"
"    return document.createElement('img');\n"
"  };\n"
"  globalThis.Audio = function(src) {\n"
"    var a = document.createElement('audio');\n"
"    if (src !== undefined) a.src = src;\n"
"    return a;\n"
"  };\n"
"  globalThis.matchMedia = function(q) {\n"
"    return { matches: false, media: q,\n"
"             addListener: function(){}, removeListener: function(){},\n"
"             addEventListener: function(){},\n"
"             removeEventListener: function(){} };\n"
"  };\n"
"  globalThis.requestAnimationFrame = function(f) {\n"
"    return setTimeout(function(){ f(Date.now()); }, 16);\n"
"  };\n"
"  globalThis.cancelAnimationFrame = function(id) {\n"
"    clearTimeout(id);\n"
"  };\n"
"})();\n";

/* make an "illegal constructor" carrying a prototype for instanceof */
static JSValue
qjs_illegal_ctor(JSContext *ctx, JSValueConst new_target,
		 int argc, JSValueConst *argv)
{
	return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static void
qjs_expose_ctor(JSContext *ctx, JSValue global, const char *name,
		JSValueConst proto)
{
	JSValue ctor = JS_NewCFunction2(ctx, qjs_illegal_ctor, name, 0,
					JS_CFUNC_constructor, 0);

	if (JS_IsException(ctor))
		return;
	JS_DefinePropertyValueStr(ctx, ctor, "prototype",
				  JS_DupValue(ctx, proto), 0);
	JS_SetPropertyStr(ctx, global, name, ctor);
}

/* ------------------------------------------------------------------ */
/* lifecycle entry points                                             */
/* ------------------------------------------------------------------ */

nserror qjs_dom_setup(struct jsthread *t)
{
	JSContext *ctx = t->ctx;
	JSRuntime *rt = t->heap->rt;
	JSValue global;
	JSValue location;
	JSValue navigator;
	JSValue setup;

	JS_SetContextOpaque(ctx, t);

	if (!t->heap->classes_ready) {
		static const JSClassDef node_def = {
			.class_name = "DOMNode",
			.finalizer = qjs_node_finalizer,
		};
		static const JSClassDef event_def = {
			.class_name = "DOMEvent",
			.finalizer = qjs_event_finalizer,
		};

		JS_NewClassID(rt, &t->heap->node_class);
		JS_NewClassID(rt, &t->heap->event_class);
		JS_NewClass(rt, t->heap->node_class, &node_def);
		JS_NewClass(rt, t->heap->event_class, &event_def);
		t->heap->classes_ready = true;
	}

	t->node_map = JS_NewObject(ctx);
	t->event_map = JS_NewObject(ctx);

	t->node_proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, t->node_proto, qjs_node_funcs,
				   sizeof(qjs_node_funcs) /
				   sizeof(qjs_node_funcs[0]));

	t->element_proto = JS_NewObjectProto(ctx, t->node_proto);
	JS_SetPropertyFunctionList(ctx, t->element_proto,
				   qjs_element_funcs,
				   sizeof(qjs_element_funcs) /
				   sizeof(qjs_element_funcs[0]));

	t->media_proto = JS_NewObjectProto(ctx, t->element_proto);
	JS_SetPropertyFunctionList(ctx, t->media_proto, qjs_media_funcs,
				   sizeof(qjs_media_funcs) /
				   sizeof(qjs_media_funcs[0]));

	t->document_proto = JS_NewObjectProto(ctx, t->node_proto);
	JS_SetPropertyFunctionList(ctx, t->document_proto,
				   qjs_document_funcs,
				   sizeof(qjs_document_funcs) /
				   sizeof(qjs_document_funcs[0]));

	t->text_proto = JS_NewObjectProto(ctx, t->node_proto);
	JS_SetPropertyFunctionList(ctx, t->text_proto, qjs_text_funcs,
				   sizeof(qjs_text_funcs) /
				   sizeof(qjs_text_funcs[0]));

	t->event_proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, t->event_proto, qjs_event_funcs,
				   sizeof(qjs_event_funcs) /
				   sizeof(qjs_event_funcs[0]));

	global = JS_GetGlobalObject(ctx);

	/* window identity aliases */
	JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
	JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
	JS_SetPropertyStr(ctx, global, "top", JS_DupValue(ctx, global));
	JS_SetPropertyStr(ctx, global, "parent", JS_DupValue(ctx, global));

	/* document */
	if (t->htmlc != NULL && t->htmlc->document != NULL) {
		JS_SetPropertyStr(
			ctx, global, "document",
			qjs_dom_wrap_node(
				t, (dom_node *)t->htmlc->document));
	}

	/* location: the object lives at __ns_location; "location" itself
	 * is an accessor so "window.location = url" navigates instead of
	 * clobbering the property */
	location = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, location, qjs_location_funcs,
				   sizeof(qjs_location_funcs) /
				   sizeof(qjs_location_funcs[0]));
	JS_SetPropertyStr(ctx, global, "__ns_location", location);
	{
		JSAtom atom = JS_NewAtom(ctx, "location");

		JS_DefinePropertyGetSet(
			ctx, global, atom,
			JS_NewCFunction2(ctx, (JSCFunction *)doc_get_location,
					 "get location", 0,
					 JS_CFUNC_getter, 0),
			JS_NewCFunction2(ctx, (JSCFunction *)doc_set_location,
					 "set location", 1,
					 JS_CFUNC_setter, 0),
			JS_PROP_ENUMERABLE);
		JS_FreeAtom(ctx, atom);
	}

	/* navigator */
	navigator = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, navigator, "userAgent",
			  JS_NewString(ctx, user_agent_string()));
	JS_SetPropertyStr(ctx, navigator, "appName",
			  JS_NewString(ctx, "Netscape"));
	JS_SetPropertyStr(ctx, navigator, "appVersion",
			  JS_NewString(ctx, "5.0 (NetSurf)"));
	JS_SetPropertyStr(ctx, navigator, "platform",
			  JS_NewString(ctx, "NetSurf"));
	JS_SetPropertyStr(ctx, navigator, "language",
			  JS_NewString(ctx, "en"));
	JS_SetPropertyStr(ctx, navigator, "cookieEnabled", JS_FALSE);
	JS_SetPropertyStr(ctx, navigator, "onLine", JS_TRUE);
	JS_SetPropertyStr(ctx, global, "navigator", navigator);

	/* window functions */
	JS_SetPropertyStr(ctx, global, "alert",
			  JS_NewCFunction(ctx, win_alert, "alert", 1));
	JS_SetPropertyStr(ctx, global, "getComputedStyle",
			  JS_NewCFunction(ctx, win_getComputedStyle,
					  "getComputedStyle", 1));
	JS_SetPropertyStr(ctx, global, "setTimeout",
			  JS_NewCFunctionMagic(ctx, qjs_settimer,
					       "setTimeout", 2,
					       JS_CFUNC_generic_magic, 0));
	JS_SetPropertyStr(ctx, global, "setInterval",
			  JS_NewCFunctionMagic(ctx, qjs_settimer,
					       "setInterval", 2,
					       JS_CFUNC_generic_magic, 1));
	JS_SetPropertyStr(ctx, global, "clearTimeout",
			  JS_NewCFunction(ctx, qjs_cleartimer,
					  "clearTimeout", 1));
	JS_SetPropertyStr(ctx, global, "clearInterval",
			  JS_NewCFunction(ctx, qjs_cleartimer,
					  "clearInterval", 1));
	JS_SetPropertyStr(ctx, global, "addEventListener",
			  JS_NewCFunction(ctx, qjs_addEventListener,
					  "addEventListener", 2));
	JS_SetPropertyStr(ctx, global, "removeEventListener",
			  JS_NewCFunction(ctx, qjs_removeEventListener,
					  "removeEventListener", 2));
	JS_SetPropertyStr(ctx, global, "dispatchEvent",
			  JS_NewCFunction(ctx, win_dispatchEvent,
					  "dispatchEvent", 1));

	/* constructor identities for instanceof */
	qjs_expose_ctor(ctx, global, "Node", t->node_proto);
	qjs_expose_ctor(ctx, global, "EventTarget", t->node_proto);
	qjs_expose_ctor(ctx, global, "Element", t->element_proto);
	qjs_expose_ctor(ctx, global, "HTMLElement", t->element_proto);
	qjs_expose_ctor(ctx, global, "Document", t->document_proto);
	qjs_expose_ctor(ctx, global, "HTMLDocument", t->document_proto);
	qjs_expose_ctor(ctx, global, "Text", t->text_proto);
	qjs_expose_ctor(ctx, global, "CharacterData", t->text_proto);
	qjs_expose_ctor(ctx, global, "HTMLMediaElement", t->media_proto);
	qjs_expose_ctor(ctx, global, "HTMLAudioElement", t->media_proto);
	qjs_expose_ctor(ctx, global, "HTMLVideoElement", t->media_proto);

	/* real Event constructor */
	{
		JSValue ctor = JS_NewCFunction2(ctx, qjs_event_ctor,
						"Event", 1,
						JS_CFUNC_constructor, 0);

		JS_DefinePropertyValueStr(ctx, ctor, "prototype",
					  JS_DupValue(ctx, t->event_proto),
					  0);
		JS_SetPropertyStr(ctx, global, "Event", ctor);
	}

	/* expose prototypes to the setup scripts, then hide them */
	JS_SetPropertyStr(ctx, global, "__ns_Element_proto",
			  JS_DupValue(ctx, t->element_proto));
	JS_SetPropertyStr(ctx, global, "__ns_Node_proto",
			  JS_DupValue(ctx, t->node_proto));
	JS_SetPropertyStr(ctx, global, "__ns_Document_proto",
			  JS_DupValue(ctx, t->document_proto));
	JS_SetPropertyStr(ctx, global, "__ns_Text_proto",
			  JS_DupValue(ctx, t->text_proto));
	JS_SetPropertyStr(ctx, global, "__ns_Event_proto",
			  JS_DupValue(ctx, t->event_proto));
	JS_SetPropertyStr(ctx, global, "__ns_Media_proto",
			  JS_DupValue(ctx, t->media_proto));

	qjs_native_setup(t);

	setup = JS_Eval(ctx, qjs_setup_script,
			sizeof(qjs_setup_script) - 1,
			"<qjs-dom-setup>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(setup))
		qjs_dump_error(ctx);
	JS_FreeValue(ctx, setup);

	/* the web platform runtime (runtime.js) */
	{
		/* the embedded array is not NUL terminated */
		char *src = malloc(runtime_js_len + 1);
		if (src != NULL) {
			memcpy(src, runtime_js, runtime_js_len);
			src[runtime_js_len] = '\0';
			setup = JS_Eval(ctx, src, runtime_js_len,
					"<netsurf-runtime>",
					JS_EVAL_TYPE_GLOBAL);
			free(src);
			if (JS_IsException(setup))
				qjs_dump_error(ctx);
			JS_FreeValue(ctx, setup);
		}
	}
	qjs_run_jobs(ctx);

	{
		static const char *hide[] = {
			"__ns_Element_proto", "__ns_Node_proto",
			"__ns_Document_proto", "__ns_Text_proto",
			"__ns_Event_proto", "__ns_Media_proto", NULL
		};
		int i;

		for (i = 0; hide[i] != NULL; i++) {
			JSAtom atom = JS_NewAtom(ctx, hide[i]);
			JS_DeleteProperty(ctx, global, atom, 0);
			JS_FreeAtom(ctx, atom);
		}
	}

	JS_FreeValue(ctx, global);

	NSLOG(netsurf, INFO, "quickjs DOM bindings ready (core set)");
	return NSERROR_OK;
}

void qjs_dom_closethread(struct jsthread *t)
{
	qjs_native_closethread(t);

	while (t->timers != NULL) {
		struct qjs_timer *tm = t->timers;

		guit->misc->schedule(-1, qjs_timer_fire, tm);
		qjs_timer_free(tm); /* unlinks from t->timers */
	}
}

void qjs_dom_teardown(struct jsthread *t)
{
	JSContext *ctx = t->ctx;

	qjs_dom_closethread(t);

	JS_FreeValue(ctx, t->node_map);
	JS_FreeValue(ctx, t->event_map);
	JS_FreeValue(ctx, t->node_proto);
	JS_FreeValue(ctx, t->element_proto);
	JS_FreeValue(ctx, t->media_proto);
	JS_FreeValue(ctx, t->document_proto);
	JS_FreeValue(ctx, t->text_proto);
	JS_FreeValue(ctx, t->event_proto);
}

/* ------------------------------------------------------------------ */
/* core-driven event entry points                                     */
/* ------------------------------------------------------------------ */

bool qjs_dom_fire_event(struct jsthread *t, const char *type,
			struct dom_document *doc, struct dom_node *target)
{
	JSContext *ctx = t->ctx;
	dom_event *evt = NULL;
	dom_string *tds = NULL;
	JSValue global;
	JSValue args[3];
	JSValue ret;

	if (t->closed)
		return true;
	if (target != NULL)
		return true; /* only window-targeted events, like dukky */

	if (dom_string_create((const uint8_t *)type, strlen(type),
			      &tds) != DOM_NO_ERR)
		return true;
	if (dom_event_create(&evt) != DOM_NO_ERR) {
		dom_string_unref(tds);
		return true;
	}
	dom_event_init(evt, tds, false, false);
	dom_string_unref(tds);

	global = JS_GetGlobalObject(ctx);

	qjs_deadline_start(t);

	/* DOMContentLoaded is dispatched at the document (and seen by
	 * the window as it bubbles); load only at the window */
	if (strcmp(type, "DOMContentLoaded") == 0 || strcmp(type, "load") == 0) {
		JSValue docv = JS_GetPropertyStr(ctx, global, "document");

		t->ready_state = (type[0] == 'l') ? 2 : 1;
		if (JS_IsObject(docv)) {
			args[0] = docv;
			args[1] = JS_NewString(ctx, "readystatechange");
			args[2] = qjs_dom_wrap_event(t, evt);
			ret = qjs_call_helper(ctx, "__ns_call", 3, args);
			JS_FreeValue(ctx, ret);
			JS_FreeValue(ctx, args[1]);
			JS_FreeValue(ctx, args[2]);
			if (type[0] == 'D') {
				args[1] = JS_NewString(ctx, type);
				args[2] = qjs_dom_wrap_event(t, evt);
				ret = qjs_call_helper(ctx, "__ns_call", 3, args);
				JS_FreeValue(ctx, ret);
				JS_FreeValue(ctx, args[1]);
				JS_FreeValue(ctx, args[2]);
			}
		}
		JS_FreeValue(ctx, docv);
	}

	args[0] = global;
	args[1] = JS_NewString(ctx, type);
	args[2] = qjs_dom_wrap_event(t, evt);
	ret = qjs_call_helper(ctx, "__ns_call", 3, args);
	JS_FreeValue(ctx, ret);
	JS_FreeValue(ctx, args[1]);
	JS_FreeValue(ctx, args[2]);

	qjs_deadline_stop(t);
	qjs_run_jobs(ctx);

	JS_FreeValue(ctx, global);
	qjs_dom_event_cleanup(t, evt);
	dom_event_unref(evt);
	return true;
}

void qjs_dom_new_element(struct jsthread *t, struct dom_element *element)
{
	JSContext *ctx = t->ctx;
	dom_namednodemap *map = NULL;
	dom_ulong idx;
	dom_ulong siz;
	dom_string *nodename = NULL;
	bool is_body = false;

	if (t->closed)
		return;

	if (dom_node_get_node_name(element, &nodename) != DOM_NO_ERR)
		return;
	if (nodename != NULL) {
		is_body = dom_string_caseless_isequal(nodename,
						      corestring_dom_BODY);
		dom_string_unref(nodename);
	}

	if (dom_node_get_attributes(element, &map) != DOM_NO_ERR ||
	    map == NULL)
		return;
	if (dom_namednodemap_get_length(map, &siz) != DOM_NO_ERR)
		goto out;

	for (idx = 0; idx < siz; idx++) {
		dom_attr *attr = NULL;
		dom_string *key = NULL;
		dom_string *val = NULL;
		const char *kdata;
		size_t klen;

		if (dom_namednodemap_item(map, idx, &attr) != DOM_NO_ERR)
			break;
		if (dom_attr_get_name(attr, &key) != DOM_NO_ERR) {
			dom_node_unref(attr);
			break;
		}

		kdata = dom_string_data(key);
		klen = dom_string_length(key);
		if (klen > 2 && kdata[0] == 'o' && kdata[1] == 'n' &&
		    dom_attr_get_value(attr, &val) == DOM_NO_ERR &&
		    val != NULL) {
			/* compile "(function(event){ <attr> })" */
			size_t vlen = dom_string_length(val);
			size_t slen = vlen + klen + 64;
			char *src = malloc(slen);

			if (src != NULL) {
				int n = snprintf(
					src, slen,
					"(function(event){\n%.*s\n})",
					(int)vlen, dom_string_data(val));
				JSValue fn = JS_Eval(
					ctx, src, (size_t)n,
					"<event-attr>",
					JS_EVAL_TYPE_GLOBAL);

				if (JS_IsException(fn)) {
					qjs_dump_error(ctx);
				} else {
					char pname[64];
					JSValue targ_js;
					dom_node *tnode =
						(dom_node *)element;
					JSValue global = JS_GetGlobalObject(ctx);

					snprintf(pname, sizeof(pname),
						 "%.*s", (int)klen, kdata);

					/* body load/error/etc forward to
					 * the window object */
					if (is_body) {
						targ_js = JS_DupValue(
							ctx, global);
						tnode = NULL;
					} else {
						targ_js = qjs_dom_wrap_node(
							t, tnode);
					}

					JS_SetPropertyStr(ctx, targ_js,
							  pname, fn);
					if (tnode != NULL) {
						qjs_ensure_dom_listener(
							t, targ_js, tnode,
							pname + 2);
					}
					JS_FreeValue(ctx, targ_js);
					JS_FreeValue(ctx, global);
				}
				free(src);
			}
			dom_string_unref(val);
		} else if (val != NULL) {
			dom_string_unref(val);
		}

		dom_string_unref(key);
		dom_node_unref(attr);
	}

out:
	dom_namednodemap_unref(map);
}

void qjs_dom_event_cleanup(struct jsthread *t, struct dom_event *evt)
{
	char key[32];
	JSAtom atom;

	if (t->closed)
		return;
	qjs_ptr_key(key, sizeof(key), evt);
	atom = JS_NewAtom(t->ctx, key);
	JS_DeleteProperty(t->ctx, t->event_map, atom, 0);
	JS_FreeAtom(t->ctx, atom);
}
