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
 * Declarative shadow DOM rendering.
 *
 * A host element whose first element child is
 * <template shadowrootmode="..."> renders the template's content in
 * place of its own children, and each <slot> in that content renders
 * the host's light-DOM children assigned to it (or its own fallback
 * content when none are). Box construction walks this "flat tree"
 * through the navigation helpers here.
 *
 * Shadow stylesheets are scoped by rewriting their selectors in terms
 * of the host's tag name (:host -> tag, other selectors become
 * descendants of the tag). This is an approximation: styles can leak
 * into slotted light-DOM content, but component styles are usually
 * identical for every instance of a tag so they are shared.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <dom/dom.h>

#include "utils/corestrings.h"
#include "html/shadow.h"

static bool is_element(dom_node *n)
{
	dom_node_type type;
	return dom_node_get_node_type(n, &type) == DOM_NO_ERR &&
			type == DOM_ELEMENT_NODE;
}

static bool has_tag(dom_node *n, dom_html_element_type want)
{
	dom_html_element_type tag;
	return is_element(n) &&
			dom_html_element_get_tag_type(n, &tag) == DOM_NO_ERR &&
			tag == want;
}

static bool is_slot_element(dom_node *n)
{
	dom_string *name = NULL;
	bool r = false;

	if (!is_element(n))
		return false;
	if (dom_node_get_node_name(n, &name) == DOM_NO_ERR && name != NULL) {
		r = dom_string_caseless_lwc_isequal(name,
				corestring_lwc_slot);
		dom_string_unref(name);
	}
	return r;
}

/* exported interface documented in html/shadow.h */
bool html_shadow_is_template(dom_node *n)
{
	bool has = false;

	if (!has_tag(n, DOM_HTML_ELEMENT_TYPE_TEMPLATE))
		return false;
	if (dom_element_has_attribute(n, corestring_dom_shadowrootmode,
			&has) == DOM_NO_ERR && has)
		return true;
	if (dom_element_has_attribute(n, corestring_dom_shadowroot,
			&has) == DOM_NO_ERR && has)
		return true;
	return false;
}

/* exported interface documented in html/shadow.h */
dom_node *html_shadow_template(dom_node *host)
{
	dom_node *c = NULL, *next;

	if (!is_element(host) ||
	    dom_node_get_first_child(host, &c) != DOM_NO_ERR)
		return NULL;
	while (c != NULL) {
		if (is_element(c)) {
			if (html_shadow_is_template(c))
				return c;
			dom_node_unref(c);
			return NULL;
		}
		if (dom_node_get_next_sibling(c, &next) != DOM_NO_ERR)
			next = NULL;
		dom_node_unref(c);
		c = next;
	}
	return NULL;
}

/**
 * Find the shadow template enclosing a node, if any.
 *
 * \return ref'd template or NULL
 */
static dom_node *enclosing_template(dom_node *n)
{
	dom_node *p = NULL, *cur = dom_node_ref(n);

	while (cur != NULL) {
		if (dom_node_get_parent_node(cur, &p) != DOM_NO_ERR)
			p = NULL;
		dom_node_unref(cur);
		cur = p;
		if (cur != NULL && html_shadow_is_template(cur))
			return cur;
	}
	return NULL;
}

/* exported interface documented in html/shadow.h */
dom_node *html_shadow_host_of(dom_node *n)
{
	dom_node *t = enclosing_template(n), *host = NULL;

	if (t != NULL) {
		if (dom_node_get_parent_node(t, &host) != DOM_NO_ERR)
			host = NULL;
		dom_node_unref(t);
	}
	return host;
}

/** the slot name a light child is assigned by ("" = default slot) */
static dom_string *slot_name_of_light(dom_node *n)
{
	dom_string *s = NULL;

	if (is_element(n) &&
	    dom_element_get_attribute(n, corestring_dom_slot, &s) ==
			DOM_NO_ERR && s != NULL)
		return s;
	return NULL;
}

static bool slot_names_equal(dom_string *a, dom_string *b)
{
	size_t la = a ? dom_string_byte_length(a) : 0;
	size_t lb = b ? dom_string_byte_length(b) : 0;

	if (la == 0 || lb == 0)
		return la == lb;
	return dom_string_isequal(a, b);
}

