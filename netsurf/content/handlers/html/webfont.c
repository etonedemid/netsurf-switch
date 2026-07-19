/*
 * Copyright 2026 NetSurf Switch port
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
 * HTML @font-face webfont fetching.
 *
 * Once a page's selection context exists, every family declared by an
 * applicable @font-face rule is looked up with css_select_font_faces(),
 * the most useful src is chosen (preferring formats freetype can open:
 * ttf/otf, then woff; woff2/eot/svg are skipped) and fetched through
 * the low level cache.  Arriving blobs are copied and handed to the
 * frontend registrar; fonts registered after layout trigger a reformat.
 *
 * Registered fonts and the attempted-family list are process-global:
 * a family is only fetched once per session, first declaration wins.
 */

#include "utils/config.h"

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include <libcss/select.h>
#include <libcss/font_face.h>

#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/content.h"
#include "content/llcache.h"

#include "html/html.h"
#include "html/private.h"
#include "html/webfont.h"

/** Upper bound on a single font blob. */
#define WEBFONT_MAX_SIZE (8 * 1024 * 1024)

/** Lifetime cap on families we will attempt to fetch. */
#define WEBFONT_MAX_FETCHES 32

static html_webfont_register_fn webfont_register_cb;

/** An in-flight font fetch. */
struct webfont_fetch {
	struct webfont_fetch *next;
	char *family;
	llcache_handle *handle;
	html_content *html; /**< owning content; NULL once it dies */
};

static struct webfont_fetch *webfont_fetch_list;

/** Family names ever attempted (fetch started), to fetch each once. */
struct webfont_attempted {
	struct webfont_attempted *next;
	char *family;
};

static struct webfont_attempted *webfont_attempted_list;
static int webfont_attempt_count;


/* exported interface documented in html/webfont.h */
void html_webfont_set_register_cb(html_webfont_register_fn cb)
{
	webfont_register_cb = cb;
}


static bool webfont_family_attempted(const char *family)
{
	struct webfont_attempted *a;

	for (a = webfont_attempted_list; a != NULL; a = a->next) {
		if (strcasecmp(a->family, family) == 0)
			return true;
	}
	return false;
}


static void webfont_mark_attempted(const char *family)
{
	struct webfont_attempted *a;

	a = malloc(sizeof(*a));
	if (a == NULL)
		return;
	a->family = strdup(family);
	if (a->family == NULL) {
		free(a);
		return;
	}
	a->next = webfont_attempted_list;
	webfont_attempted_list = a;
	webfont_attempt_count++;
}


static bool webfont_family_pending(const char *family)
{
	struct webfont_fetch *wf;

	for (wf = webfont_fetch_list; wf != NULL; wf = wf->next) {
		if (strcasecmp(wf->family, family) == 0)
			return true;
	}
	return false;
}


static void webfont_fetch_free(struct webfont_fetch *wf)
{
	struct webfont_fetch **link;

	for (link = &webfont_fetch_list; *link != NULL;
			link = &(*link)->next) {
		if (*link == wf) {
			*link = wf->next;
			break;
		}
	}

	if (wf->handle != NULL)
		llcache_handle_release(wf->handle);
	free(wf->family);
	free(wf);
}


/** Case-insensitively test whether url path (query stripped) ends in ext */
static bool webfont_url_has_ext(lwc_string *location, const char *ext)
{
	const char *url = lwc_string_data(location);
	size_t len = lwc_string_length(location);
	size_t extlen = strlen(ext);
	const char *cut;

	cut = memchr(url, '?', len);
	if (cut != NULL)
		len = cut - url;
	cut = memchr(url, '#', len);
	if (cut != NULL)
		len = cut - url;

	return (len >= extlen) &&
		(strncasecmp(url + len - extlen, ext, extlen) == 0);
}


/**
 * Rate a font-face src for fetchability/usability; 0 means unusable.
 * freetype opens sfnt (ttf/otf) and woff; not woff2 (no brotli), eot
 * or svg fonts.
 */
