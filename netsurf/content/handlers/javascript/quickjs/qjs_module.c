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
 * ES module scripts for the QuickJS backend.
 *
 * QuickJS resolves imports synchronously through a loader callback, but
 * fetching is asynchronous. So before a module is evaluated its source
 * is scanned for static imports (and literal dynamic imports), the whole
 * dependency graph is fetched, and only then is the entry module
 * evaluated with a loader that serves sources from the per-page cache.
 * Bare specifiers are resolved through the document's import map.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/utils.h"
#include "netsurf/misc.h"
#include "content/fetch.h"
#include "desktop/gui_internal.h"
#include "html/private.h"

#include "javascript/js.h"
#include "javascript/quickjs/qjs_private.h"

/** a module source in the per-page cache */
struct qjs_mod_src {
	struct qjs_mod_src *next;
	char *url;
	char *src;	/**< NUL terminated source, NULL while loading */
	size_t len;
	bool failed;
};

/** an entry module waiting for its dependencies */
struct qjs_mod_pending {
	struct qjs_mod_pending *next;
	char *url;
	char *src;
	size_t len;
};

/** an in-flight module fetch */
struct qjs_mod_fetch {
	struct qjs_mod_fetch *next;
	struct jsthread *t;
	struct fetch *fetch;
	struct qjs_mod_src *mod;
	nsurl *url;
	char *data;
	size_t len, cap;
	int redirects;
	long status;
};

/** a dynamic import() waiting for its module graph */
struct qjs_mod_waiter {
	struct qjs_mod_waiter *next;
	char *url;
	JSValue resolve, reject;
};

/** per-thread module state */
struct qjs_modules {
	struct qjs_mod_waiter *waiters;
	struct qjs_mod_src *cache;
	struct qjs_mod_pending *pending;
	struct qjs_mod_fetch *fetches;
	int outstanding;
	/* import map: specifier -> URL (prefix entries end in '/') */
	char **map_keys;
	char **map_vals;
	int map_n;
};

static struct qjs_modules *mods(struct jsthread *t)
{
	if (t->modules == NULL)
		t->modules = calloc(1, sizeof(struct qjs_modules));
	return t->modules;
}

static struct qjs_mod_src *mod_find(struct qjs_modules *m, const char *url)
{
	struct qjs_mod_src *s;
	for (s = m->cache; s != NULL; s = s->next) {
		if (strcmp(s->url, url) == 0)
			return s;
	}
	return NULL;
}

/**
 * Resolve an import specifier against a base URL, using the import map
 * for bare specifiers.
 *
 * \return malloc()ed absolute URL or NULL
 */
static char *mod_resolve(struct jsthread *t, const char *spec,
		const char *base)
{
	struct qjs_modules *m = mods(t);
	nsurl *b = NULL, *joined = NULL;
	char *out = NULL;
	int i, best = -1;
	size_t best_len = 0;
	char *mapped = NULL;

	if (m != NULL) {
		for (i = 0; i < m->map_n; i++) {
			size_t kl = strlen(m->map_keys[i]);
			if (strcmp(spec, m->map_keys[i]) == 0) {
				best = i;
				best_len = (size_t)-1;
				break;
			}
			if (kl > 0 && m->map_keys[i][kl - 1] == '/' &&
			    strncmp(spec, m->map_keys[i], kl) == 0 &&
			    kl > best_len) {
				best = i;
				best_len = kl;
			}
		}
		if (best >= 0) {
			if (best_len == (size_t)-1) {
				mapped = strdup(m->map_vals[best]);
			} else {
				size_t vl = strlen(m->map_vals[best]);
				mapped = malloc(vl + strlen(spec) - best_len + 1);
				if (mapped != NULL) {
					memcpy(mapped, m->map_vals[best], vl);
					strcpy(mapped + vl, spec + best_len);
				}
			}
			spec = mapped;
			if (spec == NULL)
				return NULL;
		}
	}

	/* bare specifiers without a mapping cannot be resolved */
	if (mapped == NULL && !(spec[0] == '/' || spec[0] == '.') &&
			strstr(spec, "://") == NULL &&
			strncmp(spec, "data:", 5) != 0) {
		return NULL;
	}

	if (nsurl_create(base, &b) == NSERROR_OK) {
		if (nsurl_join(b, spec, &joined) == NSERROR_OK) {
			nsurl *nofrag = NULL;
			if (nsurl_defragment(joined, &nofrag) == NSERROR_OK) {
				out = strdup(nsurl_access(nofrag));
				nsurl_unref(nofrag);
			}
			nsurl_unref(joined);
		}
		nsurl_unref(b);
	}
	free(mapped);
	return out;
}

