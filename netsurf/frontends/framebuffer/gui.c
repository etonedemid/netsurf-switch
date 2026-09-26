/*
 * Copyright 2008, 2014 Vincent Sanders <vince@netsurf-browser.org>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <limits.h>
#include <getopt.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <nsutils/time.h>
#include <libcss/libcss.h>

#ifdef __SWITCH__
#include <sys/stat.h>
#include <switch.h>
#endif

#include <libnsfb.h>
#include <libnsfb_plot.h>
#include <libnsfb_event.h>

#include "utils/utils.h"
#include "utils/nsoption.h"
#include "utils/filepath.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "desktop/browser_history.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"
#include "netsurf/cookie_db.h"
#include "content/fetch.h"
#include "netsurf/download.h"
#include "desktop/download.h"
#include "html/webfont.h"

#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "framebuffer/framebuffer.h"
#include "framebuffer/schedule.h"
#include "framebuffer/findfile.h"
#include "framebuffer/image_data.h"
#include "framebuffer/font.h"
#include "framebuffer/clipboard.h"
#include "framebuffer/fetch.h"
#include "framebuffer/bitmap.h"
#include "framebuffer/local_history.h"
#include "framebuffer/corewindow.h"


#define NSFB_TOOLBAR_DEFAULT_LAYOUT "blfsrukmjtc"

/* widths of the toolbar text buttons */
#define FB_JS_BUTTON_WIDTH 58
#define FB_BKM_BUTTON_WIDTH 72

fbtk_widget_t *fbtk;

static bool fb_complete = false;

struct gui_window *input_window = NULL;
struct gui_window *search_current_window;
struct gui_window *window_list = NULL;

/* private data for browser user widget */
struct browser_widget_s {
	struct browser_window *bw; /**< The browser window connected to this gui window */
	int scrollx, scrolly; /**< scroll offsets. */

	/* Pending window redraw state. */
	bool redraw_required; /**< flag indicating the foreground loop
			       * needs to redraw the browser widget.
			       */
	bbox_t redraw_box; /**< Area requiring redraw. */
	bool pan_required; /**< flag indicating the foreground loop
			    * needs to pan the window.
			    */
	int panx, pany; /**< Panning required. */
};

static struct gui_drag {
	enum state {
		GUI_DRAG_NONE,
		GUI_DRAG_PRESSED,
		GUI_DRAG_DRAG
	} state;
	int button;
	int x;
	int y;
	bool grabbed_pointer;
} gui_drag;


/**
 * Cause an abnormal program termination.
 *
 * \note This never returns and is intended to terminate without any cleanup.
 *
 * \param error The message to display to the user.
 */
static void die(const char *error)
{
	fprintf(stderr, "%s\n", error);
	exit(1);
}


/**
 * Warn the user of an event.
 *
 * \param[in] warning A warning looked up in the message translation table
 * \param[in] detail Additional text to be displayed or NULL.
 * \return NSERROR_OK on success or error code if there was a
 *           faliure displaying the message to the user.
 */
static nserror fb_warn_user(const char *warning, const char *detail)
{
	NSLOG(netsurf, INFO, "%s %s", warning, detail);
	return NSERROR_OK;
}

/* queue a redraw operation, co-ordinates are relative to the window */
static void
fb_queue_redraw(struct fbtk_widget_s *widget, int x0, int y0, int x1, int y1)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);

	bwidget->redraw_box.x0 = min(bwidget->redraw_box.x0, x0);
	bwidget->redraw_box.y0 = min(bwidget->redraw_box.y0, y0);
	bwidget->redraw_box.x1 = max(bwidget->redraw_box.x1, x1);
	bwidget->redraw_box.y1 = max(bwidget->redraw_box.y1, y1);

	if (fbtk_clip_to_widget(widget, &bwidget->redraw_box)) {
		bwidget->redraw_required = true;
		fbtk_request_redraw(widget);
	} else {
		bwidget->redraw_box.y0 = bwidget->redraw_box.x0 = INT_MAX;
		bwidget->redraw_box.y1 = bwidget->redraw_box.x1 = -(INT_MAX);
		bwidget->redraw_required = false;
	}
}

/* queue a window scroll */
static void
widget_scroll_y(struct gui_window *gw, int y, bool abs)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(gw->browser);
	int content_width, content_height;
	int height;

	NSLOG(netsurf, DEEPDEBUG, "window scroll");
	if (abs) {
		bwidget->pany = y - bwidget->scrolly;
	} else {
		bwidget->pany += y;
	}

	browser_window_get_extents(gw->bw, true,
			&content_width, &content_height);

	height = fbtk_get_height(gw->browser);

	/* dont pan off the top */
	if ((bwidget->scrolly + bwidget->pany) < 0)
		bwidget->pany = -bwidget->scrolly;

	/* do not pan off the bottom of the content */
	if ((bwidget->scrolly + bwidget->pany) > (content_height - height))
		bwidget->pany = (content_height - height) - bwidget->scrolly;

	if (bwidget->pany == 0)
		return;

	bwidget->pan_required = true;

	fbtk_request_redraw(gw->browser);

	fbtk_set_scroll_position(gw->vscroll, bwidget->scrolly + bwidget->pany);
}

/* queue a window scroll */
static void
widget_scroll_x(struct gui_window *gw, int x, bool abs)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(gw->browser);
	int content_width, content_height;
	int width;

	if (abs) {
		bwidget->panx = x - bwidget->scrollx;
	} else {
		bwidget->panx += x;
	}

	browser_window_get_extents(gw->bw, true,
			&content_width, &content_height);

	width = fbtk_get_width(gw->browser);

	/* dont pan off the left */
	if ((bwidget->scrollx + bwidget->panx) < 0)
		bwidget->panx = - bwidget->scrollx;

	/* do not pan off the right of the content */
	if ((bwidget->scrollx + bwidget->panx) > (content_width - width))
		bwidget->panx = (content_width - width) - bwidget->scrollx;

	if (bwidget->panx == 0)
		return;

	bwidget->pan_required = true;

	fbtk_request_redraw(gw->browser);

	fbtk_set_scroll_position(gw->hscroll, bwidget->scrollx + bwidget->panx);
}

static void
fb_pan(fbtk_widget_t *widget,
       struct browser_widget_s *bwidget,
       struct browser_window *bw)
{
	int x;
	int y;
	int width;
	int height;
	nsfb_bbox_t srcbox;
	nsfb_bbox_t dstbox;

	nsfb_t *nsfb = fbtk_get_nsfb(widget);

	height = fbtk_get_height(widget);
	width = fbtk_get_width(widget);

	NSLOG(netsurf, DEEPDEBUG, "panning %d, %d",
			bwidget->panx, bwidget->pany);

	x = fbtk_get_absx(widget);
	y = fbtk_get_absy(widget);

	/* if the pan exceeds the viewport size just redraw the whole area */
	if (bwidget->pany >= height || bwidget->pany <= -height ||
	    bwidget->panx >= width || bwidget->panx <= -width) {

		bwidget->scrolly += bwidget->pany;
		bwidget->scrollx += bwidget->panx;
		fb_queue_redraw(widget, 0, 0, width, height);

		/* ensure we don't try to scroll again */
		bwidget->panx = 0;
		bwidget->pany = 0;
		bwidget->pan_required = false;
		return;
	}

	if (bwidget->pany < 0) {
		/* pan up by less then viewport height */
		srcbox.x0 = x;
		srcbox.y0 = y;
		srcbox.x1 = srcbox.x0 + width;
		srcbox.y1 = srcbox.y0 + height + bwidget->pany;

		dstbox.x0 = x;
		dstbox.y0 = y - bwidget->pany;
		dstbox.x1 = dstbox.x0 + width;
		dstbox.y1 = dstbox.y0 + height + bwidget->pany;

		/* move part that remains visible up */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrolly += bwidget->pany;
		fb_queue_redraw(widget, 0, 0, width, - bwidget->pany);

	} else if (bwidget->pany > 0) {
		/* pan down by less then viewport height */
		srcbox.x0 = x;
		srcbox.y0 = y + bwidget->pany;
		srcbox.x1 = srcbox.x0 + width;
		srcbox.y1 = srcbox.y0 + height - bwidget->pany;

		dstbox.x0 = x;
		dstbox.y0 = y;
		dstbox.x1 = dstbox.x0 + width;
		dstbox.y1 = dstbox.y0 + height - bwidget->pany;

		/* move part that remains visible down */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrolly += bwidget->pany;
		fb_queue_redraw(widget, 0, height - bwidget->pany,
				width, height);
	}

	if (bwidget->panx < 0) {
		/* pan left by less then viewport width */
		srcbox.x0 = x;
		srcbox.y0 = y;
		srcbox.x1 = srcbox.x0 + width + bwidget->panx;
		srcbox.y1 = srcbox.y0 + height;

		dstbox.x0 = x - bwidget->panx;
		dstbox.y0 = y;
		dstbox.x1 = dstbox.x0 + width + bwidget->panx;
		dstbox.y1 = dstbox.y0 + height;

		/* move part that remains visible left */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrollx += bwidget->panx;
		fb_queue_redraw(widget, 0, 0, -bwidget->panx, height);

	} else if (bwidget->panx > 0) {
		/* pan right by less then viewport width */
		srcbox.x0 = x + bwidget->panx;
		srcbox.y0 = y;
		srcbox.x1 = srcbox.x0 + width - bwidget->panx;
		srcbox.y1 = srcbox.y0 + height;

		dstbox.x0 = x;
		dstbox.y0 = y;
		dstbox.x1 = dstbox.x0 + width - bwidget->panx;
		dstbox.y1 = dstbox.y0 + height;

		/* move part that remains visible right */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrollx += bwidget->panx;
		fb_queue_redraw(widget, width - bwidget->panx, 0,
				width, height);
	}

	bwidget->pan_required = false;
	bwidget->panx = 0;
	bwidget->pany = 0;
}

