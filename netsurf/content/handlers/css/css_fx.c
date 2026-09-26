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

static const struct named_colour {
	const char *name;
	uint32_t rgb;
} named_colours[] = {
	{"aliceblue",0xf0f8ff},{"antiquewhite",0xfaebd7},{"aqua",0x00ffff},
	{"aquamarine",0x7fffd4},{"azure",0xf0ffff},{"beige",0xf5f5dc},
	{"bisque",0xffe4c4},{"black",0x000000},{"blanchedalmond",0xffebcd},
	{"blue",0x0000ff},{"blueviolet",0x8a2be2},{"brown",0xa52a2a},
	{"burlywood",0xdeb887},{"cadetblue",0x5f9ea0},{"chartreuse",0x7fff00},
	{"chocolate",0xd2691e},{"coral",0xff7f50},{"cornflowerblue",0x6495ed},
	{"cornsilk",0xfff8dc},{"crimson",0xdc143c},{"cyan",0x00ffff},
	{"darkblue",0x00008b},{"darkcyan",0x008b8b},
	{"darkgoldenrod",0xb8860b},{"darkgray",0xa9a9a9},
	{"darkgreen",0x006400},{"darkgrey",0xa9a9a9},{"darkkhaki",0xbdb76b},
	{"darkmagenta",0x8b008b},{"darkolivegreen",0x556b2f},
	{"darkorange",0xff8c00},{"darkorchid",0x9932cc},{"darkred",0x8b0000},
	{"darksalmon",0xe9967a},{"darkseagreen",0x8fbc8f},
	{"darkslateblue",0x483d8b},{"darkslategray",0x2f4f4f},
	{"darkslategrey",0x2f4f4f},{"darkturquoise",0x00ced1},
	{"darkviolet",0x9400d3},{"deeppink",0xff1493},
	{"deepskyblue",0x00bfff},{"dimgray",0x696969},{"dimgrey",0x696969},
	{"dodgerblue",0x1e90ff},{"firebrick",0xb22222},
	{"floralwhite",0xfffaf0},{"forestgreen",0x228b22},
	{"fuchsia",0xff00ff},{"gainsboro",0xdcdcdc},{"ghostwhite",0xf8f8ff},
	{"gold",0xffd700},{"goldenrod",0xdaa520},{"gray",0x808080},
	{"green",0x008000},{"greenyellow",0xadff2f},{"grey",0x808080},
	{"honeydew",0xf0fff0},{"hotpink",0xff69b4},{"indianred",0xcd5c5c},
	{"indigo",0x4b0082},{"ivory",0xfffff0},{"khaki",0xf0e68c},
	{"lavender",0xe6e6fa},{"lavenderblush",0xfff0f5},
	{"lawngreen",0x7cfc00},{"lemonchiffon",0xfffacd},
	{"lightblue",0xadd8e6},{"lightcoral",0xf08080},{"lightcyan",0xe0ffff},
	{"lightgoldenrodyellow",0xfafad2},{"lightgray",0xd3d3d3},
	{"lightgreen",0x90ee90},{"lightgrey",0xd3d3d3},{"lightpink",0xffb6c1},
	{"lightsalmon",0xffa07a},{"lightseagreen",0x20b2aa},
	{"lightskyblue",0x87cefa},{"lightslategray",0x778899},
	{"lightslategrey",0x778899},{"lightsteelblue",0xb0c4de},
	{"lightyellow",0xffffe0},{"lime",0x00ff00},{"limegreen",0x32cd32},
	{"linen",0xfaf0e6},{"magenta",0xff00ff},{"maroon",0x800000},
	{"mediumaquamarine",0x66cdaa},{"mediumblue",0x0000cd},
	{"mediumorchid",0xba55d3},{"mediumpurple",0x9370db},
	{"mediumseagreen",0x3cb371},{"mediumslateblue",0x7b68ee},
	{"mediumspringgreen",0x00fa9a},{"mediumturquoise",0x48d1cc},
	{"mediumvioletred",0xc71585},{"midnightblue",0x191970},
	{"mintcream",0xf5fffa},{"mistyrose",0xffe4e1},{"moccasin",0xffe4b5},
	{"navajowhite",0xffdead},{"navy",0x000080},{"oldlace",0xfdf5e6},
	{"olive",0x808000},{"olivedrab",0x6b8e23},{"orange",0xffa500},
	{"orangered",0xff4500},{"orchid",0xda70d6},
	{"palegoldenrod",0xeee8aa},{"palegreen",0x98fb98},
	{"paleturquoise",0xafeeee},{"palevioletred",0xdb7093},
	{"papayawhip",0xffefd5},{"peachpuff",0xffdab9},{"peru",0xcd853f},
	{"pink",0xffc0cb},{"plum",0xdda0dd},{"powderblue",0xb0e0e6},
	{"purple",0x800080},{"rebeccapurple",0x663399},{"red",0xff0000},
	{"rosybrown",0xbc8f8f},{"royalblue",0x4169e1},
	{"saddlebrown",0x8b4513},{"salmon",0xfa8072},{"sandybrown",0xf4a460},
	{"seagreen",0x2e8b57},{"seashell",0xfff5ee},{"sienna",0xa0522d},
	{"silver",0xc0c0c0},{"skyblue",0x87ceeb},{"slateblue",0x6a5acd},
	{"slategray",0x708090},{"slategrey",0x708090},{"snow",0xfffafa},
	{"springgreen",0x00ff7f},{"steelblue",0x4682b4},{"tan",0xd2b48c},
	{"teal",0x008080},{"thistle",0xd8bfd8},{"tomato",0xff6347},
	{"turquoise",0x40e0d0},{"violet",0xee82ee},{"wheat",0xf5deb3},
	{"white",0xffffff},{"whitesmoke",0xf5f5f5},{"yellow",0xffff00},
	{"yellowgreen",0x9acd32},
};

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/** parse a colour-function channel: number or percentage (of scale) */
static bool parse_channel(const char **p, float scale, float *out)
{
	float v;

	skip_ws(p);
	if (match_word(p, "none")) {
		*out = 0;
		return true;
	}
	if (!parse_number(p, &v))
		return false;
	if (**p == '%') {
		(*p)++;
		v = v * scale / 100.0f;
	} else if (strncasecmp(*p, "deg", 3) == 0) {
		*p += 3;
	} else if (strncasecmp(*p, "turn", 4) == 0) {
		*p += 4;
		v *= 360.0f;
	} else if (strncasecmp(*p, "rad", 3) == 0) {
		*p += 3;
		v = v * 180.0f / M_PI;
	} else if (strncasecmp(*p, "grad", 4) == 0) {
		*p += 4;
		v *= 0.9f;
	}
	*out = v;
	return true;
}