/**
 * Find the first slot in a shadow tree with a given name, in tree order,
 * without entering nested shadow trees.
 *
 * \return ref'd slot or NULL
 */
static dom_node *find_slot(dom_node *root, dom_string *name)
{
	dom_node *c = NULL, *next, *found = NULL;

	if (dom_node_get_first_child(root, &c) != DOM_NO_ERR)
		return NULL;
	while (c != NULL && found == NULL) {
		if (is_element(c)) {
			if (is_slot_element(c)) {
				dom_string *sn = NULL;
				dom_element_get_attribute(c,
						corestring_dom_name, &sn);
				if (slot_names_equal(sn, name))
					found = dom_node_ref(c);
				if (sn != NULL)
					dom_string_unref(sn);
			}
			if (found == NULL && !html_shadow_is_template(c))
				found = find_slot(c, name);
		}
		if (dom_node_get_next_sibling(c, &next) != DOM_NO_ERR)
			next = NULL;
		dom_node_unref(c);
		c = next;
	}
	if (c != NULL)
		dom_node_unref(c);
	return found;
}

/** is n a light child of host whose slot assignment is slot? */
static bool assigned_to(dom_node *n, dom_node *tmpl, dom_node *slot)
{
	dom_string *name;
	dom_node *s;
	bool r;

	if (n == tmpl)
		return false;
	name = slot_name_of_light(n);
	s = find_slot(tmpl, name);
	if (name != NULL)
		dom_string_unref(name);
	r = (s == slot);
	if (s != NULL)
		dom_node_unref(s);
	return r;
}

/**
 * For a slot in a shadow tree, find the host and template.
 *
 * \return true if slot is a slot of a shadow tree
 */
static bool slot_context(dom_node *slot, dom_node **host, dom_node **tmpl)
{
	*tmpl = NULL;
	*host = NULL;
	if (!is_slot_element(slot))
		return false;
	*tmpl = enclosing_template(slot);
	if (*tmpl == NULL)
		return false;
	if (dom_node_get_parent_node(*tmpl, host) != DOM_NO_ERR ||
			*host == NULL) {
		dom_node_unref(*tmpl);
		*tmpl = NULL;
		return false;
	}
	return true;
}

/** first light child of host at or after n assigned to slot (ref'd) */
static dom_node *next_assigned(dom_node *n, dom_node *tmpl, dom_node *slot)
{
	dom_node *c = n ? dom_node_ref(n) : NULL, *next;

	while (c != NULL) {
		if (assigned_to(c, tmpl, slot))
			return c;
		if (dom_node_get_next_sibling(c, &next) != DOM_NO_ERR)
			next = NULL;
		dom_node_unref(c);
		c = next;
	}
	return NULL;
}

/* exported interface documented in html/shadow.h */
dom_node *html_flat_first_child(dom_node *n)
{
	dom_node *tmpl, *host, *c = NULL, *first;

	tmpl = html_shadow_template(n);
	if (tmpl != NULL) {
		if (dom_node_get_first_child(tmpl, &c) != DOM_NO_ERR)
			c = NULL;
		dom_node_unref(tmpl);
		return c;
	}

	if (slot_context(n, &host, &tmpl)) {
		if (dom_node_get_first_child(host, &first) != DOM_NO_ERR)
			first = NULL;
		c = next_assigned(first, tmpl, n);
		if (first != NULL)
			dom_node_unref(first);
		dom_node_unref(host);
		dom_node_unref(tmpl);
		if (c != NULL)
			return c;
		/* fall back to the slot's own content */
	}

	if (dom_node_get_first_child(n, &c) != DOM_NO_ERR)
		return NULL;
	return c;
}

/* exported interface documented in html/shadow.h */
dom_node *html_flat_next_sibling(dom_node *n)
{
	dom_node *parent = NULL, *next = NULL, *tmpl, *slot, *r;
	dom_string *name;

	if (dom_node_get_parent_node(n, &parent) != DOM_NO_ERR)
		parent = NULL;

	if (parent != NULL && (tmpl = html_shadow_template(parent)) != NULL) {
		/* a light child: next node assigned to the same slot */
		name = slot_name_of_light(n);
		slot = find_slot(tmpl, name);
		if (name != NULL)
			dom_string_unref(name);
		r = NULL;
		if (slot != NULL) {
			if (dom_node_get_next_sibling(n, &next) == DOM_NO_ERR &&
					next != NULL) {
				r = next_assigned(next, tmpl, slot);
				dom_node_unref(next);
			}
			dom_node_unref(slot);
		}
		dom_node_unref(tmpl);
		dom_node_unref(parent);
		return r;
	}
	if (parent != NULL)
		dom_node_unref(parent);

	if (dom_node_get_next_sibling(n, &next) != DOM_NO_ERR)
		return NULL;
	return next;
}