/* ------------------------------------------------------------------ */
/* Import scanning */

static bool is_ident(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '$';
}

/** skip a string literal, template, comment or regex-free token */
static size_t skip_trivia(const char *s, size_t len, size_t i)
{
	for (;;) {
		while (i < len && isspace((unsigned char)s[i]))
			i++;
		if (i + 1 < len && s[i] == '/' && s[i + 1] == '/') {
			while (i < len && s[i] != '\n')
				i++;
			continue;
		}
		if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
			i += 2;
			while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
				i++;
			i += 2;
			continue;
		}
		return i;
	}
}

/** read a string literal at i; returns malloc()ed contents or NULL */
static char *read_string(const char *s, size_t len, size_t *i)
{
	char q = s[*i];
	size_t start = *i + 1, j = start;
	char *out;

	if (q != '"' && q != '\'')
		return NULL;
	while (j < len && s[j] != q && s[j] != '\n') {
		if (s[j] == '\\')
			j++;
		j++;
	}
	if (j >= len || s[j] != q)
		return NULL;
	out = strndup(s + start, j - start);
	*i = j + 1;
	return out;
}

/**
 * If position i starts a string, template, comment or regex literal,
 * return the index just after it; otherwise return i.
 *
 * \param prev  the previous significant character (for regex detection)
 */
static size_t skip_noncode(const char *s, size_t len, size_t i, char prev)
{
	char c = s[i];

	if (c == '"' || c == '\'') {
		char q = c;
		i++;
		while (i < len && s[i] != q && s[i] != '\n') {
			if (s[i] == '\\')
				i++;
			i++;
		}
		return i + 1;
	}
	if (c == '`') {
		/* template literal; substitutions nest braces */
		int depth = 0;
		i++;
		while (i < len) {
			if (s[i] == '\\') {
				i += 2;
				continue;
			}
			if (depth == 0 && s[i] == '`')
				break;
			if (s[i] == '$' && i + 1 < len && s[i + 1] == '{') {
				depth++;
				i += 2;
				continue;
			}
			if (depth > 0 && s[i] == '}')
				depth--;
			else if (depth > 0 && s[i] == '{')
				depth++;
			i++;
		}
		return i + 1;
	}
	if (c == '/' && i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '*'))
		return skip_trivia(s, len, i);
	if (c == '/' && (prev == 0 || strchr("(,=:[!&|?{};+-*%<>~^", prev))) {
		/* regular expression literal */
		bool cls = false;
		i++;
		while (i < len && s[i] != '\n') {
			if (s[i] == '\\')
				i++;
			else if (s[i] == '[')
				cls = true;
			else if (s[i] == ']')
				cls = false;
			else if (s[i] == '/' && !cls)
				break;
			i++;
		}
		return i + 1;
	}
	return i;
}

typedef void (*spec_cb)(struct jsthread *t, const char *spec,
		const char *base);

/**
 * Find import specifiers in module source.
 */
