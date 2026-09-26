/*
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Dynamic document updates: rebuild the box tree after DOM mutations.
 *
 * NetSurf builds its box tree once from the DOM. When scripts (or
 * late-arriving stylesheets) change the document after the first
 * layout, the change is recorded and, after a short debounce, the box
 * tree is rebuilt from the DOM and the page is reformatted.
 *
 * State that must survive a rebuild is carried over from the old tree:
 * fetched objects (images) are handed to the new boxes for the same DOM
 * nodes, form text widgets are kept by their controls, iframe browser
 * windows are rebound and textarea focus is restored.
 */

#include <stdlib.h>
#include <string.h>

#include <dom/dom.h>
#include <nsutils/time.h>

#include "utils/log.h"
#include "utils/talloc.h"
#include "utils/corestrings.h"
#include "utils/nsoption.h"
#include "utils/utils.h"
#include "netsurf/misc.h"
#include "netsurf/content.h"
#include "content/content_protected.h"
#include "content/hlcache.h"
#include "desktop/gui_internal.h"
#include "desktop/selection.h"
#include "desktop/frames.h"

#include "css/select.h"

#include "html/html.h"
#include "html/private.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/imagemap.h"
#include "html/object.h"
#include "html/css.h"
#include "html/rebuild.h"

/** minimum debounce before rebuilding (ms) */
#define REBUILD_DELAY_MS 40

static void html_rebuild_cb(void *p);

/* exported interface documented in html/rebuild.h */
void html_rebuild_schedule(html_content *c)
{
	uint64_t now;
	int delay;

	if (c == NULL || c->aborted || !c->had_initial_layout ||
			c->frameset != NULL)
		return;
	if (c->rebuild_pending)
		return;

	c->rebuild_pending = true;

	/* rate limit: stay idle for at least twice the last rebuild's
	 * duration so script-driven animation cannot starve the UI */
	nsu_getmonotonic_ms(&now);
	delay = REBUILD_DELAY_MS;
	if (c->rebuild_next_ms > now + delay)
		delay = c->rebuild_next_ms - now;
	if (delay > 2000)
		delay = 2000;

	guit->misc->schedule(delay, html_rebuild_cb, c);
}

/* exported interface documented in html/rebuild.h */
void html_rebuild_cancel(html_content *c)
{
	if (c->rebuild_pending) {
		guit->misc->schedule(-1, html_rebuild_cb, c);
		c->rebuild_pending = false;
	}
}

/* exported interface documented in html/rebuild.h */
bool html_rebuild_node_matters(dom_node *node)
{
	dom_node *n, *parent;
	dom_exception exc;
	bool matters = true;

	/* Mutations within <head> (other than stylesheets, which are
	 * handled separately) and within scripts don't affect rendering. */
	n = dom_node_ref(node);
	while (n != NULL) {
		dom_node_type type;
		exc = dom_node_get_node_type(n, &type);
		if (exc == DOM_NO_ERR && type == DOM_ELEMENT_NODE) {
			dom_html_element_type tag;
			exc = dom_html_element_get_tag_type(n, &tag);
			if (exc == DOM_NO_ERR &&
			    (tag == DOM_HTML_ELEMENT_TYPE_HEAD ||
			     tag == DOM_HTML_ELEMENT_TYPE_SCRIPT ||
			     tag == DOM_HTML_ELEMENT_TYPE_TITLE ||
			     tag == DOM_HTML_ELEMENT_TYPE_NOSCRIPT ||
			     tag == DOM_HTML_ELEMENT_TYPE_TEMPLATE)) {
				matters = false;
				dom_node_unref(n);
				break;
			}
		}
		exc = dom_node_get_parent_node(n, &parent);
		dom_node_unref(n);
		if (exc != DOM_NO_ERR)
			break;
		n = parent;
	}
	return matters;
}

/**
 * Drop the DOM user-data pointers of boxes in a tree that is about to be
 * freed, where the node has no replacement box.
 */
static void html_rebuild_forget_boxes(struct box *box)
{
	for (; box != NULL; box = box->next) {
		if (box->node != NULL && box_for_node(box->node) == box) {
			void *old = NULL;
			dom_node_set_user_data(box->node,
				corestring_dom___ns_key_box_node_data,
				NULL, NULL, &old);
		}
		if (box->list_marker != NULL)
			html_rebuild_forget_boxes(box->list_marker);
		if (box->children != NULL)
			html_rebuild_forget_boxes(box->children);
	}
}

/**
 * Release objects left over from the previous tree.
 */
