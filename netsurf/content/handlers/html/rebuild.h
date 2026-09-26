/*
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

/**
 * \file
 * Dynamic document updates: rebuild the box tree after DOM mutations.
 */

#ifndef NETSURF_HTML_REBUILD_H
#define NETSURF_HTML_REBUILD_H

#include <stdbool.h>

struct html_content;
struct dom_node;

/**
 * Note that the document changed; the box tree will be rebuilt and the
 * page reformatted shortly.
 */
void html_rebuild_schedule(struct html_content *c);

/**
 * Suppress (or stop suppressing) rebuilds, around DOM changes whose
 * rendering is updated directly (form control values).
 */
void html_rebuild_suppress(bool suppress);

/**
 * Cancel any pending rebuild (content is being destroyed).
 */
void html_rebuild_cancel(struct html_content *c);

/**
 * Perform any pending rebuild and reformat now (scripts reading layout).
 */
void html_rebuild_flush(struct html_content *c);

/**
 * Could a mutation of this node change what is rendered?
 */
bool html_rebuild_node_matters(struct dom_node *node);

#endif
