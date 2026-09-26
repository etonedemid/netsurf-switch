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
 * Inline <svg> elements in HTML documents.
 *
 * The element's subtree is serialised to a standalone SVG document and
 * rendered as a replaced element through the normal SVG image handler,
 * fetched from a data: URL (identical icons share one content).
 *
 * While serialising, currentColor is replaced by the element's computed
 * CSS colour, and <use href="#id"> references to elements elsewhere in
 * the page (icon sprite sheets) are copied into the document's <defs>.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dom/dom.h>

#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/content_type.h"
#include "css/utils.h"

#include "html/private.h"
#include "html/box.h"
#include "html/object.h"
#include "html/inline_svg.h"

#define SVG_MAX_BYTES (512 * 1024)
#define SVG_MAX_REFS 32

struct svgbuf {
	char *d;
	size_t len, cap;
	bool fail;
};

struct svgser {
	struct svgbuf b;
	dom_document *doc;
	char colour[8];
	char *refs[SVG_MAX_REFS];
	int nrefs;
};

static void sb_add(struct svgbuf *b, const char *s, size_t n)
{
	if (b->fail)
		return;
	if (b->len + n + 1 > SVG_MAX_BYTES) {
		b->fail = true;
		return;
	}
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 4096;
		char *d;
		while (cap < b->len + n + 1)
			cap *= 2;
		d = realloc(b->d, cap);
		if (d == NULL) {
			b->fail = true;
			return;
		}
		b->d = d;
		b->cap = cap;
	}
	memcpy(b->d + b->len, s, n);
	b->len += n;
	b->d[b->len] = '\0';
}

static void sb_str(struct svgbuf *b, const char *s)
{
	sb_add(b, s, strlen(s));
}

/** append text with XML escaping and currentColor substitution */
static void sb_escaped(struct svgser *z, const char *s, size_t n, bool attr)
{
	size_t i, start = 0;

	for (i = 0; i < n; i++) {
		const char *rep = NULL;
		size_t skip = 1;

		switch (s[i]) {
		case '&': rep = "&amp;"; break;
		case '<': rep = "&lt;"; break;
		case '>': rep = "&gt;"; break;
		case '"': rep = attr ? "&quot;" : NULL; break;
		case 'c':
		case 'C':
			if (attr && i + 12 <= n &&
			    strncasecmp(s + i, "currentcolor", 12) == 0) {
				rep = z->colour;
				skip = 12;
			}
			break;
		default:
			break;
		}
		if (rep != NULL) {
			sb_add(&z->b, s + start, i - start);
			sb_str(&z->b, rep);
			i += skip - 1;
			start = i + 1;
		}
	}
	sb_add(&z->b, s + start, n - start);
}

/** SVG names are case sensitive; the HTML DOM folds them */
static const char *const svg_case_names[] = {
	/* elements */
	"altGlyph", "altGlyphDef", "altGlyphItem", "animateColor",
	"animateMotion", "animateTransform", "clipPath", "feBlend",
	"feColorMatrix", "feComponentTransfer", "feComposite",
	"feConvolveMatrix", "feDiffuseLighting", "feDisplacementMap",
	"feDistantLight", "feDropShadow", "feFlood", "feFuncA", "feFuncB",
	"feFuncG", "feFuncR", "feGaussianBlur", "feImage", "feMerge",
	"feMergeNode", "feMorphology", "feOffset", "fePointLight",
	"feSpecularLighting", "feSpotLight", "feTile", "feTurbulence",
	"foreignObject", "glyphRef", "linearGradient", "radialGradient",
	"textPath",
	/* attributes */
	"attributeName", "attributeType", "baseFrequency", "baseProfile",
	"calcMode", "clipPathUnits", "diffuseConstant", "edgeMode",
	"filterUnits", "glyphRef", "gradientTransform", "gradientUnits",
	"kernelMatrix", "kernelUnitLength", "keyPoints", "keySplines",
	"keyTimes", "lengthAdjust", "limitingConeAngle", "markerHeight",
	"markerUnits", "markerWidth", "maskContentUnits", "maskUnits",
	"numOctaves", "pathLength", "patternContentUnits",
	"patternTransform", "patternUnits", "pointsAtX", "pointsAtY",
	"pointsAtZ", "preserveAlpha", "preserveAspectRatio",
	"primitiveUnits", "refX", "refY", "repeatCount", "repeatDur",
	"requiredExtensions", "requiredFeatures", "specularConstant",
	"specularExponent", "spreadMethod", "startOffset", "stdDeviation",
	"stitchTiles", "surfaceScale", "systemLanguage", "tableValues",
	"targetX", "targetY", "textLength", "viewBox", "viewTarget",
	"xChannelSelector", "yChannelSelector", "zoomAndPan",
	NULL
};

/**
 * Write a name in its proper SVG case.
 */