static void html_rebuild_free_old_objects(html_content *c)
{
	while (c->rebuild_old_objects != NULL) {
		struct content_html_object *victim = c->rebuild_old_objects;

		c->rebuild_old_objects = victim->next;
		if (victim->content != NULL) {
			if (victim->box != NULL &&
			    content_get_status(victim->content) !=
					CONTENT_STATUS_DONE) {
				/* the fetch was counted as active */
				hlcache_handle_abort(victim->content);
				if (c->base.active > 0)
					c->base.active--;
			}
			hlcache_handle_release(victim->content);
		}
		free(victim);
	}
}

static void html_rebuild_cb(void *p)
{
	html_content *c = p;
	int *old_bctx;
	struct box *old_layout;
	struct content_html_iframe *old_iframes;
	dom_node *html = NULL;
	dom_node *focus_node = NULL;
	nserror err;
	uint64_t t0, t1;
	content_status status;

	c->rebuild_pending = false;

	if (c->aborted || c->layout == NULL || c->document == NULL)
		return;

	status = content__get_status(&c->base);
	if ((status != CONTENT_STATUS_READY && status != CONTENT_STATUS_DONE) ||
	    c->base.locked || c->box_conversion_context != NULL ||
	    c->reflowing) {
		/* not a good time; try again shortly */
		html_rebuild_schedule(c);
		return;
	}

	if (dom_document_get_document_element(c->document, (void *)&html) !=
			DOM_NO_ERR || html == NULL)
		return;

	nsu_getmonotonic_ms(&t0);

	/* styles computed for the old DOM are stale */
	nscss_invalidate_node_data(html);

	if (c->rebuild_styles_changed && c->select_ctx != NULL) {
		css_select_ctx *ctx = NULL;
		if (html_css_new_selection_context(c, &ctx) == NSERROR_OK) {
			css_select_ctx_destroy(c->select_ctx);
			c->select_ctx = ctx;
		}
		c->rebuild_styles_changed = false;
	}

	/* remember what the user was interacting with */
	if (c->focus_type == HTML_FOCUS_TEXTAREA &&
			c->focus_owner.textarea != NULL &&
			c->focus_owner.textarea->node != NULL) {
		focus_node = dom_node_ref(c->focus_owner.textarea->node);
	}
	c->drag_type = HTML_DRAG_NONE;
	c->drag_owner.no_owner = true;
	c->selection_type = HTML_SELECTION_NONE;
	c->selection_owner.none = true;
	c->focus_type = HTML_FOCUS_SELF;
	c->focus_owner.self = true;
	if (c->sel != NULL)
		selection_clear(c->sel, false);

	/* detach the old tree */
	old_bctx = c->bctx;
	old_layout = c->layout;
	old_iframes = c->iframe;
	c->bctx = NULL;
	c->iframe = NULL;
	c->rebuild_old_objects = c->object_list;
	c->object_list = NULL;
	c->num_objects = 0;
	imagemap_destroy(c);

	err = dom_to_box_sync(html, c);
	if (err != NSERROR_OK || c->layout == NULL ||
			c->layout == old_layout) {
		NSLOG(netsurf, ERROR, "box tree rebuild failed");
		/* restore the old tree */
		if (c->bctx != NULL)
			talloc_free(c->bctx);
		html_object_free_objects(c);
		c->bctx = old_bctx;
		c->layout = old_layout;
		c->iframe = old_iframes;
		c->object_list = c->rebuild_old_objects;
		c->rebuild_old_objects = NULL;
		imagemap_extract(c);
		if (focus_node != NULL)
			dom_node_unref(focus_node);
		dom_node_unref(html);
		return;
	}

	/* hand iframe browser windows over to the new boxes */
	if (c->bw != NULL && old_iframes != NULL)
		browser_window_rebind_iframes(c->bw);

	html_rebuild_forget_boxes(old_layout);
	html_rebuild_free_old_objects(c);
	if (old_bctx != NULL)
		talloc_free(old_bctx);

	imagemap_extract(c);

	if (focus_node != NULL) {
		struct box *fb = box_for_node(focus_node);
		if (fb != NULL && fb->gadget != NULL) {
			c->focus_type = HTML_FOCUS_TEXTAREA;
			c->focus_owner.textarea = fb;
		}
		dom_node_unref(focus_node);
	}

	dom_node_unref(html);

	/* lay out and redraw */
	content__reformat(&c->base, false, c->base.available_width,
			c->base.available_height);
	content__request_redraw(&c->base, 0, 0, c->base.width,
			c->base.height);

	nsu_getmonotonic_ms(&t1);
	c->rebuild_next_ms = t1 + 2 * (t1 - t0);
	NSLOG(netsurf, INFO, "box tree rebuilt in %u ms",
			(unsigned)(t1 - t0));
}