static void scan_imports(struct jsthread *t, const char *s, size_t len,
		const char *base, spec_cb cb)
{
	size_t i = 0, n;
	char prev = 0;

	while (i < len) {
		char c = s[i];

		/* skip strings, templates, comments and regexps wholesale */
		n = skip_noncode(s, len, i, prev);
		if (n != i) {
			if (c != '/' || s[i + 1] != '/' && s[i + 1] != '*')
				prev = 'a';
			i = n;
			continue;
		}
		if (!isspace((unsigned char)c))
			prev = c;

		if ((c == 'i' || c == 'e') && (i == 0 || (!is_ident(s[i - 1]) &&
				s[i - 1] != '.'))) {
			bool imp = strncmp(s + i, "import", 6) == 0 &&
					!is_ident(s[i + 6]);
			bool exp = strncmp(s + i, "export", 6) == 0 &&
					!is_ident(s[i + 6]);
			if (imp || exp) {
				size_t j = skip_trivia(s, len, i + 6);
				char *spec = NULL;

				if (imp && j < len && s[j] == '(') {
					/* dynamic import with a literal */
					j = skip_trivia(s, len, j + 1);
					spec = read_string(s, len, &j);
					if (spec != NULL) {
						j = skip_trivia(s, len, j);
						if (j >= len || s[j] != ')') {
							/* computed specifier */
							free(spec);
							spec = NULL;
						}
					}
				} else if (imp && j < len &&
						(s[j] == '"' || s[j] == '\'')) {
					spec = read_string(s, len, &j);
				} else if (!(imp && j < len && s[j] == '.')) {
					/* look for "from '...'" before ';' */
					size_t k = j, limit = j + 4000;
					while (k < len && k < limit && s[k] != ';') {
						if (s[k] == '"' || s[k] == '\'') {
							char *tmp = read_string(s, len, &k);
							free(tmp);
							if (tmp == NULL)
								k++;
							continue;
						}
						if (strncmp(s + k, "from", 4) == 0 &&
						    !is_ident(s[k + 4]) &&
						    (k == 0 || !is_ident(s[k - 1]))) {
							size_t f = skip_trivia(s, len, k + 4);
							spec = read_string(s, len, &f);
							j = f;
							break;
						}
						/* a new statement ends a clause */
						if (s[k] == '\n' && k > j + 1) {
							size_t n = skip_trivia(s, len, k);
							if (n < len && (strncmp(s + n, "import", 6) == 0 ||
							    strncmp(s + n, "export", 6) == 0 ||
							    strncmp(s + n, "const ", 6) == 0 ||
							    strncmp(s + n, "function", 8) == 0))
								break;
						}
						k++;
					}
				}
				if (spec != NULL) {
					cb(t, spec, base);
					free(spec);
				}
				i = j > i + 6 ? j : i + 6;
				for (n = i; n > 0 && isspace((unsigned char)s[n - 1]); n--)
					;
				prev = n > 0 ? s[n - 1] : 0;
				continue;
			}
		}
		i++;
	}
}

/**
 * Route dynamic import() through the asynchronous loader: every
 * `import(` call becomes `__ns_import("<base>",` so that the specifier
 * (which may be computed) is fetched with its dependencies before the
 * real import runs.
 *
 * \return malloc()ed NUL terminated source, or NULL when unchanged
 */