static void
fb_redraw(fbtk_widget_t *widget,
	  struct browser_widget_s *bwidget,
	  struct browser_window *bw)
{
	int x;
	int y;
	int caret_x, caret_y, caret_h;
	struct rect clip;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &fb_plotters
	};
	nsfb_t *nsfb = fbtk_get_nsfb(widget);

	x = fbtk_get_absx(widget);
	y = fbtk_get_absy(widget);

	/* adjust clipping co-ordinates according to window location */
	bwidget->redraw_box.y0 += y;
	bwidget->redraw_box.y1 += y;
	bwidget->redraw_box.x0 += x;
	bwidget->redraw_box.x1 += x;

	nsfb_claim(nsfb, &bwidget->redraw_box);

	/* redraw bounding box is relative to window */
	clip.x0 = bwidget->redraw_box.x0;
	clip.y0 = bwidget->redraw_box.y0;
	clip.x1 = bwidget->redraw_box.x1;
	clip.y1 = bwidget->redraw_box.y1;

	browser_window_redraw(bw,
			x - bwidget->scrollx,
			y - bwidget->scrolly,
			&clip, &ctx);

	if (fbtk_get_caret(widget, &caret_x, &caret_y, &caret_h)) {
		/* This widget has caret, so render it */
		nsfb_bbox_t line;
		nsfb_plot_pen_t pen;

		line.x0 = x - bwidget->scrollx + caret_x;
		line.y0 = y - bwidget->scrolly + caret_y;
		line.x1 = x - bwidget->scrollx + caret_x;
		line.y1 = y - bwidget->scrolly + caret_y + caret_h;

		pen.stroke_type = NFSB_PLOT_OPTYPE_SOLID;
		pen.stroke_width = 1;
		pen.stroke_colour = 0xFF0000FF;

		nsfb_plot_line(nsfb, &line, &pen);
	}

	nsfb_update(fbtk_get_nsfb(widget), &bwidget->redraw_box);

	bwidget->redraw_box.y0 = bwidget->redraw_box.x0 = INT_MAX;
	bwidget->redraw_box.y1 = bwidget->redraw_box.x1 = INT_MIN;
	bwidget->redraw_required = false;
}

static int
fb_browser_window_redraw(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_widget_s *bwidget;

	bwidget = fbtk_get_userpw(widget);
	if (bwidget == NULL) {
		NSLOG(netsurf, INFO,
		      "browser widget from widget %p was null", widget);
		return -1;
	}

	if (bwidget->pan_required) {
		fb_pan(widget, bwidget, gw->bw);
	}

	if (bwidget->redraw_required) {
		fb_redraw(widget, bwidget, gw->bw);
	} else {
		bwidget->redraw_box.x0 = 0;
		bwidget->redraw_box.y0 = 0;
		bwidget->redraw_box.x1 = fbtk_get_width(widget);
		bwidget->redraw_box.y1 = fbtk_get_height(widget);
		fb_redraw(widget, bwidget, gw->bw);
	}
	return 0;
}

static int fb_browser_window_destroy(fbtk_widget_t *widget,
		fbtk_callback_info *cbi)
{
	struct browser_widget_s *browser_widget;

	if (widget == NULL) {
		return 0;
	}

	/* Free private data */
	browser_widget = fbtk_get_userpw(widget);
	free(browser_widget);

	return 0;
}

static void
framebuffer_surface_iterator(void *ctx, const char *name, enum nsfb_type_e type)
{
	const char *arg0 = ctx;

	fprintf(stderr, "%s: %s\n", arg0, name);
}

static enum nsfb_type_e fetype = NSFB_SURFACE_COUNT;
static const char *fename;
static int febpp;
static int fewidth;
static int feheight;
static const char *feurl;

static void
framebuffer_pick_default_fename(void *ctx, const char *name, enum nsfb_type_e type)
{
	if (type < fetype) {
		fename = name;
	}
}

static bool
process_cmdline(int argc, char** argv)
{
	int opt;
	int option_index;
	static struct option long_options[] = {
		{0, 0, 0,  0 }
	}; /* no long options */

	NSLOG(netsurf, INFO, "argc %d, argv %p", argc, argv);

	nsfb_enumerate_surface_types(framebuffer_pick_default_fename, NULL);

	febpp = 32;

	fewidth = nsoption_int(window_width);
	if (fewidth <= 0) {
		fewidth = 800;
	}
	feheight = nsoption_int(window_height);
	if (feheight <= 0) {
		feheight = 600;
	}

#ifdef __SWITCH__
	/* enumeration order would pick the RAM surface; force SDL2 at 720p */
	fename = "sdl2";
	fewidth = 1280;
	feheight = 720;
#endif

	if ((nsoption_charp(homepage_url) != NULL) && 
	    (nsoption_charp(homepage_url)[0] != '\0')) {
		feurl = nsoption_charp(homepage_url);
	} else {
		feurl = NETSURF_HOMEPAGE;
	}

	while((opt = getopt_long(argc, argv, "f:b:w:h:",
				 long_options, &option_index)) != -1) {
		switch (opt) {
		case 'f':
			fename = optarg;
			break;

		case 'b':
			febpp = atoi(optarg);
			break;

		case 'w':
			fewidth = atoi(optarg);
			break;

		case 'h':
			feheight = atoi(optarg);
			break;

		default:
			fprintf(stderr,
				"Usage: %s [-f frontend] [-b bpp] [-w width] [-h height] <url>\n",
				argv[0]);
			return false;
		}
	}

	if (optind < argc) {
		feurl = argv[optind];
	}

	if (nsfb_type_from_name(fename) == NSFB_SURFACE_NONE) {
		if (strcmp(fename, "?") != 0) {
			fprintf(stderr,
				"%s: Unknown surface `%s`\n", argv[0], fename);
		}
		fprintf(stderr, "%s: Valid surface names are:\n", argv[0]);
		nsfb_enumerate_surface_types(framebuffer_surface_iterator, argv[0]);
		return false;
	}

	return true;
}

#ifdef __SWITCH__
#include "framebuffer/switch_player.h"
#include "framebuffer/switch_audio.h"
#include "desktop/hotlist.h"
#include "utils/file.h"
#include "utils/utf8.h"

/* armed by a click in the browser widget; if the core then places an
 * input caret shortly after, the native keyboard opens for that field */
static uint64_t fb_kbd_armed_time;

static void fb_html_input_kbd_cb(void *pw)
{
	struct gui_window *g = pw;
	SwkbdConfig kbd;
	char out[2048];
	size_t len, o;

	if (R_FAILED(swkbdCreate(&kbd, 0)))
		return;
	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetGuideText(&kbd, "Enter text");
	if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof(out)))) {
		len = strlen(out);
		o = 0;
		while (o < len) {
			browser_window_key_press(g->bw,
					utf8_to_ucs4(out + o, len - o));
			o = utf8_next(out, len, o);
		}
	}
	swkbdClose(&kbd);
}

/* Media handoff: NetSurf reaches the download path for content it cannot
 * render inline. Direct media links get played instead of downloaded:
 * http URLs stream straight into ffmpeg, while https URLs (the ffmpeg
 * portlib has no TLS) download to the SD through NetSurf's own TLS stack
 * first and play as a local file. Anything that is not media still gets
 * its fetch aborted (no general download support). */

#define FB_MEDIA_TMPDIR "sdmc:/switch/netsurf/tmp"

struct gui_download_window {
	download_context *ctx;
	FILE *fh;    /* NULL after a write failure; download then discarded */
	char *path;  /* temp file on the SD */
	char *name;  /* basename, for the progress readout */
	bool audio;  /* play inline rather than in the modal player */
	unsigned long long total;   /* content length, 0 if unknown */
	unsigned long long written;
	unsigned long long last_report;
};

static void fb_media_play_cb(void *pw)
{
	char *url = pw;

	switch_player_play_url(url);
	free(url);

	/* the player painted over our output; repaint everything */
	if (fbtk != NULL) {
		fbtk_request_redraw(fbtk);
	}
}

/* ---- audio transport bar -------------------------------------------
 *
 * Audio plays through switch_audio, which decodes in slices from the
 * scheduler so the browser stays live. This bar is the transport UI:
 * it is built once per window, kept unmapped, and shown while a track
 * is loaded. Video still uses the modal switch_player.
 */

#define FB_AUDIOBAR_HEIGHT 26
#define FB_AUDIOBTN_WIDTH 46
#define FB_AUDIO_TICK_MS 100

static void fb_audio_tick(void *pw);

static void fb_audio_format_time(char *buf, size_t len, double t)
{
	int secs = (t > 0.0) ? (int)t : 0;

	snprintf(buf, len, "%d:%02d", secs / 60, secs % 60);
}

static void fb_audio_update_label(struct gui_window *gw)
{
	char pos[16];
	char dur[16];
	char buf[256];

	if (gw->audio_label == NULL)
		return;

	fb_audio_format_time(pos, sizeof(pos), switch_audio_position());

	if (switch_audio_duration() > 0.0) {
		fb_audio_format_time(dur, sizeof(dur),
				     switch_audio_duration());
		snprintf(buf, sizeof(buf), "%s%s  %s / %s",
			 switch_audio_paused() ? "[paused] " : "",
			 switch_audio_title(), pos, dur);
	} else {
		snprintf(buf, sizeof(buf), "%s%s  %s",
			 switch_audio_paused() ? "[paused] " : "",
			 switch_audio_title(), pos);
	}

	fbtk_set_text(gw->audio_label, buf);
}

static void fb_audio_hide(struct gui_window *gw)
{
	if (gw->audiobar != NULL)
		fbtk_set_mapping(gw->audiobar, false);
	framebuffer_schedule(-1, fb_audio_tick, gw);
	if (fbtk != NULL)
		fbtk_request_redraw(fbtk);
}

/* keep the queue fed and the readout current */
static void fb_audio_tick(void *pw)
{
	struct gui_window *gw = pw;

	if (!switch_audio_active()) {
		fb_audio_hide(gw);
		return;
	}

	switch_audio_pump();
	fb_audio_update_label(gw);

	framebuffer_schedule(FB_AUDIO_TICK_MS, fb_audio_tick, gw);
}

static int fb_audio_playpause_click(fbtk_widget_t *widget,
				    fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	switch_audio_toggle_pause();
	fbtk_set_text(widget, switch_audio_paused() ? "Play" : "Pause");
	fb_audio_update_label(gw);
	return 1;
}

static int fb_audio_rew_click(fbtk_widget_t *widget,
			      fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	switch_audio_seek_relative(-10.0);
	fb_audio_update_label(gw);
	return 1;
}

static int fb_audio_ff_click(fbtk_widget_t *widget,
			     fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	switch_audio_seek_relative(10.0);
	fb_audio_update_label(gw);
	return 1;
}

static int fb_audio_stop_click(fbtk_widget_t *widget,
			       fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	switch_audio_stop();
	fb_audio_hide(gw);
	return 1;
}

