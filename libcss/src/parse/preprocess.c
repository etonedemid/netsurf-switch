/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * Stylesheet preprocessor.
 *
 * LibCSS implements CSS 2.1 plus a subset of CSS 3. Modern stylesheets
 * rely on features whose absence makes whole rules (or whole sheets)
 * disappear. Before parsing, stylesheet text is rewritten into an
 * equivalent the parser understands:
 *
 *  - custom properties: var() references are substituted, using
 *    declarations in the same rule first, then those on root-like
 *    selectors (:root, html, *, ...), then any other rule;
 *  - @layer blocks are unwrapped and @layer statements dropped;
 *  - @supports and @container conditions are evaluated and their
 *    contents kept or discarded;
 *  - CSS nesting is flattened;
 *  - selector lists are filtered so one unsupported selector does not
 *    invalidate the rest; :is()/:where() are expanded and
 *    :not(a, b) split;
 *  - modern colour syntax (space separated rgb/hsl, hwb, lab, lch,
 *    oklab, oklch, color(), color-mix()) is converted to rgba();
 *  - calc(), min(), max() and clamp() are folded where possible;
 *  - @keyframes and other unsupported blocks are removed.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libcss/libcss.h>

#include "parse/preprocess.h"

/* Limits guarding against pathological input */
#define PP_MAX_DEPTH 24
#define PP_MAX_VAR_DEPTH 16
#define PP_MAX_SELECTORS 256
#define PP_MAX_GLOBAL_VARS 16384

static int pp_vw = 1280;
static int pp_vh = 720;

/* exported interface documented in libcss/stylesheet.h */
void css_preprocess_set_viewport(int width, int height)
{
	if (width > 0)
		pp_vw = width;
	if (height > 0)
		pp_vh = height;
}

/* ------------------------------------------------------------------ */
/* Growable buffer */

typedef struct buf {
	char *d;
	size_t len;
	size_t cap;
	bool oom;
} buf;