static char *rewrite_dynamic_import(const char *s, size_t len,
		const char *base, size_t *out_len)
{
	size_t i = 0, n, o = 0, cap = 0, blen, count = 0;
	char prev = 0;
	char *out = NULL;
	size_t copied = 0;

	blen = strlen(base);
	while (i < len) {
		char c = s[i];

		n = skip_noncode(s, len, i, prev);
		if (n != i) {
			if (c != '/' || (s[i + 1] != '/' && s[i + 1] != '*'))
				prev = 'a';
			i = n;
			continue;
		}
		if (c == 'i' && (i == 0 || (!is_ident(s[i - 1]) &&
				s[i - 1] != '.')) &&
		    i + 6 < len && strncmp(s + i, "import", 6) == 0 &&
		    !is_ident(s[i + 6])) {
			size_t j = skip_trivia(s, len, i + 6);
			if (j < len && s[j] == '(') {
				size_t need = o + (i - copied) + blen + 32;
				if (need + (len - i) + 1 > cap) {
					char *d;
					cap = (need + (len - i) + 1) * 2;
					d = realloc(out, cap);
					if (d == NULL) {
						free(out);
						return NULL;
					}
					out = d;
				}
				memcpy(out + o, s + copied, i - copied);
				o += i - copied;
				memcpy(out + o, "__ns_import(\"", 13);
				o += 13;
				for (n = 0; n < blen; n++) {
					char b = base[n];
					if (b == '"' || b == '\\' || b == '\n')
						b = '_';
					out[o++] = b;
				}
				out[o++] = '"';
				out[o++] = ',';
				copied = j + 1;
				i = j + 1;
				prev = '(';
				count++;
				continue;
			}
		}
		if (!isspace((unsigned char)c))
			prev = c;
		i++;
	}
	if (count == 0)
		return NULL;
	if (o + (len - copied) + 1 > cap) {
		char *d = realloc(out, o + (len - copied) + 1);
		if (d == NULL) {
			free(out);
			return NULL;
		}
		out = d;
	}
	memcpy(out + o, s + copied, len - copied);
	o += len - copied;
	out[o] = '\0';
	*out_len = o;
	return out;
}

/** replace a source buffer with its dynamic-import rewrite */
static void apply_rewrite(char **src, size_t *len, const char *base)
{
	size_t nl = 0;
	char *r = rewrite_dynamic_import(*src, *len, base, &nl);
	if (r != NULL) {
		free(*src);
		*src = r;
		*len = nl;
	}
}

/* ------------------------------------------------------------------ */
/* Fetching */

static void mod_try_run(struct jsthread *t);
static void mod_fetch_callback(const fetch_msg *msg, void *p);
static void mod_want(struct jsthread *t, const char *spec, const char *base);

static void mod_fetch_free(struct qjs_mod_fetch *f)
{
	struct qjs_modules *m = f->t->modules;
	struct qjs_mod_fetch **pp;

	if (m != NULL) {
		for (pp = &m->fetches; *pp != NULL; pp = &(*pp)->next) {
			if (*pp == f) {
				*pp = f->next;
				break;
			}
		}
	}
	if (f->url != NULL)
		nsurl_unref(f->url);
	free(f->data);
	free(f);
}

/** a module fetch has finished (scheduled, not re-entrant) */
static void mod_fetch_done(void *p)
{
	struct qjs_mod_fetch *f = p;
	struct jsthread *t = f->t;
	struct qjs_mod_src *mod = f->mod;

	if (t->closed) {
		mod_fetch_free(f);
		return;
	}

	if (f->data != NULL && f->status >= 200 && f->status < 300) {
		mod->src = f->data;
		mod->len = f->len;
		f->data = NULL;
		scan_imports(t, mod->src, mod->len, mod->url, mod_want);
		apply_rewrite(&mod->src, &mod->len, mod->url);
	} else {
		NSLOG(jserrors, WARNING, "module %s failed to load (%ld)",
				mod->url, f->status);
		mod->failed = true;
	}
	t->modules->outstanding--;
	mod_fetch_free(f);
	mod_try_run(t);
}

static bool mod_fetch_begin(struct qjs_mod_fetch *f)
{
	static const char *headers[] = {
		"Accept: text/javascript, application/javascript, */*",
		NULL
	};
	nsurl *referer = NULL;

	if (f->t->htmlc != NULL)
		referer = f->t->htmlc->base_url;
	f->len = 0;
	f->status = 0;
	return fetch_start(f->url, referer, mod_fetch_callback, f, false,
			NULL, NULL, false, false, headers, &f->fetch) == NSERROR_OK;
}

static void mod_fetch_restart(void *p)
{
	struct qjs_mod_fetch *f = p;

	if (f->t->closed || !mod_fetch_begin(f)) {
		f->status = 0;
		mod_fetch_done(f);
	}
}