static void sb_name(struct svgbuf *b, const char *s, size_t n)
{
	char low[64];
	size_t i;
	int k;

	if (n >= sizeof(low)) {
		sb_add(b, s, n);
		return;
	}
	for (i = 0; i < n; i++)
		low[i] = tolower((unsigned char)s[i]);
	low[n] = '\0';
	for (k = 0; svg_case_names[k] != NULL; k++) {
		if (strlen(svg_case_names[k]) == n &&
		    strcasecmp(svg_case_names[k], low) == 0) {
			sb_str(b, svg_case_names[k]);
			return;
		}
	}
	sb_add(b, low, n);
}

static void note_ref(struct svgser *z, const char *href, size_t n)
{
	int i;

	if (n < 2 || href[0] != '#' || z->nrefs >= SVG_MAX_REFS)
		return;
	for (i = 0; i < z->nrefs; i++) {
		if (strlen(z->refs[i]) == n - 1 &&
				strncmp(z->refs[i], href + 1, n - 1) == 0)
			return;
	}
	z->refs[z->nrefs] = strndup(href + 1, n - 1);
	if (z->refs[z->nrefs] != NULL)
		z->nrefs++;
}

static void ser_node(struct svgser *z, dom_node *n, bool root, int depth);

static void ser_children(struct svgser *z, dom_node *n, int depth)
{
	dom_node *c = NULL, *next;

	if (dom_node_get_first_child(n, &c) != DOM_NO_ERR)
		return;
	while (c != NULL && !z->b.fail) {
		ser_node(z, c, false, depth + 1);
		if (dom_node_get_next_sibling(c, &next) != DOM_NO_ERR)
			next = NULL;
		dom_node_unref(c);
		c = next;
	}
	if (c != NULL)
		dom_node_unref(c);
}

static void ser_element(struct svgser *z, dom_node *n, bool root,
		const char *rename, int depth)
{
	dom_string *name = NULL;
	dom_namednodemap *attrs = NULL;
	uint32_t na = 0, i;
	bool has_xmlns = false;
	const char *tag;
	size_t taglen;

	if (depth > 64 || dom_node_get_node_name(n, &name) != DOM_NO_ERR ||
			name == NULL)
		return;
	tag = rename ? rename : dom_string_data(name);
	taglen = rename ? strlen(rename) : dom_string_byte_length(name);

	/* scripts and foreign content are of no use to the renderer */
	if (strncasecmp(tag, "script", taglen) == 0 && taglen == 6) {
		dom_string_unref(name);
		return;
	}

	sb_str(&z->b, "<");
	sb_name(&z->b, tag, taglen);

	if (dom_node_get_attributes(n, &attrs) == DOM_NO_ERR && attrs != NULL)
		dom_namednodemap_get_length(attrs, &na);
	for (i = 0; i < na; i++) {
		dom_attr *a = NULL;
		dom_string *an = NULL, *av = NULL;

		if (dom_namednodemap_item(attrs, i, (void *)&a) != DOM_NO_ERR ||
				a == NULL)
			continue;
		dom_attr_get_name(a, &an);
		dom_attr_get_value(a, &av);
		if (an != NULL) {
			const char *ad = dom_string_data(an);
			size_t al = dom_string_byte_length(an);

			if (al == 5 && strncmp(ad, "xmlns", 5) == 0)
				has_xmlns = true;
			if (strncasecmp(tag, "use", taglen) == 0 &&
			    taglen == 3 && av != NULL &&
			    ((al == 4 && strncmp(ad, "href", 4) == 0) ||
			     (al == 10 && strncmp(ad, "xlink:href", 10) == 0))) {
				note_ref(z, dom_string_data(av),
						dom_string_byte_length(av));
				/* libsvgtiny reads xlink:href */
				ad = "xlink:href";
				al = 10;
			}
			/* event handlers are dropped */
			if (!(al > 2 && ad[0] == 'o' && ad[1] == 'n')) {
				sb_str(&z->b, " ");
				sb_name(&z->b, ad, al);
				sb_str(&z->b, "=\"");
				if (av != NULL)
					sb_escaped(z, dom_string_data(av),
						dom_string_byte_length(av),
						true);
				sb_str(&z->b, "\"");
			}
		}
		if (an != NULL)
			dom_string_unref(an);
		if (av != NULL)
			dom_string_unref(av);
		dom_node_unref(a);
	}
	if (attrs != NULL)
		dom_namednodemap_unref(attrs);

	if (root) {
		if (!has_xmlns)
			sb_str(&z->b, " xmlns=\"http://www.w3.org/2000/svg\"");
		sb_str(&z->b, " xmlns:xlink=\"http://www.w3.org/1999/xlink\"");
	}
	sb_str(&z->b, ">");

	ser_children(z, n, depth);

	if (root && z->nrefs > 0) {
		/* copy referenced sprites into <defs> */
		int r;
		sb_str(&z->b, "<defs>");
		for (r = 0; r < z->nrefs && !z->b.fail; r++) {
			dom_string *id = NULL;
			dom_element *ref = NULL;
			if (dom_string_create((const uint8_t *)z->refs[r],
					strlen(z->refs[r]), &id) != DOM_NO_ERR)
				continue;
			if (dom_document_get_element_by_id(z->doc, id,
					&ref) == DOM_NO_ERR && ref != NULL) {
				/* only if not already inside this svg */
				dom_node *p = NULL, *cur = dom_node_ref(ref);
				bool inside = false;
				while (cur != NULL) {
					if (cur == n) {
						inside = true;
						dom_node_unref(cur);
						break;
					}
					if (dom_node_get_parent_node(cur, &p) !=
							DOM_NO_ERR)
						p = NULL;
					dom_node_unref(cur);
					cur = p;
				}
				if (!inside)
					ser_node(z, (dom_node *)ref, false, 1);
				dom_node_unref(ref);
			}
			dom_string_unref(id);
		}
		sb_str(&z->b, "</defs>");
	}

	sb_str(&z->b, "</");
	sb_name(&z->b, tag, taglen);
	sb_str(&z->b, ">");
	dom_string_unref(name);
}

