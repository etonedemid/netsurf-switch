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
 * HTML @font-face webfont fetching (interface).
 *
 * After a page's selection context is built, the families declared in
 * @font-face rules are collected, their best src fetched via the low
 * level cache and the font blob handed to the frontend through a
 * registered callback.  Frontends that can consume in-memory fonts
 * register the callback at startup; without one this is all a no-op.
 */

#ifndef NETSURF_HTML_WEBFONT_H
#define NETSURF_HTML_WEBFONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"

struct html_content;

/**
 * Frontend font registrar.
 *
 * Takes a family name and a font blob; the blob remains owned by the
 * caller and stays valid for the life of the process if registration
 * succeeds.  Returns true if the font was registered.
 */
typedef bool (*html_webfont_register_fn)(const char *family,
		const uint8_t *data, size_t data_len);

/** Register the frontend's font registrar (call once at startup). */
void html_webfont_set_register_cb(html_webfont_register_fn cb);

/**
 * Scan an html content's selection context for @font-face families and
 * start fetches for any not yet known.  Safe to call when no registrar
 * is set.  Fetches complete asynchronously; the content is reformatted
 * when a font arrives after layout.
 */
nserror html_webfont_start(struct html_content *c);

/** Detach a dying html content from any in-flight webfont fetches. */
void html_webfont_content_destroyed(struct html_content *c);

#endif
