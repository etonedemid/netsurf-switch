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
 * Frontend services for HTML <canvas> (text rasterisation).
 */

#ifndef NETSURF_CANVAS_H_
#define NETSURF_CANVAS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct plot_font_style;

struct gui_canvas_table {
	/**
	 * Render text into a canvas pixel buffer.
	 *
	 * The buffer is straight-alpha RGBA (bytes R, G, B, A). Glyph
	 * coverage multiplied by alpha (0..1) is composited source-over
	 * in the given colour (0xBBGGRR).
	 *
	 * \param x, y  baseline start in buffer pixels
	 * \param clip  x0, y0, x1, y1 limits in buffer pixels
	 */
	bool (*fill_text)(uint8_t *px, int w, int h, size_t stride,
			const struct plot_font_style *fs, int x, int y,
			const char *text, size_t len, uint32_t colour,
			float alpha, const int clip[4]);
};

#endif
