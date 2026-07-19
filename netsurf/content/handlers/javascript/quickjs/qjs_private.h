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
 * QuickJS backend internals shared between the engine glue (quickjs.c)
 * and the DOM binding layer (qjs_dom.c).
 */

#ifndef NETSURF_JS_QJS_PRIVATE_H
#define NETSURF_JS_QJS_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include <quickjs/quickjs.h>

#include "utils/errors.h"

struct browser_window;
struct html_content;
struct dom_document;
struct dom_node;
struct dom_element;
struct dom_event;

struct qjs_timer;

struct jsheap {
	JSRuntime *rt;
	int timeout_s;        /* script timeout in seconds */
	uint64_t deadline_ms; /* monotonic deadline while executing */

	/* JSClass ids are runtime scoped in quickjs-ng */
	JSClassID node_class;
	JSClassID event_class;
	bool classes_ready;
};

struct jsthread {
	struct jsheap *heap;
	JSContext *ctx;
	struct browser_window *bw;
	struct html_content *htmlc;
	bool closed;

	/* dom_node* / dom_event* pointer-keyed wrapper caches */
	JSValue node_map;
	JSValue event_map;

	/* prototypes for wrapped objects */
	JSValue node_proto;
	JSValue element_proto;
	JSValue media_proto;    /* <audio>/<video>, extends element */
	JSValue document_proto;
	JSValue text_proto;
	JSValue event_proto;

	/* active setTimeout/setInterval entries */
	struct qjs_timer *timers;
	uint32_t next_timer_id;
};

/* set a script-runtime deadline, run fn-ish work, then clear it */
void qjs_deadline_start(struct jsthread *thread);
void qjs_deadline_stop(struct jsthread *thread);

/* log and consume the current exception */
void qjs_dump_error(JSContext *ctx);

/* run queued promise jobs (microtasks) */
void qjs_run_jobs(JSContext *ctx);

/* DOM binding layer (qjs_dom.c) */
nserror qjs_dom_setup(struct jsthread *thread);
void qjs_dom_teardown(struct jsthread *thread);
void qjs_dom_closethread(struct jsthread *thread);

JSValue qjs_dom_wrap_node(struct jsthread *thread, struct dom_node *node);
JSValue qjs_dom_wrap_event(struct jsthread *thread, struct dom_event *evt);

bool qjs_dom_fire_event(struct jsthread *thread, const char *type,
			struct dom_document *doc, struct dom_node *target);
void qjs_dom_new_element(struct jsthread *thread, struct dom_element *element);
void qjs_dom_event_cleanup(struct jsthread *thread, struct dom_event *evt);

#endif
