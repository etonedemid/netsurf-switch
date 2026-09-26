/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * Raw-string properties and the modern shorthands built on them.
 *
 * A raw property stores its declared value as normalised text; the
 * client interprets it (border-radius, box-shadow, grid templates, ...).
 * This keeps the cascade, inheritance and !important handling in LibCSS
 * without a bespoke bytecode encoding for every modern property.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

#define RAW_MAX_TEXT 4096
#define RAW_MAX_COMPONENTS 16

/** A top-level value component (a token, or a whole function call) */
struct raw_component {
	int32_t ctx;	/**< vector index of the component's first token */
	size_t start;	/**< offset of its text in the serialised buffer */
	size_t len;	/**< length of its text */
};

/** A serialised declaration value, split into components */
struct raw_value {
	char text[RAW_MAX_TEXT];
	size_t len;
	struct raw_component comp[RAW_MAX_COMPONENTS];
	int ncomp;
	/** index of the first component after each '/' separator */
	int slash[4];
	int nslash;
	/** true if a top-level ',' was seen */
	bool comma;
};

static bool raw_append(struct raw_value *rv, const char *s, size_t len)
{
	if (rv->len + len >= RAW_MAX_TEXT)
		return false;
	memcpy(rv->text + rv->len, s, len);
	rv->len += len;
	rv->text[rv->len] = '\0';
	return true;
}

static bool raw_append_lwc(struct raw_value *rv, lwc_string *s)
{
	return raw_append(rv, lwc_string_data(s), lwc_string_length(s));
}

/**
 * Serialise one token into the buffer
 */
static bool raw_append_token(struct raw_value *rv, const css_token *t)
{
	switch (t->type) {
	case CSS_TOKEN_IDENT:
	case CSS_TOKEN_NUMBER:
	case CSS_TOKEN_DIMENSION:
	case CSS_TOKEN_CHAR:
		return raw_append_lwc(rv, t->idata);
	case CSS_TOKEN_PERCENTAGE:
		return raw_append_lwc(rv, t->idata) && raw_append(rv, "%", 1);
	case CSS_TOKEN_HASH:
		return raw_append(rv, "#", 1) && raw_append_lwc(rv, t->idata);
	case CSS_TOKEN_FUNCTION:
		return raw_append_lwc(rv, t->idata) && raw_append(rv, "(", 1);
	case CSS_TOKEN_STRING:
		return raw_append(rv, "\"", 1) && raw_append_lwc(rv, t->idata) &&
				raw_append(rv, "\"", 1);
	case CSS_TOKEN_URI:
		return raw_append(rv, "url(", 4) && raw_append_lwc(rv, t->idata) &&
				raw_append(rv, ")", 1);
	default:
		/* Unexpected token type; ignore it */
		return true;
	}
}

static inline bool raw_is_char(const css_token *t, char c)
{
	return t->type == CSS_TOKEN_CHAR && lwc_string_length(t->idata) == 1 &&
			lwc_string_data(t->idata)[0] == c;
}

/**
 * Serialise the remainder of a declaration (up to "!important").
 *
 * On success *ctx is left at the end of the value.
 */
static css_error raw_read(const parserutils_vector *vector, int32_t *ctx,
		struct raw_value *rv)
{
	const css_token *t;
	int depth = 0;
	bool need_space = false;
	int cur = -1; /* component being read, or -1 */

	rv->len = 0;
	rv->text[0] = '\0';
	rv->ncomp = 0;
	rv->nslash = 0;
	rv->comma = false;

	while ((t = parserutils_vector_peek(vector, *ctx)) != NULL) {
		if (t->type == CSS_TOKEN_S) {
			parserutils_vector_iterate(vector, ctx);
			if (rv->len > 0)
				need_space = true;
			continue;
		}

		if (depth == 0 && raw_is_char(t, '!'))
			break;

		if (depth == 0 && (raw_is_char(t, '/') || raw_is_char(t, ','))) {
			if (raw_is_char(t, '/')) {
				if (rv->nslash < 4)
					rv->slash[rv->nslash++] = rv->ncomp;
			} else {
				rv->comma = true;
			}
			if (!raw_append(rv, need_space ? " " : "", need_space) ||
					!raw_append_token(rv, t))
				return CSS_INVALID;
			need_space = true;
			parserutils_vector_iterate(vector, ctx);
			continue;
		}

		if (need_space) {
			if (!raw_append(rv, " ", 1))
				return CSS_INVALID;
			need_space = false;
		}

		if (depth == 0) {
			/* every top-level token starts a component */
			cur = -1;
			if (rv->ncomp < RAW_MAX_COMPONENTS) {
				cur = rv->ncomp++;
				rv->comp[cur].ctx = *ctx;
				rv->comp[cur].start = rv->len;
			}
		}

		if (!raw_append_token(rv, t))
			return CSS_INVALID;

		if (t->type == CSS_TOKEN_FUNCTION || raw_is_char(t, '('))
			depth++;
		else if (raw_is_char(t, ')') && depth > 0)
			depth--;

		if (depth == 0 && cur >= 0)
			rv->comp[cur].len = rv->len - rv->comp[cur].start;

		parserutils_vector_iterate(vector, ctx);
	}