/* build the bar once, unmapped, just above the status bar */
static void fb_audio_create_bar(struct gui_window *gw)
{
	int furniture = nsoption_int(fb_furniture_size);
	int w = fbtk_get_width(gw->window);
	int y = fbtk_get_height(gw->window) - furniture -
		FB_AUDIOBAR_HEIGHT;
	int x = 2;
	fbtk_widget_t *btn;

	if (gw->audiobar != NULL)
		return;

	gw->audiobar = fbtk_create_window(gw->window, 0, y, w,
					  FB_AUDIOBAR_HEIGHT,
					  FB_FRAME_COLOUR);
	if (gw->audiobar == NULL)
		return;

	btn = fbtk_create_text_button(gw->audiobar, x, 2,
				      FB_AUDIOBTN_WIDTH,
				      FB_AUDIOBAR_HEIGHT - 4,
				      FB_FRAME_COLOUR, FB_COLOUR_BLACK,
				      fb_audio_rew_click, gw);
	fbtk_set_text(btn, "<<10");
	x += FB_AUDIOBTN_WIDTH + 2;

	gw->audio_playpause = fbtk_create_text_button(
		gw->audiobar, x, 2, FB_AUDIOBTN_WIDTH,
		FB_AUDIOBAR_HEIGHT - 4, FB_FRAME_COLOUR, FB_COLOUR_BLACK,
		fb_audio_playpause_click, gw);
	fbtk_set_text(gw->audio_playpause, "Pause");
	x += FB_AUDIOBTN_WIDTH + 2;

	btn = fbtk_create_text_button(gw->audiobar, x, 2,
				      FB_AUDIOBTN_WIDTH,
				      FB_AUDIOBAR_HEIGHT - 4,
				      FB_FRAME_COLOUR, FB_COLOUR_BLACK,
				      fb_audio_ff_click, gw);
	fbtk_set_text(btn, "10>>");
	x += FB_AUDIOBTN_WIDTH + 2;

	btn = fbtk_create_text_button(gw->audiobar, x, 2,
				      FB_AUDIOBTN_WIDTH,
				      FB_AUDIOBAR_HEIGHT - 4,
				      FB_FRAME_COLOUR, FB_COLOUR_BLACK,
				      fb_audio_stop_click, gw);
	fbtk_set_text(btn, "Stop");
	x += FB_AUDIOBTN_WIDTH + 4;

	gw->audio_label = fbtk_create_text(gw->audiobar, x, 2,
					   w - x - 2,
					   FB_AUDIOBAR_HEIGHT - 4,
					   FB_FRAME_COLOUR, FB_COLOUR_BLACK,
					   false);

	fbtk_set_mapping(gw->audiobar, false);
}

/* start a local audio file playing and reveal the bar */
static void fb_audio_begin(struct gui_window *gw, const char *path,
			   const char *title, bool own_file)
{
	if (gw == NULL)
		return;

	fb_audio_create_bar(gw);

	if (switch_audio_start(path, title, own_file) == false) {
		NSLOG(netsurf, INFO, "audio: failed to start %s", path);
		fb_audio_hide(gw);
		return;
	}

	if (gw->audio_playpause != NULL)
		fbtk_set_text(gw->audio_playpause, "Pause");
	fb_audio_update_label(gw);

	if (gw->audiobar != NULL) {
		fbtk_set_zorder(gw->audiobar, INT_MIN);
		fbtk_set_mapping(gw->audiobar, true);
	}

	framebuffer_schedule(-1, fb_audio_tick, gw);
	framebuffer_schedule(FB_AUDIO_TICK_MS, fb_audio_tick, gw);
}

/* a downloaded temp file is ready: audio plays inline, video modally */
struct fb_media_ready {
	char *path;
	char *title;
	bool video;
};

static void fb_media_ready_cb(void *pw)
{
	struct fb_media_ready *mr = pw;

	if (mr->video) {
		char *url = malloc(strlen(mr->path) + strlen("file:") + 1);

		if (url != NULL) {
			strcpy(url, "file:");
			strcat(url, mr->path);
			switch_player_play_url(url);
			free(url);
		}
		remove(mr->path);
		if (fbtk != NULL)
			fbtk_request_redraw(fbtk);
	} else {
		fb_audio_begin(window_list, mr->path, mr->title, true);
	}

	free(mr->path);
	free(mr->title);
	free(mr);
}

/* extension of the url's path component (query/fragment ignored);
 * ffmpeg uses it as a demuxer hint for the temp file name */
static const char *fb_media_url_ext(const char *url)
{
	static char ext[8];
	const char *end = url;
	const char *dot = NULL;
	const char *p;

	while (*end != '\0' && *end != '?' && *end != '#')
		end++;
	for (p = url; p < end; p++) {
		if (*p == '.')
			dot = p;
		else if (*p == '/')
			dot = NULL;
	}
	if (dot == NULL || (size_t)(end - dot) >= sizeof(ext))
		return ".bin";
	memcpy(ext, dot, end - dot);
	ext[end - dot] = '\0';
	return ext;
}

/* audio (as opposed to video) decides inline playback vs modal player */
static bool fb_media_is_audio(const char *mime, const char *url)
{
	static const char *aexts[] = {
		".mp3", ".m4a", ".ogg", ".oga", ".opus", ".flac", ".wav",
		NULL
	};
	int i;

	if (mime != NULL && strncasecmp(mime, "audio/", 6) == 0)
		return true;
	if (mime != NULL && strncasecmp(mime, "video/", 6) == 0)
		return false;

	if (url != NULL) {
		size_t ulen = strlen(url);

		for (i = 0; aexts[i] != NULL; i++) {
			size_t elen = strlen(aexts[i]);

			if (ulen > elen &&
			    strncasecmp(url + ulen - elen, aexts[i],
					elen) == 0)
				return true;
		}
	}
	return false;
}

/* last path component, for the transport bar label */
static const char *fb_media_basename(const char *url)
{
	const char *slash = strrchr(url, '/');

	return (slash != NULL && slash[1] != '\0') ? slash + 1 : url;
}

/* show spooling progress where the browser shows link targets */
static void fb_download_status(struct gui_download_window *dw)
{
	char buf[128];

	if (window_list == NULL || window_list->status == NULL)
		return;

	if (dw->total > 0) {
		snprintf(buf, sizeof(buf),
			 "Downloading %s - %d%% (%.1f/%.1f MB)",
			 dw->name != NULL ? dw->name : "media",
			 (int)((dw->written * 100) / dw->total),
			 dw->written / (1024.0 * 1024.0),
			 dw->total / (1024.0 * 1024.0));
	} else {
		snprintf(buf, sizeof(buf), "Downloading %s - %.1f MB",
			 dw->name != NULL ? dw->name : "media",
			 dw->written / (1024.0 * 1024.0));
	}

	fbtk_set_text(window_list->status, buf);
}

static struct gui_download_window *
fb_download_create(download_context *ctx, struct gui_window *parent)
{
	const char *mime = download_context_get_mime_type(ctx);
	struct nsurl *url = download_context_get_url(ctx);
	const char *url_s = (url != NULL) ? nsurl_access(url) : NULL;
	struct gui_download_window *dw;
	static unsigned int seq;
	char path[128];
	bool is_audio;

	(void)parent;

	if (url_s == NULL || switch_player_is_media(mime, url_s) == false) {
		/* not media; returning NULL aborts the fetch */
		return NULL;
	}

	is_audio = fb_media_is_audio(mime, url_s);

	if (strncmp(url_s, "https:", 6) != 0) {
		/* ffmpeg can read http/file itself, so skip the spool */
		if (is_audio) {
			fb_audio_begin(window_list, url_s,
				       fb_media_basename(url_s), false);
		} else {
			framebuffer_schedule(10, fb_media_play_cb,
					     strdup(url_s));
		}
		return NULL;
	}

	/* https: switch-ffmpeg has no TLS, so spool it to the SD first */
	snprintf(path, sizeof(path), "%s/play%u%s",
		 FB_MEDIA_TMPDIR, seq++, fb_media_url_ext(url_s));

	dw = calloc(1, sizeof(*dw));
	if (dw == NULL) {
		return NULL;
	}
	dw->fh = fopen(path, "wb");
	dw->path = strdup(path);
	dw->name = strdup(fb_media_basename(url_s));
	if (dw->fh == NULL || dw->path == NULL) {
		if (dw->fh != NULL)
			fclose(dw->fh);
		free(dw->path);
		free(dw->name);
		free(dw);
		return NULL;
	}
	dw->ctx = ctx;
	dw->audio = is_audio;
	dw->total = download_context_get_total_length(ctx);

	NSLOG(netsurf, INFO, "media: spooling %s to %s (%llu bytes)",
	      url_s, path, (unsigned long long)dw->total);
	fb_download_status(dw);
	return dw;
}

static nserror
fb_download_data(struct gui_download_window *dw,
		 const char *data, unsigned int size)
{
	if (dw->fh != NULL &&
	    fwrite(data, 1, size, dw->fh) != size) {
		/* SD write failed; drop the transfer but keep accepting
		 * (and discarding) data so cleanup happens in done() */
		NSLOG(netsurf, INFO, "media: write to %s failed", dw->path);
		fclose(dw->fh);
		dw->fh = NULL;
		remove(dw->path);
		return NSERROR_OK;
	}

	dw->written += size;

	/* refresh the readout every 256K rather than every chunk */
	if (dw->written - dw->last_report >= 256 * 1024) {
		dw->last_report = dw->written;
		fb_download_status(dw);
	}

	return NSERROR_OK;
}

static void fb_download_free(struct gui_download_window *dw)
{
	download_context_destroy(dw->ctx);
	free(dw->path);
	free(dw->name);
	free(dw);
}

static void fb_download_error(struct gui_download_window *dw,
			      const char *error_msg)
{
	NSLOG(netsurf, INFO, "media: download failed: %s",
	      error_msg != NULL ? error_msg : "");

	if (window_list != NULL && window_list->status != NULL)
		fbtk_set_text(window_list->status, "Download failed");

	if (dw->fh != NULL)
		fclose(dw->fh);
	if (dw->path != NULL)
		remove(dw->path);
	fb_download_free(dw);
}

static void fb_download_done(struct gui_download_window *dw)
{
	bool complete = (dw->fh != NULL);

	if (dw->fh != NULL) {
		fclose(dw->fh);
		dw->fh = NULL;
	}

	if (window_list != NULL && window_list->status != NULL)
		fbtk_set_text(window_list->status, "");

	if (complete) {
		struct fb_media_ready *mr = calloc(1, sizeof(*mr));

		if (mr != NULL) {
			mr->path = strdup(dw->path);
			mr->title = strdup(dw->name != NULL ?
					   dw->name : "audio");
			mr->video = !dw->audio;
			/* hand off from the main loop, not mid-callback */
			framebuffer_schedule(10, fb_media_ready_cb, mr);
		}
	}

	fb_download_free(dw);
}

static struct gui_download_table framebuffer_download_table = {
	.create = fb_download_create,
	.data = fb_download_data,
	.error = fb_download_error,
	.done = fb_download_done,
};
#endif /* __SWITCH__ */

/**
 * Set option defaults for framebuffer frontend
 *
 * @param defaults The option table to update.
 * @return error status.
 */
