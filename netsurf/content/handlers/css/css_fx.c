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
 * Interpretation of LibCSS raw-string properties.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

#include "utils/utils.h"
#include "netsurf/css.h"

#include "css/utils.h"
#include "css/css_fx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* exported interface documented in css_fx.h */
const char *cssfx_raw(const css_computed_style *style,
		enum css_properties_e prop)
{
	lwc_string *s = NULL;

	if (style == NULL)
		return NULL;
	if (css_computed_raw(style, prop, &s) != CSS_RAW_SET || s == NULL)
		return NULL;
	return lwc_string_data(s);
}

static inline void skip_ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
		(*p)++;
}

/** match a case-insensitive keyword followed by a delimiter */
static bool match_word(const char **p, const char *word)
{
	size_t n = strlen(word);
	char c;

	if (strncasecmp(*p, word, n) != 0)
		return false;
	c = (*p)[n];
	if (isalnum((unsigned char)c) || c == '-' || c == '_')
		return false;
	*p += n;
	return true;
}

/** match "name(" */
static bool match_fn(const char **p, const char *name)
{
	size_t n = strlen(name);

	if (strncasecmp(*p, name, n) != 0 || (*p)[n] != '(')
		return false;
	*p += n + 1;
	return true;
}

/** skip to just past the ')' matching an already consumed '(' */
static void skip_to_close(const char **p)
{
	int depth = 1;

	while (**p != '\0') {
		if (**p == '(')
			depth++;
		else if (**p == ')' && --depth == 0) {
			(*p)++;
			return;
		}
		(*p)++;
	}
}

