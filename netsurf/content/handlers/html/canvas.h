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
 * HTML <canvas> 2D drawing.
 *
 * Each canvas owns a straight-alpha RGBA buffer (bytes R, G, B, A)
 * drawn by a software rasteriser; the page shows a copy of it.
 */

#ifndef NETSURF_HTML_CANVAS_H
#define NETSURF_HTML_CANVAS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct html_content;
struct html_canvas;
struct dom_node;
struct box;
struct rect;
struct redraw_context;

#define CANVAS_MAX_STOPS 16

/** a paint source */
struct canvas_paint {
	enum { CANVAS_PAINT_COLOUR, CANVAS_PAINT_LINEAR,
	       CANVAS_PAINT_RADIAL } type;
	uint32_t colour;             /**< 0xAABBGGRR (bytes R,G,B,A) */
	/* gradients, in user space at the time of use */
	double x0, y0, r0, x1, y1, r1;
	int nstops;
	float stop_pos[CANVAS_MAX_STOPS];
	uint32_t stop_colour[CANVAS_MAX_STOPS];
};

enum canvas_composite {
	CANVAS_OP_SOURCE_OVER,
	CANVAS_OP_COPY,
	CANVAS_OP_DESTINATION_OUT,
	CANVAS_OP_DESTINATION_OVER,
	CANVAS_OP_LIGHTER,
	CANVAS_OP_SOURCE_ATOP,
};

/** drawing state (save()/restore()) */
struct canvas_state {
	double m[6];                 /**< transform a b c d e f */
	struct canvas_paint fill, stroke;
	double line_width;
	int line_cap;                /**< 0 butt, 1 round, 2 square */
	int line_join;               /**< 0 miter, 1 round, 2 bevel */
	double global_alpha;
	enum canvas_composite op;
	/* clip rectangle in device pixels (x0,y0,x1,y1) */
	int clip[4];
	/* font */
	char font_family[64];
	double font_px;
	int font_weight;
	bool font_italic;
	int text_align;              /**< 0 start/left, 1 center, 2 end/right */
	int text_baseline;           /**< 0 alphabetic, 1 top, 2 middle, 3 bottom */
	bool smoothing;
};

struct html_canvas *html_canvas_for_node(struct html_content *c,
		struct dom_node *n, bool create);

/** Box construction for <canvas>. */
bool html_canvas_box(struct html_content *c, struct dom_node *n,
		struct box *box, bool *convert_children);

void html_canvas_intrinsic(struct html_canvas *cv, int *w, int *h);
bool html_canvas_redraw(struct box *box, int x, int y, int w, int h,
		const struct rect *clip, const struct redraw_context *ctx);
void html_canvas_destroy_all(struct html_content *c);

/** Resize (and clear) the canvas bitmap. */
void html_canvas_set_size(struct html_canvas *cv, int w, int h);
void html_canvas_get_size(struct html_canvas *cv, int *w, int *h);

struct canvas_state *html_canvas_state(struct html_canvas *cv);
void html_canvas_save(struct html_canvas *cv);
void html_canvas_restore(struct html_canvas *cv);
void html_canvas_reset(struct html_canvas *cv);

/* paths (points are transformed as they are added) */
void html_canvas_begin_path(struct html_canvas *cv);
void html_canvas_close_path(struct html_canvas *cv);
void html_canvas_move_to(struct html_canvas *cv, double x, double y);
void html_canvas_line_to(struct html_canvas *cv, double x, double y);
void html_canvas_bezier_to(struct html_canvas *cv, double c1x, double c1y,
		double c2x, double c2y, double x, double y);
void html_canvas_quad_to(struct html_canvas *cv, double cx, double cy,
		double x, double y);
void html_canvas_arc(struct html_canvas *cv, double x, double y, double r,
		double a0, double a1, bool ccw);
void html_canvas_ellipse(struct html_canvas *cv, double x, double y,
		double rx, double ry, double rot, double a0, double a1,
		bool ccw);
void html_canvas_arc_to(struct html_canvas *cv, double x1, double y1,
		double x2, double y2, double r);
void html_canvas_rect(struct html_canvas *cv, double x, double y,
		double w, double h);
void html_canvas_round_rect(struct html_canvas *cv, double x, double y,
		double w, double h, double r);

void html_canvas_fill(struct html_canvas *cv, bool evenodd);
void html_canvas_stroke(struct html_canvas *cv);
void html_canvas_clip(struct html_canvas *cv);
bool html_canvas_point_in_path(struct html_canvas *cv, double x, double y);

void html_canvas_fill_rect(struct html_canvas *cv, double x, double y,
		double w, double h);
void html_canvas_stroke_rect(struct html_canvas *cv, double x, double y,
		double w, double h);
void html_canvas_clear_rect(struct html_canvas *cv, double x, double y,
		double w, double h);

/**
 * Draw RGBA pixels (bytes R,G,B,A, straight alpha).
 * s* is the source rectangle, d* the destination in user space.
 */
void html_canvas_draw_pixels(struct html_canvas *cv, const uint8_t *px,
		int pw, int ph, size_t stride, double sx, double sy,
		double sw, double sh, double dx, double dy, double dw,
		double dh);

/** Draw another element (img, canvas, video) */
bool html_canvas_draw_element(struct html_canvas *cv, struct dom_node *src,
		double sx, double sy, double sw, double sh, double dx,
		double dy, double dw, double dh, bool have_src_rect);

/** Natural size of a drawable element, false if not drawable yet */
bool html_canvas_element_size(struct html_content *c, struct dom_node *src,
		int *w, int *h);

/* pixels */
void html_canvas_get_image_data(struct html_canvas *cv, int x, int y,
		int w, int h, uint8_t *out);
void html_canvas_put_image_data(struct html_canvas *cv, const uint8_t *in,
		int iw, int ih, int dx, int dy, int sx, int sy, int sw,
		int sh);

/* text */
void html_canvas_set_font(struct html_canvas *cv, const char *css);
void html_canvas_fill_text(struct html_canvas *cv, const char *text,
		double x, double y, bool stroke);
double html_canvas_measure_text(struct html_canvas *cv, const char *text);

/** Parse a CSS colour to 0xAABBGGRR; false if invalid */
bool html_canvas_parse_colour(const char *text, uint32_t *out);

#endif