static nserror set_defaults(struct nsoption_s *defaults)
{
	int idx;
	static const struct {
		enum nsoption_e nsc;
		colour c;
	} sys_colour_defaults[]= {
		{ NSOPTION_sys_colour_AccentColor, 0x00666666},
		{ NSOPTION_sys_colour_AccentColorText, 0x00ffffff},
		{ NSOPTION_sys_colour_ActiveText, 0x000000ee},
		{ NSOPTION_sys_colour_ButtonBorder, 0x00aaaaaa},
		{ NSOPTION_sys_colour_ButtonFace, 0x00dddddd},
		{ NSOPTION_sys_colour_ButtonText, 0x00000000},
		{ NSOPTION_sys_colour_Canvas, 0x00aaaaaa},
		{ NSOPTION_sys_colour_CanvasText, 0x00000000},
		{ NSOPTION_sys_colour_Field, 0x00f1f1f1},
		{ NSOPTION_sys_colour_FieldText, 0x00000000},
		{ NSOPTION_sys_colour_GrayText, 0x00777777},
		{ NSOPTION_sys_colour_Highlight, 0x00ee0000},
		{ NSOPTION_sys_colour_HighlightText, 0x00000000},
		{ NSOPTION_sys_colour_LinkText, 0x00ee0000},
		{ NSOPTION_sys_colour_Mark, 0x0000ffff},
		{ NSOPTION_sys_colour_MarkText, 0x00000000},
		{ NSOPTION_sys_colour_SelectedItem, 0x00e48435},
		{ NSOPTION_sys_colour_SelectedItemText, 0x00ffffff},
		{ NSOPTION_sys_colour_VisitedText, 0x008b1a55},
		{ NSOPTION_LISTEND, 0},
	};

	/* Set defaults for absent option strings */
#ifdef __SWITCH__
	/* JavaScript stays off by default: Duktape's partial DOM makes
	 * script-gated sites render blank, while their no-JS fallbacks are
	 * usable. Users can set enable_javascript:1 in
	 * sdmc:/switch/netsurf/Choices. */
	nsoption_setnull_charp(ca_bundle, strdup("romfs:/res/ca-bundle.pem"));
	nsoption_setnull_charp(cookie_file, strdup("sdmc:/switch/netsurf/Cookies"));
	nsoption_setnull_charp(cookie_jar, strdup("sdmc:/switch/netsurf/Cookies"));
#else
	nsoption_setnull_charp(cookie_file, strdup("~/.netsurf/Cookies"));
	nsoption_setnull_charp(cookie_jar, strdup("~/.netsurf/Cookies"));
#endif

	if (nsoption_charp(cookie_file) == NULL ||
	    nsoption_charp(cookie_jar) == NULL) {
		NSLOG(netsurf, INFO, "Failed initialising cookie options");
		return NSERROR_BAD_PARAMETER;
	}

	/* set system colours for framebuffer ui */
	for (idx=0; sys_colour_defaults[idx].nsc != NSOPTION_LISTEND; idx++) {
		defaults[sys_colour_defaults[idx].nsc].value.c = sys_colour_defaults[idx].c;
	}
	return NSERROR_OK;
}


/**
 * Ensures output logging stream is correctly configured
 */
static bool nslog_stream_configure(FILE *fptr)
{
        /* set log stream to be non-buffering */
	setbuf(fptr, NULL);

	return true;
}

static void framebuffer_run(void)
{
	nsfb_event_t event;
	int timeout; /* timeout in miliseconds */

	while (fb_complete != true) {
		/* run the scheduler and discover how long to wait for
		 * the next event.
		 */
		timeout = schedule_run();

		/* if redraws are pending do not wait for event,
		 * return immediately
		 */
		if (fbtk_get_redraw_pending(fbtk))
			timeout = 0;

		if (fbtk_event(fbtk, &event, timeout)) {
			if ((event.type == NSFB_EVENT_CONTROL) &&
			    (event.value.controlcode ==  NSFB_CONTROL_QUIT))
				fb_complete = true;
		}

		fbtk_redraw(fbtk);
	}
}

static void gui_quit(void)
{
	NSLOG(netsurf, INFO, "gui_quit");

	urldb_save_cookies(nsoption_charp(cookie_jar));

	framebuffer_finalise();
}

/* called back when click in browser window */
static int
fb_browser_window_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);
	browser_mouse_state mouse;
	int x = cbi->x + bwidget->scrollx;
	int y = cbi->y + bwidget->scrolly;
	uint64_t time_now;
	static struct {
		enum { CLICK_SINGLE, CLICK_DOUBLE, CLICK_TRIPLE } type;
		uint64_t time;
	} last_click;

	if (cbi->event->type != NSFB_EVENT_KEY_DOWN &&
	    cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	NSLOG(netsurf, DEEPDEBUG, "browser window clicked at %d,%d",
			cbi->x, cbi->y);

	switch (cbi->event->type) {
	case NSFB_EVENT_KEY_DOWN:
		switch (cbi->event->value.keycode) {
		case NSFB_KEY_MOUSE_1:
#ifdef __SWITCH__
			nsu_getmonotonic_ms(&fb_kbd_armed_time);
#endif
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_PRESS_1, x, y);
			gui_drag.state = GUI_DRAG_PRESSED;
			gui_drag.button = 1;
			gui_drag.x = x;
			gui_drag.y = y;
			break;

		case NSFB_KEY_MOUSE_3:
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_PRESS_2, x, y);
			gui_drag.state = GUI_DRAG_PRESSED;
			gui_drag.button = 2;
			gui_drag.x = x;
			gui_drag.y = y;
			break;

		case NSFB_KEY_MOUSE_4:
			/* scroll up */
			if (browser_window_scroll_at_point(gw->bw,
							   x, y,
							   0, -100) == false)
				widget_scroll_y(gw, -100, false);
			break;

		case NSFB_KEY_MOUSE_5:
			/* scroll down */
			if (browser_window_scroll_at_point(gw->bw,
							   x, y,
							   0, 100) == false)
				widget_scroll_y(gw, 100, false);
			break;

		default:
			break;

		}

		break;
	case NSFB_EVENT_KEY_UP:

		mouse = 0;
		nsu_getmonotonic_ms(&time_now);

		switch (cbi->event->value.keycode) {
		case NSFB_KEY_MOUSE_1:
			if (gui_drag.state == GUI_DRAG_DRAG) {
				/* End of a drag, rather than click */

				if (gui_drag.grabbed_pointer) {
					/* need to ungrab pointer */
					fbtk_tgrab_pointer(widget);
					gui_drag.grabbed_pointer = false;
				}

				gui_drag.state = GUI_DRAG_NONE;

				/* Tell core */
				browser_window_mouse_track(gw->bw, 0, x, y);
				break;
			}
			/* This is a click;
			 * clear PRESSED state and pass to core */
			gui_drag.state = GUI_DRAG_NONE;
			mouse = BROWSER_MOUSE_CLICK_1;
			break;

		case NSFB_KEY_MOUSE_3:
			if (gui_drag.state == GUI_DRAG_DRAG) {
				/* End of a drag, rather than click */
				gui_drag.state = GUI_DRAG_NONE;

				if (gui_drag.grabbed_pointer) {
					/* need to ungrab pointer */
					fbtk_tgrab_pointer(widget);
					gui_drag.grabbed_pointer = false;
				}

				/* Tell core */
				browser_window_mouse_track(gw->bw, 0, x, y);
				break;
			}
			/* This is a click;
			 * clear PRESSED state and pass to core */
			gui_drag.state = GUI_DRAG_NONE;
			mouse = BROWSER_MOUSE_CLICK_2;
			break;

		default:
			break;

		}

		/* Determine if it's a double or triple click, allowing
		 * 0.5 seconds (500ms) between clicks
		 */
		if ((time_now < (last_click.time + 500)) &&
		    (cbi->event->value.keycode != NSFB_KEY_MOUSE_4) &&
		    (cbi->event->value.keycode != NSFB_KEY_MOUSE_5)) {
			if (last_click.type == CLICK_SINGLE) {
				/* Set double click */
				mouse |= BROWSER_MOUSE_DOUBLE_CLICK;
				last_click.type = CLICK_DOUBLE;

			} else if (last_click.type == CLICK_DOUBLE) {
				/* Set triple click */
				mouse |= BROWSER_MOUSE_TRIPLE_CLICK;
				last_click.type = CLICK_TRIPLE;
			} else {
				/* Set normal click */
				last_click.type = CLICK_SINGLE;
			}
		} else {
			last_click.type = CLICK_SINGLE;
		}

		if (mouse) {
			browser_window_mouse_click(gw->bw, mouse, x, y);
		}

		last_click.time = time_now;

		break;
	default:
		break;

	}
	return 1;
}

/* called back when movement in browser window */
static int
fb_browser_window_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	browser_mouse_state mouse = 0;
	struct gui_window *gw = cbi->context;
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);
	int x = cbi->x + bwidget->scrollx;
	int y = cbi->y + bwidget->scrolly;

	if (gui_drag.state == GUI_DRAG_PRESSED &&
			(abs(x - gui_drag.x) > 5 ||
			 abs(y - gui_drag.y) > 5)) {
		/* Drag started */
		if (gui_drag.button == 1) {
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_DRAG_1,
					gui_drag.x, gui_drag.y);
		} else {
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_DRAG_2,
					gui_drag.x, gui_drag.y);
		}
		gui_drag.grabbed_pointer = fbtk_tgrab_pointer(widget);
		gui_drag.state = GUI_DRAG_DRAG;
	}

	if (gui_drag.state == GUI_DRAG_DRAG) {
		/* set up mouse state */
		mouse |= BROWSER_MOUSE_DRAG_ON;

		if (gui_drag.button == 1)
			mouse |= BROWSER_MOUSE_HOLDING_1;
		else
			mouse |= BROWSER_MOUSE_HOLDING_2;
	}

	browser_window_mouse_track(gw->bw, mouse, x, y);

	return 0;
}


static void fb_update_back_forward(struct gui_window *gw);