	if (rv->len == 0 || depth != 0)
		return CSS_INVALID;

	return CSS_OK;
}

/**
 * Emit a raw property with the given text
 */
static css_error raw_emit(css_language *c, css_style *result,
		enum css_properties_e prop, const char *text, size_t len)
{
	lwc_string *str;
	uint32_t snum;
	css_error error;

	if (len == 0)
		return CSS_INVALID;

	if (lwc_intern_string(text, len, &str) != lwc_error_ok)
		return CSS_NOMEM;

	error = css__stylesheet_string_add(c->sheet, str, &snum);
	if (error != CSS_OK)
		return error;

	error = css__stylesheet_style_appendOPV(result, prop, 0, RAW_SET);
	if (error != CSS_OK)
		return error;

	return css__stylesheet_style_append(result, snum);
}

/**
 * Handle the CSS-wide keywords for a set of properties.
 *
 * \return true if the value was a CSS-wide keyword (and was emitted)
 */
static bool raw_flag_value(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, const enum css_properties_e *props,
		int nprops, css_error *error)
{
	int32_t orig_ctx = *ctx;
	const css_token *token;
	enum flag_value flag_value;
	int i;

	token = parserutils_vector_iterate(vector, ctx);
	if (token == NULL) {
		*ctx = orig_ctx;
		return false;
	}

	flag_value = get_css_flag_value(c, token);
	if (flag_value == FLAG_VALUE__NONE) {
		*ctx = orig_ctx;
		return false;
	}

	consumeWhitespace(vector, ctx);
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && !raw_is_char(token, '!')) {
		/* keyword is part of a longer value */
		*ctx = orig_ctx;
		return false;
	}

	*error = CSS_OK;
	for (i = 0; i < nprops && *error == CSS_OK; i++) {
		*error = css_stylesheet_style_flag_value(result, flag_value,
				props[i]);
	}
	return true;
}

/**
 * Parse a generic raw-string property
 */
static css_error parse_raw(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, enum css_properties_e prop)
{
	int32_t orig_ctx = *ctx;
	struct raw_value *rv;
	css_error error;
	bool match;

	if (raw_flag_value(c, vector, ctx, result, &prop, 1, &error))
		return error;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;

	error = raw_read(vector, ctx, rv);
	if (error == CSS_OK) {
		if (rv->ncomp == 1 && rv->nslash == 0 && !rv->comma &&
				lwc_string_caseless_isequal(
					((const css_token *)parserutils_vector_peek(
						vector, rv->comp[0].ctx))->idata,
					c->strings[NONE], &match) ==
					lwc_error_ok && match &&
				((const css_token *)parserutils_vector_peek(
					vector, rv->comp[0].ctx))->type ==
					CSS_TOKEN_IDENT) {
			error = css__stylesheet_style_appendOPV(result, prop,
					0, RAW_NONE);
		} else {
			error = raw_emit(c, result, prop, rv->text, rv->len);
		}
	}

	free(rv);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}

