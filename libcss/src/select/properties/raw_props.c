/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * Raw-string properties: cascaded as uninterpreted value text.
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

#define RAW_PROPERTY(pname)						\
css_error css__cascade_##pname(uint32_t opv, css_style *style,		\
		css_select_state *state)				\
{									\
	return css__cascade_uri_none(opv, style, state, set_##pname);	\
}									\
css_error css__set_##pname##_from_hint(const css_hint *hint,		\
		css_computed_style *style)				\
{									\
	css_error error = set_##pname(style, hint->status,		\
			hint->data.string);				\
	lwc_string_unref(hint->data.string);				\
	return error;							\
}									\
css_error css__initial_##pname(css_select_state *state)			\
{									\
	return set_##pname(state->computed, CSS_RAW_NONE, NULL);	\
}									\
css_error css__copy_##pname(const css_computed_style *from,		\
		css_computed_style *to)					\
{									\
	lwc_string *v;							\
	uint8_t type = get_##pname(from, &v);				\
	if (from == to)							\
		return CSS_OK;						\
	return set_##pname(to, type, v);				\
}									\
css_error css__compose_##pname(const css_computed_style *parent,	\
		const css_computed_style *child,			\
		css_computed_style *result)				\
{									\
	lwc_string *v;							\
	uint8_t type = get_##pname(child, &v);				\
	return css__copy_##pname(					\
			type == CSS_RAW_INHERIT ? parent : child, result); \
}

RAW_PROPERTY(border_top_left_radius)
RAW_PROPERTY(border_top_right_radius)
RAW_PROPERTY(border_bottom_right_radius)
RAW_PROPERTY(border_bottom_left_radius)
RAW_PROPERTY(box_shadow)
RAW_PROPERTY(text_shadow)
RAW_PROPERTY(transform)
RAW_PROPERTY(grid_template_columns)
RAW_PROPERTY(grid_template_rows)
RAW_PROPERTY(grid_template_areas)
RAW_PROPERTY(grid_column_start)
RAW_PROPERTY(grid_column_end)
RAW_PROPERTY(grid_row_start)
RAW_PROPERTY(grid_row_end)
RAW_PROPERTY(grid_auto_flow)
RAW_PROPERTY(grid_auto_columns)
RAW_PROPERTY(grid_auto_rows)
RAW_PROPERTY(row_gap)
RAW_PROPERTY(justify_items)
RAW_PROPERTY(object_fit)
RAW_PROPERTY(aspect_ratio)
RAW_PROPERTY(text_overflow)
RAW_PROPERTY(overflow_wrap)
RAW_PROPERTY(word_break)
RAW_PROPERTY(background_size)
RAW_PROPERTY(filter)
RAW_PROPERTY(transform_origin)
RAW_PROPERTY(object_position)
RAW_PROPERTY(justify_self)
RAW_PROPERTY(pointer_events)
RAW_PROPERTY(line_clamp)