static int
fb_browser_window_input(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	static fbtk_modifier_type modifier = FBTK_MOD_CLEAR;
	int ucs4 = -1;

	NSLOG(netsurf, INFO, "got value %d", cbi->event->value.keycode);

	switch (cbi->event->type) {
	case NSFB_EVENT_KEY_DOWN:
		switch (cbi->event->value.keycode) {

		case NSFB_KEY_DELETE:
			browser_window_key_press(gw->bw, NS_KEY_DELETE_RIGHT);
			break;

		case NSFB_KEY_ESCAPE:
			/* physical B on the pad: go back a page */
			if (browser_window_back_available(gw->bw)) {
				browser_window_history_back(gw->bw, false);
				fb_update_back_forward(gw);
			}
			break;

		case NSFB_KEY_PAGEUP:
			if (browser_window_key_press(gw->bw,
					NS_KEY_PAGE_UP) == false)
				widget_scroll_y(gw, -fbtk_get_height(
						gw->browser), false);
			break;

		case NSFB_KEY_PAGEDOWN:
			if (browser_window_key_press(gw->bw,
					NS_KEY_PAGE_DOWN) == false)
				widget_scroll_y(gw, fbtk_get_height(
						gw->browser), false);
			break;

		case NSFB_KEY_RIGHT:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				/* CTRL held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_LINE_END) == false)
					widget_scroll_x(gw, INT_MAX, true);

			} else if (modifier & FBTK_MOD_RSHIFT ||
					modifier & FBTK_MOD_LSHIFT) {
				/* SHIFT held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_WORD_RIGHT) == false)
					widget_scroll_x(gw, fbtk_get_width(
						gw->browser), false);

			} else {
				/* no modifier */
				if (browser_window_key_press(gw->bw,
						NS_KEY_RIGHT) == false)
					widget_scroll_x(gw, 100, false);
			}
			break;

		case NSFB_KEY_LEFT:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				/* CTRL held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_LINE_START) == false)
					widget_scroll_x(gw, 0, true);

			} else if (modifier & FBTK_MOD_RSHIFT ||
					modifier & FBTK_MOD_LSHIFT) {
				/* SHIFT held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_WORD_LEFT) == false)
					widget_scroll_x(gw, -fbtk_get_width(
						gw->browser), false);

			} else {
				/* no modifier */
				if (browser_window_key_press(gw->bw,
						NS_KEY_LEFT) == false)
					widget_scroll_x(gw, -100, false);
			}
			break;

		case NSFB_KEY_UP:
			if (browser_window_key_press(gw->bw,
					NS_KEY_UP) == false)
				widget_scroll_y(gw, -100, false);
			break;

		case NSFB_KEY_DOWN:
			if (browser_window_key_press(gw->bw,
					NS_KEY_DOWN) == false)
				widget_scroll_y(gw, 100, false);
			break;

		case NSFB_KEY_MINUS:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				browser_window_set_scale(gw->bw, -0.1, false);
			}
			break;

		case NSFB_KEY_EQUALS: /* PLUS */
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				browser_window_set_scale(gw->bw, 0.1, false);
			}
			break;

		case NSFB_KEY_0:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				browser_window_set_scale(gw->bw, 1.0, true);
			}
			break;

		case NSFB_KEY_RSHIFT:
			modifier |= FBTK_MOD_RSHIFT;
			break;

		case NSFB_KEY_LSHIFT:
			modifier |= FBTK_MOD_LSHIFT;
			break;

		case NSFB_KEY_RCTRL:
			modifier |= FBTK_MOD_RCTRL;
			break;

		case NSFB_KEY_LCTRL:
			modifier |= FBTK_MOD_LCTRL;
			break;

		case NSFB_KEY_y:
		case NSFB_KEY_z:
			if (cbi->event->value.keycode == NSFB_KEY_z &&
					(modifier & FBTK_MOD_RCTRL ||
					 modifier & FBTK_MOD_LCTRL) &&
					(modifier & FBTK_MOD_RSHIFT ||
					 modifier & FBTK_MOD_LSHIFT)) {
				/* Z pressed with CTRL and SHIFT held */
				browser_window_key_press(gw->bw, NS_KEY_REDO);
				break;

			} else if (cbi->event->value.keycode == NSFB_KEY_z &&
					(modifier & FBTK_MOD_RCTRL ||
					 modifier & FBTK_MOD_LCTRL)) {
				/* Z pressed with CTRL held */
				browser_window_key_press(gw->bw, NS_KEY_UNDO);
				break;

			} else if (cbi->event->value.keycode == NSFB_KEY_y &&
					(modifier & FBTK_MOD_RCTRL ||
					 modifier & FBTK_MOD_LCTRL)) {
				/* Y pressed with CTRL held */
				browser_window_key_press(gw->bw, NS_KEY_REDO);
				break;
			}
			/* Z or Y pressed but not undo or redo; */
			fallthrough;

		default:
			ucs4 = fbtk_keycode_to_ucs4(cbi->event->value.keycode,
						    modifier);
			if (ucs4 != -1)
				browser_window_key_press(gw->bw, ucs4);
			break;
		}
		break;

	case NSFB_EVENT_KEY_UP:
		switch (cbi->event->value.keycode) {
		case NSFB_KEY_RSHIFT:
			modifier &= ~FBTK_MOD_RSHIFT;
			break;

		case NSFB_KEY_LSHIFT:
			modifier &= ~FBTK_MOD_LSHIFT;
			break;

		case NSFB_KEY_RCTRL:
			modifier &= ~FBTK_MOD_RCTRL;
			break;

		case NSFB_KEY_LCTRL:
			modifier &= ~FBTK_MOD_LCTRL;
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}

	return 0;
}

static void
fb_update_back_forward(struct gui_window *gw)
{
	struct browser_window *bw = gw->bw;

	fbtk_set_bitmap(gw->back,
			(browser_window_back_available(bw)) ?
			&left_arrow : &left_arrow_g);
	fbtk_set_bitmap(gw->forward,
			(browser_window_forward_available(bw)) ?
			&right_arrow : &right_arrow_g);
}

/* left icon click routine */
static int
fb_leftarrow_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_window *bw = gw->bw;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	if (browser_window_back_available(bw))
		browser_window_history_back(bw, false);

	fb_update_back_forward(gw);

	return 1;
}

/* right arrow icon click routine */
static int
fb_rightarrow_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_window *bw = gw->bw;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	if (browser_window_forward_available(bw))
		browser_window_history_forward(bw, false);

	fb_update_back_forward(gw);
	return 1;

}

/* reload icon click routine */
static int
fb_reload_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct browser_window *bw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	browser_window_reload(bw, true);
	return 1;
}

/* Bookmarks are NetSurf's hotlist, which persists as an HTML file. The
 * "list" button exports a fresh copy and simply browses to it, so no
 * bookmark-manager window is needed. */
#define FB_HOTLIST_PATH "sdmc:/switch/netsurf/Hotlist"
#define FB_BOOKMARKS_PAGE "sdmc:/switch/netsurf/bookmarks.html"

/* reflect whether the page on screen is bookmarked */
static void fb_update_bookmark_button(struct gui_window *gw)
{
	struct nsurl *url;

	if (gw == NULL || gw->bookmark == NULL)
		return;

	url = browser_window_access_url(gw->bw);
	fbtk_set_text(gw->bookmark,
		      (url != NULL && hotlist_has_url(url)) ?
			      "-Bkmk" : "+Bkmk");
}

/* one button both ways: bookmark the page, or drop it if already saved */
static int fb_addbookmark_click(fbtk_widget_t *widget,
				fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct nsurl *url;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	url = browser_window_access_url(gw->bw);
	if (url == NULL)
		return 1;

	if (hotlist_has_url(url)) {
		hotlist_remove_url(url);
		fbtk_set_text(gw->status, "Bookmark removed");
	} else if (hotlist_add_url(url) == NSERROR_OK) {
		fbtk_set_text(gw->status, "Bookmark added");
	} else {
		fbtk_set_text(gw->status, "Could not add bookmark");
	}

	fb_update_bookmark_button(gw);
	return 1;
}

static int fb_showbookmarks_click(fbtk_widget_t *widget,
				  fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct nsurl *url;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	if (hotlist_export(FB_BOOKMARKS_PAGE, "Bookmarks") != NSERROR_OK) {
		fbtk_set_text(gw->status, "No bookmarks yet");
		return 1;
	}

	if (netsurf_path_to_nsurl(FB_BOOKMARKS_PAGE, &url) != NSERROR_OK)
		return 1;

	browser_window_navigate(gw->bw, url, NULL, BW_NAVIGATE_HISTORY,
				NULL, NULL, NULL);
	nsurl_unref(url);
	return 1;
}

/* JS toggle: flips enable_javascript for subsequent page loads,
 * persists the choice and reloads the current page so it applies.
 * The engine behind it is Duktape (QuickJS has no DOM bindings yet). */
static int
fb_jsbtn_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	bool enable;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	enable = !nsoption_bool(enable_javascript);
	nsoption_set_bool(enable_javascript, enable);
	fbtk_set_text(widget, enable ? "JS on" : "JS off");

#ifdef __SWITCH__
	/* keep the setting across boots */
	nsoption_write("sdmc:/switch/netsurf/Choices",
		       nsoptions, nsoptions_default);
#endif

	browser_window_reload(gw->bw, true);
	return 1;
}

/* stop icon click routine */
static int
fb_stop_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct browser_window *bw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	browser_window_stop(bw);
	return 0;
}

static int
fb_osk_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	map_osk();

	return 0;
}

/* close browser window icon click routine */
static int
fb_close_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	fb_complete = true;

	return 0;
}

static int
fb_scroll_callback(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	switch (cbi->type) {
	case FBTK_CBT_SCROLLY:
		widget_scroll_y(gw, cbi->y, true);
		break;

	case FBTK_CBT_SCROLLX:
		widget_scroll_x(gw, cbi->x, true);
		break;

	default:
		break;
	}
	return 0;
}

static int
fb_url_enter(void *pw, char *text)
{
	struct browser_window *bw = pw;
	nsurl *url;
	nserror error;

	error = nsurl_create(text, &url);
	if (error != NSERROR_OK) {
		fb_warn_user("Errorcode:", messages_get_errorcode(error));
	} else {
		browser_window_navigate(bw, url, NULL, BW_NAVIGATE_HISTORY,
				NULL, NULL, NULL);
		nsurl_unref(url);
	}

	return 0;
}

static int
fb_url_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	framebuffer_set_cursor(&caret_image);
	return 0;
}

static int
set_ptr_default_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	framebuffer_set_cursor(&pointer_image);
	return 0;
}

static int
fb_localhistory_btn_clik(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	fb_local_history_present(fbtk, gw->bw);

	return 0;
}


/** Create a toolbar window and populate it with buttons. 
 *
 * The toolbar layout uses a character to define buttons type and position:
 * b - back
 * l - local history
 * f - forward
 * s - stop 
 * r - refresh
 * u - url bar expands to fit remaining space
 * t - throbber/activity indicator
 * c - close the current window
 *
 * The default layout is "blfsrut" there should be no more than a
 * single url bar entry or behaviour will be undefined.
 *
 * @param gw Parent window 
 * @param toolbar_height The height in pixels of the toolbar
 * @param padding The padding in pixels round each element of the toolbar
 * @param frame_col Frame colour.
 * @param toolbar_layout A string defining which buttons and controls
 *                       should be added to the toolbar. May be empty
 *                       string to disable the bar..
 * 
 */