#define RAW_PARSER(pname, PNAME)					\
css_error css__parse_##pname(css_language *c,				\
		const parserutils_vector *vector, int32_t *ctx,		\
		css_style *result)					\
{									\
	return parse_raw(c, vector, ctx, result, CSS_PROP_##PNAME);	\
}

RAW_PARSER(border_top_left_radius, BORDER_TOP_LEFT_RADIUS)
RAW_PARSER(border_top_right_radius, BORDER_TOP_RIGHT_RADIUS)
RAW_PARSER(border_bottom_right_radius, BORDER_BOTTOM_RIGHT_RADIUS)
RAW_PARSER(border_bottom_left_radius, BORDER_BOTTOM_LEFT_RADIUS)
RAW_PARSER(box_shadow, BOX_SHADOW)
RAW_PARSER(text_shadow, TEXT_SHADOW)
RAW_PARSER(transform, TRANSFORM)
RAW_PARSER(grid_template_columns, GRID_TEMPLATE_COLUMNS)
RAW_PARSER(grid_template_rows, GRID_TEMPLATE_ROWS)
RAW_PARSER(grid_template_areas, GRID_TEMPLATE_AREAS)
RAW_PARSER(grid_column_start, GRID_COLUMN_START)
RAW_PARSER(grid_column_end, GRID_COLUMN_END)
RAW_PARSER(grid_row_start, GRID_ROW_START)
RAW_PARSER(grid_row_end, GRID_ROW_END)
RAW_PARSER(grid_auto_flow, GRID_AUTO_FLOW)
RAW_PARSER(grid_auto_columns, GRID_AUTO_COLUMNS)
RAW_PARSER(grid_auto_rows, GRID_AUTO_ROWS)
RAW_PARSER(row_gap, ROW_GAP)
RAW_PARSER(justify_items, JUSTIFY_ITEMS)
RAW_PARSER(object_fit, OBJECT_FIT)
RAW_PARSER(aspect_ratio, ASPECT_RATIO)
RAW_PARSER(text_overflow, TEXT_OVERFLOW)
RAW_PARSER(overflow_wrap, OVERFLOW_WRAP)
RAW_PARSER(word_break, WORD_BREAK)
RAW_PARSER(background_size, BACKGROUND_SIZE)
RAW_PARSER(filter, FILTER)
RAW_PARSER(transform_origin, TRANSFORM_ORIGIN)
RAW_PARSER(object_position, OBJECT_POSITION)
RAW_PARSER(justify_self, JUSTIFY_SELF)
RAW_PARSER(pointer_events, POINTER_EVENTS)
RAW_PARSER(line_clamp, LINE_CLAMP)

/**
 * Text of component range [from, to) of a raw value
 */
static void raw_range(const struct raw_value *rv, int from, int to,
		const char **text, size_t *len)
{
	if (from >= to || from >= rv->ncomp) {
		*text = "";
		*len = 0;
		return;
	}
	if (to > rv->ncomp)
		to = rv->ncomp;
	*text = rv->text + rv->comp[from].start;
	*len = rv->comp[to - 1].start + rv->comp[to - 1].len -
			rv->comp[from].start;
}

/**
 * border-radius: 1-4 horizontal radii [ / 1-4 vertical radii ]
 */
css_error css__parse_border_radius(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const enum css_properties_e props[4] = {
		CSS_PROP_BORDER_TOP_LEFT_RADIUS,
		CSS_PROP_BORDER_TOP_RIGHT_RADIUS,
		CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS,
		CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS
	};
	/* which of the 1-4 given values each corner uses */
	static const int pick[4][4] = {
		{ 0, 0, 0, 0 },
		{ 0, 1, 0, 1 },
		{ 0, 1, 2, 1 },
		{ 0, 1, 2, 3 }
	};
	int32_t orig_ctx = *ctx;
	struct raw_value *rv;
	css_error error = CSS_OK;
	int nh, nv, i;

	if (raw_flag_value(c, vector, ctx, result, props, 4, &error))
		return error;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;

	error = raw_read(vector, ctx, rv);
	if (error != CSS_OK)
		goto out;

	nh = rv->nslash > 0 ? rv->slash[0] : rv->ncomp;
	nv = rv->ncomp - nh;
	if (nh < 1 || nh > 4 || nv > 4 || rv->nslash > 1 ||
			(rv->nslash == 1 && nv < 1) || rv->comma) {
		error = CSS_INVALID;
		goto out;
	}

	for (i = 0; i < 4 && error == CSS_OK; i++) {
		struct raw_component *h = &rv->comp[pick[nh - 1][i]];
		char buf[256];
		size_t len = h->len < 120 ? h->len : 120;

		memcpy(buf, rv->text + h->start, len);
		if (nv > 0) {
			struct raw_component *v =
					&rv->comp[nh + pick[nv - 1][i]];
			size_t vlen = v->len < 120 ? v->len : 120;
			buf[len++] = ' ';
			memcpy(buf + len, rv->text + v->start, vlen);
			len += vlen;
		}
		error = raw_emit(c, result, props[i], buf, len);
	}

out:
	free(rv);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}

/**
 * Is the component a custom identifier (a grid line or area name)?
 */
static bool raw_comp_is_ident(const parserutils_vector *vector,
		const struct raw_value *rv, int i)
{
	const css_token *t;

	if (i >= rv->ncomp)
		return false;
	t = parserutils_vector_peek(vector, rv->comp[i].ctx);
	if (t == NULL || t->type != CSS_TOKEN_IDENT)
		return false;
	/* "auto" and "span" are keywords, not names */
	if (lwc_string_length(t->idata) == 4 &&
			strncasecmp(lwc_string_data(t->idata), "auto", 4) == 0)
		return false;
	if (lwc_string_length(t->idata) == 4 &&
			strncasecmp(lwc_string_data(t->idata), "span", 4) == 0)
		return false;
	return rv->comp[i].len == lwc_string_length(t->idata);
}

/**
 * Split a '/'-separated grid placement shorthand into up to 4 longhands.
 */
static css_error parse_grid_placement(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, const enum css_properties_e *props,
		int nprops)
{
	int32_t orig_ctx = *ctx;
	struct raw_value *rv;
	css_error error = CSS_OK;
	int bound[5];
	int i, n;

	if (raw_flag_value(c, vector, ctx, result, props, nprops, &error))
		return error;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;

	error = raw_read(vector, ctx, rv);
	if (error != CSS_OK)
		goto out;

	n = rv->nslash + 1;
	if (n > nprops || rv->comma) {
		error = CSS_INVALID;
		goto out;
	}
	bound[0] = 0;
	for (i = 0; i < rv->nslash; i++)
		bound[i + 1] = rv->slash[i];
	bound[n] = rv->ncomp;

	for (i = 0; i < nprops && error == CSS_OK; i++) {
		const char *text;
		size_t len;

		if (i < n) {
			raw_range(rv, bound[i], bound[i + 1], &text, &len);
		} else {
			/* Omitted value: copy a custom ident from the
			 * matching earlier value, else auto */
			int from = (nprops == 4) ? (i >= 2 ? i - 2 : 0) : 0;
			if (from < n && bound[from + 1] - bound[from] == 1 &&
					raw_comp_is_ident(vector, rv,
						bound[from])) {
				raw_range(rv, bound[from], bound[from] + 1,
						&text, &len);
			} else {
				text = "auto";
				len = 4;
			}
		}
		if (len == 0) {
			error = CSS_INVALID;
			break;
		}
		error = raw_emit(c, result, props[i], text, len);
	}

out:
	free(rv);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}

css_error css__parse_grid_column(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const enum css_properties_e props[2] = {
		CSS_PROP_GRID_COLUMN_START, CSS_PROP_GRID_COLUMN_END
	};
	return parse_grid_placement(c, vector, ctx, result, props, 2);
}

css_error css__parse_grid_row(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const enum css_properties_e props[2] = {
		CSS_PROP_GRID_ROW_START, CSS_PROP_GRID_ROW_END
	};
	return parse_grid_placement(c, vector, ctx, result, props, 2);
}

css_error css__parse_grid_area(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const enum css_properties_e props[4] = {
		CSS_PROP_GRID_ROW_START, CSS_PROP_GRID_COLUMN_START,
		CSS_PROP_GRID_ROW_END, CSS_PROP_GRID_COLUMN_END
	};
	return parse_grid_placement(c, vector, ctx, result, props, 4);
}

/**
 * grid-template: <rows> / <columns> (the areas form keeps only the
 * track lists; strings go to grid-template-areas)
 */
css_error css__parse_grid_template(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const enum css_properties_e props[3] = {
		CSS_PROP_GRID_TEMPLATE_ROWS, CSS_PROP_GRID_TEMPLATE_COLUMNS,
		CSS_PROP_GRID_TEMPLATE_AREAS
	};
	int32_t orig_ctx = *ctx;
	struct raw_value *rv;
	css_error error = CSS_OK;
	const char *text;
	size_t len;

	if (raw_flag_value(c, vector, ctx, result, props, 3, &error))
		return error;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;

	error = raw_read(vector, ctx, rv);
	if (error != CSS_OK || rv->nslash != 1) {
		error = CSS_INVALID;
		goto out;
	}

	raw_range(rv, 0, rv->slash[0], &text, &len);
	error = raw_emit(c, result, props[0], text, len);
	if (error == CSS_OK) {
		raw_range(rv, rv->slash[0], rv->ncomp, &text, &len);
		error = raw_emit(c, result, props[1], text, len);
	}
	if (error == CSS_OK)
		error = css__stylesheet_style_appendOPV(result, props[2],
				0, RAW_NONE);
out:
	free(rv);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}

typedef css_error (*longhand_parser)(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result);

/**
 * Parse one component of a multi-value shorthand with an existing
 * longhand parser, by rewinding the token stream to that component.
 */
static css_error parse_component_with(css_language *c,
		const parserutils_vector *vector, const struct raw_value *rv,
		int i, css_style *result, longhand_parser fn)
{
	int32_t cctx = rv->comp[i].ctx;
	css_error error = fn(c, vector, &cctx, result);
	return error;
}

/**
 * gap: <row-gap> [<column-gap>]
 */
css_error css__parse_gap(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const enum css_properties_e props[2] = {
		CSS_PROP_ROW_GAP, CSS_PROP_COLUMN_GAP
	};
	int32_t orig_ctx = *ctx;
	struct raw_value *rv;
	css_error error = CSS_OK;

	if (raw_flag_value(c, vector, ctx, result, props, 2, &error))
		return error;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;

	error = raw_read(vector, ctx, rv);
	if (error != CSS_OK || rv->ncomp < 1 || rv->ncomp > 2 ||
			rv->nslash != 0 || rv->comma) {
		error = CSS_INVALID;
		goto out;
	}

	error = raw_emit(c, result, CSS_PROP_ROW_GAP,
			rv->text + rv->comp[0].start, rv->comp[0].len);
	if (error == CSS_OK)
		error = parse_component_with(c, vector, rv, rv->ncomp - 1,
				result, css__parse_column_gap);
out:
	free(rv);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}

/**
 * inset: 1-4 values for top/right/bottom/left
 */
css_error css__parse_inset(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const enum css_properties_e props[4] = {
		CSS_PROP_TOP, CSS_PROP_RIGHT, CSS_PROP_BOTTOM, CSS_PROP_LEFT
	};
	static const longhand_parser fns[4] = {
		css__parse_top, css__parse_right,
		css__parse_bottom, css__parse_left
	};
	static const int pick[4][4] = {
		{ 0, 0, 0, 0 },
		{ 0, 1, 0, 1 },
		{ 0, 1, 2, 1 },
		{ 0, 1, 2, 3 }
	};
	int32_t orig_ctx = *ctx;
	struct raw_value *rv;
	css_error error = CSS_OK;
	int i;

	if (raw_flag_value(c, vector, ctx, result, props, 4, &error))
		return error;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;

	error = raw_read(vector, ctx, rv);
	if (error != CSS_OK || rv->ncomp < 1 || rv->ncomp > 4 ||
			rv->nslash != 0 || rv->comma) {
		error = CSS_INVALID;
		goto out;
	}

	for (i = 0; i < 4 && error == CSS_OK; i++) {
		error = parse_component_with(c, vector, rv,
				pick[rv->ncomp - 1][i], result, fns[i]);
	}
out:
	free(rv);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}

/**
 * place-*: <align> [<justify>]
 */
static css_error parse_place(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, enum css_properties_e aprop,
		longhand_parser afn, enum css_properties_e jprop,
		longhand_parser jfn)
{
	enum css_properties_e props[2] = { aprop, jprop };
	int32_t orig_ctx = *ctx;
	struct raw_value *rv;
	css_error error = CSS_OK;

	if (raw_flag_value(c, vector, ctx, result, props, 2, &error))
		return error;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;

	error = raw_read(vector, ctx, rv);
	if (error != CSS_OK || rv->ncomp < 1 || rv->ncomp > 2 ||
			rv->nslash != 0 || rv->comma) {
		error = CSS_INVALID;
		goto out;
	}

	/* The align value may be a keyword the longhand parser does not
	 * know (e.g. "start"); that should not lose the whole rule. */
	if (parse_component_with(c, vector, rv, 0, result, afn) != CSS_OK) {
		/* ignore */
	}
	if (jfn != NULL) {
		if (parse_component_with(c, vector, rv, rv->ncomp - 1,
				result, jfn) != CSS_OK) {
			/* ignore */
		}
	} else {
		error = raw_emit(c, result, jprop,
				rv->text + rv->comp[rv->ncomp - 1].start,
				rv->comp[rv->ncomp - 1].len);
	}
out:
	free(rv);
	if (error != CSS_OK)
		*ctx = orig_ctx;
	return error;
}

css_error css__parse_place_items(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_place(c, vector, ctx, result,
			CSS_PROP_ALIGN_ITEMS, css__parse_align_items,
			CSS_PROP_JUSTIFY_ITEMS, NULL);
}

css_error css__parse_place_content(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_place(c, vector, ctx, result,
			CSS_PROP_ALIGN_CONTENT, css__parse_align_content,
			CSS_PROP_JUSTIFY_CONTENT, css__parse_justify_content);
}

css_error css__parse_place_self(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_place(c, vector, ctx, result,
			CSS_PROP_ALIGN_SELF, css__parse_align_self,
			CSS_PROP_JUSTIFY_SELF, NULL);
}

/**
 * Serialise an image function (gradient) starting at *ctx into a string
 * prefixed with "gradient:", for storage as a background image "URL".
 *
 * \return CSS_OK and *ctx advanced past the function, or CSS_INVALID
 */
css_error css__parse_image_function(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		lwc_string **result)
{
	int32_t orig_ctx = *ctx;
	const css_token *t;
	char *buf;
	size_t len;
	int depth = 0;
	bool need_space = false;
	struct raw_value *rv;
	lwc_error lerror;

	UNUSED(c);

	t = parserutils_vector_peek(vector, *ctx);
	if (t == NULL || t->type != CSS_TOKEN_FUNCTION)
		return CSS_INVALID;
	len = lwc_string_length(t->idata);
	if (len < 8 || strncasecmp(lwc_string_data(t->idata) + len - 8,
			"gradient", 8) != 0)
		return CSS_INVALID;

	rv = malloc(sizeof(*rv));
	if (rv == NULL)
		return CSS_NOMEM;
	rv->len = 0;
	raw_append(rv, "gradient:", 9);

	do {
		t = parserutils_vector_iterate(vector, ctx);
		if (t == NULL)
			break;
		if (t->type == CSS_TOKEN_S) {
			need_space = true;
			continue;
		}
		if (need_space && !raw_is_char(t, ')') &&
				!raw_is_char(t, ',') &&
				rv->text[rv->len - 1] != '(')
			raw_append(rv, " ", 1);
		need_space = false;
		raw_append_token(rv, t);
		if (t->type == CSS_TOKEN_FUNCTION || raw_is_char(t, '('))
			depth++;
		else if (raw_is_char(t, ')'))
			depth--;
	} while (depth > 0);

	if (depth != 0) {
		free(rv);
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	buf = rv->text;
	lerror = lwc_intern_string(buf, rv->len, result);
	free(rv);
	if (lerror != lwc_error_ok) {
		*ctx = orig_ctx;
		return CSS_NOMEM;
	}
	return CSS_OK;
}

/**
 * Parse the <bg-size> after '/' in the background shorthand: one or two
 * of <length-percentage> | auto, or cover | contain.
 */
css_error css__parse_background_size_value(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	const css_token *t;
	char buf[128];
	size_t len = 0;
	int n = 0;
	struct raw_value rv;

	while (n < 2) {
		consumeWhitespace(vector, ctx);
		t = parserutils_vector_peek(vector, *ctx);
		if (t == NULL)
			break;
		if (t->type == CSS_TOKEN_IDENT) {
			const char *s = lwc_string_data(t->idata);
			size_t l = lwc_string_length(t->idata);
			bool single = (l == 5 && strncasecmp(s, "cover", 5) == 0) ||
				(l == 7 && strncasecmp(s, "contain", 7) == 0);
			if (!single && !(l == 4 && strncasecmp(s, "auto", 4) == 0))
				break;
			if (single && n > 0)
				break;
			if (single)
				n = 1;
		} else if (t->type != CSS_TOKEN_DIMENSION &&
				t->type != CSS_TOKEN_PERCENTAGE &&
				t->type != CSS_TOKEN_NUMBER) {
			break;
		}
		rv.len = 0;
		raw_append_token(&rv, t);
		if (len + rv.len + 1 >= sizeof(buf))
			break;
		if (len > 0)
			buf[len++] = ' ';
		memcpy(buf + len, rv.text, rv.len);
		len += rv.len;
		n++;
		parserutils_vector_iterate(vector, ctx);
	}

	if (len == 0) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}
	return raw_emit(c, result, CSS_PROP_BACKGROUND_SIZE, buf, len);
}
