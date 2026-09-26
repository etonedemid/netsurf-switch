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
 * QuickJS-ng backend for NetSurf's javascript interface.
 *
 * Engine glue: heap/thread lifecycle, script execution with a
 * wallclock timeout, console. The DOM surface lives in qjs_dom.c
 * (hand-written core bindings over libdom). Build with
 * NETSURF_USE_QUICKJS=YES; the default backend remains duktape.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs/quickjs.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsoption.h"
#include "content/content.h"

#include "javascript/js.h"
#include "javascript/content.h"
#include "javascript/quickjs/qjs_private.h"

#include <nsutils/time.h>

#define QJS_MEMORY_LIMIT (64 * 1024 * 1024)

/* abort long-running scripts via the runtime interrupt hook */
static int qjs_interrupt_handler(JSRuntime *rt, void *opaque)
{
	struct jsheap *heap = opaque;
	uint64_t now;

	if (heap->deadline_ms == 0)
		return 0;
	if (nsu_getmonotonic_ms(&now) != NSUERROR_OK)
		return 0;
	return (now > heap->deadline_ms) ? 1 : 0;
}

void qjs_deadline_start(struct jsthread *thread)
{
	uint64_t now;

	if (thread->heap->timeout_s > 0 &&
	    nsu_getmonotonic_ms(&now) == NSUERROR_OK) {
		thread->heap->deadline_ms =
			now + (uint64_t)thread->heap->timeout_s * 1000;
	}
}

void qjs_deadline_stop(struct jsthread *thread)
{
	thread->heap->deadline_ms = 0;
}

/* minimal console for script diagnostics */
static JSValue qjs_console_log(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	int i;

	for (i = 0; i < argc; i++) {
		const char *s = JS_ToCString(ctx, argv[i]);
		if (s != NULL) {
			NSLOG(netsurf, INFO, "js console: %s", s);
			JS_FreeCString(ctx, s);
		}
	}
	return JS_UNDEFINED;
}

static void qjs_setup_console(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue console = JS_NewObject(ctx);

	JS_SetPropertyStr(ctx, console, "log",
			  JS_NewCFunction(ctx, qjs_console_log, "log", 1));
	JS_SetPropertyStr(ctx, console, "warn",
			  JS_NewCFunction(ctx, qjs_console_log, "warn", 1));
	JS_SetPropertyStr(ctx, console, "error",
			  JS_NewCFunction(ctx, qjs_console_log, "error", 1));
	JS_SetPropertyStr(ctx, console, "info",
			  JS_NewCFunction(ctx, qjs_console_log, "info", 1));
	JS_SetPropertyStr(ctx, console, "debug",
			  JS_NewCFunction(ctx, qjs_console_log, "debug", 1));
	JS_SetPropertyStr(ctx, global, "console", console);

	JS_FreeValue(ctx, global);
}

/* exported interface documented in js.h */
void js_initialise(void)
{
	/* register the javascript content handler so page scripts are
	 * recognised and fetched */
	javascript_init();

	NSLOG(netsurf, INFO,
	      "QuickJS backend initialised (core DOM bindings)");
}

/* exported interface documented in js.h */
void js_finalise(void)
{
}

/* exported interface documented in js.h */
nserror js_newheap(int timeout, jsheap **heap_out)
{
	struct jsheap *heap;

	heap = calloc(1, sizeof(*heap));
	if (heap == NULL)
		return NSERROR_NOMEM;

	heap->rt = JS_NewRuntime();
	if (heap->rt == NULL) {
		free(heap);
		return NSERROR_NOMEM;
	}

	JS_SetMemoryLimit(heap->rt, QJS_MEMORY_LIMIT);
	JS_SetInterruptHandler(heap->rt, qjs_interrupt_handler, heap);
	JS_SetRuntimeOpaque(heap->rt, heap);
	heap->timeout_s = timeout;

	*heap_out = heap;
	return NSERROR_OK;
}

/* exported interface documented in js.h */
void js_destroyheap(jsheap *heap)
{
	if (heap == NULL)
		return;
	JS_FreeRuntime(heap->rt);
	free(heap);
}

/* exported interface documented in js.h */
nserror
js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **out)
{
	struct jsthread *thread;

	if (heap == NULL)
		return NSERROR_BAD_PARAMETER;

	thread = calloc(1, sizeof(*thread));
	if (thread == NULL)
		return NSERROR_NOMEM;

	thread->ctx = JS_NewContext(heap->rt);
	if (thread->ctx == NULL) {
		free(thread);
		return NSERROR_NOMEM;
	}

	thread->heap = heap;
	thread->bw = win_priv;
	thread->htmlc = doc_priv;

	qjs_setup_console(thread->ctx);
	qjs_dom_setup(thread);

	*out = thread;
	return NSERROR_OK;
}

/* exported interface documented in js.h */
nserror js_closethread(jsthread *thread)
{
	if (thread == NULL)
		return NSERROR_OK;
	thread->closed = true;
	qjs_dom_closethread(thread);
	return NSERROR_OK;
}

/* exported interface documented in js.h */
void js_destroythread(jsthread *thread)
{
	if (thread == NULL)
		return;
	thread->closed = true;
	qjs_dom_teardown(thread);
	JS_FreeContext(thread->ctx);
	free(thread);
}

/* exported interface documented in js.h */
bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen,
	     const char *name)
{
	JSValue v;
	bool ok = true;

	if (thread == NULL || thread->closed || txt == NULL || txtlen == 0)
		return false;

	qjs_deadline_start(thread);

	v = JS_Eval(thread->ctx, (const char *)txt, txtlen,
		    name ? name : "<script>", JS_EVAL_TYPE_GLOBAL);

	qjs_deadline_stop(thread);

	if (JS_IsException(v)) {
		qjs_dump_error(thread->ctx);
		ok = false;
	}
	JS_FreeValue(thread->ctx, v);

	qjs_run_jobs(thread->ctx);

	return ok;
}

/* exported interface documented in js.h */
bool js_fire_event(jsthread *thread, const char *type,
		   struct dom_document *doc, struct dom_node *target)
{
	if (thread == NULL)
		return true;
	return qjs_dom_fire_event(thread, type, doc, target);
}

/* exported interface documented in js.h */
bool
js_dom_event_add_listener(jsthread *thread,
			  struct dom_document *document,
			  struct dom_node *node,
			  struct dom_string *event_type_dom,
			  void *js_funcval)
{
	/* no producer of engine function values exists for this backend */
	return false;
}

/* exported interface documented in js.h */
void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	if (thread == NULL || node == NULL)
		return;
	qjs_dom_new_element(thread, node);
}

/* exported interface documented in js.h */
void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	if (thread == NULL || evt == NULL)
		return;
	qjs_dom_event_cleanup(thread, evt);
}
