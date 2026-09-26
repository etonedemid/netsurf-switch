/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/**
 * Skip the remaining layers of a comma-separated background value, up
 * to the end of the declaration.
 */
static void skip_layers(const parserutils_vector *vector, int32_t *ctx)
{
	const css_token *t;
	int depth = 0;

	while ((t = parserutils_vector_peek(vector, *ctx)) != NULL) {
		if (depth == 0 && tokenIsChar(t, '!'))
			break;
		if (t->type == CSS_TOKEN_FUNCTION || tokenIsChar(t, '('))
			depth++;
		else if (tokenIsChar(t, ')') && depth > 0)
			depth--;
		parserutils_vector_iterate(vector, ctx);
	}
}

/**
 * Parse background-image
 *
 * Accepts none, url(), and gradient functions (stored as a string with a
 * "gradient:" prefix for the client to render). Only the first layer of
 * a multi-layer value is kept.
 */
static css_error parse_background_image(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, bool skip)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	enum flag_value flag_value;
	lwc_string *str = NULL;
	uint32_t snumber;
	bool match;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL)
		return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_BACKGROUND_IMAGE);
	}

	if (token->type == CSS_TOKEN_IDENT &&
			lwc_string_caseless_isequal(token->idata,
				c->strings[NONE], &match) == lwc_error_ok &&
			match) {
		parserutils_vector_iterate(vector, ctx);
		error = css__stylesheet_style_appendOPV(result,
				CSS_PROP_BACKGROUND_IMAGE, 0,
				BACKGROUND_IMAGE_NONE);
	} else if (token->type == CSS_TOKEN_URI) {
		parserutils_vector_iterate(vector, ctx);
		error = c->sheet->resolve(c->sheet->resolve_pw,
				c->sheet->url, token->idata, &str);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}
		goto emit;
	} else if (token->type == CSS_TOKEN_FUNCTION) {
		error = css__parse_image_function(c, vector, ctx, &str);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}
		goto emit;
	} else {
		return CSS_INVALID;
	}

	return error;

emit:
	error = css__stylesheet_string_add(c->sheet, str, &snumber);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_appendOPV(result,
			CSS_PROP_BACKGROUND_IMAGE, 0, BACKGROUND_IMAGE_URI);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	error = css__stylesheet_style_append(result, snumber);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return error;
	}

	/* Standalone property: drop any further layers */
	consumeWhitespace(vector, ctx);
	token = parserutils_vector_peek(vector, *ctx);
	if (skip && token != NULL && tokenIsChar(token, ','))
		skip_layers(vector, ctx);

	return CSS_OK;
}

css_error css__parse_background_image(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_background_image(c, vector, ctx, result, true);
}

css_error css__parse_background_image_layer(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_background_image(c, vector, ctx, result, false);
}
