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
 * CSS grid layout.
 *
 * Grid containers use the flex container box types (their children are
 * normalised the same way); layout_flex() dispatches here when the
 * container's display is grid or inline-grid.
 *
 * Supported: grid-template-columns/rows with px/%/em/fr/auto/
 * min-content/max-content/minmax()/fit-content()/repeat() including
 * auto-fill and auto-fit, grid-template-areas, line-number, span and
 * area-name placement, row/column auto-placement (with dense), gaps,
 * grid-auto-rows/columns, justify/align-items/-self and
 * justify/align-content.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "utils/log.h"
#include "utils/utils.h"
#include "netsurf/content.h"

#include "html/html.h"
#include "html/private.h"
#include "html/box.h"
#include "html/layout.h"
#include "html/layout_internal.h"
#include "css/css_fx.h"

#define GRID_MAX_TRACKS 256
#define GRID_MAX_AREAS 64

/** Track sizing function kinds */
enum grid_size_kind {
	GS_FIXED,	/**< a definite length */
	GS_AUTO,	/**< auto */
	GS_MIN_CONTENT,	/**< min-content */
	GS_MAX_CONTENT,	/**< max-content */
	GS_FR,		/**< flexible */
};

/** A track sizing function: minmax(min, max) */
struct grid_track_def {
	enum grid_size_kind min_kind, max_kind;
	float min_val, max_val; /* px for GS_FIXED, factor for GS_FR */
	int fit_content; /* >= 0: fit-content(limit) */
};

/** A resolved track */
struct grid_track {
	struct grid_track_def def;
	int base;	/**< base size */
	int limit;	/**< growth limit (-1 = infinite) */
	int pos;	/**< offset from content edge */
	int size;	/**< final size */
};

/** A named grid area from grid-template-areas */
struct grid_area {
	char name[32];
	int r0, c0, r1, c1; /* 0-based, end exclusive */
};

/** Grid item placement */
struct grid_item {
	struct box *box;
	int r0, r1, c0, c1; /* 0-based lines, end exclusive */
	bool placed_r, placed_c;
	int order;
};

struct grid_ctx {
	html_content *content;
	const css_unit_ctx *uctx;
	struct box *grid;

	struct grid_track_def col_defs[GRID_MAX_TRACKS];
	int ncol_defs;
	struct grid_track_def row_defs[GRID_MAX_TRACKS];
	int nrow_defs;
	struct grid_track_def auto_col, auto_row;
	bool auto_fit_cols;

	struct grid_area areas[GRID_MAX_AREAS];
	int nareas;
	int area_rows, area_cols;

	struct grid_item *items;
	int nitems;

	int ncols, nrows;
	struct grid_track *cols, *rows;

	int col_gap, row_gap;
	bool flow_column, dense;
};

/* ------------------------------------------------------------------ */
/* Parsing */

static void g_skip_ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n')
		(*p)++;
}

static bool g_word(const char **p, const char *w)
{
	size_t n = strlen(w);
	char c;
	if (strncasecmp(*p, w, n) != 0)
		return false;
	c = (*p)[n];
	if (isalnum((unsigned char)c) || c == '-' || c == '_')
		return false;
	*p += n;
	return true;
}

static bool g_fn(const char **p, const char *w)
{
	size_t n = strlen(w);
	if (strncasecmp(*p, w, n) != 0 || (*p)[n] != '(')
		return false;
	*p += n + 1;
	return true;
}

/** parse a single sizing value (not minmax) */
static bool g_parse_breadth(const char **p, const struct grid_ctx *g,
		float pct_base, enum grid_size_kind *kind, float *val)
{
	const char *s = *p;
	float v;
	char *end;

	g_skip_ws(&s);
	if (g_word(&s, "auto")) {
		*kind = GS_AUTO;
	} else if (g_word(&s, "min-content")) {
		*kind = GS_MIN_CONTENT;
	} else if (g_word(&s, "max-content")) {
		*kind = GS_MAX_CONTENT;
	} else {
		v = strtof(s, &end);
		if (end != s && strncasecmp(end, "fr", 2) == 0 &&
				!isalpha((unsigned char)end[2])) {
			*kind = GS_FR;
			*val = v;
			s = end + 2;
		} else if (pct_base < 0 && end != s && *end == '%') {
			/* percentage of an indefinite size: auto */
			*kind = GS_AUTO;
			s = end + 1;
		} else if (cssfx_length(&s, g->grid->style, g->uctx,
				pct_base < 0 ? 0 : pct_base, &v)) {
			*kind = GS_FIXED;
			*val = v;
		} else {
			return false;
		}
	}
	*p = s;
	return true;
}

/** parse a track size: breadth | minmax() | fit-content() */
static bool g_parse_track_size(const char **p, const struct grid_ctx *g,
		float pct_base, struct grid_track_def *d)
{
	const char *s = *p;

	memset(d, 0, sizeof(*d));
	d->fit_content = -1;
	g_skip_ws(&s);

	if (g_fn(&s, "minmax")) {
		if (!g_parse_breadth(&s, g, pct_base, &d->min_kind,
				&d->min_val))
			return false;
		g_skip_ws(&s);
		if (*s != ',')
			return false;
		s++;
		if (!g_parse_breadth(&s, g, pct_base, &d->max_kind,
				&d->max_val))
			return false;
		g_skip_ws(&s);
		if (*s != ')')
			return false;
		s++;
		/* a flexible minimum is invalid; treat as auto */
		if (d->min_kind == GS_FR)
			d->min_kind = GS_AUTO;
	} else if (g_fn(&s, "fit-content")) {
		float v;
		if (!cssfx_length(&s, g->grid->style, g->uctx,
				pct_base < 0 ? 0 : pct_base, &v))
			return false;
		g_skip_ws(&s);
		if (*s == ')')
			s++;
		d->min_kind = GS_AUTO;
		d->max_kind = GS_MAX_CONTENT;
		d->fit_content = (int)v;
	} else {
		enum grid_size_kind k;
		float v = 0;
		if (!g_parse_breadth(&s, g, pct_base, &k, &v))
			return false;
		d->max_kind = k;
		d->max_val = v;
		if (k == GS_FR) {
			d->min_kind = GS_AUTO;
		} else {
			d->min_kind = k;
			d->min_val = v;
		}
	}
	*p = s;
	return true;
}

