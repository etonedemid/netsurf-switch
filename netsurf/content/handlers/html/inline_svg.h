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
 * Inline <svg> elements rendered as replaced content.
 */

#ifndef NETSURF_HTML_INLINE_SVG_H
#define NETSURF_HTML_INLINE_SVG_H

#include <stdbool.h>

struct dom_node;
struct html_content;
struct box;

/** is n an <svg> element in the SVG namespace? */
bool html_inline_svg_is_svg(struct dom_node *n);

/**
 * Make box a replaced element rendering the inline SVG rooted at n.
 *
 * \return false on memory exhaustion
 */
bool html_inline_svg_box(struct dom_node *n, struct html_content *content,
		struct box *box, bool *convert_children);

#endif