static fbtk_widget_t *
create_toolbar(struct gui_window *gw, 
	       int toolbar_height, 
	       int padding, 
	       colour frame_col,
	       const char *toolbar_layout)
{
	fbtk_widget_t *toolbar;
	fbtk_widget_t *widget;

	int xpos; /* The position of the next widget. */
	int xlhs = 0; /* extent of the left hand side widgets */
	int xdir = 1; /* the direction of movement + or - 1 */
	const char *itmtype; /* type of the next item */

	if (toolbar_layout == NULL) {
		toolbar_layout = NSFB_TOOLBAR_DEFAULT_LAYOUT;
	}

	NSLOG(netsurf, INFO, "Using toolbar layout %s", toolbar_layout);

	itmtype = toolbar_layout;

	/* check for the toolbar being disabled */
	if ((*itmtype == 0) || (*itmtype == 'q')) {
		return NULL;
	}

	toolbar = fbtk_create_window(gw->window, 0, 0, 0, 
				     toolbar_height, 
				     frame_col);

	if (toolbar == NULL) {
		return NULL;
	}

	fbtk_set_handler(toolbar, 
			 FBTK_CBT_POINTERENTER, 
			 set_ptr_default_move, 
			 NULL);


	xpos = padding;

	/* loop proceeds creating widget on the left hand side until
	 * it runs out of layout or encounters a url bar declaration
	 * wherupon it works backwards from the end of the layout
	 * untill the space left is for the url bar
	 */
	while ((itmtype >= toolbar_layout) && 
	       (*itmtype != 0) && 
	       (xdir !=0)) {

		NSLOG(netsurf, INFO, "toolbar adding %c", *itmtype);


		switch (*itmtype) {

		case 'b': /* back */
			widget = fbtk_create_button(toolbar, 
						    (xdir == 1) ? xpos : 
						     xpos - left_arrow.width, 
						    padding, 
						    left_arrow.width, 
						    -padding, 
						    frame_col, 
						    &left_arrow, 
						    fb_leftarrow_click, 
						    gw);
			gw->back = widget; /* keep reference */
			break;

		case 'l': /* local history */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1) ? xpos : 
						     xpos - history_image.width,
						    padding,
						    history_image.width,
						    -padding,
						    frame_col,
						    &history_image,
						    fb_localhistory_btn_clik,
						    gw);
			gw->history = widget;
			break;

		case 'f': /* forward */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - right_arrow.width,
						    padding,
						    right_arrow.width,
						    -padding,
						    frame_col,
						    &right_arrow,
						    fb_rightarrow_click,
						    gw);
			gw->forward = widget;
			break;

		case 'c': /* close the current window */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - stop_image_g.width,
						    padding,
						    stop_image_g.width,
						    -padding,
						    frame_col,
						    &stop_image_g,
						    fb_close_click,
						    gw->bw);
			gw->close = widget;
			break;

		case 's': /* stop  */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - stop_image.width,
						    padding,
						    stop_image.width,
						    -padding,
						    frame_col,
						    &stop_image,
						    fb_stop_click,
						    gw->bw);
			gw->stop = widget;
			break;

		case 'r': /* reload */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - reload.width,
						    padding,
						    reload.width,
						    -padding,
						    frame_col,
						    &reload,
						    fb_reload_click,
						    gw->bw);
			gw->reload = widget;
			break;

		case 't': /* throbber/activity indicator */
			widget = fbtk_create_bitmap(toolbar,
						    (xdir == 1)?xpos :
						     xpos - throbber0.width,
						    padding,
						    throbber0.width,
						    -padding,
						    frame_col,
						    &throbber0);
			gw->throbber = widget;
			break;

		case 'j': /* javascript toggle */
			widget = fbtk_create_text_button(toolbar,
						    (xdir == 1) ? xpos :
						     xpos - FB_JS_BUTTON_WIDTH,
						    padding,
						    FB_JS_BUTTON_WIDTH,
						    -padding,
						    frame_col,
						    FB_COLOUR_BLACK,
						    fb_jsbtn_click,
						    gw);
			fbtk_set_text(widget,
				      nsoption_bool(enable_javascript) ?
						"JS on" : "JS off");
			gw->jsbutton = widget;
			break;

		case 'k': /* bookmark this page */
			widget = fbtk_create_text_button(toolbar,
						    (xdir == 1) ? xpos :
						     xpos - FB_BKM_BUTTON_WIDTH,
						    padding,
						    FB_BKM_BUTTON_WIDTH,
						    -padding,
						    frame_col,
						    FB_COLOUR_BLACK,
						    fb_addbookmark_click,
						    gw);
			fbtk_set_text(widget, "+Bkmk");
			gw->bookmark = widget;
			break;

		case 'm': /* show bookmarks */
			widget = fbtk_create_text_button(toolbar,
						    (xdir == 1) ? xpos :
						     xpos - FB_BKM_BUTTON_WIDTH,
						    padding,
						    FB_BKM_BUTTON_WIDTH,
						    -padding,
						    frame_col,
						    FB_COLOUR_BLACK,
						    fb_showbookmarks_click,
						    gw);
			fbtk_set_text(widget, "Bkmks");
			break;


		case 'u': /* url bar*/
			if (xdir == -1) {
				/* met the u going backwards add url
				 * now we know available extent 
				 */ 

				widget = fbtk_create_writable_text(toolbar,
						   xlhs,
						   padding,
						   xpos - xlhs,
						   -padding,
						   FB_COLOUR_WHITE,
						   FB_COLOUR_BLACK,
						   true,
						   fb_url_enter,
						   gw->bw);

				fbtk_set_handler(widget, 
						 FBTK_CBT_POINTERENTER, 
						 fb_url_move, gw->bw);

				gw->url = widget; /* keep reference */

				/* toolbar is complete */
				xdir = 0;
				break;
			}
			/* met url going forwards, note position and
			 * reverse direction 
			 */
			itmtype = toolbar_layout + strlen(toolbar_layout);
			xdir = -1;
			xlhs = xpos;
			xpos = (2 * fbtk_get_width(toolbar));
			widget = toolbar;
			break;

		default:
			widget = NULL;
			xdir = 0;
			NSLOG(netsurf, INFO,
			      "Unknown element %c in toolbar layout",
			      *itmtype);
		        break;

		}

		if (widget != NULL) {
			xpos += (xdir * (fbtk_get_width(widget) + padding));
		}

		NSLOG(netsurf, INFO, "xpos is %d", xpos);

		itmtype += xdir;
	}

	fbtk_set_mapping(toolbar, true);

	return toolbar;
}


/** Resize a toolbar.
 *
 * @param gw Parent window
 * @param toolbar_height The height in pixels of the toolbar
 * @param padding The padding in pixels round each element of the toolbar
 * @param toolbar_layout A string defining which buttons and controls
 *                       should be added to the toolbar. May be empty
 *                       string to disable the bar.
 */
static void
resize_toolbar(struct gui_window *gw,
	       int toolbar_height,
	       int padding,
	       const char *toolbar_layout)
{
	fbtk_widget_t *widget;

	int xpos; /* The position of the next widget. */
	int xlhs = 0; /* extent of the left hand side widgets */
	int xdir = 1; /* the direction of movement + or - 1 */
	const char *itmtype; /* type of the next item */
	int x = 0, y = 0, w = 0, h = 0;

	if (gw->toolbar == NULL) {
		return;
	}

	if (toolbar_layout == NULL) {
		toolbar_layout = NSFB_TOOLBAR_DEFAULT_LAYOUT;
	}

	itmtype = toolbar_layout;

	if (*itmtype == 0) {
		return;
	}

	fbtk_set_pos_and_size(gw->toolbar, 0, 0, 0, toolbar_height);

	xpos = padding;

	/* loop proceeds creating widget on the left hand side until
	 * it runs out of layout or encounters a url bar declaration
	 * wherupon it works backwards from the end of the layout
	 * untill the space left is for the url bar
	 */
	while (itmtype >= toolbar_layout && xdir != 0) {

		switch (*itmtype) {
		case 'b': /* back */
			widget = gw->back;
			x = (xdir == 1) ? xpos : xpos - left_arrow.width;
			y = padding;
			w = left_arrow.width;
			h = -padding;
			break;

		case 'l': /* local history */
			widget = gw->history;
			x = (xdir == 1) ? xpos : xpos - history_image.width;
			y = padding;
			w = history_image.width;
			h = -padding;
			break;

		case 'f': /* forward */
			widget = gw->forward;
			x = (xdir == 1) ? xpos : xpos - right_arrow.width;
			y = padding;
			w = right_arrow.width;
			h = -padding;
			break;

		case 'c': /* close the current window */
			widget = gw->close;
			x = (xdir == 1) ? xpos : xpos - stop_image_g.width;
			y = padding;
			w = stop_image_g.width;
			h = -padding;
			break;

		case 's': /* stop  */
			widget = gw->stop;
			x = (xdir == 1) ? xpos : xpos - stop_image.width;
			y = padding;
			w = stop_image.width;
			h = -padding;
			break;

		case 'r': /* reload */
			widget = gw->reload;
			x = (xdir == 1) ? xpos : xpos - reload.width;
			y = padding;
			w = reload.width;
			h = -padding;
			break;

		case 't': /* throbber/activity indicator */
			widget = gw->throbber;
			x = (xdir == 1) ? xpos : xpos - throbber0.width;
			y = padding;
			w = throbber0.width;
			h = -padding;
			break;

		case 'j': /* javascript toggle */
			widget = gw->jsbutton;
			x = (xdir == 1) ? xpos : xpos - FB_JS_BUTTON_WIDTH;
			y = padding;
			w = FB_JS_BUTTON_WIDTH;
			h = -padding;
			break;

		case 'k': /* bookmark this page */
		case 'm': /* show bookmarks */
			/* not tracked for resize; skip their width */
			widget = NULL;
			x = y = h = 0;
			w = FB_BKM_BUTTON_WIDTH;
			xpos += xdir * (w + padding);
			itmtype += xdir;
			continue;


		case 'u': /* url bar*/
			if (xdir == -1) {
				/* met the u going backwards add url
				 * now we know available extent
				 */
				widget = gw->url;
				x = xlhs;
				y = padding;
				w = xpos - xlhs;
				h = -padding;

				/* toolbar is complete */
				xdir = 0;
				break;
			}
			/* met url going forwards, note position and
			 * reverse direction
			 */
			itmtype = toolbar_layout + strlen(toolbar_layout);
			xdir = -1;
			xlhs = xpos;
			w = fbtk_get_width(gw->toolbar);
			xpos = 2 * w;
			widget = gw->toolbar;
			break;

		default:
			widget = NULL;
		        break;

		}

		if (widget != NULL) {
			if (widget != gw->toolbar)
				fbtk_set_pos_and_size(widget, x, y, w, h);
			xpos += xdir * (w + padding);
		}

		itmtype += xdir;
	}
}

/** Routine called when "stripped of focus" event occours for browser widget.
 *
 * @param widget The widget reciving "stripped of focus" event.
 * @param cbi The callback parameters.
 * @return The callback result.
 */
static int
fb_browser_window_strip_focus(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	fbtk_set_caret(widget, false, 0, 0, 0, NULL);

	return 0;
}

static void
create_browser_widget(struct gui_window *gw, int toolbar_height, int furniture_width)
{
	struct browser_widget_s *browser_widget;
	browser_widget = calloc(1, sizeof(struct browser_widget_s));

	gw->browser = fbtk_create_user(gw->window,
				       0,
				       toolbar_height,
				       -furniture_width,
				       -furniture_width,
				       browser_widget);

	fbtk_set_handler(gw->browser, FBTK_CBT_REDRAW, fb_browser_window_redraw, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_DESTROY, fb_browser_window_destroy, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_INPUT, fb_browser_window_input, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_CLICK, fb_browser_window_click, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_STRIP_FOCUS, fb_browser_window_strip_focus, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_POINTERMOVE, fb_browser_window_move, gw);
}

