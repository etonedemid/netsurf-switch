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
 * Declarative shadow DOM: flat tree navigation and style scoping.
 */

#ifndef NETSURF_HTML_SHADOW_H
#define NETSURF_HTML_SHADOW_H

#include <stdbool.h>
#include <stddef.h>

struct dom_node;

/** is n a <template shadowrootmode> (declarative shadow root)? */
bool html_shadow_is_template(struct dom_node *n);

/** the shadow root template of a host element (ref'd), or NULL */
struct dom_node *html_shadow_template(struct dom_node *host);

/** the shadow host whose shadow tree contains n (ref'd), or NULL */
struct dom_node *html_shadow_host_of(struct dom_node *n);

/*
 * Flat tree navigation. All return a new reference or NULL and do not
 * consume the argument.
 */
struct dom_node *html_flat_first_child(struct dom_node *n);
struct dom_node *html_flat_next_sibling(struct dom_node *n);
struct dom_node *html_flat_parent(struct dom_node *n);
bool html_flat_has_children(struct dom_node *n);

/**
 * Rewrite a shadow tree stylesheet so its selectors apply to the host
 * tag in the document.
 *
 * \return malloc()ed NUL terminated CSS text, or NULL on failure
 */
char *html_shadow_scope_css(const char *css, size_t len, const char *host);

#endif