static void mod_fetch_callback(const fetch_msg *msg, void *p)
{
	struct qjs_mod_fetch *f = p;

	switch (msg->type) {
	case FETCH_HEADER:
	case FETCH_DATA:
		if (f->status == 0 && f->fetch != NULL)
			f->status = fetch_http_code(f->fetch);
		if (msg->type == FETCH_DATA) {
			size_t n = msg->data.header_or_data.len;
			if (f->len + n + 1 > f->cap) {
				size_t cap = f->cap ? f->cap * 2 : 16384;
				char *d;
				while (cap < f->len + n + 1)
					cap *= 2;
				d = realloc(f->data, cap);
				if (d == NULL)
					break;
				f->data = d;
				f->cap = cap;
			}
			memcpy(f->data + f->len, msg->data.header_or_data.buf, n);
			f->len += n;
			f->data[f->len] = '\0';
		}
		break;

	case FETCH_FINISHED:
		if (f->status == 0)
			f->status = 200;
		if (f->data == NULL) {
			f->data = calloc(1, 1);
			f->len = 0;
		}
		f->fetch = NULL;
		guit->misc->schedule(0, mod_fetch_done, f);
		break;

	case FETCH_REDIRECT: {
		nsurl *next = NULL;
		f->fetch = NULL;
		if (f->redirects++ < 10 && msg->data.redirect != NULL &&
		    nsurl_join(f->url, msg->data.redirect, &next) ==
				NSERROR_OK) {
			nsurl_unref(f->url);
			f->url = next;
			guit->misc->schedule(0, mod_fetch_restart, f);
		} else {
			f->status = 0;
			guit->misc->schedule(0, mod_fetch_done, f);
		}
		break;
	}

	case FETCH_ERROR:
	case FETCH_TIMEDOUT:
	case FETCH_AUTH:
	case FETCH_CERT_ERR:
	case FETCH_SSL_ERR:
	case FETCH_NOTMODIFIED:
		f->fetch = NULL;
		f->status = 0;
		guit->misc->schedule(0, mod_fetch_done, f);
		break;

	default:
		break;
	}
}

/** ensure a module (by resolved URL) is in the cache or being fetched */
static void mod_want_url(struct jsthread *t, const char *url)
{
	struct qjs_modules *m = mods(t);
	struct qjs_mod_src *mod;
	struct qjs_mod_fetch *f;
	nsurl *nu;

	if (m == NULL || mod_find(m, url) != NULL)
		return;

	mod = calloc(1, sizeof(*mod));
	if (mod == NULL)
		return;
	mod->url = strdup(url);
	mod->next = m->cache;
	m->cache = mod;

	if (nsurl_create(url, &nu) != NSERROR_OK || !fetch_can_fetch(nu)) {
		if (nu != NULL)
			nsurl_unref(nu);
		mod->failed = true;
		return;
	}
	f = calloc(1, sizeof(*f));
	if (f == NULL) {
		nsurl_unref(nu);
		mod->failed = true;
		return;
	}
	f->t = t;
	f->mod = mod;
	f->url = nu;
	f->next = m->fetches;
	m->fetches = f;
	m->outstanding++;
	if (!mod_fetch_begin(f)) {
		f->status = 0;
		guit->misc->schedule(0, mod_fetch_done, f);
	}
}

static void mod_want(struct jsthread *t, const char *spec, const char *base)
{
	char *url = mod_resolve(t, spec, base);

	if (url != NULL) {
		mod_want_url(t, url);
		free(url);
	}
}

/* ------------------------------------------------------------------ */
/* QuickJS loader */

static char *mod_normalize(JSContext *ctx, const char *base,
		const char *name, void *opaque)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	char *url = t ? mod_resolve(t, name, base) : NULL;
	char *out;

	if (url == NULL) {
		/* keep the specifier; the loader reports the failure */
		return js_strdup(ctx, name);
	}
	out = js_strdup(ctx, url);
	free(url);
	return out;
}