static void
resize_browser_widget(struct gui_window *gw, int x, int y,
		int width, int height)
{
	fbtk_set_pos_and_size(gw->browser, x, y, width, height);
	browser_window_schedule_reformat(gw->bw);
}

static void
create_normal_browser_window(struct gui_window *gw, int furniture_width)
{
	fbtk_widget_t *widget;
	fbtk_widget_t *toolbar;
	int statusbar_width = 0;
	int toolbar_height = nsoption_int(fb_toolbar_size);

	NSLOG(netsurf, INFO, "Normal window");

	gw->window = fbtk_create_window(fbtk, 0, 0, 0, 0, 0);

	statusbar_width = nsoption_int(toolbar_status_size) *
		fbtk_get_width(gw->window) / 10000;

	/* toolbar */
	toolbar = create_toolbar(gw, 
				 toolbar_height, 
				 2, 
				 FB_FRAME_COLOUR, 
				 nsoption_charp(fb_toolbar_layout));
	gw->toolbar = toolbar;

	/* set the actually created toolbar height */
	if (toolbar != NULL) {
		toolbar_height = fbtk_get_height(toolbar);
	} else {
		toolbar_height = 0;
	}

	/* status bar */
	gw->status = fbtk_create_text(gw->window,
				      0,
				      fbtk_get_height(gw->window) - furniture_width,
				      statusbar_width, furniture_width,
				      FB_FRAME_COLOUR, FB_COLOUR_BLACK,
				      false);
	fbtk_set_handler(gw->status, FBTK_CBT_POINTERENTER, set_ptr_default_move, NULL);

	NSLOG(netsurf, INFO, "status bar %p at %d,%d", gw->status,
	      fbtk_get_absx(gw->status), fbtk_get_absy(gw->status));

	/* create horizontal scrollbar */
	gw->hscroll = fbtk_create_hscroll(gw->window,
					  statusbar_width,
					  fbtk_get_height(gw->window) - furniture_width,
					  fbtk_get_width(gw->window) - statusbar_width - furniture_width,
					  furniture_width,
					  FB_SCROLL_COLOUR,
					  FB_FRAME_COLOUR,
					  fb_scroll_callback,
					  gw);

	/* fill bottom right area */

	if (nsoption_bool(fb_osk) == true) {
		widget = fbtk_create_text_button(gw->window,
						 fbtk_get_width(gw->window) - furniture_width,
						 fbtk_get_height(gw->window) - furniture_width,
						 furniture_width,
						 furniture_width,
						 FB_FRAME_COLOUR, FB_COLOUR_BLACK,
						 fb_osk_click,
						 NULL);
		widget = fbtk_create_button(gw->window,
				fbtk_get_width(gw->window) - furniture_width,
				fbtk_get_height(gw->window) - furniture_width,
				furniture_width,
				furniture_width,
				FB_FRAME_COLOUR,
				&osk_image,
				fb_osk_click,
				NULL);
	} else {
		widget = fbtk_create_fill(gw->window,
					  fbtk_get_width(gw->window) - furniture_width,
					  fbtk_get_height(gw->window) - furniture_width,
					  furniture_width,
					  furniture_width,
					  FB_FRAME_COLOUR);

		fbtk_set_handler(widget, FBTK_CBT_POINTERENTER, set_ptr_default_move, NULL);
	}

	gw->bottom_right = widget;

	/* create vertical scrollbar */
	gw->vscroll = fbtk_create_vscroll(gw->window,
					  fbtk_get_width(gw->window) - furniture_width,
					  toolbar_height,
					  furniture_width,
					  fbtk_get_height(gw->window) - toolbar_height - furniture_width,
					  FB_SCROLL_COLOUR,
					  FB_FRAME_COLOUR,
					  fb_scroll_callback,
					  gw);

	/* browser widget */
	create_browser_widget(gw, toolbar_height, nsoption_int(fb_furniture_size));

	/* Give browser_window's user widget input focus */
	fbtk_set_focus(gw->browser);
}

static void
resize_normal_browser_window(struct gui_window *gw, int furniture_width)
{
	bool resized;
	int width, height;
	int statusbar_width;
	int toolbar_height = fbtk_get_height(gw->toolbar);

	/* Resize the main window widget */
	resized = fbtk_set_pos_and_size(gw->window, 0, 0, 0, 0);
	if (!resized)
		return;

	width = fbtk_get_width(gw->window);
	height = fbtk_get_height(gw->window);
	statusbar_width = nsoption_int(toolbar_status_size) * width / 10000;

	resize_toolbar(gw, toolbar_height, 2,
			nsoption_charp(fb_toolbar_layout));
	fbtk_set_pos_and_size(gw->status,
			0, height - furniture_width,
			statusbar_width, furniture_width);
	fbtk_reposition_hscroll(gw->hscroll,
			statusbar_width, height - furniture_width,
			width - statusbar_width - furniture_width,
			furniture_width);
	fbtk_set_pos_and_size(gw->bottom_right,
			width - furniture_width, height - furniture_width,
			furniture_width, furniture_width);
	fbtk_reposition_vscroll(gw->vscroll,
			width - furniture_width,
			toolbar_height, furniture_width,
			height - toolbar_height - furniture_width);
	resize_browser_widget(gw,
			0, toolbar_height,
			width - furniture_width,
			height - furniture_width - toolbar_height);
}

static void gui_window_add_to_window_list(struct gui_window *gw)
{
	gw->next = NULL;
	gw->prev = NULL;

	if (window_list == NULL) {
		window_list = gw;
	} else {
		window_list->prev = gw;
		gw->next = window_list;
		window_list = gw;
	}
}

static void gui_window_remove_from_window_list(struct gui_window *gw)
{
	struct gui_window *list;

	for (list = window_list; list != NULL; list = list->next) {
		if (list != gw)
			continue;

		if (list == window_list) {
			window_list = list->next;
			if (window_list != NULL)
				window_list->prev = NULL;
		} else {
			list->prev->next = list->next;
			if (list->next != NULL) {
				list->next->prev = list->prev;
			}
		}
		break;
	}
}


static struct gui_window *
gui_window_create(struct browser_window *bw,
		struct gui_window *existing,
		gui_window_create_flags flags)
{
	struct gui_window *gw;

	gw = calloc(1, sizeof(struct gui_window));

	if (gw == NULL)
		return NULL;

	/* associate the gui window with the underlying browser window
	 */
	gw->bw = bw;

	create_normal_browser_window(gw, nsoption_int(fb_furniture_size));

	/* map and request redraw of gui window */
	fbtk_set_mapping(gw->window, true);

	/* Add it to the window list */
	gui_window_add_to_window_list(gw);

	return gw;
}

static void
gui_window_destroy(struct gui_window *gw)
{
	gui_window_remove_from_window_list(gw);

	fbtk_destroy_widget(gw->window);

	free(gw);
}


/**
 * Invalidates an area of a framebuffer browser window
 *
 * \param g The netsurf window being invalidated.
 * \param rect area to redraw or NULL for the entire window area
 * \return NSERROR_OK on success or appropriate error code
 */
static nserror
fb_window_invalidate_area(struct gui_window *g, const struct rect *rect)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

	if (rect != NULL) {
		fb_queue_redraw(g->browser,
				rect->x0 - bwidget->scrollx,
				rect->y0 - bwidget->scrolly,
				rect->x1 - bwidget->scrollx,
				rect->y1 - bwidget->scrolly);
	} else {
		fb_queue_redraw(g->browser,
				0,
				0,
				fbtk_get_width(g->browser),
				fbtk_get_height(g->browser));
	}
	return NSERROR_OK;
}

static bool
gui_window_get_scroll(struct gui_window *g, int *sx, int *sy)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

	*sx = bwidget->scrollx;
	*sy = bwidget->scrolly;

	return true;
}

/**
 * Set the scroll position of a framebuffer browser window.
 *
 * Scrolls the viewport to ensure the specified rectangle of the
 *   content is shown. The framebuffer implementation scrolls the contents so
 *   the specified point in the content is at the top of the viewport.
 *
 * \param gw gui_window to scroll
 * \param rect The rectangle to ensure is shown.
 * \return NSERROR_OK on success or apropriate error code.
 */
static nserror
gui_window_set_scroll(struct gui_window *gw, const struct rect *rect)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(gw->browser);

	assert(bwidget);

	widget_scroll_x(gw, rect->x0, true);
	widget_scroll_y(gw, rect->y0, true);

	return NSERROR_OK;
}


/**
 * Find the current dimensions of a framebuffer browser window content area.
 *
 * \param gw The gui window to measure content area of.
 * \param width receives width of window
 * \param height receives height of window
 * \return NSERROR_OK on sucess and width and height updated.
 */
static nserror
gui_window_get_dimensions(struct gui_window *gw, int *width, int *height)
{
	*width = fbtk_get_width(gw->browser);
	*height = fbtk_get_height(gw->browser);

	return NSERROR_OK;
}

static void
gui_window_update_extent(struct gui_window *gw)
{
	int w, h;
	browser_window_get_extents(gw->bw, true, &w, &h);

	fbtk_set_scroll_parameters(gw->hscroll, 0, w,
			fbtk_get_width(gw->browser), 100);

	fbtk_set_scroll_parameters(gw->vscroll, 0, h,
			fbtk_get_height(gw->browser), 100);
}

static void
gui_window_set_status(struct gui_window *g, const char *text)
{
	fbtk_set_text(g->status, text);
}

static void
gui_window_set_pointer(struct gui_window *g, gui_pointer_shape shape)
{
	switch (shape) {
	case GUI_POINTER_POINT:
		framebuffer_set_cursor(&hand_image);
		break;

	case GUI_POINTER_CARET:
		framebuffer_set_cursor(&caret_image);
		break;

	case GUI_POINTER_MENU:
		framebuffer_set_cursor(&menu_image);
		break;

	case GUI_POINTER_PROGRESS:
		framebuffer_set_cursor(&progress_image);
		break;

	case GUI_POINTER_MOVE:
		framebuffer_set_cursor(&move_image);
		break;

	default:
		framebuffer_set_cursor(&pointer_image);
		break;
	}
}

static nserror
gui_window_set_url(struct gui_window *g, nsurl *url)
{
	fbtk_set_text(g->url, nsurl_access(url));
	fb_update_bookmark_button(g);
	return NSERROR_OK;
}

static void
throbber_advance(void *pw)
{
	struct gui_window *g = pw;
	struct fbtk_bitmap *image;

	switch (g->throbber_index) {
	case 0:
		image = &throbber1;
		g->throbber_index = 1;
		break;

	case 1:
		image = &throbber2;
		g->throbber_index = 2;
		break;

	case 2:
		image = &throbber3;
		g->throbber_index = 3;
		break;

	case 3:
		image = &throbber4;
		g->throbber_index = 4;
		break;

	case 4:
		image = &throbber5;
		g->throbber_index = 5;
		break;

	case 5:
		image = &throbber6;
		g->throbber_index = 6;
		break;

	case 6:
		image = &throbber7;
		g->throbber_index = 7;
		break;

	case 7:
		image = &throbber8;
		g->throbber_index = 0;
		break;

	default:
		return;
	}

	if (g->throbber_index >= 0) {
		fbtk_set_bitmap(g->throbber, image);
		framebuffer_schedule(100, throbber_advance, g);
	}
}