/**
 * Parse up to 3 channels and an optional alpha, in either the legacy
 * comma syntax or the modern space syntax with "/ alpha".
 */
static bool parse_channels(const char **p, const float scale[3],
		float ch[3], float *alpha)
{
	int i;

	*alpha = 1.0f;
	for (i = 0; i < 3; i++) {
		if (!parse_channel(p, scale[i], &ch[i]))
			return false;
		skip_ws(p);
		if (**p == ',')
			(*p)++;
	}
	skip_ws(p);
	if (**p == '/' || **p == ',') {
		(*p)++;
		if (!parse_channel(p, 1.0f, alpha))
			return false;
		skip_ws(p);
	} else if (isdigit((unsigned char)**p) || **p == '.') {
		/* legacy rgba(r, g, b, a) handled by the comma above */
		if (!parse_channel(p, 1.0f, alpha))
			return false;
		skip_ws(p);
	}
	if (**p != ')')
		return false;
	(*p)++;
	*alpha = clampf(*alpha, 0.0f, 1.0f);
	return true;
}

static float hue2rgb(float p, float q, float t)
{
	if (t < 0) t += 1;
	if (t > 1) t -= 1;
	if (t < 1.0f / 6) return p + (q - p) * 6 * t;
	if (t < 1.0f / 2) return q;
	if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
	return p;
}

static float srgb_gamma(float v)
{
	v = clampf(v, 0.0f, 1.0f);
	return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
}

