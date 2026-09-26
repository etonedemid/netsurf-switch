/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * Colour parsing from text, covering modern CSS Color 4/5 syntax:
 * hex (3/4/6/8 digits), rgb()/hsl() in legacy and space syntax, hwb(),
 * lab(), lch(), oklab(), oklch(), color() and color-mix(), named colours,
 * transparent and currentColor.
 */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libcss/libcss.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* exported interface documented in libcss/stylesheet.h */
bool css_parse_color_text(const char **text, css_color current,
		css_color *out)
{
	int r, g, b, a;

	if (!parse_rgba(text, current, &r, &g, &b, &a))
		return false;
	if (r < 0) r = 0;
	if (r > 255) r = 255;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (b < 0) b = 0;
	if (b > 255) b = 255;
	if (a < 0) a = 0;
	if (a > 255) a = 255;
	*out = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
			((uint32_t)g << 8) | (uint32_t)b;
	return true;
}