static void ser_node(struct svgser *z, dom_node *n, bool root, int depth)
{
	dom_node_type type;

	if (dom_node_get_node_type(n, &type) != DOM_NO_ERR)
		return;

	if (type == DOM_ELEMENT_NODE) {
		dom_string *name = NULL;
		const char *rename = NULL;

		/* <symbol> is rendered through <use> as a nested <svg> */
		if (dom_node_get_node_name(n, &name) == DOM_NO_ERR &&
				name != NULL) {
			if (dom_string_byte_length(name) == 6 &&
			    strncasecmp(dom_string_data(name), "symbol",
					6) == 0)
				rename = "svg";
			dom_string_unref(name);
		}
		ser_element(z, n, root, rename, depth);
	} else if (type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE) {
		dom_string *t = NULL;
		if (dom_characterdata_get_data(n, &t) == DOM_NO_ERR &&
				t != NULL) {
			sb_escaped(z, dom_string_data(t),
					dom_string_byte_length(t), false);
			dom_string_unref(t);
		}
	}
}

/* exported interface documented in html/inline_svg.h */
bool html_inline_svg_is_svg(dom_node *n)
{
	dom_string *name = NULL, *ns = NULL;
	bool r = false;

	if (dom_node_get_node_name(n, &name) != DOM_NO_ERR || name == NULL)
		return false;
	if (dom_string_byte_length(name) == 3 &&
			strncasecmp(dom_string_data(name), "svg", 3) == 0) {
		/* must be in the SVG namespace (not an HTML element) */
		if (dom_node_get_namespace(n, &ns) == DOM_NO_ERR &&
				ns != NULL) {
			r = strstr(dom_string_data(ns), "svg") != NULL;
			dom_string_unref(ns);
		}
	}
	dom_string_unref(name);
	return r;
}

static const char b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* exported interface documented in html/inline_svg.h */
bool html_inline_svg_box(dom_node *n, html_content *content,
		struct box *box, bool *convert_children)
{
	struct svgser z;
	char *url_s, *o;
	size_t i, prefix;
	nsurl *url = NULL;
	css_color colour = 0xff000000;
	bool ok;
	int r;

	*convert_children = false;

	if (box->style != NULL) {
		if (ns_computed_display(box->style, false) == CSS_DISPLAY_NONE)
			return true;
		css_computed_color(box->style, &colour);
	}

	memset(&z, 0, sizeof(z));
	z.doc = content->document;
	snprintf(z.colour, sizeof(z.colour), "#%02x%02x%02x",
			(unsigned)((colour >> 16) & 0xff),
			(unsigned)((colour >> 8) & 0xff),
			(unsigned)(colour & 0xff));

	ser_node(&z, n, true, 0);
	for (r = 0; r < z.nrefs; r++)
		free(z.refs[r]);
	if (z.b.fail || z.b.d == NULL) {
		free(z.b.d);
		return true;
	}

	/* base64 data: URL */
	prefix = strlen("data:image/svg+xml;base64,");
	url_s = malloc(prefix + ((z.b.len + 2) / 3) * 4 + 1);
	if (url_s == NULL) {
		free(z.b.d);
		return false;
	}
	memcpy(url_s, "data:image/svg+xml;base64,", prefix);
	o = url_s + prefix;
	for (i = 0; i < z.b.len; i += 3) {
		uint32_t v = (uint8_t)z.b.d[i] << 16;
		size_t rem = z.b.len - i;
		if (rem > 1)
			v |= (uint8_t)z.b.d[i + 1] << 8;
		if (rem > 2)
			v |= (uint8_t)z.b.d[i + 2];
		*o++ = b64[(v >> 18) & 63];
		*o++ = b64[(v >> 12) & 63];
		*o++ = rem > 1 ? b64[(v >> 6) & 63] : '=';
		*o++ = rem > 2 ? b64[v & 63] : '=';
	}
	*o = '\0';
	free(z.b.d);

	if (nsurl_create(url_s, &url) != NSERROR_OK) {
		free(url_s);
		return true;
	}
	free(url_s);

	box->flags |= IS_REPLACED;
	ok = html_fetch_object(content, url, box, CONTENT_IMAGE, false);
	nsurl_unref(url);
	return ok;
}