static JSModuleDef *mod_loader(JSContext *ctx, const char *name,
		void *opaque)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	struct qjs_mod_src *mod = t && t->modules ?
			mod_find(t->modules, name) : NULL;
	JSValue fn, meta;
	JSModuleDef *m;

	if (mod == NULL || mod->src == NULL) {
		if (mod == NULL && t != NULL) {
			/* start fetching it for a later dynamic import */
			mod_want_url(t, name);
		}
		JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
		return NULL;
	}

	fn = JS_Eval(ctx, mod->src, mod->len, name,
			JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	if (JS_IsException(fn))
		return NULL;
	m = JS_VALUE_GET_PTR(fn);
	meta = JS_GetImportMeta(ctx, m);
	if (!JS_IsException(meta)) {
		JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, name));
		JS_SetPropertyStr(ctx, meta, "main", JS_FALSE);
		JS_FreeValue(ctx, meta);
	}
	JS_FreeValue(ctx, fn);
	return m;
}

/* exported interface documented in qjs_private.h */
void qjs_modules_setup(JSRuntime *rt)
{
	JS_SetModuleLoaderFunc(rt, mod_normalize, mod_loader, NULL);
}

/** evaluate one entry module */
static void mod_eval(struct jsthread *t, const char *url, const char *src,
		size_t len)
{
	JSContext *ctx = t->ctx;
	JSValue v;

	qjs_deadline_start(t);
	v = JS_Eval(ctx, src, len, url,
			JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	if (!JS_IsException(v)) {
		JSValue meta = JS_GetImportMeta(ctx, JS_VALUE_GET_PTR(v));
		if (!JS_IsException(meta)) {
			/* inline modules report the document URL */
			const char *hash = strstr(url, "#inline-module-");
			JS_SetPropertyStr(ctx, meta, "url", hash ?
					JS_NewStringLen(ctx, url, hash - url) :
					JS_NewString(ctx, url));
			JS_SetPropertyStr(ctx, meta, "main", JS_TRUE);
			JS_FreeValue(ctx, meta);
		}
		v = JS_EvalFunction(ctx, v);
	}
	qjs_deadline_stop(t);
	if (JS_IsException(v)) {
		qjs_dump_error(ctx);
	} else if (JS_IsObject(v)) {
		/* evaluation returns a promise (top level await) */
		qjs_run_jobs(ctx);
		if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
			JSValue r = JS_PromiseResult(ctx, v);
			JS_Throw(ctx, r);
			qjs_dump_error(ctx);
		}
	}
	JS_FreeValue(ctx, v);
	qjs_run_jobs(ctx);
}

/** run entry modules whose graphs have finished loading */
static void mod_try_run(struct jsthread *t)
{
	struct qjs_modules *m = t->modules;

	if (m == NULL || m->outstanding > 0 || t->closed)
		return;
	while (m->waiters != NULL) {
		struct qjs_mod_waiter *w = m->waiters;
		struct qjs_mod_src *src = mod_find(m, w->url);
		JSValue arg = JS_NewString(t->ctx, w->url), r;

		m->waiters = w->next;
		if (src != NULL && src->src != NULL) {
			r = JS_Call(t->ctx, w->resolve, JS_UNDEFINED, 1, &arg);
		} else {
			JSValue e = JS_NewError(t->ctx);
			JS_SetPropertyStr(t->ctx, e, "message", JS_NewString(
				t->ctx, "Failed to fetch dynamically imported module"));
			r = JS_Call(t->ctx, w->reject, JS_UNDEFINED, 1, &e);
			JS_FreeValue(t->ctx, e);
		}
		JS_FreeValue(t->ctx, r);
		JS_FreeValue(t->ctx, arg);
		JS_FreeValue(t->ctx, w->resolve);
		JS_FreeValue(t->ctx, w->reject);
		free(w->url);
		free(w);
	}
	qjs_run_jobs(t->ctx);
	while (m->pending != NULL) {
		struct qjs_mod_pending *p = m->pending;
		m->pending = p->next;
		mod_eval(t, p->url, p->src, p->len);
		free(p->url);
		free(p->src);
		free(p);
		if (m->outstanding > 0)
			break;
	}
}

