/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 */

#ifndef css_parse_preprocess_h_
#define css_parse_preprocess_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libcss/errors.h>

/**
 * Rewrite stylesheet text into a form the parser supports.
 *
 * \param data          stylesheet bytes (ASCII compatible encoding)
 * \param len           length of data
 * \param inline_style  data is a declaration list (style attribute)
 * \param out           receives malloc()ed output, owned by caller
 * \param out_len       receives length of output
 * \return CSS_OK on success, CSS_INVALID if the data should be parsed
 *         unmodified, CSS_NOMEM on memory exhaustion
 */
css_error css__preprocess(const uint8_t *data, size_t len, bool inline_style,
		uint8_t **out, size_t *out_len);

#endif