/** skip a [line-names] group */
static void g_skip_names(const char **p)
{
	g_skip_ws(p);
	while (**p == '[') {
		while (**p != '\0' && **p != ']')
			(*p)++;
		if (**p == ']')
			(*p)++;
		g_skip_ws(p);
	}
}

/**
 * Parse a track list.
 *
 * \param avail  available size for auto-fill/auto-fit (-1 if unknown)
 * \param gap    gap between tracks
 * \return number of tracks
 */
static int g_parse_track_list(const char *text, const struct grid_ctx *g,
		float pct_base, int avail, int gap,
		struct grid_track_def *out, int max, bool *auto_fit)
{
	const char *p = text;
	int n = 0;

	if (text == NULL)
		return 0;
	g_skip_ws(&p);
	if (g_word(&p, "none") || g_word(&p, "subgrid") ||
			g_word(&p, "masonry"))
		return 0;

	while (*p != '\0' && n < max) {
		g_skip_names(&p);
		if (*p == '\0')
			break;

		if (g_fn(&p, "repeat")) {
			struct grid_track_def defs[32];
			int ndefs = 0, count = 1, i, k;
			bool fill = false, fit = false;
			char *end;

			g_skip_ws(&p);
			if (g_word(&p, "auto-fill")) {
				fill = true;
			} else if (g_word(&p, "auto-fit")) {
				fill = true;
				fit = true;
			} else {
				count = strtol(p, &end, 10);
				if (end == p || count < 1)
					return n;
				p = end;
			}
			g_skip_ws(&p);
			if (*p != ',')
				return n;
			p++;
			while (ndefs < 32) {
				g_skip_names(&p);
				if (*p == ')' || *p == '\0')
					break;
				if (!g_parse_track_size(&p, g, pct_base,
						&defs[ndefs]))
					return n;
				ndefs++;
			}
			if (*p == ')')
				p++;
			if (ndefs == 0)
				continue;
			if (fill) {
				/* as many repetitions as fit */
				int one = 0, fixed_other = 0;
				for (i = 0; i < ndefs; i++) {
					const struct grid_track_def *d = &defs[i];
					int sz = 0;
					if (d->max_kind == GS_FIXED)
						sz = d->max_val;
					if (d->min_kind == GS_FIXED &&
					    (d->max_kind != GS_FIXED ||
					     d->min_val > sz))
						sz = d->min_val;
					if (sz <= 0)
						sz = 1;
					one += sz;
				}
				one += gap * ndefs;
				/* space used by the other tracks is not
				 * known yet; assume none */
				(void)fixed_other;
				count = (avail > 0 && one > 0) ?
						(avail + gap) / one : 1;
				if (count < 1)
					count = 1;
				if (auto_fit != NULL && fit)
					*auto_fit = true;
			}
			for (k = 0; k < count && n < max; k++) {
				for (i = 0; i < ndefs && n < max; i++)
					out[n++] = defs[i];
			}
			continue;
		}

		if (!g_parse_track_size(&p, g, pct_base, &out[n]))
			return n;
		n++;
	}
	return n;
}

/** parse grid-template-areas */
static void g_parse_areas(struct grid_ctx *g, const char *text)
{
	const char *p = text;
	int row = 0;

	g->nareas = 0;
	g->area_rows = g->area_cols = 0;
	if (text == NULL)
		return;

	while (*p != '\0') {
		int col = 0;
		g_skip_ws(&p);
		if (*p != '"' && *p != '\'')
			break;
		{
			char q = *p++;
			while (*p != '\0' && *p != q) {
				char name[32];
				int k = 0, a;
				while (*p == ' ' || *p == '\t')
					p++;
				if (*p == q || *p == '\0')
					break;
				while (*p != '\0' && *p != q && *p != ' ' &&
						*p != '\t') {
					if (k < 31)
						name[k++] = *p;
					p++;
				}
				name[k] = '\0';
				if (name[0] != '.' && k > 0) {
					for (a = 0; a < g->nareas; a++) {
						if (strcmp(g->areas[a].name,
								name) == 0)
							break;
					}
					if (a == g->nareas &&
					    g->nareas < GRID_MAX_AREAS) {
						strcpy(g->areas[a].name, name);
						g->areas[a].r0 = row;
						g->areas[a].c0 = col;
						g->areas[a].r1 = row + 1;
						g->areas[a].c1 = col + 1;
						g->nareas++;
					} else if (a < g->nareas) {
						if (row + 1 > g->areas[a].r1)
							g->areas[a].r1 = row + 1;
						if (col + 1 > g->areas[a].c1)
							g->areas[a].c1 = col + 1;
					}
				}
				col++;
			}
			if (*p == q)
				p++;
		}
		if (col > g->area_cols)
			g->area_cols = col;
		row++;
	}
	g->area_rows = row;
}

/**
 * Resolve one grid-*-start/end value.
 *
 * \param line  set to the 0-based line for a definite position
 * \param span  set to the span for "span N"
 * \return 0 auto, 1 definite line, 2 span
 */