static void mod_try_run_cb(void *p)
{
	mod_try_run(p);
}

/* exported interface documented in js.h */
bool js_exec_module(jsthread *t, const uint8_t *txt, size_t txtlen,
		const char *url)
{
	struct qjs_modules *m;
	struct qjs_mod_pending *p, **pp;
	char name[64];
	static unsigned inline_count;

	if (t == NULL || t->closed || txt == NULL)
		return false;
	m = mods(t);
	if (m == NULL)
		return false;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return false;
	p->src = malloc(txtlen + 1);
	if (p->src == NULL) {
		free(p);
		return false;
	}
	memcpy(p->src, txt, txtlen);
	p->src[txtlen] = '\0';
	p->len = txtlen;
	p->url = strdup(url != NULL ? url : "about:blank");
	if (url != NULL && t->htmlc != NULL &&
			strcmp(url, nsurl_access(content_get_url(
					&t->htmlc->base))) == 0) {
		/* inline module: a unique name, but resolve against the
		 * document */
		snprintf(name, sizeof(name), "#inline-module-%u",
				++inline_count);
		free(p->url);
		p->url = malloc(strlen(url) + strlen(name) + 1);
		if (p->url != NULL) {
			strcpy(p->url, url);
			strcat(p->url, name);
		}
	}
	if (p->url == NULL) {
		free(p->src);
		free(p);
		return false;
	}

	/* queue in document order and fetch the dependency graph */
	for (pp = &m->pending; *pp != NULL; pp = &(*pp)->next)
		;
	*pp = p;
	scan_imports(t, p->src, p->len, p->url, mod_want);
	apply_rewrite(&p->src, &p->len, p->url);
	mod_try_run(t);
	return true;
}