/* exported interface documented in html/shadow.h */
dom_node *html_flat_parent(dom_node *n)
{
	dom_node *p = NULL, *host = NULL, *tmpl;

	if (dom_node_get_parent_node(n, &p) != DOM_NO_ERR || p == NULL)
		return NULL;

	if (html_shadow_is_template(p)) {
		/* shadow tree top level: parent is the host */
		if (dom_node_get_parent_node(p, &host) != DOM_NO_ERR)
			host = NULL;
		dom_node_unref(p);
		return host;
	}

	tmpl = html_shadow_template(p);
	if (tmpl != NULL && tmpl != n) {
		/* light child of a host: parent is its slot */
		dom_string *name = slot_name_of_light(n);
		dom_node *slot = find_slot(tmpl, name);
		if (name != NULL)
			dom_string_unref(name);
		dom_node_unref(tmpl);
		dom_node_unref(p);
		return slot;
	}
	if (tmpl != NULL)
		dom_node_unref(tmpl);
	return p;
}

/* exported interface documented in html/shadow.h */
bool html_flat_has_children(dom_node *n)
{
	dom_node *c = html_flat_first_child(n);

	if (c != NULL) {
		dom_node_unref(c);
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Shadow style scoping */

struct sbuf {
	char *d;
	size_t len, cap;
	bool oom;
};

static void sb_put(struct sbuf *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		char *d;
		while (cap < b->len + n + 1)
			cap *= 2;
		d = realloc(b->d, cap);
		if (d == NULL) {
			b->oom = true;
			return;
		}
		b->d = d;
		b->cap = cap;
	}
	memcpy(b->d + b->len, s, n);
	b->len += n;
	b->d[b->len] = '\0';
}

static void sb_puts(struct sbuf *b, const char *s)
{
	sb_put(b, s, strlen(s));
}

/** index of the ')' matching the '(' at i, or len */
static size_t match_paren(const char *s, size_t len, size_t i)
{
	int depth = 0;

	for (; i < len; i++) {
		if (s[i] == '(')
			depth++;
		else if (s[i] == ')' && --depth == 0)
			return i;
		else if (s[i] == '"' || s[i] == '\'') {
			char q = s[i++];
			while (i < len && s[i] != q) {
				if (s[i] == '\\')
					i++;
				i++;
			}
		}
	}
	return len;
}

/** rewrite one selector (s[0..len)) */
static void scope_selector(struct sbuf *b, const char *s, size_t len,
		const char *host)
{
	size_t i = 0, j;
	bool saw_host = false;

	while (len > 0 && isspace((unsigned char)s[0])) {
		s++;
		len--;
	}
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		len--;
	if (len == 0)
		return;

	/* does the selector start from :host? */
	if (len >= 5 && strncmp(s, ":host", 5) == 0)
		saw_host = true;
	if (!saw_host) {
		sb_puts(b, host);
		sb_puts(b, " ");
	}

	while (i < len) {
		if (s[i] == ':' && i + 13 <= len &&
				strncmp(s + i, ":host-context(", 14) == 0) {
			j = match_paren(s, len, i + 13);
			sb_put(b, s + i + 14, j - (i + 14));
			sb_puts(b, " ");
			sb_puts(b, host);
			i = j + 1;
		} else if (s[i] == ':' && i + 6 <= len &&
				strncmp(s + i, ":host(", 6) == 0) {
			j = match_paren(s, len, i + 5);
			sb_puts(b, host);
			sb_put(b, s + i + 6, j - (i + 6));
			i = j + 1;
		} else if (s[i] == ':' && i + 5 <= len &&
				strncmp(s + i, ":host", 5) == 0 &&
				(i + 5 == len || !(isalnum((unsigned char)s[i + 5]) ||
				s[i + 5] == '-'))) {
			sb_puts(b, host);
			i += 5;
		} else if (s[i] == ':' && i + 10 <= len &&
				strncmp(s + i, "::slotted(", 10) == 0) {
			/* slotted content is a child of the host */
			j = match_paren(s, len, i + 9);
			sb_puts(b, " > ");
			sb_put(b, s + i + 10, j - (i + 10));
			i = j + 1;
		} else if (s[i] == '(') {
			j = match_paren(s, len, i);
			sb_put(b, s + i, (j < len ? j + 1 : len) - i);
			i = j + 1;
		} else {
			sb_put(b, s + i, 1);
			i++;
		}
	}
}

/** rewrite a comma separated selector list */
static void scope_selector_list(struct sbuf *b, const char *s, size_t len,
		const char *host)
{
	size_t i, start = 0;
	int depth = 0;
	bool first = true;

	for (i = 0; i <= len; i++) {
		if (i < len && s[i] == '(')
			depth++;
		else if (i < len && s[i] == ')')
			depth--;
		if (i == len || (s[i] == ',' && depth == 0)) {
			if (!first)
				sb_puts(b, ", ");
			scope_selector(b, s + start, i - start, host);
			first = false;
			start = i + 1;
		}
	}
}

/** skip a block starting at '{', returning the index after its '}' */
static size_t skip_block(const char *s, size_t len, size_t i)
{
	int depth = 0;

	for (; i < len; i++) {
		if (s[i] == '{')
			depth++;
		else if (s[i] == '}' && --depth == 0)
			return i + 1;
		else if (s[i] == '"' || s[i] == '\'') {
			char q = s[i++];
			while (i < len && s[i] != q) {
				if (s[i] == '\\')
					i++;
				i++;
			}
		} else if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') {
			i += 2;
			while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
				i++;
			i++;
		}
	}
	return len;
}

