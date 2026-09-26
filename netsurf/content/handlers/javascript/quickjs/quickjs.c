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
	if (getenv("NS_JS_LEAKS") != NULL)
		JS_SetDumpFlags(heap->rt, JS_DUMP_LEAKS);
	JS_SetInterruptHandler(heap->rt, qjs_interrupt_handler, heap);
	JS_SetRuntimeOpaque(heap->rt, heap);
	qjs_modules_setup(heap->rt);
	heap->timeout_s = timeout;

	*heap_out = heap;
	return NSERROR_OK;
}

/* exported interface documented in js.h */
void js_destroyheap(jsheap *heap)
{
	if (heap == NULL)
		return;
	if (heap->nthreads > 0) {
		/* page contexts can outlive their window (the content is
		 * still cached); free the runtime with the last of them */
		heap->dying = true;
		return;
	}
	JS_RunGC(heap->rt);
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
	heap->nthreads++;
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
	qjs_modules_closethread(thread);
	qjs_dom_closethread(thread);
	return NSERROR_OK;
}

/* exported interface documented in js.h */
void js_destroythread(jsthread *thread)
{
	if (thread == NULL)
		return;
	jsheap *heap;

	thread->closed = true;
	heap = thread->heap;
	qjs_modules_closethread(thread);
	qjs_dom_teardown(thread);
	JS_FreeContext(thread->ctx);
	free(thread);

	if (--heap->nthreads == 0 && heap->dying) {
		heap->nthreads = 0;
		heap->dying = false;
		js_destroyheap(heap);
	}
}

/**
 * Copy script source into a NUL terminated UTF-8 buffer.
 *
 * Invalid UTF-8 is taken to be Windows-1252/Latin-1 and converted.
 */
static char *qjs_source_utf8(const uint8_t *txt, size_t len, size_t *outlen)
{
	size_t i = 0, o = 0;
	bool valid = true;
	char *out;

	/* skip a UTF-8 byte order mark */
	if (len >= 3 && txt[0] == 0xef && txt[1] == 0xbb && txt[2] == 0xbf) {
		txt += 3;
		len -= 3;
	}

	while (i < len) {
		uint8_t c = txt[i];
		size_t n;
		if (c < 0x80) {
			i++;
			continue;
		}
		n = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 :
			(c & 0xf8) == 0xf0 ? 4 : 0;
		if (n == 0 || i + n > len) {
			valid = false;
			break;
		}
		for (size_t k = 1; k < n; k++) {
			if ((txt[i + k] & 0xc0) != 0x80) {
				valid = false;
				break;
			}
		}
		if (!valid)
			break;
		i += n;
	}

	if (valid) {
		out = malloc(len + 1);
		if (out == NULL)
			return NULL;
		memcpy(out, txt, len);
		out[len] = '\0';
		*outlen = len;
		return out;
	}

	out = malloc(len * 2 + 1);
	if (out == NULL)
		return NULL;
	for (i = 0; i < len; i++) {
		uint8_t c = txt[i];
		if (c < 0x80) {
			out[o++] = c;
		} else {
			out[o++] = 0xc0 | (c >> 6);
			out[o++] = 0x80 | (c & 0x3f);
		}
	}
	out[o] = '\0';
	*outlen = o;
	return out;
}

/* exported interface documented in js.h */
bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen,
	     const char *name)
{
	JSValue v;
	bool ok = true;
	char *buf;
	size_t buflen;

	if (thread == NULL || thread->closed || txt == NULL || txtlen == 0)
		return false;

	/* QuickJS needs NUL terminated UTF-8 source */
	buf = qjs_source_utf8(txt, txtlen, &buflen);
	if (buf == NULL)
		return false;

	qjs_deadline_start(thread);

	v = JS_Eval(thread->ctx, buf, buflen,
		    name ? name : "<script>", JS_EVAL_TYPE_GLOBAL);

	qjs_deadline_stop(thread);
	free(buf);

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