/* exported interface documented in js.h */
void js_set_importmap(jsthread *t, const uint8_t *txt, size_t txtlen,
		const char *base)
{
	struct qjs_modules *m;
	JSContext *ctx;
	JSValue json, imports;
	JSPropertyEnum *props = NULL;
	uint32_t n = 0, i;
	char *src;

	if (t == NULL || t->closed || txt == NULL)
		return;
	m = mods(t);
	ctx = t->ctx;
	src = strndup((const char *)txt, txtlen);
	if (src == NULL || m == NULL) {
		free(src);
		return;
	}
	json = JS_ParseJSON(ctx, src, txtlen, "<importmap>");
	free(src);
	if (JS_IsException(json)) {
		qjs_dump_error(ctx);
		return;
	}
	imports = JS_GetPropertyStr(ctx, json, "imports");
	if (JS_IsObject(imports) &&
	    JS_GetOwnPropertyNames(ctx, &props, &n, imports,
			JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
		for (i = 0; i < n; i++) {
			JSValue v = JS_GetProperty(ctx, imports, props[i].atom);
			const char *k = JS_AtomToCString(ctx, props[i].atom);
			const char *vs = JS_ToCString(ctx, v);
			char *resolved = NULL;
			nsurl *b = NULL, *j = NULL;

			if (k != NULL && vs != NULL && base != NULL &&
			    nsurl_create(base, &b) == NSERROR_OK) {
				if (nsurl_join(b, vs, &j) == NSERROR_OK) {
					resolved = strdup(nsurl_access(j));
					nsurl_unref(j);
				}
				nsurl_unref(b);
			}
			if (resolved != NULL) {
				char **nk = realloc(m->map_keys,
						(m->map_n + 1) * sizeof(char *));
				char **nv = realloc(m->map_vals,
						(m->map_n + 1) * sizeof(char *));
				if (nk != NULL)
					m->map_keys = nk;
				if (nv != NULL)
					m->map_vals = nv;
				if (nk != NULL && nv != NULL) {
					m->map_keys[m->map_n] = strdup(k);
					m->map_vals[m->map_n] = resolved;
					m->map_n++;
				} else {
					free(resolved);
				}
			}
			if (k != NULL)
				JS_FreeCString(ctx, k);
			if (vs != NULL)
				JS_FreeCString(ctx, vs);
			JS_FreeValue(ctx, v);
		}
		JS_FreePropertyEnum(ctx, props, n);
	}
	JS_FreeValue(ctx, imports);
	JS_FreeValue(ctx, json);
}

/**
 * __ns.modulePrefetch(base, specifier) -> Promise<resolved url>
 *
 * Resolves once the module and its static dependencies are cached.
 */
static JSValue mod_prefetch(JSContext *ctx, JSValueConst this_val,
		int argc, JSValueConst *argv)
{
	struct jsthread *t = JS_GetContextOpaque(ctx);
	struct qjs_modules *m;
	struct qjs_mod_waiter *w;
	const char *base, *spec;
	JSValue funcs[2], promise;
	char *url = NULL;

	if (t == NULL || argc < 2 || (m = mods(t)) == NULL)
		return JS_ThrowTypeError(ctx, "module loader unavailable");
	base = JS_ToCString(ctx, argv[0]);
	spec = JS_ToCString(ctx, argv[1]);
	if (base != NULL && spec != NULL)
		url = mod_resolve(t, spec, base);
	if (url == NULL) {
		JSValue e = JS_ThrowTypeError(ctx,
				"Failed to resolve module specifier '%s'",
				spec ? spec : "");
		JS_FreeCString(ctx, base);
		JS_FreeCString(ctx, spec);
		return e;
	}
	JS_FreeCString(ctx, base);
	JS_FreeCString(ctx, spec);

	promise = JS_NewPromiseCapability(ctx, funcs);
	if (JS_IsException(promise)) {
		free(url);
		return promise;
	}
	w = calloc(1, sizeof(*w));
	if (w == NULL) {
		free(url);
		JS_FreeValue(ctx, funcs[0]);
		JS_FreeValue(ctx, funcs[1]);
		return promise;
	}
	w->url = url;
	w->resolve = funcs[0];
	w->reject = funcs[1];
	w->next = m->waiters;
	m->waiters = w;

	mod_want_url(t, url);
	if (m->outstanding == 0)
		guit->misc->schedule(0, mod_try_run_cb, t);
	return promise;
}

/* exported interface documented in qjs_private.h */
void qjs_modules_install(struct jsthread *t, JSValue ns)
{
	JS_SetPropertyStr(t->ctx, ns, "modulePrefetch",
			JS_NewCFunction(t->ctx, mod_prefetch,
					"modulePrefetch", 2));
}

/* exported interface documented in qjs_private.h */
void qjs_modules_closethread(struct jsthread *t)
{
	struct qjs_modules *m = t->modules;
	int i;

	if (m == NULL)
		return;
	guit->misc->schedule(-1, mod_try_run_cb, t);
	while (m->waiters != NULL) {
		struct qjs_mod_waiter *w = m->waiters;
		m->waiters = w->next;
		JS_FreeValue(t->ctx, w->resolve);
		JS_FreeValue(t->ctx, w->reject);
		free(w->url);
		free(w);
	}
	while (m->fetches != NULL) {
		struct qjs_mod_fetch *f = m->fetches;
		if (f->fetch != NULL)
			fetch_abort(f->fetch);
		f->fetch = NULL;
		guit->misc->schedule(-1, mod_fetch_done, f);
		guit->misc->schedule(-1, mod_fetch_restart, f);
		mod_fetch_free(f);
	}
	while (m->pending != NULL) {
		struct qjs_mod_pending *p = m->pending;
		m->pending = p->next;
		free(p->url);
		free(p->src);
		free(p);
	}
	while (m->cache != NULL) {
		struct qjs_mod_src *s = m->cache;
		m->cache = s->next;
		free(s->url);
		free(s->src);
		free(s);
	}
	for (i = 0; i < m->map_n; i++) {
		free(m->map_keys[i]);
		free(m->map_vals[i]);
	}
	free(m->map_keys);
	free(m->map_vals);
	free(m);
	t->modules = NULL;
}