static void scope_rules(struct sbuf *b, const char *s, size_t len,
		const char *host)
{
	size_t i = 0, start;

	while (i < len) {
		/* copy whitespace and comments */
		while (i < len && isspace((unsigned char)s[i]))
			sb_put(b, s + i++, 1);
		if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
			start = i;
			i += 2;
			while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
				i++;
			i = i + 2 > len ? len : i + 2;
			sb_put(b, s + start, i - start);
			continue;
		}
		if (i >= len)
			break;
		if (s[i] == '}') {
			/* stray */
			i++;
			continue;
		}

		/* prelude up to '{' or ';' */
		start = i;
		while (i < len && s[i] != '{' && s[i] != ';') {
			if (s[i] == '"' || s[i] == '\'') {
				char q = s[i++];
				while (i < len && s[i] != q) {
					if (s[i] == '\\')
						i++;
					i++;
				}
			} else if (s[i] == '(') {
				i = match_paren(s, len, i);
			}
			if (i < len)
				i++;
		}
		if (i >= len) {
			sb_put(b, s + start, len - start);
			break;
		}
		if (s[i] == ';') {
			/* @import, @charset, ... */
			sb_put(b, s + start, i + 1 - start);
			i++;
			continue;
		}

		if (s[start] == '@') {
			size_t end = skip_block(s, len, i);
			if (strncmp(s + start, "@media", 6) == 0 ||
			    strncmp(s + start, "@supports", 9) == 0 ||
			    strncmp(s + start, "@layer", 6) == 0 ||
			    strncmp(s + start, "@container", 10) == 0) {
				/* conditional group: scope nested rules */
				sb_put(b, s + start, i + 1 - start);
				scope_rules(b, s + i + 1,
						end > i + 1 ? end - 1 - (i + 1) : 0,
						host);
				sb_puts(b, "}");
			} else {
				/* @keyframes, @font-face, ... verbatim */
				sb_put(b, s + start, end - start);
			}
			i = end;
			continue;
		}

		scope_selector_list(b, s + start, i - start, host);
		start = i;
		i = skip_block(s, len, i);
		sb_put(b, s + start, i - start);
	}
}

/* exported interface documented in html/shadow.h */
char *html_shadow_scope_css(const char *css, size_t len, const char *host)
{
	struct sbuf b = { NULL, 0, 0, false };

	scope_rules(&b, css, len, host);
	if (b.oom) {
		free(b.d);
		return NULL;
	}
	if (b.d == NULL)
		return strdup("");
	return b.d;
}