static int webfont_src_score(const css_font_face_src *src)
{
	lwc_string *loc = NULL;
	css_font_face_format fmt;

	if (css_font_face_src_location_type(src) !=
			CSS_FONT_FACE_LOCATION_TYPE_URI)
		return 0;

	if ((css_font_face_src_get_location(src, &loc) != CSS_OK) ||
			(loc == NULL))
		return 0;

	fmt = css_font_face_src_format(src);

	if (fmt & CSS_FONT_FACE_FORMAT_OPENTYPE)
		return 100;
	if (fmt & CSS_FONT_FACE_FORMAT_WOFF) {
		/* format(woff) sometimes decorates .woff2 urls */
		if (webfont_url_has_ext(loc, ".woff2"))
			return 0;
		return 80;
	}
	if ((fmt != CSS_FONT_FACE_FORMAT_UNSPECIFIED) &&
			((fmt & CSS_FONT_FACE_FORMAT_UNKNOWN) == 0))
		return 0; /* eot/svg only */

	/* no (usable) format hint: guess from the url */
	if (webfont_url_has_ext(loc, ".ttf") ||
			webfont_url_has_ext(loc, ".otf"))
		return 90;
	if (webfont_url_has_ext(loc, ".woff"))
		return 70;
	if (webfont_url_has_ext(loc, ".woff2") ||
			webfont_url_has_ext(loc, ".eot") ||
			webfont_url_has_ext(loc, ".svg") ||
			webfont_url_has_ext(loc, ".svgz"))
		return 0;

	return (fmt == CSS_FONT_FACE_FORMAT_UNSPECIFIED) ? 50 : 10;
}


/** Prefer the face carrying the family's normal weight/style. */
static int webfont_face_bonus(const css_font_face *face)
{
	uint8_t weight = css_font_face_font_weight(face);
	uint8_t style = css_font_face_font_style(face);
	int bonus = 0;

	if (weight == CSS_FONT_WEIGHT_INHERIT ||
			weight == CSS_FONT_WEIGHT_NORMAL ||
			weight == CSS_FONT_WEIGHT_400)
		bonus += 15;
	if (style == CSS_FONT_STYLE_INHERIT ||
			style == CSS_FONT_STYLE_NORMAL)
		bonus += 15;

	return bonus;
}


static nserror
webfont_llcache_cb(llcache_handle *handle,
		const llcache_event *event,
		void *pw)
{
	struct webfont_fetch *wf = pw;

	switch (event->type) {
	case LLCACHE_EVENT_DONE: {
		const uint8_t *data;
		uint8_t *copy = NULL;
		size_t len = 0;
		bool registered = false;

		data = llcache_handle_get_source_data(handle, &len);
		if (data != NULL && len > 0 && len <= WEBFONT_MAX_SIZE)
			copy = malloc(len);

		if (copy != NULL) {
			memcpy(copy, data, len);
			registered = webfont_register_cb(wf->family,
							 copy, len);
			if (registered == false)
				free(copy);
		}

		NSLOG(netsurf, INFO, "webfont '%s': %s (%zu bytes)",
		      wf->family,
		      registered ? "registered" : "rejected",
		      len);

		if (registered && wf->html != NULL &&
		    (wf->html->base.status == CONTENT_STATUS_READY ||
		     wf->html->base.status == CONTENT_STATUS_DONE)) {
			content__reformat(&wf->html->base, false,
					  wf->html->base.available_width,
					  wf->html->base.available_height);
		}

		webfont_fetch_free(wf);
		break;
	}

	case LLCACHE_EVENT_ERROR:
		NSLOG(netsurf, INFO, "webfont '%s': fetch failed: %s",
		      wf->family,
		      event->data.error.msg != NULL ?
				event->data.error.msg : "");
		webfont_fetch_free(wf);
		break;

	default:
		/* headers/data/progress/redirect: llcache handles these */
		break;
	}

	return NSERROR_OK;
}