/* Headless test hook: NS_SCREENSHOT=file.ppm dumps the screen once the
 * page has finished loading (plus NS_SCREENSHOT_DELAY ms) and exits. */
static void fb_screenshot_cb(void *pw)
{
	const char *path = getenv("NS_SCREENSHOT");
	uint8_t *ptr;
	int stride, w, h, x, y;
	FILE *f;

	fbtk_redraw(fbtk);
	fbtk_redraw(fbtk);
	nsfb_t *nsfb = fbtk_get_nsfb(fbtk);
	nsfb_get_geometry(nsfb, &w, &h, NULL);
	nsfb_get_buffer(nsfb, &ptr, &stride);
	f = fopen(path, "wb");
	if (f != NULL) {
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)(ptr + y * stride);
			for (x = 0; x < w; x++) {
				uint32_t p = row[x];
				fputc((p >> 16) & 0xff, f);
				fputc((p >> 8) & 0xff, f);
				fputc(p & 0xff, f);
			}
		}
		fclose(f);
	}
	fb_complete = true;
}

static void
gui_window_start_throbber(struct gui_window *g)
{
	g->throbber_index = 0;
	framebuffer_schedule(100, throbber_advance, g);
}

static void
gui_window_stop_throbber(struct gui_window *gw)
{
	gw->throbber_index = -1;
	fbtk_set_bitmap(gw->throbber, &throbber0);

	if (getenv("NS_SCREENSHOT") != NULL) {
		const char *d = getenv("NS_SCREENSHOT_DELAY");
		framebuffer_schedule(d ? atoi(d) : 1000, fb_screenshot_cb, NULL);
	}

	fb_update_back_forward(gw);

}

static void
gui_window_remove_caret_cb(fbtk_widget_t *widget)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);
	int c_x, c_y, c_h;

	if (fbtk_get_caret(widget, &c_x, &c_y, &c_h)) {
		/* browser window already had caret:
		 * redraw its area to remove it first */
		fb_queue_redraw(widget,
				c_x - bwidget->scrollx,
				c_y - bwidget->scrolly,
				c_x + 1 - bwidget->scrollx,
				c_y + c_h - bwidget->scrolly);
	}
}

static void
gui_window_place_caret(struct gui_window *g, int x, int y, int height,
		const struct rect *clip)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

#ifdef __SWITCH__
	/* a click just focused an editable field: offer the software
	 * keyboard from the main loop (not from inside this callback) */
	if (fb_kbd_armed_time != 0) {
		uint64_t now;
		nsu_getmonotonic_ms(&now);
		if (now - fb_kbd_armed_time < 500) {
			framebuffer_schedule(50, fb_html_input_kbd_cb, g);
		}
		fb_kbd_armed_time = 0;
	}
#endif

	/* set new pos */
	fbtk_set_caret(g->browser, true, x, y, height,
			gui_window_remove_caret_cb);

	/* redraw new caret pos */
	fb_queue_redraw(g->browser,
			x - bwidget->scrollx,
			y - bwidget->scrolly,
			x + 1 - bwidget->scrollx,
			y + height - bwidget->scrolly);
}

static void
gui_window_remove_caret(struct gui_window *g)
{
	int c_x, c_y, c_h;

	if (fbtk_get_caret(g->browser, &c_x, &c_y, &c_h)) {
		/* browser window owns the caret, so can remove it */
		fbtk_set_caret(g->browser, false, 0, 0, 0, NULL);
	}
}

/**
 * process miscellaneous window events
 *
 * \param gw The window receiving the event.
 * \param event The event code.
 * \return NSERROR_OK when processed ok
 */
static nserror
gui_window_event(struct gui_window *gw, enum gui_window_event event)
{
	switch (event) {
	case GW_EVENT_UPDATE_EXTENT:
		gui_window_update_extent(gw);
		break;

	case GW_EVENT_REMOVE_CARET:
		gui_window_remove_caret(gw);
		break;

	case GW_EVENT_START_THROBBER:
		gui_window_start_throbber(gw);
		break;

	case GW_EVENT_STOP_THROBBER:
		gui_window_stop_throbber(gw);
		break;

	default:
		break;
	}
	return NSERROR_OK;
}

static struct gui_window_table framebuffer_window_table = {
	.create = gui_window_create,
	.destroy = gui_window_destroy,
	.invalidate = fb_window_invalidate_area,
	.get_scroll = gui_window_get_scroll,
	.set_scroll = gui_window_set_scroll,
	.get_dimensions = gui_window_get_dimensions,
	.event = gui_window_event,

	.set_url = gui_window_set_url,
	.set_status = gui_window_set_status,
	.set_pointer = gui_window_set_pointer,
	.place_caret = gui_window_place_caret,
};


static struct gui_misc_table framebuffer_misc_table = {
	.schedule = framebuffer_schedule,

	.quit = gui_quit,
};

/**
 * Entry point from OS.
 *
 * /param argc The number of arguments in the string vector.
 * /param argv The argument string vector.
 * /return The return code to the OS
 */
int
main(int argc, char** argv)
{
	struct browser_window *bw;
	char *options;
	char *messages;
	nsurl *url;
	nserror ret;
	nsfb_t *nsfb;
	struct netsurf_table framebuffer_table = {
		.misc = &framebuffer_misc_table,
#ifdef __SWITCH__
		.download = &framebuffer_download_table,
#endif
		.window = &framebuffer_window_table,
		.corewindow = framebuffer_core_window_table,
		.clipboard = framebuffer_clipboard_table,
		.fetch = framebuffer_fetch_table,
		.utf8 = framebuffer_utf8_table,
		.bitmap = framebuffer_bitmap_table,
		.layout = framebuffer_layout_table,
	};

#ifdef __SWITCH__
	romfsInit();
	socketInitializeDefault();
	mkdir("sdmc:/switch", 0777);
	mkdir("sdmc:/switch/netsurf", 0777);
	mkdir(FB_MEDIA_TMPDIR, 0777);
	/* homebrew has no console; capture diagnostics on the SD card */
	freopen("sdmc:/switch/netsurf/stderr.log", "w", stderr);
	setvbuf(stderr, NULL, _IONBF, 0);
	fprintf(stderr, "netsurf switch build %s %s\n", __DATE__, __TIME__);
#endif

        ret = netsurf_register(&framebuffer_table);
        if (ret != NSERROR_OK) {
		die("NetSurf operation table failed registration");
        }

	respaths = fb_init_resource_path(NETSURF_FB_RESPATH":"NETSURF_FB_FONTPATH);

	/* initialise logging. Not fatal if it fails but not much we
	 * can do about it either.
	 */
	nslog_init(nslog_stream_configure, &argc, argv);

	/* user options setup */
	ret = nsoption_init(set_defaults, &nsoptions, &nsoptions_default);
	if (ret != NSERROR_OK) {
		die("Options failed to initialise");
	}
	options = filepath_find(respaths, "Choices");
	nsoption_read(options, nsoptions);
	free(options);
#ifdef __SWITCH__
	/* user-editable options on the SD card override romfs defaults */
	nsoption_read("sdmc:/switch/netsurf/Choices", nsoptions);
#endif
	nsoption_commandline(&argc, argv, nsoptions);

	/* message init */
	messages = filepath_find(respaths, "Messages");
        ret = messages_add_from_file(messages);
	free(messages);
	if (ret != NSERROR_OK) {
		fprintf(stderr, "Message translations failed to load\n");
	}

	/* common initialisation */
	ret = netsurf_init(NULL);
	if (ret != NSERROR_OK) {
		die("NetSurf failed to initialise");
	}

	/* Override, since we have no support for non-core SELECT menu */
	nsoption_set_bool(core_select_menu, true);

	if (process_cmdline(argc,argv) != true)
		die("unable to process command line.\n");

	nsfb = framebuffer_initialise(fename, fewidth, feheight, febpp);
	/* media, container and viewport-unit evaluation in the CSS preprocessor */
	css_preprocess_set_viewport(fewidth, feheight);
	if (nsfb == NULL)
		die("Unable to initialise framebuffer");

	framebuffer_set_cursor(&pointer_image);

	if (fb_font_init() == false)
		die("Unable to initialise the font system");

	/* let the html core hand fetched @font-face blobs to freetype */
	html_webfont_set_register_cb(fb_font_register_webfont);

	fbtk = fbtk_init(nsfb);

	fbtk_enable_oskb(fbtk);

	urldb_load_cookies(nsoption_charp(cookie_file));

	/* Bookmarks live in an HTML hotlist file on the SD card. This must
	 * come after fb_font_init(): hotlist_init builds a treeview, which
	 * measures its text with the font system. */
	ret = hotlist_init(FB_HOTLIST_PATH, FB_HOTLIST_PATH);
	if (ret != NSERROR_OK) {
		NSLOG(netsurf, INFO, "hotlist init failed: %d", ret);
	}

	/* create an initial browser window */

	NSLOG(netsurf, INFO, "calling browser_window_create");

	ret = nsurl_create(feurl, &url);
	if (ret == NSERROR_OK) {
		ret = browser_window_create(BW_CREATE_HISTORY,
					      url,
					      NULL,
					      NULL,
					      &bw);
		nsurl_unref(url);
	}
	if (ret != NSERROR_OK) {
		fb_warn_user("Errorcode:", messages_get_errorcode(ret));
	} else {
		framebuffer_run();

		browser_window_destroy(bw);
	}

	/* stop any audio and flush bookmarks before the core goes down */
	switch_audio_stop();
	hotlist_fini();

	netsurf_exit();

	if (fb_font_finalise() == false)
		NSLOG(netsurf, INFO, "Font finalisation failed.");

	/* finalise options */
	nsoption_finalise(nsoptions, nsoptions_default);

	/* finalise logging */
	nslog_finalise();

#ifdef __SWITCH__
	socketExit();
	romfsExit();
#endif

	return 0;
}

void gui_resize(fbtk_widget_t *root, int width, int height)
{
	struct gui_window *gw;
	nsfb_t *nsfb = fbtk_get_nsfb(root);

	/* Enforce a minimum */
	if (width < 300)
		width = 300;
	if (height < 200)
		height = 200;

	if (framebuffer_resize(nsfb, width, height, febpp) == false) {
		return;
	}

	fbtk_set_pos_and_size(root, 0, 0, width, height);

	fewidth = width;
	feheight = height;

	for (gw = window_list; gw != NULL; gw = gw->next) {
		resize_normal_browser_window(gw,
				nsoption_int(fb_furniture_size));
	}

	fbtk_request_redraw(root);
}


/*
 * Local Variables:
 * c-basic-offset:8
 * End:
 */