static int g_parse_line(const struct grid_ctx *g, const char *text,
		bool row, bool start, int explicit_tracks, int *line,
		int *span)
{
	const char *p = text;
	char *end;
	long v;
	int a;

	*span = 1;
	if (text == NULL)
		return 0;
	g_skip_ws(&p);
	if (g_word(&p, "auto"))
		return 0;
	if (g_word(&p, "span")) {
		g_skip_ws(&p);
		v = strtol(p, &end, 10);
		*span = (end != p && v > 0) ? v : 1;
		return 2;
	}
	v = strtol(p, &end, 10);
	if (end != p && v != 0) {
		/* line numbers are 1-based; negatives count from the end */
		if (v > 0)
			*line = v - 1;
		else
			*line = explicit_tracks + 1 + v;
		if (*line < 0)
			*line = 0;
		return 1;
	}
	/* named area / line: "name", "name-start", "name-end" */
	for (a = 0; a < g->nareas; a++) {
		size_t n = strlen(g->areas[a].name);
		bool match_start = strncmp(p, g->areas[a].name, n) == 0 &&
				(p[n] == '\0' || p[n] == ' ' ||
				 strncmp(p + n, "-start", 6) == 0 ||
				 strncmp(p + n, "-end", 4) == 0);
		if (!match_start)
			continue;
		if (strncmp(p + n, "-end", 4) == 0)
			start = false;
		else if (strncmp(p + n, "-start", 6) == 0)
			start = true;
		if (row)
			*line = start ? g->areas[a].r0 : g->areas[a].r1;
		else
			*line = start ? g->areas[a].c0 : g->areas[a].c1;
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */

static int g_item_cmp(const void *a, const void *b)
{
	const struct grid_item *x = a, *y = b;
	if (x->order != y->order)
		return x->order - y->order;
	/* stable: keep document order */
	return (x < y) ? -1 : 1;
}

/** occupancy grid */
struct g_occ {
	unsigned char *cell;
	int rows, cols;
};

static bool g_occ_ensure(struct g_occ *o, int rows)
{
	unsigned char *c;
	if (rows <= o->rows)
		return true;
	rows = rows + 8;
	c = realloc(o->cell, (size_t)rows * o->cols);
	if (c == NULL)
		return false;
	memset(c + (size_t)o->rows * o->cols, 0,
			(size_t)(rows - o->rows) * o->cols);
	o->cell = c;
	o->rows = rows;
	return true;
}

static bool g_occ_free(struct g_occ *o, int r0, int r1, int c0, int c1)
{
	int r, c;
	if (c1 > o->cols || c0 < 0)
		return false;
	if (!g_occ_ensure(o, r1))
		return false;
	for (r = r0; r < r1; r++)
		for (c = c0; c < c1; c++)
			if (o->cell[(size_t)r * o->cols + c])
				return false;
	return true;
}

static void g_occ_mark(struct g_occ *o, int r0, int r1, int c0, int c1)
{
	int r, c;
	if (!g_occ_ensure(o, r1))
		return;
	for (r = r0; r < r1; r++)
		for (c = c0; c < c1 && c < o->cols; c++)
			o->cell[(size_t)r * o->cols + c] = 1;
}

/**
 * Parse the container's templates and place all items.
 *
 * \param avail_width content width of the container, or -1
 */
static bool g_setup(struct grid_ctx *g, struct box *grid,
		html_content *content, int avail_width)
{
	const css_computed_style *style = grid->style;
	const char *t;
	struct box *b;
	int i, explicit_cols, explicit_rows;
	struct g_occ occ = { NULL, 0, 0 };
	css_fixed gap_len = 0;
	css_unit gap_unit = CSS_UNIT_PX;
	int auto_cursor_r = 0, auto_cursor_c = 0;

	memset(g, 0, sizeof(*g));
	g->content = content;
	g->uctx = &content->unit_len_ctx;
	g->grid = grid;

	/* gaps */
	if (css_computed_column_gap(style, &gap_len, &gap_unit) ==
			CSS_COLUMN_GAP_SET) {
		if (gap_unit == CSS_UNIT_PCT)
			g->col_gap = avail_width > 0 ?
				FPCT_OF_INT_TOINT(gap_len, avail_width) : 0;
		else
			g->col_gap = FIXTOINT(css_unit_len2device_px(style,
					g->uctx, gap_len, gap_unit));
	}
	t = cssfx_raw(style, CSS_PROP_ROW_GAP);
	if (t != NULL) {
		float v;
		if (strncasecmp(t, "normal", 6) != 0 &&
				cssfx_length(&t, style, g->uctx, 0, &v))
			g->row_gap = v;
	}
	if (g->col_gap < 0) g->col_gap = 0;
	if (g->row_gap < 0) g->row_gap = 0;

	t = cssfx_raw(style, CSS_PROP_GRID_AUTO_FLOW);
	if (t != NULL) {
		g->flow_column = strstr(t, "column") != NULL;
		g->dense = strstr(t, "dense") != NULL;
	}

	g_parse_areas(g, cssfx_raw(style, CSS_PROP_GRID_TEMPLATE_AREAS));

	g->ncol_defs = g_parse_track_list(
			cssfx_raw(style, CSS_PROP_GRID_TEMPLATE_COLUMNS), g,
			avail_width, avail_width, g->col_gap, g->col_defs,
			GRID_MAX_TRACKS, &g->auto_fit_cols);
	g->nrow_defs = g_parse_track_list(
			cssfx_raw(style, CSS_PROP_GRID_TEMPLATE_ROWS), g,
			-1, -1, g->row_gap, g->row_defs, GRID_MAX_TRACKS, NULL);

	/* implicit track sizes */
	memset(&g->auto_col, 0, sizeof(g->auto_col));
	g->auto_col.min_kind = g->auto_col.max_kind = GS_AUTO;
	g->auto_col.fit_content = -1;
	g->auto_row = g->auto_col;
	t = cssfx_raw(style, CSS_PROP_GRID_AUTO_COLUMNS);
	if (t != NULL)
		g_parse_track_size(&t, g, avail_width, &g->auto_col);
	t = cssfx_raw(style, CSS_PROP_GRID_AUTO_ROWS);
	if (t != NULL)
		g_parse_track_size(&t, g, -1, &g->auto_row);

	explicit_cols = g->ncol_defs > g->area_cols ?
			g->ncol_defs : g->area_cols;
	explicit_rows = g->nrow_defs > g->area_rows ?
			g->nrow_defs : g->area_rows;

	/* items */
	for (b = grid->children; b != NULL; b = b->next)
		g->nitems++;
	g->items = calloc(g->nitems ? g->nitems : 1, sizeof(struct grid_item));
	if (g->items == NULL)
		return false;
	i = 0;
	for (b = grid->children; b != NULL; b = b->next) {
		struct grid_item *it = &g->items[i++];
		int32_t order = 0;
		it->box = b;
		if (b->style != NULL)
			css_computed_order(b->style, &order);
		it->order = order;
	}
	qsort(g->items, g->nitems, sizeof(struct grid_item), g_item_cmp);

	/* the column count for auto-placement */
	g->ncols = explicit_cols > 0 ? explicit_cols : 1;

	/* resolve definite positions first, growing the grid as needed */
	for (i = 0; i < g->nitems; i++) {
		struct grid_item *it = &g->items[i];
		const css_computed_style *s = it->box->style;
		int ls, le, ss, se, ks, ke;

		if (s == NULL || lh__box_is_absolute(it->box))
			continue;

		ks = g_parse_line(g, cssfx_raw(s, CSS_PROP_GRID_COLUMN_START),
				false, true, explicit_cols, &ls, &ss);
		ke = g_parse_line(g, cssfx_raw(s, CSS_PROP_GRID_COLUMN_END),
				false, false, explicit_cols, &le, &se);
		if (ks == 1 && ke == 1) {
			it->c0 = ls < le ? ls : le;
			it->c1 = ls < le ? le : ls;
			if (it->c1 == it->c0)
				it->c1 = it->c0 + 1;
			it->placed_c = true;
		} else if (ks == 1) {
			it->c0 = ls;
			it->c1 = ls + (ke == 2 ? se : 1);
			it->placed_c = true;
		} else if (ke == 1) {
			int span = ks == 2 ? ss : 1;
			it->c1 = le;
			it->c0 = le - span < 0 ? 0 : le - span;
			if (it->c1 <= it->c0)
				it->c1 = it->c0 + 1;
			it->placed_c = true;
		} else {
			it->c0 = 0;
			it->c1 = ks == 2 ? ss : (ke == 2 ? se : 1);
		}

		ks = g_parse_line(g, cssfx_raw(s, CSS_PROP_GRID_ROW_START),
				true, true, explicit_rows, &ls, &ss);
		ke = g_parse_line(g, cssfx_raw(s, CSS_PROP_GRID_ROW_END),
				true, false, explicit_rows, &le, &se);
		if (ks == 1 && ke == 1) {
			it->r0 = ls < le ? ls : le;
			it->r1 = ls < le ? le : ls;
			if (it->r1 == it->r0)
				it->r1 = it->r0 + 1;
			it->placed_r = true;
		} else if (ks == 1) {
			it->r0 = ls;
			it->r1 = ls + (ke == 2 ? se : 1);
			it->placed_r = true;
		} else if (ke == 1) {
			int span = ks == 2 ? ss : 1;
			it->r1 = le;
			it->r0 = le - span < 0 ? 0 : le - span;
			if (it->r1 <= it->r0)
				it->r1 = it->r0 + 1;
			it->placed_r = true;
		} else {
			it->r0 = 0;
			it->r1 = ks == 2 ? ss : (ke == 2 ? se : 1);
		}

		if (it->c1 > GRID_MAX_TRACKS)
			it->c1 = GRID_MAX_TRACKS;
		if (it->c0 >= it->c1)
			it->c0 = it->c1 - 1;
		if (it->r1 > GRID_MAX_TRACKS * 8)
			it->r1 = GRID_MAX_TRACKS * 8;
		if (it->r0 >= it->r1)
			it->r0 = it->r1 - 1;

		if (it->placed_c && it->c1 > g->ncols)
			g->ncols = it->c1;
		if (!it->placed_c && it->c1 - it->c0 > g->ncols)
			g->ncols = it->c1 - it->c0;
	}

	if (g->flow_column) {
		/* column flow: transpose the problem by treating rows as
		 * the fixed dimension */
		int nrows = explicit_rows > 0 ? explicit_rows : 1;
		for (i = 0; i < g->nitems; i++) {
			struct grid_item *it = &g->items[i];
			if (it->placed_r && it->r1 > nrows)
				nrows = it->r1;
		}
		occ.cols = nrows; /* occupancy in (column, row) order */
		for (i = 0; i < g->nitems; i++) {
			struct grid_item *it = &g->items[i];
			if (it->box->style == NULL ||
					lh__box_is_absolute(it->box))
				continue;
			if (it->placed_c && it->placed_r)
				g_occ_mark(&occ, it->c0, it->c1,
						it->r0, it->r1);
		}
		for (i = 0; i < g->nitems; i++) {
			struct grid_item *it = &g->items[i];
			int span_r = it->r1 - it->r0;
			int span_c = it->c1 - it->c0;
			int c, r;
			if (it->box->style == NULL ||
					lh__box_is_absolute(it->box) ||
					(it->placed_c && it->placed_r))
				continue;
			for (c = g->dense ? 0 : auto_cursor_c;; c++) {
				bool found = false;
				for (r = 0; r + span_r <= occ.cols; r++) {
					if (it->placed_r && r != it->r0)
						continue;
					if (g_occ_free(&occ, c, c + span_c,
							r, r + span_r)) {
						found = true;
						break;
					}
				}
				if (found) {
					it->c0 = c;
					it->c1 = c + span_c;
					it->r0 = r;
					it->r1 = r + span_r;
					g_occ_mark(&occ, c, c + span_c,
							r, r + span_r);
					auto_cursor_c = c;
					break;
				}
				if (c > 4096)
					break;
			}
		}
		g->nrows = nrows;
		for (i = 0; i < g->nitems; i++) {
			if (g->items[i].c1 > g->ncols)
				g->ncols = g->items[i].c1;
		}
	} else {
		occ.cols = g->ncols;
		/* items with both positions fixed, then row-locked ones */
		for (i = 0; i < g->nitems; i++) {
			struct grid_item *it = &g->items[i];
			if (it->box->style == NULL ||
					lh__box_is_absolute(it->box))
				continue;
			if (it->placed_c && it->placed_r)
				g_occ_mark(&occ, it->r0, it->r1,
						it->c0, it->c1);
		}
		for (i = 0; i < g->nitems; i++) {
			struct grid_item *it = &g->items[i];
			int span_c = it->c1 - it->c0;
			int c;
			if (it->box->style == NULL ||
					lh__box_is_absolute(it->box) ||
					!it->placed_r || it->placed_c)
				continue;
			for (c = 0; c + span_c <= g->ncols; c++) {
				if (g_occ_free(&occ, it->r0, it->r1,
						c, c + span_c))
					break;
			}
			if (c + span_c > g->ncols)
				c = 0;
			it->c0 = c;
			it->c1 = c + span_c;
			g_occ_mark(&occ, it->r0, it->r1, it->c0, it->c1);
		}
		/* auto-placed items */
		for (i = 0; i < g->nitems; i++) {
			struct grid_item *it = &g->items[i];
			int span_r = it->r1 - it->r0;
			int span_c = it->c1 - it->c0;
			int r, c;
			if (it->box->style == NULL ||
					lh__box_is_absolute(it->box) ||
					it->placed_r)
				continue;
			if (span_c > g->ncols)
				span_c = g->ncols;
			r = g->dense ? 0 : auto_cursor_r;
			c = g->dense ? 0 : auto_cursor_c;
			for (;; r++, c = 0) {
				bool found = false;
				if (it->placed_c) {
					if (g_occ_free(&occ, r, r + span_r,
							it->c0, it->c1) &&
					    (g->dense || r > auto_cursor_r ||
					     it->c0 >= auto_cursor_c))
						found = true;
					c = it->c0;
				} else {
					for (; c + span_c <= g->ncols; c++) {
						if (g_occ_free(&occ, r,
							r + span_r, c,
							c + span_c)) {
							found = true;
							break;
						}
					}
				}
				if (found)
					break;
				if (r > 100000)
					break;
			}
			it->r0 = r;
			it->r1 = r + span_r;
			if (!it->placed_c) {
				it->c0 = c;
				it->c1 = c + span_c;
			}
			g_occ_mark(&occ, it->r0, it->r1, it->c0, it->c1);
			auto_cursor_r = r;
			auto_cursor_c = it->c1;
		}
		g->nrows = explicit_rows;
		for (i = 0; i < g->nitems; i++) {
			if (g->items[i].r1 > g->nrows)
				g->nrows = g->items[i].r1;
		}
	}
	free(occ.cell);

	if (g->nrows < 1)
		g->nrows = 1;
	if (g->ncols < 1)
		g->ncols = 1;
	if (g->ncols > GRID_MAX_TRACKS)
		g->ncols = GRID_MAX_TRACKS;

	/* auto-fit: collapse trailing empty repeated columns */
	if (g->auto_fit_cols && !g->flow_column) {
		int used = 0;
		for (i = 0; i < g->nitems; i++) {
			if (g->items[i].c1 > used)
				used = g->items[i].c1;
		}
		if (used > 0 && used < g->ncols && used >= 1)
			g->ncols = used;
	}

	g->cols = calloc(g->ncols, sizeof(struct grid_track));
	g->rows = calloc(g->nrows, sizeof(struct grid_track));
	if (g->cols == NULL || g->rows == NULL)
		return false;
	for (i = 0; i < g->ncols; i++)
		g->cols[i].def = i < g->ncol_defs ? g->col_defs[i] : g->auto_col;
	for (i = 0; i < g->nrows; i++)
		g->rows[i].def = i < g->nrow_defs ? g->row_defs[i] : g->auto_row;

	return true;
}

static void g_free(struct grid_ctx *g)
{
	free(g->items);
	free(g->cols);
	free(g->rows);
}

/**
 * Size tracks along one axis.
 *
 * \param tracks  tracks to size
 * \param n       number of tracks
 * \param avail   available space (content size), or -1 if indefinite
 * \param gap     gap between tracks
 * \param item_min/item_max  per-item min/max contributions (outer)
 * \param span_start/span_end  per-item track span
 */
static void g_size_tracks(struct grid_track *tracks, int n, int avail,
		int gap, int nitems, const int *item_min, const int *item_max,
		const int *span_start, const int *span_end, bool stretch)
{
	int i, k, free_space, total_fr_base;
	float total_fr = 0;

	/* initialise */
	for (i = 0; i < n; i++) {
		struct grid_track *t = &tracks[i];
		t->base = t->def.min_kind == GS_FIXED ? t->def.min_val : 0;
		if (t->def.max_kind == GS_FIXED)
			t->limit = t->def.max_val;
		else if (t->def.max_kind == GS_FR)
			t->limit = -1;
		else
			t->limit = -2; /* content-sized, filled below */
		if (t->def.max_kind == GS_FR)
			total_fr += t->def.max_val;
	}

	/* single-span items */
	for (k = 0; k < nitems; k++) {
		struct grid_track *t;
		if (span_start[k] < 0 || span_end[k] - span_start[k] != 1)
			continue;
		t = &tracks[span_start[k]];
		if (t->def.min_kind == GS_AUTO ||
				t->def.min_kind == GS_MIN_CONTENT) {
			if (t->base < item_min[k])
				t->base = item_min[k];
		} else if (t->def.min_kind == GS_MAX_CONTENT) {
			if (t->base < item_max[k])
				t->base = item_max[k];
		}
		if (t->def.max_kind == GS_AUTO ||
				t->def.max_kind == GS_MAX_CONTENT) {
			int lim = item_max[k];
			if (t->def.fit_content >= 0 && lim > t->def.fit_content)
				lim = t->def.fit_content;
			if (t->limit < lim)
				t->limit = lim;
		} else if (t->def.max_kind == GS_MIN_CONTENT) {
			if (t->limit < item_min[k])
				t->limit = item_min[k];
		}
	}

	/* spanning items: distribute excess over content-sized tracks */
	for (k = 0; k < nitems; k++) {
		int s0 = span_start[k], s1 = span_end[k];
		int have = 0, grow = 0, need;
		if (s0 < 0 || s1 - s0 < 2)
			continue;
		for (i = s0; i < s1 && i < n; i++) {
			have += tracks[i].base;
			if (tracks[i].def.min_kind != GS_FIXED &&
					tracks[i].def.max_kind != GS_FR)
				grow++;
		}
		have += gap * (s1 - s0 - 1);
		need = item_min[k] - have;
		if (need > 0 && grow > 0) {
			for (i = s0; i < s1 && i < n; i++) {
				if (tracks[i].def.min_kind != GS_FIXED &&
						tracks[i].def.max_kind != GS_FR)
					tracks[i].base += need / grow;
			}
		}
	}

	for (i = 0; i < n; i++) {
		struct grid_track *t = &tracks[i];
		if (t->limit == -2)
			t->limit = t->base;
		if (t->limit >= 0 && t->limit < t->base)
			t->limit = t->base;
		t->size = t->base;
	}

	if (avail < 0) {
		/* indefinite: content-sized tracks at their limits,
		 * flexible tracks at their max-content */
		for (i = 0; i < n; i++) {
			struct grid_track *t = &tracks[i];
			if (t->limit >= 0)
				t->size = t->limit;
		}
		return;
	}

	free_space = avail - gap * (n > 0 ? n - 1 : 0);
	for (i = 0; i < n; i++)
		free_space -= tracks[i].base;

	/* grow content-sized tracks towards their limits */
	if (free_space > 0) {
		int want = 0;
		for (i = 0; i < n; i++) {
			if (tracks[i].limit > tracks[i].base &&
					tracks[i].def.max_kind != GS_FR)
				want += tracks[i].limit - tracks[i].base;
		}
		if (want > 0) {
			float f = want > free_space ?
					(float)free_space / want : 1.0f;
			int used = 0;
			for (i = 0; i < n; i++) {
				struct grid_track *t = &tracks[i];
				if (t->limit > t->base &&
						t->def.max_kind != GS_FR) {
					int d = (t->limit - t->base) * f;
					t->size = t->base + d;
					used += d;
				}
			}
			free_space -= used;
		}
	}

	/* flexible tracks */
	if (total_fr > 0) {
		int space = avail - gap * (n > 0 ? n - 1 : 0);
		float fr_size;
		bool changed = true;
		bool *inflexible = calloc(n, sizeof(bool));

		if (inflexible == NULL)
			return;
		/* find the fr size, treating tracks whose base exceeds
		 * their share as inflexible */
		while (changed) {
			float frs = 0;
			int fixed = 0;
			changed = false;
			for (i = 0; i < n; i++) {
				if (tracks[i].def.max_kind == GS_FR &&
						!inflexible[i])
					frs += tracks[i].def.max_val;
				else
					fixed += tracks[i].size;
			}
			if (frs <= 0)
				break;
			fr_size = (space - fixed) / (frs < 1 ? 1 : frs);
			if (frs < 1)
				fr_size = (space - fixed) * frs / frs;
			for (i = 0; i < n; i++) {
				if (tracks[i].def.max_kind == GS_FR &&
						!inflexible[i] &&
						fr_size * tracks[i].def.max_val <
						tracks[i].base) {
					inflexible[i] = true;
					tracks[i].size = tracks[i].base;
					changed = true;
				}
			}
			if (!changed) {
				for (i = 0; i < n; i++) {
					if (tracks[i].def.max_kind == GS_FR &&
							!inflexible[i]) {
						int s = fr_size *
							tracks[i].def.max_val;
						if (frs < 1)
							s = (space - fixed) *
							tracks[i].def.max_val;
						tracks[i].size = s > 0 ? s : 0;
					}
				}
			}
		}
		free(inflexible);
		(void)total_fr_base;
		return;
	}

	/* stretch auto tracks into remaining space */
	if (stretch && free_space > 0) {
		int nauto = 0;
		for (i = 0; i < n; i++) {
			if (tracks[i].def.max_kind == GS_AUTO)
				nauto++;
		}
		if (nauto > 0) {
			for (i = 0; i < n; i++) {
				if (tracks[i].def.max_kind == GS_AUTO)
					tracks[i].size += free_space / nauto;
			}
		}
	}
}

/** compute positions of tracks with content alignment */
static int g_position_tracks(struct grid_track *t, int n, int gap,
		int avail, int align)
{
	int total = 0, i, pos = 0, extra_gap = 0;

	for (i = 0; i < n; i++)
		total += t[i].size;
	total += gap * (n > 0 ? n - 1 : 0);

	if (avail > total) {
		int free_space = avail - total;
		switch (align) {
		case CSS_JUSTIFY_CONTENT_CENTER:
			pos = free_space / 2;
			break;
		case CSS_JUSTIFY_CONTENT_FLEX_END:
			pos = free_space;
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
			if (n > 1)
				extra_gap = free_space / (n - 1);
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_AROUND:
			extra_gap = free_space / n;
			pos = extra_gap / 2;
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
			extra_gap = free_space / (n + 1);
			pos = extra_gap;
			break;
		default:
			break;
		}
	}
	for (i = 0; i < n; i++) {
		t[i].pos = pos;
		pos += t[i].size + gap + extra_gap;
	}
	return total;
}

/** lay out a grid item at the given width */
static bool g_layout_item(struct grid_ctx *g, struct box *b, int width)
{
	bool ok;

	b->float_container = b->parent;
	switch (b->type) {
	case BOX_BLOCK:
		ok = layout_block_context(b, -1, g->content);
		break;
	case BOX_TABLE:
		ok = layout_table(b, width, g->content);
		break;
	case BOX_FLEX:
		ok = layout_flex(b, width, g->content);
		break;
	default:
		ok = true;
		break;
	}
	b->float_container = NULL;
	return ok;
}

/** alignment keyword from a raw justify-items/-self value */
static int g_raw_align(const css_computed_style *s, enum css_properties_e p)
{
	const char *t = cssfx_raw(s, p);
	if (t == NULL)
		return -1;
	while (*t == ' ')
		t++;
	if (strncasecmp(t, "center", 6) == 0)
		return 1;
	if (strncasecmp(t, "end", 3) == 0 || strncasecmp(t, "flex-end", 8) == 0 ||
			strncasecmp(t, "right", 5) == 0 ||
			strncasecmp(t, "self-end", 8) == 0)
		return 2;
	if (strncasecmp(t, "start", 5) == 0 ||
			strncasecmp(t, "flex-start", 10) == 0 ||
			strncasecmp(t, "left", 4) == 0 ||
			strncasecmp(t, "self-start", 10) == 0 ||
			strncasecmp(t, "baseline", 8) == 0)
		return 0;
	if (strncasecmp(t, "stretch", 7) == 0 ||
			strncasecmp(t, "normal", 6) == 0)
		return 3;
	return -1;
}

/* exported interface documented in layout_internal.h */
bool layout_grid_is_grid(const struct box *b)
{
	uint8_t d;

	if (b->style == NULL)
		return false;
	d = css_computed_display(b->style, false);
	return d == CSS_DISPLAY_GRID || d == CSS_DISPLAY_INLINE_GRID;
}

/* exported interface documented in layout_internal.h */
bool layout_grid(struct box *grid, int available_width,
		html_content *content)
{
	struct grid_ctx g;
	int max_height, min_height;
	int *imin = NULL, *imax = NULL, *s0 = NULL, *s1 = NULL;
	int i, total_h;
	bool ok = false;
	int justify_items, justify_container;
	int avail_h;

	layout_find_dimensions(&content->unit_len_ctx, available_width, -1,
			grid, grid->style, NULL, &grid->height,
			NULL, NULL, &max_height, &min_height,
			grid->margin, grid->padding, grid->border);

	available_width = min(available_width, grid->width);
	if (available_width < 0)
		available_width = 0;

	if (!g_setup(&g, grid, content, available_width))
		goto cleanup;

	imin = calloc(g.nitems + 1, sizeof(int));
	imax = calloc(g.nitems + 1, sizeof(int));
	s0 = calloc(g.nitems + 1, sizeof(int));
	s1 = calloc(g.nitems + 1, sizeof(int));
	if (imin == NULL || imax == NULL || s0 == NULL || s1 == NULL)
		goto cleanup;

	/* item dimensions and column contributions */
	for (i = 0; i < g.nitems; i++) {
		struct box *b = g.items[i].box;
		int w;

		s0[i] = -1;
		b->float_container = b->parent;
		layout_find_dimensions(&content->unit_len_ctx,
				available_width, -1, b, b->style,
				&b->width, &b->height, NULL, NULL,
				NULL, NULL, b->margin, b->padding, b->border);
		b->float_container = NULL;
		if (b->style == NULL || lh__box_is_absolute(b))
			continue;

		s0[i] = g.items[i].c0;
		s1[i] = g.items[i].c1 > g.ncols ? g.ncols : g.items[i].c1;
		if (b->width != AUTO) {
			w = b->width + lh__delta_outer_width(b);
			imin[i] = imax[i] = w;
		} else {
			/* min/max widths are outer widths */
			imin[i] = b->min_width;
			imax[i] = b->max_width;
			if (b->min_width == UNKNOWN_MAX_WIDTH)
				imin[i] = 0;
			if (b->max_width == UNKNOWN_MAX_WIDTH)
				imax[i] = available_width;
			if (imax[i] < imin[i])
				imax[i] = imin[i];
		}
	}

	justify_container = css_computed_justify_content(grid->style);
	g_size_tracks(g.cols, g.ncols, available_width, g.col_gap, g.nitems,
			imin, imax, s0, s1,
			justify_container == CSS_JUSTIFY_CONTENT_FLEX_START);
	g_position_tracks(g.cols, g.ncols, g.col_gap, available_width,
			justify_container);

	justify_items = g_raw_align(grid->style, CSS_PROP_JUSTIFY_ITEMS);

	/* lay out items at their column widths */
	for (i = 0; i < g.nitems; i++) {
		struct grid_item *it = &g.items[i];
		struct box *b = it->box;
		int area_w, c1, j, js;

		if (b->style == NULL)
			continue;
		if (lh__box_is_absolute(b)) {
			/* static position: the grid's content origin */
			if (b->width == AUTO)
				b->width = min(max(b->min_width, 0),
						b->max_width);
			if (!g_layout_item(&g, b, b->width))
				goto cleanup;
			b->x = grid->padding[LEFT] + b->border[LEFT].width +
				lh__non_auto_margin(b, LEFT);
			b->y = grid->padding[TOP] + b->border[TOP].width +
				lh__non_auto_margin(b, TOP);
			continue;
		}

		c1 = it->c1 > g.ncols ? g.ncols : it->c1;
		area_w = 0;
		for (j = it->c0; j < c1; j++)
			area_w += g.cols[j].size;
		area_w += g.col_gap * (c1 - it->c0 - 1);

		js = g_raw_align(b->style, CSS_PROP_JUSTIFY_SELF);
		if (js < 0 || js == 3)
			js = justify_items;
		if (b->width == AUTO) {
			if ((js >= 0 && js != 3) || b->margin[LEFT] == AUTO ||
					b->margin[RIGHT] == AUTO) {
				/* shrink to fit */
				int d = lh__delta_outer_width(b);
				int w = b->max_width - d;
				int avail = area_w - d;
				if (b->max_width == UNKNOWN_MAX_WIDTH || w > avail)
					w = avail;
				if (b->min_width != UNKNOWN_MAX_WIDTH &&
						w < b->min_width - d)
					w = b->min_width - d < avail ?
						b->min_width - d : avail;
				b->width = w;
			} else {
				b->width = area_w - lh__delta_outer_width(b);
			}
			if (b->width < 0)
				b->width = 0;
		}
		if (!g_layout_item(&g, b, b->width))
			goto cleanup;

		/* horizontal placement within the area */
		{
			int outer = b->width + lh__delta_outer_width(b);
			int off = 0;
			if (b->margin[LEFT] == AUTO &&
					b->margin[RIGHT] == AUTO)
				off = (area_w - outer) / 2;
			else if (b->margin[LEFT] == AUTO)
				off = area_w - outer;
			else if (js == 1)
				off = (area_w - outer) / 2;
			else if (js == 2)
				off = area_w - outer;
			if (off < 0)
				off = 0;
			b->x = grid->padding[LEFT] + g.cols[it->c0].pos + off +
				lh__non_auto_margin(b, LEFT) +
				b->border[LEFT].width;
		}
	}

	/* rows: contributions are item heights */
	for (i = 0; i < g.nitems; i++) {
		struct grid_item *it = &g.items[i];
		struct box *b = it->box;
		s0[i] = -1;
		if (b->style == NULL || lh__box_is_absolute(b))
			continue;
		s0[i] = it->r0;
		s1[i] = it->r1 > g.nrows ? g.nrows : it->r1;
		imin[i] = imax[i] = b->height + lh__delta_outer_height(b);
	}
	avail_h = grid->height != AUTO ? grid->height : -1;
	g_size_tracks(g.rows, g.nrows, avail_h, g.row_gap, g.nitems,
			imin, imax, s0, s1,
			css_computed_align_content(grid->style) ==
				CSS_ALIGN_CONTENT_STRETCH);
	if (avail_h < 0) {
		/* indefinite height: flexible rows size to content */
		for (i = 0; i < g.nrows; i++) {
			if (g.rows[i].def.max_kind == GS_FR &&
					g.rows[i].size < g.rows[i].base)
				g.rows[i].size = g.rows[i].base;
		}
	}
	total_h = g_position_tracks(g.rows, g.nrows, g.row_gap,
			avail_h < 0 ? 0 : avail_h,
			css_computed_align_content(grid->style) ==
				CSS_ALIGN_CONTENT_CENTER ?
				CSS_JUSTIFY_CONTENT_CENTER :
			css_computed_align_content(grid->style) ==
				CSS_ALIGN_CONTENT_FLEX_END ?
				CSS_JUSTIFY_CONTENT_FLEX_END :
			css_computed_align_content(grid->style) ==
				CSS_ALIGN_CONTENT_SPACE_BETWEEN ?
				CSS_JUSTIFY_CONTENT_SPACE_BETWEEN :
				CSS_JUSTIFY_CONTENT_FLEX_START);

	/* vertical placement and stretching */
	for (i = 0; i < g.nitems; i++) {
		struct grid_item *it = &g.items[i];
		struct box *b = it->box;
		int area_h = 0, r1, j, outer, off = 0;
		enum css_align_self_e as;

		if (b->style == NULL || lh__box_is_absolute(b))
			continue;
		r1 = it->r1 > g.nrows ? g.nrows : it->r1;
		for (j = it->r0; j < r1; j++)
			area_h += g.rows[j].size;
		area_h += g.row_gap * (r1 - it->r0 - 1);

		as = lh__box_align_self(grid, b);
		outer = b->height + lh__delta_outer_height(b);
		if ((as == CSS_ALIGN_SELF_STRETCH ||
		     as == CSS_ALIGN_SELF_AUTO) &&
				lh__box_size_cross_is_auto(true, b) &&
				b->margin[TOP] != AUTO &&
				b->margin[BOTTOM] != AUTO &&
				area_h > outer) {
			b->height = area_h - lh__delta_outer_height(b);
			outer = area_h;
		}
		if (b->margin[TOP] == AUTO && b->margin[BOTTOM] == AUTO)
			off = (area_h - outer) / 2;
		else if (b->margin[TOP] == AUTO)
			off = area_h - outer;
		else if (as == CSS_ALIGN_SELF_CENTER)
			off = (area_h - outer) / 2;
		else if (as == CSS_ALIGN_SELF_FLEX_END)
			off = area_h - outer;
		if (off < 0)
			off = 0;
		b->y = grid->padding[TOP] + g.rows[it->r0].pos + off +
			lh__non_auto_margin(b, TOP) + b->border[TOP].width;
	}

	if (grid->height == AUTO)
		grid->height = total_h;
	if (max_height >= 0 && grid->height > max_height)
		grid->height = max_height;
	if (min_height > 0 && grid->height < min_height)
		grid->height = min_height;

	ok = true;

cleanup:
	free(imin);
	free(imax);
	free(s0);
	free(s1);
	g_free(&g);
	return ok;
}

/* exported interface documented in layout_internal.h */
void layout_grid_minmax(struct box *grid, const html_content *content,
		int *min, int *max)
{
	struct grid_ctx g;
	int *cmin, *cmax, i, j, gaps;

	*min = *max = 0;
	if (!g_setup(&g, grid, (html_content *)content, -1)) {
		g_free(&g);
		return;
	}
	cmin = calloc(g.ncols, sizeof(int));
	cmax = calloc(g.ncols, sizeof(int));
	if (cmin == NULL || cmax == NULL) {
		free(cmin);
		free(cmax);
		g_free(&g);
		return;
	}
	for (j = 0; j < g.ncols; j++) {
		const struct grid_track_def *d = &g.cols[j].def;
		if (d->min_kind == GS_FIXED)
			cmin[j] = d->min_val;
		if (d->max_kind == GS_FIXED)
			cmax[j] = d->max_val;
	}
	for (i = 0; i < g.nitems; i++) {
		struct grid_item *it = &g.items[i];
		struct box *b = it->box;
		int span, mn, mx, c1;
		if (b->style == NULL || lh__box_is_absolute(b) ||
				b->max_width == UNKNOWN_MAX_WIDTH)
			continue;
		c1 = it->c1 > g.ncols ? g.ncols : it->c1;
		span = c1 - it->c0;
		if (span < 1)
			continue;
		mn = b->min_width / span;
		mx = b->max_width / span;
		for (j = it->c0; j < c1; j++) {
			const struct grid_track_def *d = &g.cols[j].def;
			if (d->min_kind != GS_FIXED && cmin[j] < mn)
				cmin[j] = mn;
			if (d->max_kind != GS_FIXED && cmax[j] < mx)
				cmax[j] = mx;
		}
	}
	gaps = g.col_gap * (g.ncols - 1);
	*min = gaps;
	*max = gaps;
	for (j = 0; j < g.ncols; j++) {
		*min += cmin[j];
		*max += cmax[j] > cmin[j] ? cmax[j] : cmin[j];
	}
	free(cmin);
	free(cmax);
	g_free(&g);
}