/** skip one component value (token or function call) */
static void skip_component(const char **p)
{
	skip_ws(p);
	while (**p != '\0' && **p != ' ' && **p != ',' && **p != ')' &&
			**p != '/') {
		if (**p == '(') {
			(*p)++;
			skip_to_close(p);
		} else {
			(*p)++;
		}
	}
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static bool parse_number(const char **p, float *out)
{
	char *end;
	float v = strtof(*p, &end);

	if (end == *p)
		return false;
	*out = v;
	*p = end;
	return true;
}

static const struct {
	const char *name;
	css_unit unit;
} unit_names[] = {
	{ "px", CSS_UNIT_PX }, { "em", CSS_UNIT_EM }, { "rem", CSS_UNIT_REM },
	{ "ex", CSS_UNIT_EX }, { "ch", CSS_UNIT_CH }, { "vw", CSS_UNIT_VW },
	{ "vh", CSS_UNIT_VH }, { "vmin", CSS_UNIT_VMIN },
	{ "vmax", CSS_UNIT_VMAX }, { "pt", CSS_UNIT_PT }, { "pc", CSS_UNIT_PC },
	{ "cm", CSS_UNIT_CM }, { "mm", CSS_UNIT_MM }, { "in", CSS_UNIT_IN },
	{ "q", CSS_UNIT_Q }, { "lh", CSS_UNIT_LH }, { "svh", CSS_UNIT_VH },
	{ "dvh", CSS_UNIT_VH }, { "lvh", CSS_UNIT_VH }, { "svw", CSS_UNIT_VW },
	{ "dvw", CSS_UNIT_VW }, { "lvw", CSS_UNIT_VW },
};

static float to_px(float v, css_unit unit, const css_computed_style *style,
		const css_unit_ctx *uctx)
{
	if (unit == CSS_UNIT_PX || uctx == NULL)
		return v;
	return FIXTOFLT(css_unit_len2device_px(style, uctx, FLTTOFIX(v), unit));
}

static bool calc_sum(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float pct_base, float *out);

/** a calc() operand: number, length, percentage or nested expression */
static bool calc_value(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float pct_base, float *out,
		bool *is_number)
{
	skip_ws(p);
	*is_number = false;
	if (**p == '(') {
		(*p)++;
		if (!calc_sum(p, style, uctx, pct_base, out))
			return false;
		skip_ws(p);
		if (**p != ')')
			return false;
		(*p)++;
		return true;
	}
	if (match_fn(p, "calc") || match_fn(p, "-webkit-calc")) {
		if (!calc_sum(p, style, uctx, pct_base, out))
			return false;
		skip_ws(p);
		if (**p != ')')
			return false;
		(*p)++;
		return true;
	}
	if (cssfx_length(p, style, uctx, pct_base, out))
		return true;
	if (parse_number(p, out)) {
		*is_number = true;
		return true;
	}
	return false;
}

static bool calc_product(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float pct_base, float *out)
{
	float v, r;
	bool num, rnum;

	if (!calc_value(p, style, uctx, pct_base, &v, &num))
		return false;
	for (;;) {
		const char *save = *p;
		char op;
		skip_ws(p);
		op = **p;
		if (op != '*' && op != '/') {
			*p = save;
			break;
		}
		(*p)++;
		if (!calc_value(p, style, uctx, pct_base, &r, &rnum))
			return false;
		if (op == '*')
			v *= r;
		else if (r != 0.0f)
			v /= r;
	}
	*out = v;
	return true;
}

static bool calc_sum(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float pct_base, float *out)
{
	float v, r;

	if (!calc_product(p, style, uctx, pct_base, &v))
		return false;
	for (;;) {
		const char *save = *p;
		char op;
		skip_ws(p);
		op = **p;
		if ((op != '+' && op != '-') || (*p)[1] != ' ') {
			*p = save;
			break;
		}
		(*p)++;
		if (!calc_product(p, style, uctx, pct_base, &r))
			return false;
		v = (op == '+') ? v + r : v - r;
	}
	*out = v;
	return true;
}

/** min(), max() and clamp() argument lists */
static bool calc_minmax(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float pct_base, float *out, int kind)
{
	float vals[8];
	int n = 0, i;

	for (;;) {
		if (n >= 8 || !calc_sum(p, style, uctx, pct_base, &vals[n]))
			return false;
		n++;
		skip_ws(p);
		if (**p == ',') {
			(*p)++;
			continue;
		}
		if (**p != ')')
			return false;
		(*p)++;
		break;
	}
	if (kind == 2) {
		/* clamp(min, val, max) */
		if (n != 3)
			return false;
		*out = fmaxf(vals[0], fminf(vals[1], vals[2]));
		return true;
	}
	*out = vals[0];
	for (i = 1; i < n; i++)
		*out = kind == 0 ? fminf(*out, vals[i]) : fmaxf(*out, vals[i]);
	return true;
}

/* exported interface documented in css_fx.h */
bool cssfx_length(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float pct_base, float *out)
{
	const char *s = *p;
	float v;
	size_t i;

	skip_ws(&s);

	if (match_fn(&s, "calc") || match_fn(&s, "-webkit-calc")) {
		if (!calc_sum(&s, style, uctx, pct_base, &v))
			return false;
		skip_ws(&s);
		if (*s != ')')
			return false;
		*p = s + 1;
		*out = v;
		return true;
	}
	if (match_fn(&s, "min")) {
		if (!calc_minmax(&s, style, uctx, pct_base, out, 0))
			return false;
		*p = s;
		return true;
	}
	if (match_fn(&s, "max")) {
		if (!calc_minmax(&s, style, uctx, pct_base, out, 1))
			return false;
		*p = s;
		return true;
	}
	if (match_fn(&s, "clamp")) {
		if (!calc_minmax(&s, style, uctx, pct_base, out, 2))
			return false;
		*p = s;
		return true;
	}

	if (!parse_number(&s, &v))
		return false;

	if (*s == '%') {
		*out = v * pct_base / 100.0f;
		*p = s + 1;
		return true;
	}

	for (i = 0; i < sizeof(unit_names) / sizeof(unit_names[0]); i++) {
		size_t n = strlen(unit_names[i].name);
		if (strncasecmp(s, unit_names[i].name, n) == 0 &&
				!isalpha((unsigned char)s[n])) {
			*out = to_px(v, unit_names[i].unit, style, uctx);
			*p = s + n;
			return true;
		}
	}

	/* unitless zero */
	if (v == 0.0f && !isalpha((unsigned char)*s)) {
		*out = 0.0f;
		*p = s;
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Colours */

/* exported interface documented in css_fx.h */
bool cssfx_colour(const char **p, const css_computed_style *style,
		colour *out)
{
	css_color current = 0xff000000;
	css_color c;

	if (style != NULL)
		css_computed_color(style, &current);
	if (!css_parse_color_text(p, current, &c))
		return false;
	*out = nscss_color_to_ns(c);
	return true;
}

/* exported interface documented in css_fx.h */
bool cssfx_parse_css_colour(const char *text, css_color *out)
{
	const char *p = text;

	return css_parse_color_text(&p, 0xff000000, out);
}

/* ------------------------------------------------------------------ */
/* Radii */

/* exported interface documented in css_fx.h */
bool cssfx_radii(const css_computed_style *style, const css_unit_ctx *uctx,
		int w, int h, float scale, struct plot_radii *out)
{
	static const enum css_properties_e props[4] = {
		CSS_PROP_BORDER_TOP_LEFT_RADIUS,
		CSS_PROP_BORDER_TOP_RIGHT_RADIUS,
		CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS,
		CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS
	};
	bool any = false;
	int i;

	for (i = 0; i < 4; i++) {
		const char *t = cssfx_raw(style, props[i]);
		float rh = 0, rv;

		out->h[i] = out->v[i] = 0;
		if (t == NULL)
			continue;
		if (!cssfx_length(&t, style, uctx, w / scale, &rh))
			continue;
		rv = rh;
		skip_ws(&t);
		if (*t != '\0') {
			/* percentage vertical radius is relative to height */
			const char *t0 = t;
			if (!cssfx_length(&t, style, uctx, h / scale, &rv))
				rv = rh;
			(void)t0;
		} else {
			/* single percentage applies to each axis */
			const char *v = cssfx_raw(style, props[i]);
			if (strchr(v, '%') != NULL)
				cssfx_length(&v, style, uctx, h / scale, &rv);
		}
		out->h[i] = (int)(rh * scale + 0.5f);
		out->v[i] = (int)(rv * scale + 0.5f);
		if (out->h[i] > 0 && out->v[i] > 0)
			any = true;
		else
			out->h[i] = out->v[i] = 0;
	}
	return any;
}

/* ------------------------------------------------------------------ */
/* Shadows */

/**
 * Parse one shadow: [inset] <x> <y> [<blur> [<spread>]] [<colour>]
 */
static bool parse_shadow(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float scale, bool allow_spread,
		struct plot_shadow *sh)
{
	float len[4];
	int nlen = 0;
	bool have_colour = false;
	colour c = 0;

	sh->inset = false;
	for (;;) {
		skip_ws(p);
		if (**p == '\0' || **p == ',')
			break;
		if (match_word(p, "inset")) {
			sh->inset = true;
			continue;
		}
		if (nlen < 4 && cssfx_length(p, style, uctx, 0, &len[nlen])) {
			nlen++;
			continue;
		}
		if (!have_colour && cssfx_colour(p, style, &c)) {
			have_colour = true;
			continue;
		}
		return false;
	}
	if (nlen < 2 || (!allow_spread && nlen > 3))
		return false;
	if (!have_colour) {
		css_color cur = 0xff000000;
		css_computed_color(style, &cur);
		c = nscss_color_to_ns(cur);
	}
	sh->offset_x = (int)(len[0] * scale);
	sh->offset_y = (int)(len[1] * scale);
	sh->blur = nlen > 2 ? (int)(len[2] * scale) : 0;
	sh->spread = nlen > 3 ? (int)(len[3] * scale) : 0;
	if (sh->blur < 0)
		sh->blur = 0;
	sh->colour = c;
	return true;
}

/* exported interface documented in css_fx.h */
int cssfx_box_shadows(const css_computed_style *style,
		const css_unit_ctx *uctx, float scale,
		struct plot_shadow *out, int max)
{
	const char *t = cssfx_raw(style, CSS_PROP_BOX_SHADOW);
	int n = 0;

	if (t == NULL)
		return 0;
	while (n < max) {
		if (!parse_shadow(&t, style, uctx, scale, true, &out[n]))
			return n;
		/* fully transparent shadows are common resets */
		if (((out[n].colour >> 24) & 0xff) != 0xff)
			n++;
		skip_ws(&t);
		if (*t != ',')
			break;
		t++;
	}
	return n;
}

/* exported interface documented in css_fx.h */
bool cssfx_text_shadow(const css_computed_style *style,
		const css_unit_ctx *uctx, float scale,
		struct plot_shadow *out)
{
	const char *t = cssfx_raw(style, CSS_PROP_TEXT_SHADOW);

	if (t == NULL)
		return false;
	if (!parse_shadow(&t, style, uctx, scale, false, out))
		return false;
	return ((out->colour >> 24) & 0xff) != 0xff;
}

/* ------------------------------------------------------------------ */
/* Gradients */

/** parse an angle in degrees */
static bool parse_angle(const char **p, float *deg)
{
	const char *s = *p;
	float v;

	skip_ws(&s);
	if (!parse_number(&s, &v))
		return false;
	if (strncasecmp(s, "deg", 3) == 0) {
		s += 3;
	} else if (strncasecmp(s, "grad", 4) == 0) {
		s += 4;
		v *= 0.9f;
	} else if (strncasecmp(s, "rad", 3) == 0) {
		s += 3;
		v = v * 180.0f / M_PI;
	} else if (strncasecmp(s, "turn", 4) == 0) {
		s += 4;
		v *= 360.0f;
	} else if (v != 0.0f) {
		return false;
	}
	*deg = v;
	*p = s;
	return true;
}

/**
 * Parse the colour stop list up to the closing ')'.
 *
 * \param length gradient ray length in CSS px, for length positions
 */
static bool parse_stops(const char **p, const css_computed_style *style,
		const css_unit_ctx *uctx, float length,
		struct plot_gradient *g)
{
	float pos[PLOT_GRADIENT_MAX_STOPS];
	bool have[PLOT_GRADIENT_MAX_STOPS];
	int n = 0, i, j;

	for (;;) {
		colour c;
		float v;

		skip_ws(p);
		if (**p == ')') {
			(*p)++;
			break;
		}
		if (**p == ',') {
			(*p)++;
			continue;
		}
		if (!cssfx_colour(p, style, &c)) {
			/* colour hint (bare position) or unknown: skip */
			if (cssfx_length(p, style, uctx, length, &v))
				continue;
			return false;
		}
		if (n >= PLOT_GRADIENT_MAX_STOPS)
			continue;
		g->stop_colour[n] = c;
		have[n] = false;
		skip_ws(p);
		if (cssfx_length(p, style, uctx, length, &v)) {
			pos[n] = length > 0 ? v / length : 0;
			have[n] = true;
			skip_ws(p);
			if (n + 1 < PLOT_GRADIENT_MAX_STOPS &&
					cssfx_length(p, style, uctx, length, &v)) {
				/* double position: duplicate the stop */
				n++;
				g->stop_colour[n] = c;
				pos[n] = length > 0 ? v / length : 0;
				have[n] = true;
			}
		}
		n++;
	}
	if (n == 0)
		return false;
	if (n == 1) {
		g->stop_colour[1] = g->stop_colour[0];
		pos[0] = 0;
		pos[1] = 1;
		have[0] = have[1] = true;
		n = 2;
	}

	/* fix up positions (CSS Images 3, 3.5.3) */
	if (!have[0]) { pos[0] = 0; have[0] = true; }
	if (!have[n - 1]) { pos[n - 1] = 1; have[n - 1] = true; }
	for (i = 1; i < n; i++) {
		if (have[i] && pos[i] < pos[i - 1])
			pos[i] = pos[i - 1];
	}
	for (i = 1; i < n; i++) {
		if (!have[i]) {
			for (j = i + 1; j < n && !have[j]; j++)
				;
			/* j has a position; spread i..j-1 evenly */
			{
				int k;
				float a = pos[i - 1], b = pos[j];
				for (k = i; k < j; k++) {
					pos[k] = a + (b - a) * (k - i + 1) /
							(float)(j - i + 1);
					have[k] = true;
				}
			}
		}
	}
	for (i = 0; i < n; i++)
		g->stop_pos[i] = pos[i];
	g->nstops = n;
	return true;
}

/** parse a position component ("left", "center", "30%", "10px") */
static bool parse_pos_component(const char **p,
		const css_computed_style *style, const css_unit_ctx *uctx,
		float extent, bool horizontal, float *out)
{
	skip_ws(p);
	if (match_word(p, "center")) {
		*out = extent / 2;
		return true;
	}
	if (horizontal && match_word(p, "left")) {
		*out = 0;
		return true;
	}
	if (horizontal && match_word(p, "right")) {
		*out = extent;
		return true;
	}
	if (!horizontal && match_word(p, "top")) {
		*out = 0;
		return true;
	}
	if (!horizontal && match_word(p, "bottom")) {
		*out = extent;
		return true;
	}
	return cssfx_length(p, style, uctx, extent, out);
}

/* exported interface documented in css_fx.h */
bool cssfx_gradient(const char *text, const css_computed_style *style,
		const css_unit_ctx *uctx, const struct rect *box,
		struct plot_gradient *g)
{
	const char *p = text;
	float w = box->x1 - box->x0;
	float h = box->y1 - box->y0;
	float cx = w / 2, cy = h / 2;
	/* CSS px dimensions for resolving lengths; device scale is folded
	 * in by treating device px as CSS px here */

	memset(g, 0, sizeof(*g));
	skip_ws(&p);

	if (match_fn(&p, "repeating-linear-gradient")) {
		g->repeating = true;
		goto linear;
	}
	if (match_fn(&p, "linear-gradient") ||
			match_fn(&p, "-webkit-linear-gradient") ||
			match_fn(&p, "-moz-linear-gradient")) {
		float angle = 180.0f, dx, dy, len;
		const char *s;
		bool legacy;
linear:
		legacy = (text[1] == 'w' || text[1] == 'm');
		s = p;
		skip_ws(&s);
		dx = 0; dy = 1;
		if (parse_angle(&s, &angle)) {
			float a;
			if (legacy)
				angle = 90.0f - angle;
			a = angle * M_PI / 180.0f;
			dx = sinf(a);
			dy = -cosf(a);
			skip_ws(&s);
			if (*s == ',')
				s++;
			p = s;
		} else if (match_word(&s, "to") || legacy) {
			bool top = false, bottom = false, left = false,
					right = false;
			for (;;) {
				skip_ws(&s);
				if (match_word(&s, "top")) top = true;
				else if (match_word(&s, "bottom")) bottom = true;
				else if (match_word(&s, "left")) left = true;
				else if (match_word(&s, "right")) right = true;
				else break;
			}
			if (top || bottom || left || right) {
				if (legacy) {
					/* legacy syntax names the start side */
					bool t = top; top = bottom; bottom = t;
					t = left; left = right; right = t;
				}
				if ((top || bottom) && (left || right)) {
					/* perpendicular to the other diagonal */
					float nx = right ? h : -h;
					float ny = bottom ? w : -w;
					float n = sqrtf(nx * nx + ny * ny);
					dx = n > 0 ? nx / n : 0;
					dy = n > 0 ? ny / n : 1;
				} else {
					dx = right ? 1 : (left ? -1 : 0);
					dy = bottom ? 1 : (top ? -1 : 0);
				}
				skip_ws(&s);
				if (*s == ',')
					s++;
				p = s;
			}
		}
		len = fabsf(w * dx) + fabsf(h * dy);
		if (len < 1)
			len = 1;
		g->dx = dx;
		g->dy = dy;
		g->length = len;
		g->x0 = box->x0 + w / 2 - dx * len / 2;
		g->y0 = box->y0 + h / 2 - dy * len / 2;
		return parse_stops(&p, style, uctx, len, g);
	}

	if (match_fn(&p, "repeating-radial-gradient")) {
		g->repeating = true;
		goto radial;
	}
	if (match_fn(&p, "radial-gradient") ||
			match_fn(&p, "-webkit-radial-gradient")) {
		bool circle = false;
		int size = 3; /* 0 closest-side 1 farthest-side
			       * 2 closest-corner 3 farthest-corner 4 explicit */
		float ex = 0, ey = 0;
		float dl, dr, dt, db;
		const char *s;
		bool any = false;
radial:
		s = p;
		for (;;) {
			float v;
			skip_ws(&s);
			if (match_word(&s, "circle")) { circle = true; any = true; }
			else if (match_word(&s, "ellipse")) { any = true; }
			else if (match_word(&s, "closest-side")) { size = 0; any = true; }
			else if (match_word(&s, "farthest-side")) { size = 1; any = true; }
			else if (match_word(&s, "closest-corner")) { size = 2; any = true; }
			else if (match_word(&s, "farthest-corner")) { size = 3; any = true; }
			else if (match_word(&s, "at")) {
				float px, py;
				any = true;
				if (!parse_pos_component(&s, style, uctx, w, true, &px))
					return false;
				cx = px;
				skip_ws(&s);
				if (parse_pos_component(&s, style, uctx, h, false, &py))
					cy = py;
			} else if (cssfx_length(&s, style, uctx, w, &v)) {
				any = true;
				size = 4;
				ex = ey = v;
				skip_ws(&s);
				if (cssfx_length(&s, style, uctx, h, &v))
					ey = v;
				else
					circle = true;
			} else {
				break;
			}
		}
		if (any) {
			skip_ws(&s);
			if (*s == ',')
				s++;
			p = s;
		}
		dl = fabsf(cx); dr = fabsf(w - cx);
		dt = fabsf(cy); db = fabsf(h - cy);
		switch (size) {
		case 0:
			ex = fminf(dl, dr); ey = fminf(dt, db);
			if (circle) ex = ey = fminf(ex, ey);
			break;
		case 1:
			ex = fmaxf(dl, dr); ey = fmaxf(dt, db);
			if (circle) ex = ey = fmaxf(ex, ey);
			break;
		case 2:
			ex = fminf(dl, dr); ey = fminf(dt, db);
			if (circle) ex = ey = sqrtf(ex * ex + ey * ey);
			else { ex *= 1.41421356f; ey *= 1.41421356f; }
			break;
		case 3:
			ex = fmaxf(dl, dr); ey = fmaxf(dt, db);
			if (circle) ex = ey = sqrtf(ex * ex + ey * ey);
			else { ex *= 1.41421356f; ey *= 1.41421356f; }
			break;
		default:
			break;
		}
		g->radial = true;
		g->cx = box->x0 + cx;
		g->cy = box->y0 + cy;
		g->rx = ex > 0.5f ? ex : 0.5f;
		g->ry = ey > 0.5f ? ey : 0.5f;
		return parse_stops(&p, style, uctx, g->rx, g);
	}

	return false;
}

/* ------------------------------------------------------------------ */
/* Filters, opacity and transforms */

/** parse a filter amount: number or percentage */
static bool parse_amount(const char **p, float *v)
{
	skip_ws(p);
	if (**p == ')') {
		*v = 1.0f;
		(*p)++;
		return true;
	}
	if (!parse_number(p, v))
		return false;
	if (**p == '%') {
		(*p)++;
		*v /= 100.0f;
	}
	skip_ws(p);
	if (**p == ')')
		(*p)++;
	return true;
}

/* exported interface documented in css_fx.h */
bool cssfx_layer_effects(const css_computed_style *style,
		struct plot_layer *out)
{
	const char *t;
	css_fixed op = INTTOFIX(1);
	bool any = false;

	memset(out, 0, sizeof(*out));
	out->opacity = 1.0f;
	out->brightness = 1.0f;

	if (style == NULL)
		return false;

	if (css_computed_opacity(style, &op) == CSS_OPACITY_SET &&
			op < INTTOFIX(1)) {
		out->opacity = FIXTOFLT(op);
		if (out->opacity < 0)
			out->opacity = 0;
		any = true;
	}

	t = cssfx_raw(style, CSS_PROP_FILTER);
	while (t != NULL && *t != '\0') {
		float v;
		skip_ws(&t);
		if (*t == '\0')
			break;
		if (match_fn(&t, "grayscale")) {
			if (!parse_amount(&t, &v)) break;
			out->grayscale = clampf(v, 0, 1);
		} else if (match_fn(&t, "sepia")) {
			if (!parse_amount(&t, &v)) break;
			out->sepia = clampf(v, 0, 1);
		} else if (match_fn(&t, "invert")) {
			if (!parse_amount(&t, &v)) break;
			out->invert = clampf(v, 0, 1);
		} else if (match_fn(&t, "brightness")) {
			if (!parse_amount(&t, &v)) break;
			out->brightness = v < 0 ? 0 : v;
		} else if (match_fn(&t, "opacity")) {
			if (!parse_amount(&t, &v)) break;
			out->opacity *= clampf(v, 0, 1);
		} else {
			/* blur(), drop-shadow() etc: not rendered */
			while (*t != '\0' && *t != '(')
				t++;
			if (*t == '(') {
				t++;
				skip_to_close(&t);
			}
			continue;
		}
		any = true;
	}

	return any && (out->opacity < 1.0f || out->grayscale > 0 ||
			out->sepia > 0 || out->invert > 0 ||
			out->brightness != 1.0f);
}

/* exported interface documented in css_fx.h */
bool cssfx_translation(const css_computed_style *style,
		const css_unit_ctx *uctx, int w, int h, int *dx, int *dy)
{
	const char *t = cssfx_raw(style, CSS_PROP_TRANSFORM);
	float tx = 0, ty = 0;

	if (t == NULL)
		return false;

	while (*t != '\0') {
		float a, b;
		skip_ws(&t);
		if (*t == '\0')
			break;
		if (match_fn(&t, "translate") || match_fn(&t, "translate3d")) {
			if (!cssfx_length(&t, style, uctx, w, &a))
				return false;
			tx += a;
			skip_ws(&t);
			if (*t == ',') {
				t++;
				if (cssfx_length(&t, style, uctx, h, &b))
					ty += b;
			}
			skip_to_close(&t);
		} else if (match_fn(&t, "translateX")) {
			if (cssfx_length(&t, style, uctx, w, &a))
				tx += a;
			skip_to_close(&t);
		} else if (match_fn(&t, "translateY")) {
			if (cssfx_length(&t, style, uctx, h, &a))
				ty += a;
			skip_to_close(&t);
		} else if (match_fn(&t, "matrix")) {
			float m[6];
			int i;
			for (i = 0; i < 6; i++) {
				skip_ws(&t);
				if (!parse_number(&t, &m[i]))
					break;
				skip_ws(&t);
				if (*t == ',')
					t++;
			}
			if (i == 6) {
				tx += m[4];
				ty += m[5];
			}
			skip_to_close(&t);
		} else {
			/* scale/rotate/skew etc are not rendered */
			skip_component(&t);
			if (*t == ')')
				t++;
		}
	}

	*dx = (int)tx;
	*dy = (int)ty;
	return *dx != 0 || *dy != 0;
}

/* ------------------------------------------------------------------ */
/* mask-image                                                         */
/* ------------------------------------------------------------------ */

/** find the first url(...) in text; returns malloc()ed contents */
static char *first_url(const char *t)
{
	const char *u, *e;
	char *out;

	if (t == NULL)
		return NULL;
	u = strstr(t, "url(");
	if (u == NULL)
		return NULL;
	u += 4;
	while (*u == ' ' || *u == '"' || *u == '\'')
		u++;
	e = u;
	while (*e != '\0' && *e != ')' && *e != '"' && *e != '\'')
		e++;
	while (e > u && e[-1] == ' ')
		e--;
	if (e == u)
		return NULL;
	out = malloc(e - u + 1);
	if (out != NULL) {
		memcpy(out, u, e - u);
		out[e - u] = '\0';
	}
	return out;
}

/* exported interface documented in css_fx.h */
char *cssfx_mask_url(const css_computed_style *style)
{
	char *u = first_url(cssfx_raw(style, CSS_PROP_MASK_IMAGE));

	if (u == NULL)
		u = first_url(cssfx_raw(style, CSS_PROP_MASK));
	return u;
}

/** parse a position keyword or percentage into a fraction */
static bool mask_pos_word(const char **p, float *out, bool *vertical)
{
	char *end;
	float v;

	*vertical = false;
	if (match_word(p, "left") || match_word(p, "top")) {
		*vertical = ((*p)[-1] == 'p');
		*out = 0;
		return true;
	}
	if (match_word(p, "right") || match_word(p, "bottom")) {
		*vertical = ((*p)[-1] == 'm');
		*out = 1;
		return true;
	}
	if (match_word(p, "center")) {
		*out = 0.5f;
		return true;
	}
	v = strtof(*p, &end);
	if (end != *p && *end == '%') {
		*out = v / 100.0f;
		*p = end + 1;
		return true;
	}
	return false;
}

/* exported interface documented in css_fx.h */
bool cssfx_mask_geometry(const css_computed_style *style,
		const css_unit_ctx *uctx, int bw, int bh,
		struct cssfx_mask *out)
{
	const char *size = cssfx_raw(style, CSS_PROP_MASK_SIZE);
	const char *pos = cssfx_raw(style, CSS_PROP_MASK_POSITION);
	const char *rep = cssfx_raw(style, CSS_PROP_MASK_REPEAT);
	const char *sh = cssfx_raw(style, CSS_PROP_MASK);
	const char *p;

	out->fit = CSSFX_MASK_STRETCH;
	out->w = out->h = -1;
	out->px = out->py = 0;
	out->repeat = true;

	/* the shorthand provides defaults for the longhands */
	if (sh != NULL) {
		/* skip the url(), which contains slashes of its own */
		const char *rest = strrchr(sh, ')');
		const char *slash = strchr(rest != NULL ? rest : sh, '/');
		if (strstr(sh, "no-repeat") != NULL)
			out->repeat = false;
		if (strstr(sh, "center") != NULL)
			out->px = out->py = 0.5f;
		if (slash != NULL && size == NULL)
			size = slash + 1;
		else if (size == NULL && strstr(sh, "contain") != NULL)
			size = "contain";
		else if (size == NULL && strstr(sh, "cover") != NULL)
			size = "cover";
	}

	if (rep != NULL)
		out->repeat = strstr(rep, "no-repeat") == NULL;

	if (size != NULL) {
		p = size;
		skip_ws(&p);
		if (match_word(&p, "contain")) {
			out->fit = CSSFX_MASK_CONTAIN;
		} else if (match_word(&p, "cover")) {
			out->fit = CSSFX_MASK_COVER;
		} else {
			float w = -1, h = -1;
			if (!match_word(&p, "auto") &&
			    !cssfx_length(&p, style, uctx, bw, &w))
				w = -1;
			skip_ws(&p);
			if (*p != '\0' && *p != ',' && *p != ' ') {
				if (!match_word(&p, "auto") &&
				    !cssfx_length(&p, style, uctx, bh, &h))
					h = -1;
			} else if (w >= 0) {
				h = -2; /* auto: keep aspect */
			}
			if (w >= 0 || h >= 0) {
				out->fit = CSSFX_MASK_EXPLICIT;
				out->w = w;
				out->h = h;
			}
		}
	}

	if (pos != NULL) {
		float a, b;
		bool va, vb;
		p = pos;
		skip_ws(&p);
		if (mask_pos_word(&p, &a, &va)) {
			skip_ws(&p);
			if (mask_pos_word(&p, &b, &vb)) {
				if (va && !vb) {
					out->px = b;
					out->py = a;
				} else {
					out->px = a;
					out->py = b;
				}
			} else if (va) {
				out->py = a;
				out->px = 0.5f;
			} else {
				out->px = a;
				out->py = 0.5f;
			}
		}
	}
	return true;
}
