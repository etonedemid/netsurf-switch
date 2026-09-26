/*
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

/**
 * \file
 * Framebuffer extended plotter operations (rounded shapes, gradients,
 * shadows, compositing layers).
 */

#ifndef NETSURF_FB_FBFX_H
#define NETSURF_FB_FBFX_H

struct redraw_context;
struct rect;
struct plot_radii;
struct plot_gradient;
struct plot_shadow;
struct plot_layer;
struct nsfb_s;

nserror fbfx_rounded_fill(const struct redraw_context *ctx,
		const struct rect *outer, const struct plot_radii *oradii,
		const struct rect *inner, const struct plot_radii *iradii,
		colour c);
nserror fbfx_gradient(const struct redraw_context *ctx,
		const struct rect *area, const struct plot_gradient *g);
nserror fbfx_shadow(const struct redraw_context *ctx,
		const struct rect *box, const struct plot_radii *radii,
		const struct plot_shadow *sh);
nserror fbfx_layer_begin(const struct redraw_context *ctx,
		const struct rect *area);
nserror fbfx_layer_end(const struct redraw_context *ctx,
		const struct plot_layer *p);
nserror fbfx_tint(const struct redraw_context *ctx, bool enable, colour c);

/**
 * Plot a bitmap as a tint mask if tint mode is active.
 *
 * \return false if tint mode is off (plot normally)
 */
bool fbfx_tint_bitmap(struct nsfb_s *bm, int x, int y, int width, int height);

nserror fbfx_path(const struct redraw_context *ctx,
		const plot_style_t *pstyle, const float *p, unsigned int n,
		const float transform[6]);

#endif