/** OKLab to sRGB (0..255) */
static void oklab_to_rgb(float L, float a, float b, float rgb[3])
{
	float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
	float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
	float s_ = L - 0.0894841775f * a - 1.2914855480f * b;
	float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;

	rgb[0] = 255 * srgb_gamma(+4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
	rgb[1] = 255 * srgb_gamma(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
	rgb[2] = 255 * srgb_gamma(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
}

/** CIE Lab (D50) to sRGB (0..255) */
static void lab_to_rgb(float L, float a, float b, float rgb[3])
{
	float fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
	float e = 216.0f / 24389, k = 24389.0f / 27;
	float x = (fx * fx * fx > e ? fx * fx * fx : (116 * fx - 16) / k) * 0.3457f / 0.3585f;
	float y = L > k * e ? fy * fy * fy : L / k;
	float z = (fz * fz * fz > e ? fz * fz * fz : (116 * fz - 16) / k) * (1 - 0.3457f - 0.3585f) / 0.3585f;
	/* D50 -> D65 (Bradford) */
	float X = 0.9554734527f * x - 0.0230985369f * y + 0.0632593086f * z;
	float Y = -0.0283697094f * x + 1.0099954580f * y + 0.0210413990f * z;
	float Z = 0.0123140016f * x - 0.0205076964f * y + 1.3303659366f * z;

	rgb[0] = 255 * srgb_gamma(3.2409699419f * X - 1.5373831776f * Y - 0.4986107603f * Z);
	rgb[1] = 255 * srgb_gamma(-0.9692436363f * X + 1.8759675015f * Y + 0.0415550574f * Z);
	rgb[2] = 255 * srgb_gamma(0.0556300797f * X - 0.2039769589f * Y + 1.0569715142f * Z);
}

/**
 * Parse a colour to 8-bit RGBA.
 *
 * \param current currentColor as ARGB, or 0 for black
 */
static bool parse_rgba(const char **p, uint32_t current, int *r, int *g,
		int *b, int *a)
{
	const char *s = *p;
	float ch[3], alpha;
	size_t i;

	skip_ws(&s);

	if (*s == '#') {
		int n = 0, v[8];
		s++;
		while (n < 8 && hexval(s[n]) >= 0) {
			v[n] = hexval(s[n]);
			n++;
		}
		if (isalnum((unsigned char)s[n]))
			return false;
		switch (n) {
		case 3: case 4:
			*r = v[0] * 17; *g = v[1] * 17; *b = v[2] * 17;
			*a = n == 4 ? v[3] * 17 : 255;
			break;
		case 6: case 8:
			*r = v[0] * 16 + v[1]; *g = v[2] * 16 + v[3];
			*b = v[4] * 16 + v[5];
			*a = n == 8 ? v[6] * 16 + v[7] : 255;
			break;
		default:
			return false;
		}
		*p = s + n;
		return true;
	}

	if (match_fn(&s, "rgb") || match_fn(&s, "rgba")) {
		static const float sc[3] = { 255, 255, 255 };
		if (!parse_channels(&s, sc, ch, &alpha))
			return false;
		*r = clampf(ch[0], 0, 255) + 0.5f;
		*g = clampf(ch[1], 0, 255) + 0.5f;
		*b = clampf(ch[2], 0, 255) + 0.5f;
		*a = alpha * 255 + 0.5f;
		*p = s;
		return true;
	}

	if (match_fn(&s, "hsl") || match_fn(&s, "hsla")) {
		static const float sc[3] = { 360, 100, 100 };
		float h, sat, l, q, pp;
		if (!parse_channels(&s, sc, ch, &alpha))
			return false;
		h = fmodf(ch[0], 360.0f) / 360.0f;
		if (h < 0)
			h += 1;
		sat = clampf(ch[1] / 100.0f, 0, 1);
		l = clampf(ch[2] / 100.0f, 0, 1);
		q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat;
		pp = 2 * l - q;
		*r = 255 * hue2rgb(pp, q, h + 1.0f / 3) + 0.5f;
		*g = 255 * hue2rgb(pp, q, h) + 0.5f;
		*b = 255 * hue2rgb(pp, q, h - 1.0f / 3) + 0.5f;
		*a = alpha * 255 + 0.5f;
		*p = s;
		return true;
	}

	if (match_fn(&s, "hwb")) {
		static const float sc[3] = { 360, 100, 100 };
		float h, w, bl, rgb[3];
		int k;
		if (!parse_channels(&s, sc, ch, &alpha))
			return false;
		h = fmodf(ch[0], 360.0f) / 360.0f;
		if (h < 0)
			h += 1;
		w = clampf(ch[1] / 100.0f, 0, 1);
		bl = clampf(ch[2] / 100.0f, 0, 1);
		if (w + bl > 1) {
			w /= (w + bl);
			bl = 1 - w;
		}
		rgb[0] = hue2rgb(0, 1, h + 1.0f / 3);
		rgb[1] = hue2rgb(0, 1, h);
		rgb[2] = hue2rgb(0, 1, h - 1.0f / 3);
		for (k = 0; k < 3; k++)
			rgb[k] = 255 * (rgb[k] * (1 - w - bl) + w);
		*r = rgb[0] + 0.5f; *g = rgb[1] + 0.5f; *b = rgb[2] + 0.5f;
		*a = alpha * 255 + 0.5f;
		*p = s;
		return true;
	}

	{
		bool ok = false, polar = false, lab = false;
		float sc[3], rgb[3], L, A, B;

		if (match_fn(&s, "oklch")) { ok = true; polar = true; lab = true; }
		else if (match_fn(&s, "oklab")) { ok = true; lab = true; }
		else if (match_fn(&s, "lch")) { polar = true; lab = true; }
		else if (match_fn(&s, "lab")) { lab = true; }
		if (!lab)
			goto not_lab;
		sc[0] = ok ? 1.0f : 100.0f;
		sc[1] = ok ? (polar ? 0.4f : 0.4f) : (polar ? 150.0f : 125.0f);
		sc[2] = ok ? (polar ? 360.0f : 0.4f) : (polar ? 360.0f : 125.0f);
		if (!parse_channels(&s, sc, ch, &alpha))
			return false;
		L = ch[0];
		if (polar) {
			float hr = ch[2] * M_PI / 180.0f;
			A = ch[1] * cosf(hr);
			B = ch[1] * sinf(hr);
		} else {
			A = ch[1];
			B = ch[2];
		}
		if (ok)
			oklab_to_rgb(L, A, B, rgb);
		else
			lab_to_rgb(L, A, B, rgb);
		*r = rgb[0] + 0.5f; *g = rgb[1] + 0.5f; *b = rgb[2] + 0.5f;
		*a = alpha * 255 + 0.5f;
		*p = s;
		return true;
	}
not_lab:

	if (match_fn(&s, "color")) {
		/* color(srgb r g b / a) and friends: treat as sRGB */
		static const float sc[3] = { 1, 1, 1 };
		skip_ws(&s);
		while (isalnum((unsigned char)*s) || *s == '-')
			s++;
		if (!parse_channels(&s, sc, ch, &alpha))
			return false;
		*r = clampf(ch[0], 0, 1) * 255 + 0.5f;
		*g = clampf(ch[1], 0, 1) * 255 + 0.5f;
		*b = clampf(ch[2], 0, 1) * 255 + 0.5f;
		*a = alpha * 255 + 0.5f;
		*p = s;
		return true;
	}

	if (match_fn(&s, "color-mix")) {
		/* color-mix(in <space>, c1 [p1], c2 [p2]) mixed in sRGB */
		int r1, g1, b1, a1, r2, g2, b2, a2;
		float p1 = -1, p2 = -1, f;
		skip_ws(&s);
		while (*s != '\0' && *s != ',')
			s++;
		if (*s != ',')
			return false;
		s++;
		if (!parse_rgba(&s, current, &r1, &g1, &b1, &a1))
			return false;
		skip_ws(&s);
		if (parse_number(&s, &p1) && *s == '%')
			s++;
		skip_ws(&s);
		if (*s != ',')
			return false;
		s++;
		if (!parse_rgba(&s, current, &r2, &g2, &b2, &a2))
			return false;
		skip_ws(&s);
		if (parse_number(&s, &p2) && *s == '%')
			s++;
		skip_ws(&s);
		if (*s != ')')
			return false;
		s++;
		if (p1 < 0 && p2 < 0) { p1 = 50; p2 = 50; }
		else if (p1 < 0) p1 = 100 - p2;
		else if (p2 < 0) p2 = 100 - p1;
		f = (p1 + p2) > 0 ? p2 / (p1 + p2) : 0.5f;
		*r = r1 + (r2 - r1) * f;
		*g = g1 + (g2 - g1) * f;
		*b = b1 + (b2 - b1) * f;
		*a = a1 + (a2 - a1) * f;
		if (p1 + p2 < 100)
			*a = *a * (p1 + p2) / 100;
		*p = s;
		return true;
	}

	if (match_word(&s, "transparent")) {
		*r = *g = *b = *a = 0;
		*p = s;
		return true;
	}
	if (match_word(&s, "currentcolor")) {
		*a = (current >> 24) & 0xff;
		*r = (current >> 16) & 0xff;
		*g = (current >> 8) & 0xff;
		*b = current & 0xff;
		*p = s;
		return true;
	}

	for (i = 0; i < sizeof(named_colours) / sizeof(named_colours[0]); i++) {
		const char *t = s;
		if (match_word(&t, named_colours[i].name)) {
			*r = (named_colours[i].rgb >> 16) & 0xff;
			*g = (named_colours[i].rgb >> 8) & 0xff;
			*b = named_colours[i].rgb & 0xff;
			*a = 255;
			*p = t;
			return true;
		}
	}
	return false;
}

/* exported interface documented in css_fx.h */
bool cssfx_colour(const char **p, const css_computed_style *style,
		colour *out)
{
	css_color current = 0xff000000;
	int r, g, b, a;

	if (style != NULL)
		css_computed_color(style, &current);
	if (!parse_rgba(p, current, &r, &g, &b, &a))
		return false;
	*out = ((uint32_t)(255 - a) << 24) | ((uint32_t)b << 16) |
			((uint32_t)g << 8) | (uint32_t)r;
	return true;
}

/* exported interface documented in css_fx.h */
bool cssfx_parse_css_colour(const char *text, css_color *out)
{
	int r, g, b, a;
	const char *p = text;

	if (!parse_rgba(&p, 0xff000000, &r, &g, &b, &a))
		return false;
	*out = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
			((uint32_t)g << 8) | (uint32_t)b;
	return true;
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