static void
webfont_fetch_start(html_content *c, lwc_string *family, lwc_string *location)
{
	struct webfont_fetch *wf;
	nsurl *url = NULL;
	nserror error;

	error = nsurl_create(lwc_string_data(location), &url);
	if (error != NSERROR_OK)
		return;

	wf = calloc(1, sizeof(*wf));
	if (wf == NULL) {
		nsurl_unref(url);
		return;
	}

	wf->family = strdup(lwc_string_data(family));
	if (wf->family == NULL) {
		nsurl_unref(url);
		free(wf);
		return;
	}
	wf->html = c;

	error = llcache_handle_retrieve(url, 0,
			content_get_url(&c->base), NULL,
			webfont_llcache_cb, wf,
			&wf->handle);
	nsurl_unref(url);

	if (error != NSERROR_OK) {
		NSLOG(netsurf, INFO, "webfont '%s': retrieve failed: %d",
		      wf->family, error);
		free(wf->family);
		free(wf);
		return;
	}

	NSLOG(netsurf, INFO, "webfont '%s': fetching %s",
	      wf->family, lwc_string_data(location));

	webfont_mark_attempted(wf->family);
	wf->next = webfont_fetch_list;
	webfont_fetch_list = wf;
}


static void webfont_try_family(html_content *c, lwc_string *family)
{
	css_select_font_faces_results *results = NULL;
	const css_font_face_src *best_src = NULL;
	int best_score = 0;
	uint32_t i;

	if (webfont_attempt_count >= WEBFONT_MAX_FETCHES)
		return;

	if (css_select_font_faces(c->select_ctx, &c->media, &c->unit_len_ctx,
			family, &results) != CSS_OK || results == NULL)
		return;

	for (i = 0; i < results->n_font_faces; i++) {
		const css_font_face *face = results->font_faces[i];
		uint32_t n_srcs = 0;
		uint32_t j;
		int bonus = webfont_face_bonus(face);

		if (css_font_face_count_srcs(face, &n_srcs) != CSS_OK)
			continue;

		for (j = 0; j < n_srcs; j++) {
			const css_font_face_src *src;
			int score;

			if (css_font_face_get_src(face, j, &src) != CSS_OK)
				continue;

			score = webfont_src_score(src);
			if (score > 0 && score + bonus > best_score) {
				best_score = score + bonus;
				best_src = src;
			}
		}
	}

	if (best_src != NULL) {
		lwc_string *loc = NULL;

		if (css_font_face_src_get_location(best_src, &loc) == CSS_OK &&
				loc != NULL) {
			webfont_fetch_start(c, family, loc);
		}
	} else {
		NSLOG(netsurf, INFO, "webfont '%s': no usable src",
		      lwc_string_data(family));
	}

	css_select_font_faces_results_destroy(results);
}


/** Families collected from one selection context scan. */
struct webfont_scan {
	lwc_string **families;
	uint32_t n;
	uint32_t alloc;
};


static css_error webfont_scan_cb(void *pw, lwc_string *family)
{
	struct webfont_scan *scan = pw;
	const char *name = lwc_string_data(family);
	uint32_t i;

	/* skip families already collected, fetched or in flight */
	for (i = 0; i < scan->n; i++) {
		if (strcasecmp(lwc_string_data(scan->families[i]), name) == 0)
			return CSS_OK;
	}
	if (webfont_family_attempted(name) || webfont_family_pending(name))
		return CSS_OK;

	if (scan->n == scan->alloc) {
		lwc_string **bigger;
		uint32_t newalloc = (scan->alloc == 0) ? 8 : scan->alloc * 2;

		bigger = realloc(scan->families,
				 newalloc * sizeof(*bigger));
		if (bigger == NULL)
			return CSS_NOMEM;
		scan->families = bigger;
		scan->alloc = newalloc;
	}

	scan->families[scan->n++] = lwc_string_ref(family);

	return CSS_OK;
}


/* exported interface documented in html/webfont.h */
nserror html_webfont_start(html_content *c)
{
	struct webfont_scan scan = { NULL, 0, 0 };
	uint32_t i;

	if (webfont_register_cb == NULL || c->select_ctx == NULL)
		return NSERROR_OK;

	css_select_font_face_families(c->select_ctx, webfont_scan_cb, &scan);

	for (i = 0; i < scan.n; i++) {
		webfont_try_family(c, scan.families[i]);
		lwc_string_unref(scan.families[i]);
	}
	free(scan.families);

	return NSERROR_OK;
}


/* exported interface documented in html/webfont.h */
void html_webfont_content_destroyed(html_content *c)
{
	struct webfont_fetch *wf;

	for (wf = webfont_fetch_list; wf != NULL; wf = wf->next) {
		if (wf->html == c)
			wf->html = NULL;
	}
}
