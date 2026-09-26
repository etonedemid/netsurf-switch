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
 * Interpretation of LibCSS raw-string properties (modern CSS values that
 * LibCSS cascades as text): lengths, colours, radii, shadows, gradients,
 * filters and transforms.
 */

#ifndef NETSURF_CSS_CSS_FX_H_
#define NETSURF_CSS_CSS_FX_H_

#include <stdbool.h>
#include <libcss/libcss.h>

#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/plotters.h"

/** Maximum number of shadows honoured per box */
#define CSSFX_MAX_SHADOWS 4

/**
 * Get the text of a raw property, or NULL if unset/none.
 */
const char *cssfx_raw(const css_computed_style *style,
		enum css_properties_e prop);

/**
 * Parse a length or percentage, advancing *p.
 *
 * \param p        text pointer, advanced past the value on success
 * \param style    style for font-relative units
 * \param uctx     unit conversion context
 * \param pct_base value of 100% in device pixels
 * \param out      resulting length in device pixels
 * \return true on success
 */
bool cssfx_length(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float pct_base, float *out);

/**
 * Parse a colour, advancing *p.
 *
 * \param p      text pointer, advanced past the value on success
 * \param style  style supplying currentColor (may be NULL)
 * \param out    NetSurf colour (inverted alpha in top byte)
 * \return true on success
 */
bool cssfx_colour(const char **p, const css_computed_style *style,
		colour *out);

/**
 * Parse a CSS colour string into a libcss ARGB colour.
 */
bool cssfx_parse_css_colour(const char *text, css_color *out);

/**
 * Resolve border radii for a border box of w x h device pixels.
 *
 * \return true if any corner is rounded
 */
bool cssfx_radii(const css_computed_style *style, const css_unit_ctx *uctx,
		int w, int h, float scale, struct plot_radii *out);

/**
 * Resolve box-shadow.
 *
 * \return number of shadows written to out
 */
int cssfx_box_shadows(const css_computed_style *style,
		const css_unit_ctx *uctx, float scale,
		struct plot_shadow *out, int max);

/**
 * Resolve text-shadow (first shadow only).
 *
 * \return true if a shadow applies
 */
bool cssfx_text_shadow(const css_computed_style *style,
		const css_unit_ctx *uctx, float scale,
		struct plot_shadow *out);

/**
 * Resolve a gradient for a box.
 *
 * \param text   gradient text, e.g. "linear-gradient(red, blue)"
 * \param box    device rectangle the gradient is sized to
 * \param out    resolved gradient
 * \return true on success
 */
bool cssfx_gradient(const char *text, const css_computed_style *style,
		const css_unit_ctx *uctx, const struct rect *box,
		struct plot_gradient *out);

/**
 * Resolve filter functions and opacity into layer parameters.
 *
 * \return true if the layer does anything (opacity < 1 or a filter)
 */
bool cssfx_layer_effects(const css_computed_style *style,
		struct plot_layer *out);

/**
 * Resolve the translation part of transform for a w x h box.
 *
 * \return true if the box is translated
 */
bool cssfx_translation(const css_computed_style *style,
		const css_unit_ctx *uctx, int w, int h, int *dx, int *dy);

/**
 * Preferred aspect ratio (width / height) from aspect-ratio.
 *
 * \return false if none
 */
bool cssfx_aspect_ratio(const css_computed_style *style, float *ratio);

/**
 * Number of lines from -webkit-line-clamp / line-clamp, or 0.
 */
int cssfx_line_clamp(const css_computed_style *style);

/** how a mask image is sized within the box */
enum cssfx_mask_fit {
	CSSFX_MASK_STRETCH,  /**< 100% 100% (or auto for SVG) */
	CSSFX_MASK_CONTAIN,
	CSSFX_MASK_COVER,
	CSSFX_MASK_EXPLICIT, /**< width/height given */
};

struct cssfx_mask {
	enum cssfx_mask_fit fit;
	float w, h;      /**< explicit size (px), <0 for auto */
	float px, py;    /**< position as fraction of free space (0..1) */
	bool repeat;
};

/**
 * Get the url() of an element's mask image.
 *
 * 
eturn malloc()ed absolute URL or NULL if no image mask
 */
char *cssfx_mask_url(const css_computed_style *style);

/**
 * Get mask sizing and positioning.
 */
bool cssfx_mask_geometry(const css_computed_style *style,
		const css_unit_ctx *uctx, int bw, int bh,
		struct cssfx_mask *out);

#endif