static void buf_add(buf *b, const char *s, size_t n)
{
	if (b->oom || n == 0)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
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

static void buf_str(buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

static void buf_chr(buf *b, char c)
{
	buf_add(b, &c, 1);
}

static void buf_free(buf *b)
{
	free(b->d);
	b->d = NULL;
	b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Text slices and scanning */

typedef struct slice {
	const char *s;
	size_t n;
} slice;

static inline bool pp_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static slice sl_trim(slice x)
{
	while (x.n > 0 && pp_space(x.s[0])) {
		x.s++;
		x.n--;
	}
	while (x.n > 0 && pp_space(x.s[x.n - 1]))
		x.n--;
	return x;
}

static bool sl_eq(slice x, const char *lit)
{
	size_t n = strlen(lit);
	return x.n == n && strncasecmp(x.s, lit, n) == 0;
}

static bool sl_prefix(slice x, const char *lit)
{
	size_t n = strlen(lit);
	return x.n >= n && strncasecmp(x.s, lit, n) == 0;
}

/**
 * Skip one lexical unit starting at i: a string, comment, escape,
 * bracketed group (recursively) or a single character.
 */
static size_t pp_skip_unit(const char *s, size_t len, size_t i)
{
	char c = s[i];

	if (c == '"' || c == '\'') {
		i++;
		while (i < len && s[i] != c && s[i] != '\n') {
			if (s[i] == '\\' && i + 1 < len)
				i++;
			i++;
		}
		return i < len ? i + 1 : len;
	}
	if (c == '/' && i + 1 < len && s[i + 1] == '*') {
		i += 2;
		while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
			i++;
		return i + 1 < len ? i + 2 : len;
	}
	if (c == '\\')
		return i + 2 <= len ? i + 2 : len;
	if (c == '(' || c == '[' || c == '{') {
		char close = c == '(' ? ')' : (c == '[' ? ']' : '}');
		int depth = 0;
		i++;
		while (i < len) {
			if (s[i] == close) {
				return i + 1;
			}
			if (depth++ > 100000)
				break;
			i = pp_skip_unit(s, len, i);
		}
		return len;
	}
	return i + 1;
}

/**
 * Find the first of the characters in stop at bracket depth 0.
 */
static size_t pp_scan_to(const char *s, size_t len, size_t i,
		const char *stop)
{
	while (i < len) {
		if (strchr(stop, s[i]) != NULL)
			return i;
		i = pp_skip_unit(s, len, i);
	}
	return len;
}

/** skip whitespace, comments and HTML comment tokens */
static size_t pp_skip_ws(const char *s, size_t len, size_t i)
{
	for (;;) {
		while (i < len && pp_space(s[i]))
			i++;
		if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
			i = pp_skip_unit(s, len, i);
			continue;
		}
		if (i + 3 < len && strncmp(s + i, "<!--", 4) == 0) {
			i += 4;
			continue;
		}
		if (i + 2 < len && strncmp(s + i, "-->", 3) == 0) {
			i += 3;
			continue;
		}
		return i;
	}
}

/** split a slice at top-level occurrences of sep */
static int pp_split(slice x, char sep, slice *out, int max)
{
	size_t i = 0, start = 0;
	int n = 0;
	char stop[2] = { sep, 0 };

	while (n < max) {
		i = pp_scan_to(x.s, x.n, i, stop);
		out[n].s = x.s + start;
		out[n].n = i - start;
		out[n] = sl_trim(out[n]);
		n++;
		if (i >= x.n)
			break;
		i++;
		start = i;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Custom property storage */

typedef struct var_entry {
	char *name;
	char *value;
} var_entry;

/** a scope of custom properties declared in one rule */
typedef struct var_frame {
	var_entry *v;
	int n, cap;
	struct var_frame *parent;
} var_frame;

/** process-wide variables: root-like first, then any other rule */
static var_entry *global_root;
static int global_root_n, global_root_cap;
static var_entry *global_any;
static int global_any_n, global_any_cap;

static char *pp_strndup(const char *s, size_t n)
{
	char *d = malloc(n + 1);
	if (d != NULL) {
		memcpy(d, s, n);
		d[n] = '\0';
	}
	return d;
}

static var_entry *var_find(var_entry *v, int n, slice name)
{
	int i;
	/* latest definition wins */
	for (i = n - 1; i >= 0; i--) {
		if (strlen(v[i].name) == name.n &&
				memcmp(v[i].name, name.s, name.n) == 0)
			return &v[i];
	}
	return NULL;
}

static void var_set(var_entry **v, int *n, int *cap, slice name,
		slice value, bool replace)
{
	var_entry *e = var_find(*v, *n, name);
	char *val;

	if (e != NULL) {
		if (!replace)
			return;
		val = pp_strndup(value.s, value.n);
		if (val == NULL)
			return;
		free(e->value);
		e->value = val;
		return;
	}
	if (*n >= *cap) {
		int ncap = *cap ? *cap * 2 : 16;
		var_entry *nv;
		if (ncap > PP_MAX_GLOBAL_VARS)
			return;
		nv = realloc(*v, ncap * sizeof(var_entry));
		if (nv == NULL)
			return;
		*v = nv;
		*cap = ncap;
	}
	(*v)[*n].name = pp_strndup(name.s, name.n);
	(*v)[*n].value = pp_strndup(value.s, value.n);
	if ((*v)[*n].name == NULL || (*v)[*n].value == NULL) {
		free((*v)[*n].name);
		free((*v)[*n].value);
		return;
	}
	(*n)++;
}

static void frame_free(var_frame *f)
{
	int i;
	for (i = 0; i < f->n; i++) {
		free(f->v[i].name);
		free(f->v[i].value);
	}
	free(f->v);
}

static const char *var_lookup(const var_frame *f, slice name)
{
	var_entry *e;

	for (; f != NULL; f = f->parent) {
		e = var_find(f->v, f->n, name);
		if (e != NULL)
			return e->value;
	}
	e = var_find(global_root, global_root_n, name);
	if (e != NULL)
		return e->value;
	e = var_find(global_any, global_any_n, name);
	if (e != NULL)
		return e->value;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Preprocessor state */

typedef struct pp_ctx {
	const char *src;
	buf out;
	int depth;
	bool media_ok; /**< current media conditions match the device */
	bool inline_style;
} pp_ctx;

static void pp_rules(pp_ctx *pp, slice text, slice *parent, int nparent,
		var_frame *frame);
static void pp_style_block(pp_ctx *pp, slice *sel, int nsel, slice block,
		var_frame *frame, bool emit_selectors);

/* ------------------------------------------------------------------ */
/* Conditions: media, container, supports */

/** convert a length in a media feature to px */
static bool pp_media_length(slice v, float *px)
{
	char tmp[64];
	char *end;
	float f;

	v = sl_trim(v);
	if (v.n == 0 || v.n >= sizeof(tmp))
		return false;
	memcpy(tmp, v.s, v.n);
	tmp[v.n] = '\0';
	f = strtof(tmp, &end);
	if (end == tmp)
		return false;
	if (strncasecmp(end, "em", 2) == 0 || strncasecmp(end, "rem", 3) == 0)
		f *= 16;
	else if (strncasecmp(end, "vw", 2) == 0)
		f *= pp_vw / 100.0f;
	else if (strncasecmp(end, "vh", 2) == 0)
		f *= pp_vh / 100.0f;
	else if (strncasecmp(end, "pt", 2) == 0)
		f *= 96.0f / 72.0f;
	else if (strncasecmp(end, "dppx", 4) == 0 || *end == 'x')
		f *= 96.0f;
	else if (strncasecmp(end, "dpi", 3) == 0)
		f *= 1.0f;
	*px = f;
	return true;
}

/** evaluate one media feature "(name: value)" or range form */
static bool pp_media_feature(slice f)
{
	slice parts[2];
	slice name, value;
	float v, cmp;
	size_t i;

	f = sl_trim(f);
	/* range syntax: (width >= 600px), (400px <= width < 700px) */
	for (i = 0; i < f.n; i++) {
		if (f.s[i] == '<' || f.s[i] == '>') {
			const char *s = f.s;
			size_t n = f.n;
			bool width;
			char buf2[128];
			char a[64], op1[3] = "", b2[64], op2[3] = "", c[64];
			int k;
			if (n >= sizeof(buf2))
				return true;
			memcpy(buf2, s, n);
			buf2[n] = '\0';
			k = sscanf(buf2, " %63[^<>=] %2[<>=] %63[^<>=] %2[<>=] %63s",
					a, op1, b2, op2, c);
			if (k == 3) {
				slice sa = sl_trim((slice){ a, strlen(a) });
				slice sb = sl_trim((slice){ b2, strlen(b2) });
				bool name_first = isalpha((unsigned char)sa.s[0]);
				slice nm = name_first ? sa : sb;
				slice val = name_first ? sb : sa;
				float dim;
				width = sl_prefix(nm, "width") ||
						sl_prefix(nm, "inline-size");
				if (!width && !sl_prefix(nm, "height") &&
						!sl_prefix(nm, "block-size"))
					return true;
				dim = width ? pp_vw : pp_vh;
				if (!pp_media_length(val, &v))
					return true;
				if (!name_first) {
					/* "600px < width" */
					if (op1[0] == '<')
						return op1[1] == '=' ? v <= dim : v < dim;
					return op1[1] == '=' ? v >= dim : v > dim;
				}
				if (op1[0] == '<')
					return op1[1] == '=' ? dim <= v : dim < v;
				if (op1[0] == '>')
					return op1[1] == '=' ? dim >= v : dim > v;
				return fabsf(dim - v) < 0.5f;
			}
			if (k == 5) {
				slice sb = sl_trim((slice){ b2, strlen(b2) });
				float lo, hi, dim;
				if (!pp_media_length((slice){ a, strlen(a) }, &lo) ||
				    !pp_media_length((slice){ c, strlen(c) }, &hi))
					return true;
				dim = sl_prefix(sb, "height") ? pp_vh : pp_vw;
				return (op1[1] == '=' ? lo <= dim : lo < dim) &&
					(op2[1] == '=' ? dim <= hi : dim < hi);
			}
			return true;
		}
	}

	if (pp_split(f, ':', parts, 2) < 2) {
		/* boolean feature */
		if (sl_eq(f, "color") || sl_eq(f, "hover") ||
				sl_eq(f, "pointer") || sl_eq(f, "any-hover") ||
				sl_eq(f, "any-pointer"))
			return true;
		if (sl_eq(f, "grid") || sl_eq(f, "prefers-reduced-motion") ||
				sl_eq(f, "prefers-contrast") ||
				sl_eq(f, "forced-colors") ||
				sl_eq(f, "inverted-colors"))
			return false;
		return true;
	}
	name = parts[0];
	value = parts[1];

	if (sl_eq(name, "prefers-color-scheme"))
		return sl_eq(value, "light");
	if (sl_eq(name, "prefers-reduced-motion"))
		return sl_eq(value, "no-preference");
	if (sl_eq(name, "prefers-contrast") || sl_eq(name, "forced-colors") ||
			sl_eq(name, "prefers-reduced-transparency") ||
			sl_eq(name, "prefers-reduced-data"))
		return sl_eq(value, "no-preference") || sl_eq(value, "none");
	if (sl_eq(name, "orientation"))
		return sl_eq(value, pp_vw >= pp_vh ? "landscape" : "portrait");
	if (sl_eq(name, "hover") || sl_eq(name, "any-hover"))
		return sl_eq(value, "hover");
	if (sl_eq(name, "pointer") || sl_eq(name, "any-pointer"))
		return sl_eq(value, "fine");
	if (sl_eq(name, "display-mode"))
		return sl_eq(value, "browser");
	if (sl_eq(name, "scripting"))
		return sl_eq(value, "enabled");

	if (!pp_media_length(value, &v))
		return true;
	if (sl_eq(name, "min-width") || sl_eq(name, "min-device-width") ||
			sl_eq(name, "min-inline-size"))
		return pp_vw >= v;
	if (sl_eq(name, "max-width") || sl_eq(name, "max-device-width") ||
			sl_eq(name, "max-inline-size"))
		return pp_vw <= v;
	if (sl_eq(name, "min-height") || sl_eq(name, "min-device-height"))
		return pp_vh >= v;
	if (sl_eq(name, "max-height") || sl_eq(name, "max-device-height"))
		return pp_vh <= v;
	if (sl_eq(name, "width"))
		return fabsf(pp_vw - v) < 0.5f;
	if (sl_eq(name, "height"))
		return fabsf(pp_vh - v) < 0.5f;
	cmp = 96.0f; /* device resolution: 1dppx */
	if (sl_eq(name, "min-resolution") ||
			sl_eq(name, "-webkit-min-device-pixel-ratio") ||
			sl_eq(name, "min-device-pixel-ratio")) {
		if (!sl_eq(name, "min-resolution"))
			v *= 96.0f;
		return cmp >= v;
	}
	if (sl_eq(name, "max-resolution") ||
			sl_eq(name, "-webkit-max-device-pixel-ratio")) {
		if (!sl_eq(name, "max-resolution"))
			v *= 96.0f;
		return cmp <= v;
	}
	return true;
}

/** evaluate a media query list against the device */
static bool pp_media_eval(slice q)
{
	slice list[32];
	int n = pp_split(q, ',', list, 32), i;

	if (n == 1 && list[0].n == 0)
		return true;

	for (i = 0; i < n; i++) {
		slice m = list[i];
		size_t p = 0;
		bool neg = false, ok = true;

		p = pp_skip_ws(m.s, m.n, p);
		if (m.n - p >= 4 && strncasecmp(m.s + p, "not ", 4) == 0) {
			neg = true;
			p += 4;
		} else if (m.n - p >= 5 &&
				strncasecmp(m.s + p, "only ", 5) == 0) {
			p += 5;
		}
		while (p < m.n) {
			p = pp_skip_ws(m.s, m.n, p);
			if (p >= m.n)
				break;
			if (m.s[p] == '(') {
				size_t e = pp_skip_unit(m.s, m.n, p);
				slice f = { m.s + p + 1, e - p - 2 };
				if (e - p < 2)
					break;
				f = sl_trim(f);
				if (f.n > 0 && f.s[0] == '(') {
					/* nested condition */
					ok = ok && pp_media_eval(f);
				} else if (sl_prefix(f, "not ")) {
					slice g = { f.s + 4, f.n - 4 };
					g = sl_trim(g);
					if (g.n > 1 && g.s[0] == '(')
						g = (slice){ g.s + 1, g.n - 2 };
					ok = ok && !pp_media_feature(g);
				} else {
					ok = ok && pp_media_feature(f);
				}
				p = e;
			} else {
				size_t e = p;
				slice w;
				while (e < m.n && !pp_space(m.s[e]) &&
						m.s[e] != '(')
					e++;
				w = (slice){ m.s + p, e - p };
				if (sl_eq(w, "and")) {
					/* conjunction */
				} else if (sl_eq(w, "or")) {
					if (ok && !neg)
						return true;
					ok = true;
				} else if (sl_eq(w, "all") || sl_eq(w, "screen")) {
					/* matches */
				} else {
					/* print, speech, tv, ... */
					ok = false;
				}
				p = e;
			}
		}
		if (ok != neg)
			return true;
	}
	return false;
}

/* exported interface documented in libcss/stylesheet.h */
bool css_media_query_matches(const char *query)
{
	slice q;
	if (query == NULL)
		return false;
	q.s = query;
	q.n = strlen(query);
	return pp_media_eval(sl_trim(q));
}

/** is a property name one LibCSS knows? */
static bool pp_known_property(slice name)
{
	static const char *const known[] = {
		"align-content", "align-items", "align-self", "background",
		"background-attachment", "background-color",
		"background-image", "background-position",
		"background-repeat", "background-size", "border",
		"border-bottom", "border-collapse", "border-color",
		"border-left", "border-radius", "border-right",
		"border-spacing", "border-style", "border-top",
		"border-width", "bottom", "box-shadow", "box-sizing",
		"clear", "clip", "color", "column-count", "column-gap",
		"columns", "content", "cursor", "direction", "display",
		"filter", "flex", "flex-basis", "flex-direction",
		"flex-flow", "flex-grow", "flex-shrink", "flex-wrap",
		"float", "font", "font-family", "font-size", "font-style",
		"font-variant", "font-weight", "gap", "grid",
		"grid-area", "grid-column", "grid-row",
		"grid-template-columns", "grid-template-rows",
		"grid-template-areas", "height", "inset",
		"justify-content", "justify-items", "left",
		"letter-spacing", "line-height", "list-style", "margin",
		"max-height", "max-width", "min-height", "min-width",
		"object-fit", "opacity", "order", "outline", "overflow",
		"overflow-x", "overflow-y", "overflow-wrap", "padding",
		"place-items", "place-content", "position", "right",
		"row-gap", "text-align", "text-decoration", "text-indent",
		"text-overflow", "text-shadow", "text-transform", "top",
		"transform", "aspect-ratio", "vertical-align",
		"visibility", "white-space", "width", "word-break",
		"word-spacing", "z-index", "border-top-left-radius",
		"border-top-right-radius", "border-bottom-left-radius",
		"border-bottom-right-radius", "pointer-events",
		"line-clamp", "-webkit-line-clamp", "object-position",
		"transform-origin", "justify-self", "grid-auto-flow",
		"grid-auto-columns", "grid-auto-rows", "word-wrap",
		NULL
	};
	int i;

	for (i = 0; known[i] != NULL; i++) {
		if (sl_eq(name, known[i]))
			return true;
	}
	return false;
}

static bool pp_selector_supported(slice sel);

/** evaluate an @supports condition */
static bool pp_supports_eval(slice c)
{
	size_t p = 0;
	bool result = true, have = false, neg = false;
	int op = 0; /* 0 none, 1 and, 2 or */

	while (p < c.n) {
		bool v;
		slice w;
		size_t e;

		p = pp_skip_ws(c.s, c.n, p);
		if (p >= c.n)
			break;

		if (c.s[p] == '(') {
			slice inner;
			e = pp_skip_unit(c.s, c.n, p);
			if (e - p < 2)
				break;
			inner = sl_trim((slice){ c.s + p + 1, e - p - 2 });
			if (inner.n > 0 && (inner.s[0] == '(' ||
					sl_prefix(inner, "not ") ||
					sl_prefix(inner, "selector("))) {
				v = pp_supports_eval(inner);
			} else {
				slice parts[2];
				if (pp_split(inner, ':', parts, 2) == 2) {
					v = pp_known_property(parts[0]);
					/* values we cannot render */
					if (v && sl_eq(parts[0], "display") &&
					    (sl_prefix(parts[1], "subgrid") ||
					     sl_prefix(parts[1], "ruby")))
						v = false;
					if (v && sl_prefix(parts[1], "subgrid"))
						v = false;
				} else {
					v = false;
				}
			}
			p = e;
		} else {
			e = p;
			while (e < c.n && !pp_space(c.s[e]) && c.s[e] != '(')
				e++;
			w = (slice){ c.s + p, e - p };
			if (sl_eq(w, "not")) {
				neg = !neg;
				p = e;
				continue;
			}
			if (sl_eq(w, "and")) {
				op = 1;
				p = e;
				continue;
			}
			if (sl_eq(w, "or")) {
				op = 2;
				p = e;
				continue;
			}
			if (e < c.n && c.s[e] == '(') {
				size_t fe = pp_skip_unit(c.s, c.n, e);
				slice arg = sl_trim((slice){ c.s + e + 1,
						fe - e - 2 });
				if (sl_eq(w, "selector"))
					v = pp_selector_supported(arg);
				else
					v = false; /* font-tech() etc */
				p = fe;
			} else {
				v = false;
				p = e;
			}
		}

		if (neg) {
			v = !v;
			neg = false;
		}
		if (!have) {
			result = v;
			have = true;
		} else if (op == 2) {
			result = result || v;
		} else {
			result = result && v;
		}
	}
	return result;
}

/* ------------------------------------------------------------------ */
/* Selectors */

/** pseudo-classes and elements LibCSS understands */
static bool pp_pseudo_supported(slice name, bool element)
{
	static const char *const classes[] = {
		"first-child", "link", "visited", "hover", "active", "focus",
		"lang", "root", "nth-child", "nth-last-child", "nth-of-type",
		"nth-last-of-type", "last-child", "first-of-type",
		"last-of-type", "only-child", "only-of-type", "empty",
		"target", "enabled", "disabled", "checked", "not",
		"first-line", "first-letter", "before", "after", NULL
	};
	static const char *const elements[] = {
		"first-line", "first-letter", "before", "after", NULL
	};
	const char *const *list = element ? elements : classes;
	int i;

	for (i = 0; list[i] != NULL; i++) {
		if (sl_eq(name, list[i]))
			return true;
	}
	return false;
}

/**
 * Rewrite a single complex selector (no top-level commas) so LibCSS can
 * parse it, appending the result(s) to out as a comma separated list.
 *
 * \return number of selectors written (0 if unsupported)
 */
static int pp_selector_rewrite(slice sel, buf *out, int budget);

/**
 * Expand the first :is()/:where()/:matches()/:any() in sel.
 */
static int pp_selector_expand(slice sel, size_t at, size_t name_len,
		size_t close, buf *out, int budget)
{
	slice args[64];
	slice inner = { sel.s + at + name_len + 1,
			close - (at + name_len + 1) - 1 };
	int n = pp_split(inner, ',', args, 64);
	int i, written = 0;

	for (i = 0; i < n && written < budget; i++) {
		buf tmp = { 0 };
		slice arg = args[i];
		int w;
		/* forgiving list: skip empties */
		if (arg.n == 0)
			continue;
		buf_add(&tmp, sel.s, at);
		/* complex arguments are only exact at the start of the
		 * selector or after a combinator */
		buf_add(&tmp, arg.s, arg.n);
		buf_add(&tmp, sel.s + close, sel.n - close);
		if (tmp.oom) {
			buf_free(&tmp);
			return written;
		}
		w = pp_selector_rewrite((slice){ tmp.d, tmp.len }, out,
				budget - written);
		written += w;
		buf_free(&tmp);
	}
	return written;
}

static int pp_selector_rewrite(slice sel, buf *out, int budget)
{
	size_t i = 0;
	buf res = { 0 };

	sel = sl_trim(sel);
	if (sel.n == 0 || budget <= 0)
		return 0;

	while (i < sel.n) {
		char c = sel.s[i];

		if (c == '[') {
			/* attribute selector: strip case-sensitivity flags */
			size_t e = pp_skip_unit(sel.s, sel.n, i);
			size_t k = e - 1;
			size_t start = i;
			if (e - i >= 2) {
				size_t j = k;
				while (j > start && pp_space(sel.s[j - 1]))
					j--;
				if (j > start + 2 &&
				    (sel.s[j - 1] == 'i' || sel.s[j - 1] == 's' ||
				     sel.s[j - 1] == 'I' || sel.s[j - 1] == 'S') &&
				    pp_space(sel.s[j - 2])) {
					buf_add(&res, sel.s + i, j - 2 - i);
					buf_chr(&res, ']');
					i = e;
					continue;
				}
			}
			buf_add(&res, sel.s + i, e - i);
			i = e;
			continue;
		}

		if (c == '"' || c == '\'' || c == '\\') {
			size_t e = pp_skip_unit(sel.s, sel.n, i);
			buf_add(&res, sel.s + i, e - i);
			i = e;
			continue;
		}

		if (c == ':') {
			bool element = (i + 1 < sel.n && sel.s[i + 1] == ':');
			size_t ns = i + (element ? 2 : 1), ne = ns;
			slice name;

			while (ne < sel.n && (isalnum((unsigned char)sel.s[ne]) ||
					sel.s[ne] == '-' || sel.s[ne] == '_'))
				ne++;
			name = (slice){ sel.s + ns, ne - ns };

			if (!element && ne < sel.n && sel.s[ne] == '(' &&
			    (sl_eq(name, "is") || sl_eq(name, "where") ||
			     sl_eq(name, "matches") || sl_eq(name, "-webkit-any") ||
			     sl_eq(name, "-moz-any"))) {
				size_t close = pp_skip_unit(sel.s, sel.n, ne);
				/* rebuild the selector with what we have so
				 * far and expand the function */
				buf whole = { 0 };
				int w;
				buf_add(&whole, res.d ? res.d : "", res.len);
				buf_add(&whole, sel.s + i, sel.n - i);
				if (whole.oom) {
					buf_free(&whole);
					buf_free(&res);
					return 0;
				}
				w = pp_selector_expand(
					(slice){ whole.d, whole.len }, res.len,
					(ne - i), res.len + (close - i), out,
					budget);
				buf_free(&whole);
				buf_free(&res);
				return w;
			}

			if (!element && ne < sel.n && sel.s[ne] == '(' &&
					sl_eq(name, "not")) {
				size_t close = pp_skip_unit(sel.s, sel.n, ne);
				slice args[16];
				slice inner = { sel.s + ne + 1, close - ne - 2 };
				int n = pp_split(inner, ',', args, 16), k;
				for (k = 0; k < n; k++) {
					size_t j;
					bool simple = true;
					/* :not() only takes simple selectors */
					for (j = 0; j < args[k].n; j++) {
						char ch = args[k].s[j];
						if (pp_space(ch) || ch == '>' ||
						    ch == '+' || ch == '~' ||
						    ch == ':' || ch == '(') {
							simple = ch == ':' &&
								j == 0;
							if (!simple)
								break;
						}
					}
					if (!simple || args[k].n == 0) {
						buf_free(&res);
						return 0;
					}
					buf_str(&res, ":not(");
					buf_add(&res, args[k].s, args[k].n);
					buf_chr(&res, ')');
				}
				i = close;
				continue;
			}

			/* map near-equivalents */
			if (!element && sl_eq(name, "focus-visible")) {
				buf_str(&res, ":focus");
				i = ne;
				continue;
			}
			if (!element && sl_eq(name, "any-link")) {
				buf_str(&res, ":link");
				i = ne;
				continue;
			}
			if (!element && (sl_eq(name, "hover") ||
					sl_eq(name, "active"))) {
				buf_add(&res, sel.s + i, ne - i);
				i = ne;
				continue;
			}

			if (!pp_pseudo_supported(name, element)) {
				buf_free(&res);
				return 0;
			}
			if (ne < sel.n && sel.s[ne] == '(') {
				size_t close = pp_skip_unit(sel.s, sel.n, ne);
				slice arg = { sel.s + ne + 1, close - ne - 2 };
				/* nth-child(An+B of S) is unsupported */
				if (arg.n > 3) {
					size_t j;
					for (j = 0; j + 3 < arg.n; j++) {
						if (strncasecmp(arg.s + j,
							" of ", 4) == 0) {
							buf_free(&res);
							return 0;
						}
					}
				}
				buf_add(&res, sel.s + i, close - i);
				i = close;
			} else {
				buf_add(&res, sel.s + i, ne - i);
				i = ne;
			}
			continue;
		}

		if (c == '&') {
			/* stray nesting selector with no parent */
			buf_free(&res);
			return 0;
		}

		buf_chr(&res, c);
		i++;
	}

	if (res.oom || res.len == 0) {
		buf_free(&res);
		return 0;
	}
	if (out->len > 0 && out->d[out->len - 1] != ',')
		buf_chr(out, ',');
	buf_add(out, res.d, res.len);
	buf_free(&res);
	return 1;
}

static bool pp_selector_supported(slice sel)
{
	buf tmp = { 0 };
	int n = pp_selector_rewrite(sel, &tmp, 1);
	buf_free(&tmp);
	return n > 0;
}

/**
 * Combine a parent selector list with a nested rule's selector list.
 */
static int pp_selectors_nest(slice *parent, int nparent, slice nested,
		buf *store, slice *out, int max)
{
	slice kids[PP_MAX_SELECTORS];
	int nk = pp_split(nested, ',', kids, PP_MAX_SELECTORS);
	int p, k, n = 0;
	size_t *offsets = malloc(sizeof(size_t) * (max + 1));
	size_t *lens = malloc(sizeof(size_t) * (max + 1));

	if (offsets == NULL || lens == NULL) {
		free(offsets);
		free(lens);
		return 0;
	}

	for (p = 0; p < nparent; p++) {
		for (k = 0; k < nk && n < max; k++) {
			slice kid = kids[k];
			size_t start = store->len, j;
			bool amp = false;

			for (j = 0; j < kid.n; j++) {
				if (kid.s[j] == '&') {
					amp = true;
					break;
				}
			}
			if (amp) {
				for (j = 0; j < kid.n; j++) {
					if (kid.s[j] == '&')
						buf_add(store, parent[p].s,
								parent[p].n);
					else
						buf_chr(store, kid.s[j]);
				}
			} else {
				buf_add(store, parent[p].s, parent[p].n);
				buf_chr(store, ' ');
				buf_add(store, kid.s, kid.n);
			}
			offsets[n] = start;
			lens[n] = store->len - start;
			n++;
		}
	}
	/* store may have moved; resolve slices at the end */
	for (k = 0; k < n; k++) {
		out[k].s = store->d + offsets[k];
		out[k].n = lens[k];
	}
	free(offsets);
	free(lens);
	return store->oom ? 0 : n;
}

/** write a selector list to the output, filtering unsupported ones */
static bool pp_emit_selectors(pp_ctx *pp, slice *sel, int nsel)
{
	buf list = { 0 };
	int i, total = 0;

	for (i = 0; i < nsel; i++) {
		total += pp_selector_rewrite(sel[i], &list,
				PP_MAX_SELECTORS - total);
		if (total >= PP_MAX_SELECTORS)
			break;
	}
	if (total == 0 || list.oom) {
		buf_free(&list);
		return false;
	}
	buf_add(&pp->out, list.d, list.len);
	buf_free(&list);
	return true;
}

/** is a selector list "root-like" for custom property purposes? */
static bool pp_selectors_rootish(slice *sel, int nsel)
{
	int i;

	for (i = 0; i < nsel; i++) {
		if (sl_eq(sel[i], ":root") || sl_eq(sel[i], "html") ||
				sl_eq(sel[i], "*") || sl_eq(sel[i], ":host") ||
				sl_eq(sel[i], "body") ||
				sl_eq(sel[i], "::backdrop") ||
				sl_eq(sel[i], ":where(:root)") ||
				sl_eq(sel[i], ":root, :host"))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Values */

static bool pp_subst_vars(slice v, const var_frame *frame, buf *out,
		int depth);

/**
 * Substitute var() and env() references in a value.
 *
 * \return false if a reference could not be resolved
 */
static bool pp_subst_vars(slice v, const var_frame *frame, buf *out,
		int depth)
{
	size_t i = 0, last = 0;

	if (depth > PP_MAX_VAR_DEPTH)
		return false;

	while (i < v.n) {
		char c = v.s[i];
		bool is_var = false, is_env = false;

		if (c == '"' || c == '\'' || (c == '/' && i + 1 < v.n &&
				v.s[i + 1] == '*')) {
			i = pp_skip_unit(v.s, v.n, i);
			continue;
		}
		if ((c == 'v' || c == 'V') && i + 4 <= v.n &&
				strncasecmp(v.s + i, "var(", 4) == 0 &&
				(i == 0 || !(isalnum((unsigned char)v.s[i - 1]) ||
					v.s[i - 1] == '-')))
			is_var = true;
		if ((c == 'e' || c == 'E') && i + 4 <= v.n &&
				strncasecmp(v.s + i, "env(", 4) == 0 &&
				(i == 0 || !(isalnum((unsigned char)v.s[i - 1]) ||
					v.s[i - 1] == '-')))
			is_env = true;

		if (is_var || is_env) {
			size_t open = i + 3;
			size_t close = pp_skip_unit(v.s, v.n, open);
			slice inner = { v.s + open + 1,
					close > open + 1 ? close - open - 2 : 0 };
			size_t comma = pp_scan_to(inner.s, inner.n, 0, ",");
			slice name = sl_trim((slice){ inner.s, comma });
			const char *val = NULL;

			buf_add(out, v.s + last, i - last);

			if (is_var)
				val = var_lookup(frame, name);
			if (val != NULL) {
				slice sv = sl_trim((slice){ val, strlen(val) });
				if (!pp_subst_vars(sv, frame, out, depth + 1))
					return false;
			} else if (comma < inner.n) {
				slice fb = sl_trim((slice){ inner.s + comma + 1,
						inner.n - comma - 1 });
				if (!pp_subst_vars(fb, frame, out, depth + 1))
					return false;
			} else if (is_env) {
				buf_str(out, "0px");
			} else {
				return false;
			}
			i = close;
			last = i;
			continue;
		}
		i++;
	}
	buf_add(out, v.s + last, v.n - last);
	return true;
}

/* calc() evaluation: a value is a number with a unit class */
typedef struct pp_num {
	float v;
	char unit[8]; /* "" for plain numbers */
	bool px_only; /* converted to px */
} pp_num;

static bool pp_calc_sum(const char **p, const char *end, pp_num *out);

static void pp_calc_ws(const char **p, const char *end)
{
	while (*p < end && pp_space(**p))
		(*p)++;
}

/** convert a unit to px if possible (font-relative units assume 16px) */
static bool pp_unit_to_px(pp_num *n)
{
	const char *u = n->unit;
	float f;

	if (u[0] == '\0' || strcmp(u, "px") == 0)
		f = 1;
	else if (strcasecmp(u, "rem") == 0 || strcasecmp(u, "em") == 0)
		f = 16;
	else if (strcasecmp(u, "vw") == 0 || strcasecmp(u, "svw") == 0 ||
			strcasecmp(u, "dvw") == 0 || strcasecmp(u, "lvw") == 0)
		f = pp_vw / 100.0f;
	else if (strcasecmp(u, "vh") == 0 || strcasecmp(u, "svh") == 0 ||
			strcasecmp(u, "dvh") == 0 || strcasecmp(u, "lvh") == 0)
		f = pp_vh / 100.0f;
	else if (strcasecmp(u, "vmin") == 0)
		f = (pp_vw < pp_vh ? pp_vw : pp_vh) / 100.0f;
	else if (strcasecmp(u, "vmax") == 0)
		f = (pp_vw > pp_vh ? pp_vw : pp_vh) / 100.0f;
	else if (strcasecmp(u, "pt") == 0)
		f = 96.0f / 72.0f;
	else if (strcasecmp(u, "pc") == 0)
		f = 16;
	else if (strcasecmp(u, "in") == 0)
		f = 96;
	else if (strcasecmp(u, "cm") == 0)
		f = 96.0f / 2.54f;
	else if (strcasecmp(u, "mm") == 0)
		f = 96.0f / 25.4f;
	else if (strcasecmp(u, "ch") == 0 || strcasecmp(u, "ex") == 0)
		f = 8;
	else
		return false;
	if (u[0] != '\0') {
		n->v *= f;
		strcpy(n->unit, "px");
	}
	return true;
}

static bool pp_calc_value(const char **p, const char *end, pp_num *out)
{
	char *e;
	size_t k = 0;

	pp_calc_ws(p, end);
	if (*p >= end)
		return false;
	if (**p == '(') {
		(*p)++;
		if (!pp_calc_sum(p, end, out))
			return false;
		pp_calc_ws(p, end);
		if (*p >= end || **p != ')')
			return false;
		(*p)++;
		return true;
	}
	if (end - *p > 5 && strncasecmp(*p, "calc(", 5) == 0) {
		*p += 5;
		if (!pp_calc_sum(p, end, out))
			return false;
		pp_calc_ws(p, end);
		if (*p >= end || **p != ')')
			return false;
		(*p)++;
		return true;
	}
	out->v = strtof(*p, &e);
	if (e == *p || e > end)
		return false;
	*p = e;
	out->unit[0] = '\0';
	out->px_only = false;
	if (*p < end && **p == '%') {
		strcpy(out->unit, "%");
		(*p)++;
		return true;
	}
	while (*p < end && isalpha((unsigned char)**p) && k < 7)
		out->unit[k++] = *(*p)++;
	out->unit[k] = '\0';
	return true;
}

/** make a and b compatible for addition */
static bool pp_calc_unify(pp_num *a, pp_num *b)
{
	if (strcasecmp(a->unit, b->unit) == 0)
		return true;
	if (a->unit[0] == '%' || b->unit[0] == '%')
		return false;
	/* adding 0 of anything */
	if (a->v == 0.0f && a->unit[0] == '\0') {
		strcpy(a->unit, b->unit);
		return true;
	}
	if (b->v == 0.0f && b->unit[0] == '\0') {
		strcpy(b->unit, a->unit);
		return true;
	}
	return pp_unit_to_px(a) && pp_unit_to_px(b);
}

static bool pp_calc_product(const char **p, const char *end, pp_num *out)
{
	pp_num r;

	if (!pp_calc_value(p, end, out))
		return false;
	for (;;) {
		const char *save = *p;
		char op;
		pp_calc_ws(p, end);
		if (*p >= end || (**p != '*' && **p != '/')) {
			*p = save;
			return true;
		}
		op = *(*p)++;
		if (!pp_calc_value(p, end, &r))
			return false;
		if (op == '*') {
			if (out->unit[0] == '\0') {
				out->v *= r.v;
				strcpy(out->unit, r.unit);
			} else if (r.unit[0] == '\0') {
				out->v *= r.v;
			} else {
				return false;
			}
		} else {
			if (r.unit[0] != '\0' || r.v == 0.0f)
				return false;
			out->v /= r.v;
		}
	}
}

static bool pp_calc_sum(const char **p, const char *end, pp_num *out)
{
	pp_num r;

	if (!pp_calc_product(p, end, out))
		return false;
	for (;;) {
		const char *save = *p;
		char op;
		pp_calc_ws(p, end);
		if (*p >= end || (**p != '+' && **p != '-')) {
			*p = save;
			return true;
		}
		op = *(*p)++;
		if (!pp_calc_product(p, end, &r))
			return false;
		if (!pp_calc_unify(out, &r))
			return false;
		out->v = op == '+' ? out->v + r.v : out->v - r.v;
	}
}

/** evaluate min()/max()/clamp() argument lists */
static bool pp_calc_minmax(const char **p, const char *end, int kind,
		pp_num *out)
{
	pp_num vals[8];
	int n = 0, i;

	for (;;) {
		if (n >= 8 || !pp_calc_sum(p, end, &vals[n]))
			return false;
		n++;
		pp_calc_ws(p, end);
		if (*p < end && **p == ',') {
			(*p)++;
			continue;
		}
		if (*p < end && **p == ')') {
			(*p)++;
			break;
		}
		return false;
	}
	for (i = 1; i < n; i++) {
		if (!pp_calc_unify(&vals[0], &vals[i]))
			return false;
	}
	for (i = 1; i < n; i++) {
		if (!pp_calc_unify(&vals[0], &vals[i]))
			return false;
	}
	if (kind == 2) {
		float lo, mid, hi;
		if (n != 3)
			return false;
		lo = vals[0].v; mid = vals[1].v; hi = vals[2].v;
		*out = vals[0];
		out->v = mid < lo ? lo : (mid > hi ? hi : mid);
		if (out->v < lo)
			out->v = lo;
		return true;
	}
	*out = vals[0];
	for (i = 1; i < n; i++) {
		if (kind == 0 ? vals[i].v < out->v : vals[i].v > out->v)
			out->v = vals[i].v;
	}
	return true;
}

/** write a number compactly */
static void pp_emit_num(buf *out, const pp_num *n)
{
	char tmp[48];
	float v = n->v;

	if (fabsf(v - roundf(v)) < 0.0005f)
		snprintf(tmp, sizeof(tmp), "%d%s", (int)roundf(v), n->unit);
	else
		snprintf(tmp, sizeof(tmp), "%.3f%s", v, n->unit);
	buf_str(out, tmp);
}

/**
 * Rewrite a (var-substituted) value: fold math functions and convert
 * modern colour syntax.
 */
static void pp_rewrite_value(slice v, buf *out)
{
	size_t i = 0, last = 0;

	while (i < v.n) {
		char c = v.s[i];
		size_t ne;
		slice fname;

		if (c == '"' || c == '\'') {
			i = pp_skip_unit(v.s, v.n, i);
			continue;
		}
		if (!isalpha((unsigned char)c) && c != '-') {
			i++;
			continue;
		}
		if (i > 0 && (isalnum((unsigned char)v.s[i - 1]) ||
				v.s[i - 1] == '-' || v.s[i - 1] == '#')) {
			i++;
			continue;
		}
		ne = i;
		while (ne < v.n && (isalnum((unsigned char)v.s[ne]) ||
				v.s[ne] == '-'))
			ne++;
		fname = (slice){ v.s + i, ne - i };
		if (ne >= v.n || v.s[ne] != '(') {
			i = ne;
			continue;
		}

		if (sl_eq(fname, "url")) {
			i = pp_skip_unit(v.s, v.n, ne);
			continue;
		}

		if (sl_eq(fname, "calc") || sl_eq(fname, "-webkit-calc") ||
				sl_eq(fname, "min") || sl_eq(fname, "max") ||
				sl_eq(fname, "clamp")) {
			size_t close = pp_skip_unit(v.s, v.n, ne);
			const char *p = v.s + ne + 1;
			const char *end = v.s + close;
			pp_num n;
			bool ok;

			if (sl_eq(fname, "calc") || sl_eq(fname, "-webkit-calc")) {
				ok = pp_calc_sum(&p, end, &n);
				pp_calc_ws(&p, end);
				ok = ok && p == end - 1 && *p == ')';
			} else {
				int kind = sl_eq(fname, "min") ? 0 :
						(sl_eq(fname, "max") ? 1 : 2);
				ok = pp_calc_minmax(&p, end, kind, &n) &&
						p == end;
			}
			if (ok) {
				buf_add(out, v.s + last, i - last);
				pp_emit_num(out, &n);
				last = i = close;
			} else {
				i = close;
			}
			continue;
		}

		if (sl_eq(fname, "rgb") || sl_eq(fname, "rgba") ||
				sl_eq(fname, "hsl") || sl_eq(fname, "hsla") ||
				sl_eq(fname, "hwb") || sl_eq(fname, "lab") ||
				sl_eq(fname, "lch") || sl_eq(fname, "oklab") ||
				sl_eq(fname, "oklch") || sl_eq(fname, "color") ||
				sl_eq(fname, "color-mix")) {
			const char *p = v.s + i;
			css_color col;
			size_t close = pp_skip_unit(v.s, v.n, ne);

			/* currentColor inside color-mix() cannot be
			 * resolved here; leave such values alone */
			if (css_parse_color_text(&p, 0xff000000, &col) &&
					p == v.s + close) {
				char tmp[64];
				unsigned a = (col >> 24) & 0xff;
				buf_add(out, v.s + last, i - last);
				if (a == 255)
					snprintf(tmp, sizeof(tmp), "#%06x",
						(unsigned)(col & 0xffffff));
				else
					snprintf(tmp, sizeof(tmp),
						"rgba(%u,%u,%u,%.3f)",
						(unsigned)((col >> 16) & 0xff),
						(unsigned)((col >> 8) & 0xff),
						(unsigned)(col & 0xff),
						a / 255.0);
				buf_str(out, tmp);
				last = i = close;
			} else {
				i = close;
			}
			continue;
		}

		i = ne;
	}
	buf_add(out, v.s + last, v.n - last);
}

/**
 * Process one declaration and write it to the output.
 */
static void pp_declaration(pp_ctx *pp, slice decl, const var_frame *frame,
		bool *first)
{
	size_t colon = pp_scan_to(decl.s, decl.n, 0, ":");
	slice name, value;
	buf sub = { 0 }, fin = { 0 };
	bool important = false;

	if (colon >= decl.n)
		return;
	name = sl_trim((slice){ decl.s, colon });
	value = sl_trim((slice){ decl.s + colon + 1, decl.n - colon - 1 });
	if (name.n == 0 || (name.n >= 2 && name.s[0] == '-' &&
			name.s[1] == '-'))
		return;

	/* separate !important */
	if (value.n >= 10) {
		size_t k = value.n;
		while (k > 0 && pp_space(value.s[k - 1]))
			k--;
		if (k >= 9 && strncasecmp(value.s + k - 9, "important", 9) == 0) {
			size_t j = k - 9;
			while (j > 0 && pp_space(value.s[j - 1]))
				j--;
			if (j > 0 && value.s[j - 1] == '!') {
				important = true;
				value = sl_trim((slice){ value.s, j - 1 });
			}
		}
	}

	if (!pp_subst_vars(value, frame, &sub, 0) || sub.oom) {
		/* invalid at computed-value time: drop */
		buf_free(&sub);
		return;
	}
	pp_rewrite_value((slice){ sub.d ? sub.d : "", sub.len }, &fin);

	if (!*first)
		buf_chr(&pp->out, ';');
	*first = false;
	buf_add(&pp->out, name.s, name.n);
	buf_chr(&pp->out, ':');
	buf_add(&pp->out, fin.d ? fin.d : "", fin.len);
	if (important)
		buf_str(&pp->out, " !important");

	buf_free(&sub);
	buf_free(&fin);
}

/* ------------------------------------------------------------------ */
/* Rules */

/**
 * An item inside a declaration block: a declaration or a nested rule.
 */
typedef struct pp_item {
	slice prelude; /**< declaration text, or nested rule prelude */
	slice block;   /**< nested rule block contents (n == 0: decl) */
	bool nested;
} pp_item;

/** split a block into items */
static int pp_block_items(slice block, pp_item **items)
{
	size_t i = 0;
	int n = 0, cap = 0;
	pp_item *v = NULL;

	for (;;) {
		size_t e;
		pp_item it;

		i = pp_skip_ws(block.s, block.n, i);
		if (i >= block.n)
			break;
		if (block.s[i] == ';') {
			i++;
			continue;
		}
		e = pp_scan_to(block.s, block.n, i, ";{");
		it.prelude = sl_trim((slice){ block.s + i, e - i });
		it.nested = false;
		it.block = (slice){ NULL, 0 };
		if (e < block.n && block.s[e] == '{') {
			size_t close = pp_skip_unit(block.s, block.n, e);
			/* custom property values may contain blocks */
			if (it.prelude.n >= 2 && it.prelude.s[0] == '-' &&
					it.prelude.s[1] == '-' &&
					memchr(it.prelude.s, ':',
						it.prelude.n) != NULL) {
				e = pp_scan_to(block.s, block.n, close, ";");
				it.prelude = sl_trim((slice){ block.s + i,
						e - i });
				i = e;
			} else {
				it.nested = true;
				it.block = (slice){ block.s + e + 1,
					close > e + 1 ? close - e - 2 : 0 };
				i = close;
			}
		} else {
			i = e;
		}
		if (n >= cap) {
			int ncap = cap ? cap * 2 : 16;
			pp_item *nv = realloc(v, ncap * sizeof(pp_item));
			if (nv == NULL)
				break;
			v = nv;
			cap = ncap;
		}
		v[n++] = it;
	}
	*items = v;
	return n;
}

/**
 * Handle an at-rule. parent is NULL at the top level.
 */
static void pp_at_rule(pp_ctx *pp, slice name, slice prelude, slice block,
		bool has_block, slice *parent, int nparent, var_frame *frame,
		slice whole)
{
	bool saved_ok = pp->media_ok;

	if (sl_eq(name, "layer")) {
		if (has_block)
			pp_rules(pp, block, parent, nparent, frame);
		return;
	}

	if (sl_eq(name, "supports")) {
		if (has_block && pp_supports_eval(prelude))
			pp_rules(pp, block, parent, nparent, frame);
		return;
	}

	if (sl_eq(name, "container")) {
		/* drop the container name, evaluate against the viewport */
		size_t p = 0;
		while (p < prelude.n && prelude.s[p] != '(' &&
				!sl_prefix((slice){ prelude.s + p,
						prelude.n - p }, "not "))
			p++;
		if (has_block && pp_media_eval((slice){ prelude.s + p,
				prelude.n - p }))
			pp_rules(pp, block, parent, nparent, frame);
		return;
	}

	if (sl_eq(name, "media")) {
		if (!has_block)
			return;
		pp->media_ok = pp->media_ok && pp_media_eval(prelude);
		buf_str(&pp->out, "@media ");
		buf_add(&pp->out, prelude.s, prelude.n);
		buf_chr(&pp->out, '{');
		pp_rules(pp, block, parent, nparent, frame);
		buf_chr(&pp->out, '}');
		pp->media_ok = saved_ok;
		return;
	}

	if (sl_eq(name, "property")) {
		/* registered custom property: its initial value */
		pp_item *items = NULL;
		int n = pp_block_items(block, &items), i;
		slice pname = sl_trim(prelude);
		for (i = 0; i < n; i++) {
			slice parts[2];
			if (items[i].nested)
				continue;
			if (pp_split(items[i].prelude, ':', parts, 2) == 2 &&
					sl_eq(parts[0], "initial-value"))
				var_set(&global_root, &global_root_n,
						&global_root_cap, pname,
						parts[1], false);
		}
		free(items);
		return;
	}

	if (sl_eq(name, "keyframes") || sl_eq(name, "-webkit-keyframes") ||
			sl_eq(name, "-moz-keyframes") ||
			sl_eq(name, "-o-keyframes") ||
			sl_eq(name, "starting-style") ||
			sl_eq(name, "scope") || sl_eq(name, "view-transition") ||
			sl_eq(name, "counter-style") ||
			sl_eq(name, "font-feature-values") ||
			sl_eq(name, "font-palette-values") ||
			sl_eq(name, "position-try")) {
		return;
	}

	if (sl_eq(name, "import") && parent == NULL) {
		/* strip layer() and supports() import conditions */
		size_t p = 0;
		buf_str(&pp->out, "@import ");
		while (p < prelude.n) {
			size_t e;
			slice w;
			p = pp_skip_ws(prelude.s, prelude.n, p);
			if (p >= prelude.n)
				break;
			e = pp_skip_unit(prelude.s, prelude.n, p);
			while (e < prelude.n && !pp_space(prelude.s[e]) &&
					prelude.s[e] != ',')
				e = pp_skip_unit(prelude.s, prelude.n, e);
			w = (slice){ prelude.s + p, e - p };
			if (!sl_eq(w, "layer") && !sl_prefix(w, "layer(") &&
					!sl_prefix(w, "supports(")) {
				buf_add(&pp->out, w.s, w.n);
				buf_chr(&pp->out, ' ');
			}
			p = e;
		}
		buf_str(&pp->out, ";");
		return;
	}

	if (parent != NULL)
		return; /* other at-rules cannot be nested */

	/* anything else (@font-face, @page, @charset, @namespace): copy */
	buf_add(&pp->out, whole.s, whole.n);
	if (!has_block && (whole.n == 0 || whole.s[whole.n - 1] != ';'))
		buf_chr(&pp->out, ';');
}

/**
 * Process a list of rules. With a parent selector list, the rules are
 * nested inside a style rule.
 */
static void pp_rules(pp_ctx *pp, slice text, slice *parent, int nparent,
		var_frame *frame)
{
	size_t i = 0;

	if (++pp->depth > PP_MAX_DEPTH) {
		pp->depth--;
		return;
	}

	if (parent != NULL) {
		/* inside a style rule: the text is a declaration block */
		pp_style_block(pp, parent, nparent, text, frame, true);
		pp->depth--;
		return;
	}

	while (i < text.n) {
		size_t e, start;

		i = pp_skip_ws(text.s, text.n, i);
		if (i >= text.n)
			break;
		start = i;

		if (text.s[i] == '@') {
			size_t ne = i + 1;
			slice name, prelude, block = { NULL, 0 };
			bool has_block = false;

			while (ne < text.n && (isalnum((unsigned char)text.s[ne]) ||
					text.s[ne] == '-' || text.s[ne] == '_'))
				ne++;
			name = (slice){ text.s + i + 1, ne - i - 1 };
			e = pp_scan_to(text.s, text.n, ne, ";{");
			prelude = sl_trim((slice){ text.s + ne, e - ne });
			if (e < text.n && text.s[e] == '{') {
				size_t close = pp_skip_unit(text.s, text.n, e);
				has_block = true;
				block = (slice){ text.s + e + 1,
					close > e + 1 ? close - e - 2 : 0 };
				i = close;
			} else {
				i = e < text.n ? e + 1 : e;
			}
			pp_at_rule(pp, name, prelude, block, has_block, NULL, 0,
					frame, (slice){ text.s + start,
							i - start });
			continue;
		}

		e = pp_scan_to(text.s, text.n, i, "{;}");
		if (e >= text.n)
			break;
		if (text.s[e] != '{') {
			/* stray text: skip past it */
			i = e + 1;
			continue;
		}
		{
			size_t close = pp_skip_unit(text.s, text.n, e);
			slice prelude = sl_trim((slice){ text.s + i, e - i });
			slice block = { text.s + e + 1,
				close > e + 1 ? close - e - 2 : 0 };
			slice sel[PP_MAX_SELECTORS];
			int nsel = pp_split(prelude, ',', sel,
					PP_MAX_SELECTORS);

			pp_style_block(pp, sel, nsel, block, frame, true);
			i = close;
		}
	}
	pp->depth--;
}

/**
 * Process a style rule's declaration block (which may contain nested
 * rules), writing the rule and any flattened nested rules.
 */
static void pp_style_block(pp_ctx *pp, slice *sel, int nsel, slice block,
		var_frame *parent_frame, bool emit_selectors)
{
	pp_item *items = NULL;
	int n = pp_block_items(block, &items), i;
	var_frame frame = { NULL, 0, 0, parent_frame };
	bool rootish = emit_selectors && pp_selectors_rootish(sel, nsel);
	bool has_decls = false;

	if (++pp->depth > PP_MAX_DEPTH) {
		pp->depth--;
		free(items);
		return;
	}

	/* collect custom properties */
	for (i = 0; i < n; i++) {
		slice nm, val;
		size_t colon;
		if (items[i].nested)
			continue;
		colon = pp_scan_to(items[i].prelude.s, items[i].prelude.n, 0,
				":");
		if (colon >= items[i].prelude.n)
			continue;
		nm = sl_trim((slice){ items[i].prelude.s, colon });
		if (nm.n < 2 || nm.s[0] != '-' || nm.s[1] != '-') {
			has_decls = true;
			continue;
		}
		val = sl_trim((slice){ items[i].prelude.s + colon + 1,
				items[i].prelude.n - colon - 1 });
		/* strip !important */
		if (val.n > 10 && strncasecmp(val.s + val.n - 9,
				"important", 9) == 0) {
			size_t j = val.n - 9;
			while (j > 0 && pp_space(val.s[j - 1]))
				j--;
			if (j > 0 && val.s[j - 1] == '!')
				val = sl_trim((slice){ val.s, j - 1 });
		}
		var_set(&frame.v, &frame.n, &frame.cap, nm, val, true);
		if (!pp->media_ok)
			continue;
		if (rootish || pp->inline_style == false) {
			if (rootish)
				var_set(&global_root, &global_root_n,
					&global_root_cap, nm, val, true);
			else
				var_set(&global_any, &global_any_n,
					&global_any_cap, nm, val, false);
		}
	}

	/* the rule itself */
	if (has_decls) {
		size_t mark = pp->out.len;
		bool first = true;
		bool ok = true;

		if (emit_selectors && sel != NULL) {
			ok = pp_emit_selectors(pp, sel, nsel);
			if (ok)
				buf_chr(&pp->out, '{');
		}
		if (ok) {
			for (i = 0; i < n; i++) {
				if (!items[i].nested)
					pp_declaration(pp, items[i].prelude,
							&frame, &first);
			}
			if (emit_selectors && sel != NULL)
				buf_chr(&pp->out, '}');
		}
		if (first && emit_selectors && sel != NULL) {
			/* nothing survived: remove the empty rule */
			pp->out.len = mark;
			if (pp->out.d != NULL)
				pp->out.d[mark] = '\0';
		}
	}

	/* nested rules */
	for (i = 0; i < n; i++) {
		slice pre;
		if (!items[i].nested || sel == NULL)
			continue;
		pre = items[i].prelude;
		if (pre.n > 0 && pre.s[0] == '@') {
			size_t ne = 1;
			slice name, prelude;
			while (ne < pre.n && (isalnum((unsigned char)pre.s[ne]) ||
					pre.s[ne] == '-'))
				ne++;
			name = (slice){ pre.s + 1, ne - 1 };
			prelude = sl_trim((slice){ pre.s + ne, pre.n - ne });
			pp_at_rule(pp, name, prelude, items[i].block, true,
					sel, nsel, &frame, pre);
		} else {
			buf store = { 0 };
			slice *kids = malloc(sizeof(slice) * PP_MAX_SELECTORS);
			int nk;
			if (kids == NULL)
				continue;
			nk = pp_selectors_nest(sel, nsel, pre, &store, kids,
					PP_MAX_SELECTORS);
			if (nk > 0)
				pp_style_block(pp, kids, nk, items[i].block,
						&frame, true);
			free(kids);
			buf_free(&store);
		}
	}

	frame_free(&frame);
	free(items);
	pp->depth--;
}

/* exported interface documented in parse/preprocess.h */
css_error css__preprocess(const uint8_t *data, size_t len, bool inline_style,
		uint8_t **out, size_t *out_len)
{
	pp_ctx pp;
	slice text = { (const char *) data, len };

	*out = NULL;
	*out_len = 0;

	/* UTF-16/32 stylesheets are passed through untouched */
	if (len >= 2 && ((data[0] == 0xfe && data[1] == 0xff) ||
			(data[0] == 0xff && data[1] == 0xfe) ||
			data[0] == 0 || data[1] == 0))
		return CSS_INVALID;

	memset(&pp, 0, sizeof(pp));
	pp.src = (const char *) data;
	pp.media_ok = true;
	pp.inline_style = inline_style;

	/* keep a leading @charset rule verbatim for charset detection */
	if (!inline_style && len > 10 && strncmp(text.s, "@charset \"", 10) == 0) {
		size_t e = pp_scan_to(text.s, text.n, 0, ";");
		if (e < text.n) {
			buf_add(&pp.out, text.s, e + 1);
			text.s += e + 1;
			text.n -= e + 1;
		}
	}

	if (inline_style) {
		pp_style_block(&pp, NULL, 0, text, NULL, false);
	} else {
		pp_rules(&pp, text, NULL, 0, NULL);
	}

	if (pp.out.oom) {
		buf_free(&pp.out);
		return CSS_NOMEM;
	}
	if (pp.out.d == NULL)
		buf_add(&pp.out, " ", 1);

	*out = (uint8_t *) pp.out.d;
	*out_len = pp.out.len;
	return CSS_OK;
}
