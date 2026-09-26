/*
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
 *           2008 Vincent Sanders <vince@simtec.co.uk>
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

#include <assert.h>

#include <ft2build.h>
#include FT_CACHE_H

#include <ctype.h>
#include <strings.h>
#include <libwapcaplet/libwapcaplet.h>

#include "netsurf/inttypes.h"
#include "utils/filepath.h"
#include "utils/utf8.h"
#include "utils/log.h"
#include "utils/nsoption.h"
#include "netsurf/utf8.h"
#include "netsurf/layout.h"
#include "netsurf/browser.h"
#include "netsurf/plot_style.h"

#include "framebuffer/gui.h"
#include "framebuffer/font.h"
#include "framebuffer/findfile.h"

/* glyph cache minimum size */
#define CACHE_MIN_SIZE (100 * 1024)

#define BOLD_WEIGHT 700

static FT_Library library; 
static FTC_Manager ft_cmanager;
static FTC_CMapCache ft_cmap_cache ;
static FTC_ImageCache ft_image_cache;

int ft_load_type;

/* cache manager faceID data to create freetype faceid on demand */
typedef struct fb_faceid_s {
        char *fontfile; /* path to font */
        int index; /* index of font */
        int cidx; /* character map index for unicode */
        const uint8_t *mem; /* in-memory font (webfont); overrides fontfile */
        size_t mem_len;
} fb_faceid_t;


enum fb_face_e {
	FB_FACE_SANS_SERIF = 0,
	FB_FACE_SANS_SERIF_BOLD,
	FB_FACE_SANS_SERIF_ITALIC,
	FB_FACE_SANS_SERIF_ITALIC_BOLD,
	FB_FACE_SERIF,
	FB_FACE_SERIF_BOLD,
	FB_FACE_MONOSPACE,
	FB_FACE_MONOSPACE_BOLD,
	FB_FACE_CURSIVE,
	FB_FACE_FANTASY,
	FB_FACE_COUNT
};

/* defines for accesing the faces */
#define FB_FACE_DEFAULT 0

static fb_faceid_t *fb_faces[FB_FACE_COUNT];

/*
 * Family-name handling.
 *
 * The plot style carries the actual CSS font-family names; map well-known
 * names onto our generic faces, and registered webfonts (in-memory faces)
 * onto their own freetype faces. Webfont *fetching* is not wired up yet;
 * fb_font_register_webfont() is the entry point for it.
 */

#define FB_WEBFONT_MAX 16

struct fb_webfont {
	char *family; /* lowercased family name */
	fb_faceid_t *face;
};

static struct fb_webfont fb_webfonts[FB_WEBFONT_MAX];
static int fb_webfont_count;

struct fb_family_alias {
	const char *name;
	plot_font_generic_family_t family;
};

static const struct fb_family_alias fb_family_aliases[] = {
	{ "arial", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "helvetica", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "helvetica neue", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "verdana", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "tahoma", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "segoe ui", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "roboto", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "open sans", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "lato", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "noto sans", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "system-ui", PLOT_FONT_FAMILY_SANS_SERIF },
	{ "times", PLOT_FONT_FAMILY_SERIF },
	{ "times new roman", PLOT_FONT_FAMILY_SERIF },
	{ "georgia", PLOT_FONT_FAMILY_SERIF },
	{ "garamond", PLOT_FONT_FAMILY_SERIF },
	{ "palatino", PLOT_FONT_FAMILY_SERIF },
	{ "courier", PLOT_FONT_FAMILY_MONOSPACE },
	{ "courier new", PLOT_FONT_FAMILY_MONOSPACE },
	{ "consolas", PLOT_FONT_FAMILY_MONOSPACE },
	{ "menlo", PLOT_FONT_FAMILY_MONOSPACE },
	{ "monaco", PLOT_FONT_FAMILY_MONOSPACE },
	{ "source code pro", PLOT_FONT_FAMILY_MONOSPACE },
	{ "comic sans ms", PLOT_FONT_FAMILY_CURSIVE },
	{ "impact", PLOT_FONT_FAMILY_FANTASY },
	{ NULL, 0 }
};

static bool
fb_name_matches(const lwc_string *lwcname, const char *name)
{
	size_t len = lwc_string_length((lwc_string *)lwcname);
	const char *data = lwc_string_data((lwc_string *)lwcname);

	return (strlen(name) == len) && (strncasecmp(data, name, len) == 0);
}

/* look up a registered webfont face for any of the style's families */
static fb_faceid_t *
fb_webfont_lookup(lwc_string * const *families)
{
	int i, f;

	for (f = 0; families[f] != NULL; f++) {
		for (i = 0; i < fb_webfont_count; i++) {
			if (fb_name_matches(families[f],
					    fb_webfonts[i].family)) {
				return fb_webfonts[i].face;
			}
		}
	}
	return NULL;
}

/* map family names to a generic family via the alias table */
static plot_font_generic_family_t
fb_alias_lookup(lwc_string * const *families,
		plot_font_generic_family_t fallback)
{
	int i, f;

	for (f = 0; families[f] != NULL; f++) {
		for (i = 0; fb_family_aliases[i].name != NULL; i++) {
			if (fb_name_matches(families[f],
					    fb_family_aliases[i].name)) {
				return fb_family_aliases[i].family;
			}
		}
	}
	return fallback;
}

/* exported interface: register an in-memory font under a family name.
 * Takes ownership of nothing; data must stay valid for the process life.
 * Returns false (caller keeps ownership of data) if the registry is
 * full, the family is already registered, or freetype cannot open the
 * blob (e.g. a woff2 file; only sfnt and woff are supported). */
bool
fb_font_register_webfont(const char *family,
			 const uint8_t *data, size_t data_len)
{
	fb_faceid_t *newf;
	char *family_lc;
	FT_Face probe;
	size_t i;

	if (fb_webfont_count >= FB_WEBFONT_MAX || library == NULL)
		return false;

	/* first registration of a family wins */
	for (i = 0; i < (size_t)fb_webfont_count; i++) {
		if (strcasecmp(fb_webfonts[i].family, family) == 0)
			return false;
	}

	/* probe the blob so a broken font cannot enter the registry */
	if (FT_New_Memory_Face(library, data, data_len, 0, &probe) != 0) {
		NSLOG(netsurf, INFO,
		      "webfont family '%s' unusable (%zu bytes)",
		      family, data_len);
		return false;
	}
	FT_Done_Face(probe);

	family_lc = strdup(family);
	if (family_lc == NULL)
		return false;
	for (i = 0; family_lc[i]; i++)
		family_lc[i] = tolower((unsigned char)family_lc[i]);

	newf = calloc(1, sizeof(fb_faceid_t));
	if (newf == NULL) {
		free(family_lc);
		return false;
	}
	newf->mem = data;
	newf->mem_len = data_len;
	newf->fontfile = strdup(family); /* for diagnostics only */

	fb_webfonts[fb_webfont_count].family = family_lc;
	fb_webfonts[fb_webfont_count].face = newf;
	fb_webfont_count++;

	NSLOG(netsurf, INFO, "registered webfont family '%s' (%zu bytes)",
	      family, data_len);
	return true;
}

/**
 * map cache manager handle to face id
 */
static FT_Error
ft_face_requester(FTC_FaceID face_id,
		  FT_Library  library,
		  FT_Pointer request_data,
		  FT_Face *face )
{
        FT_Error error;
        fb_faceid_t *fb_face = (fb_faceid_t *)face_id;
        int cidx;

        if (fb_face->mem != NULL) {
                error = FT_New_Memory_Face(library, fb_face->mem,
                                           fb_face->mem_len,
                                           fb_face->index, face);
        } else {
                error = FT_New_Face(library, fb_face->fontfile,
                                    fb_face->index, face);
        }
        if (error) {
                NSLOG(netsurf, INFO, "Could not find font (code %d)", error);
        } else {

                error = FT_Select_Charmap(*face, FT_ENCODING_UNICODE);
                if (error) {
                        NSLOG(netsurf, INFO,
                              "Could not select charmap (code %d)", error);
                } else {
                        for (cidx = 0; cidx < (*face)->num_charmaps; cidx++) {
                                if ((*face)->charmap == (*face)->charmaps[cidx]) {
                                        fb_face->cidx = cidx;
                                        break;
                                }
                        }
                }
        }
        NSLOG(netsurf, INFO, "Loaded face from %s", fb_face->fontfile);

        return error;
}

/**
 * create new framebuffer face and cause it to be loaded to check its ok
 */
static fb_faceid_t *
fb_new_face(const char *option, const char *resname, const char *fontname)
{
        fb_faceid_t *newf;
        FT_Error error;
        FT_Face aface;
	char buf[PATH_MAX];

        newf = calloc(1, sizeof(fb_faceid_t));

        if (option != NULL) {
                newf->fontfile = strdup(option);
        } else {
		filepath_sfind(respaths, buf, fontname);
                newf->fontfile = strdup(buf);
        }

        error = FTC_Manager_LookupFace(ft_cmanager, (FTC_FaceID)newf, &aface);
        if (error) {
                NSLOG(netsurf, INFO, "Could not find font face %s (code %d)",
                      fontname, error);
                free(newf->fontfile);
                free(newf);
                newf = NULL;
        }

        return newf;
}

/* exported interface documented in framebuffer/font.h */
bool fb_font_init(void)
{
        FT_Error error;
        FT_ULong max_cache_size;
        FT_UInt max_faces = 6;
	fb_faceid_t *fb_face;

        /* freetype library initialise */
        error = FT_Init_FreeType( &library ); 
        if (error) {
                NSLOG(netsurf, INFO,
                      "Freetype could not initialised (code %d)", error);
                return false;
        }

        /* set the Glyph cache size up */
        max_cache_size = nsoption_int(fb_font_cachesize) * 1024; 

	if (max_cache_size < CACHE_MIN_SIZE) {
		max_cache_size = CACHE_MIN_SIZE;
	}

        /* cache manager initialise */
        error = FTC_Manager_New(library, 
                                max_faces, 
                                0, 
                                max_cache_size, 
                                ft_face_requester, 
                                NULL, 
                                &ft_cmanager);
        if (error) {
                NSLOG(netsurf, INFO,
                      "Freetype could not initialise cache manager (code %d)",
                      error);
                FT_Done_FreeType(library);
                return false;
        }

        error = FTC_CMapCache_New(ft_cmanager, &ft_cmap_cache);

        error = FTC_ImageCache_New(ft_cmanager, &ft_image_cache);

	/* need to obtain the generic font faces */

	/* Start with the sans serif font */
	fb_face = fb_new_face(nsoption_charp(fb_face_sans_serif),
			      "sans_serif.ttf",
			      NETSURF_FB_FONT_SANS_SERIF);
	if (fb_face == NULL) {
		/* The sans serif font is the default and must be found. */
                NSLOG(netsurf, INFO, "Could not find the default font");
                FTC_Manager_Done(ft_cmanager);
                FT_Done_FreeType(library);
                return false;
        } else {
		fb_faces[FB_FACE_SANS_SERIF] = fb_face;
	}

	/* Bold sans serif face */
	fb_face = fb_new_face(nsoption_charp(fb_face_sans_serif_bold),
                            "sans_serif_bold.ttf",
                            NETSURF_FB_FONT_SANS_SERIF_BOLD);
	if (fb_face == NULL) {
		/* seperate bold face unavailabe use the normal weight version */
		fb_faces[FB_FACE_SANS_SERIF_BOLD] = fb_faces[FB_FACE_SANS_SERIF];
	} else {
		fb_faces[FB_FACE_SANS_SERIF_BOLD] = fb_face;
	}

	/* Italic sans serif face */
	fb_face = fb_new_face(nsoption_charp(fb_face_sans_serif_italic),
			      "sans_serif_italic.ttf",
			      NETSURF_FB_FONT_SANS_SERIF_ITALIC);
	if (fb_face == NULL) {
		/* seperate italic face unavailabe use the normal weight version */
		fb_faces[FB_FACE_SANS_SERIF_ITALIC] = fb_faces[FB_FACE_SANS_SERIF];
	} else {
		fb_faces[FB_FACE_SANS_SERIF_ITALIC] = fb_face;
	}

	/* Bold italic sans serif face */
	fb_face = fb_new_face(nsoption_charp(fb_face_sans_serif_italic_bold), 
			      "sans_serif_italic_bold.ttf",
			      NETSURF_FB_FONT_SANS_SERIF_ITALIC_BOLD);
	if (fb_face == NULL) {
		/* seperate italic face unavailabe use the normal weight version */
		fb_faces[FB_FACE_SANS_SERIF_ITALIC_BOLD] = fb_faces[FB_FACE_SANS_SERIF];
	} else {
		fb_faces[FB_FACE_SANS_SERIF_ITALIC_BOLD] = fb_face;
	}

	/* serif face */
	fb_face = fb_new_face(nsoption_charp(fb_face_serif),
                            "serif.ttf",
			      NETSURF_FB_FONT_SERIF);
	if (fb_face == NULL) {
		/* serif face unavailabe use the default */
		fb_faces[FB_FACE_SERIF] = fb_faces[FB_FACE_SANS_SERIF];
	} else {
		fb_faces[FB_FACE_SERIF] = fb_face;
	}

	/* bold serif face*/
	fb_face = fb_new_face(nsoption_charp(fb_face_serif_bold),
			      "serif_bold.ttf",
			      NETSURF_FB_FONT_SERIF_BOLD);
	if (fb_face == NULL) {
		/* bold serif face unavailabe use the normal weight */
		fb_faces[FB_FACE_SERIF_BOLD] = fb_faces[FB_FACE_SERIF];
	} else {
		fb_faces[FB_FACE_SERIF_BOLD] = fb_face;
	}


	/* monospace face */
	fb_face = fb_new_face(nsoption_charp(fb_face_monospace),
			      "monospace.ttf",
			      NETSURF_FB_FONT_MONOSPACE);
	if (fb_face == NULL) {
		/* serif face unavailabe use the default */
		fb_faces[FB_FACE_MONOSPACE] = fb_faces[FB_FACE_SANS_SERIF];
	} else {
		fb_faces[FB_FACE_MONOSPACE] = fb_face;
	}

	/* bold monospace face*/
	fb_face = fb_new_face(nsoption_charp(fb_face_monospace_bold),
			      "monospace_bold.ttf",
			      NETSURF_FB_FONT_MONOSPACE_BOLD);
	if (fb_face == NULL) {
		/* bold serif face unavailabe use the normal weight */
		fb_faces[FB_FACE_MONOSPACE_BOLD] = fb_faces[FB_FACE_MONOSPACE];
	} else {
		fb_faces[FB_FACE_MONOSPACE_BOLD] = fb_face;
	}

	/* cursive face */
	fb_face = fb_new_face(nsoption_charp(fb_face_cursive),
			      "cursive.ttf",
			      NETSURF_FB_FONT_CURSIVE);
	if (fb_face == NULL) {
		/* cursive face unavailabe use the default */
		fb_faces[FB_FACE_CURSIVE] = fb_faces[FB_FACE_SANS_SERIF];
	} else {
		fb_faces[FB_FACE_CURSIVE] = fb_face;
	}

	/* fantasy face */
	fb_face = fb_new_face(nsoption_charp(fb_face_fantasy),
			      "fantasy.ttf",
			      NETSURF_FB_FONT_FANTASY);
	if (fb_face == NULL) {
		/* fantasy face unavailabe use the default */
		fb_faces[FB_FACE_FANTASY] = fb_faces[FB_FACE_SANS_SERIF];
	} else {
		fb_faces[FB_FACE_FANTASY] = fb_face;
	}

        
        /* set the default render mode */
        if (nsoption_bool(fb_font_monochrome) == true)
                ft_load_type = FT_LOAD_MONOCHROME; /* faster but less pretty */
        else
                ft_load_type = 0;
        
        return true;
}

/* exported interface documented in framebuffer/font.h */
bool fb_font_finalise(void)
{
	int i, j;

        FTC_Manager_Done(ft_cmanager);
        FT_Done_FreeType(library);

	for (i = 0; i < FB_FACE_COUNT; i++) {
		if (fb_faces[i] == NULL)
			continue;

		/* Unset any faces that duplicate this one */
		for (j = i + 1; j < FB_FACE_COUNT; j++) {
			if (fb_faces[i] == fb_faces[j])
				fb_faces[j] = NULL;
		}

		free(fb_faces[i]->fontfile);
		free(fb_faces[i]);

		fb_faces[i] = NULL;
	}

        return true;
}

/**
 * fill freetype scalar
 */
static void fb_fill_scalar(const plot_font_style_t *fstyle, FTC_Scaler srec)
{
        int selected_face = FB_FACE_DEFAULT;
        plot_font_generic_family_t family = fstyle->family;

	if (fstyle->families != NULL) {
		/* registered webfont face wins outright */
		fb_faceid_t *wf = fb_webfont_lookup(fstyle->families);
		if (wf != NULL) {
			srec->face_id = (FTC_FaceID)wf;
			srec->width = srec->height =
				(fstyle->size * 64) / PLOT_STYLE_SCALE;
			srec->pixel = 0;
			srec->x_res = srec->y_res = browser_get_dpi();
			return;
		}
		/* otherwise honour well-known family names */
		family = fb_alias_lookup(fstyle->families, family);
	}

	switch (family) {
                                
	case PLOT_FONT_FAMILY_SERIF:
		if (fstyle->weight >= BOLD_WEIGHT) {
                        selected_face = FB_FACE_SERIF_BOLD;
		} else {
                        selected_face = FB_FACE_SERIF;
                }
		break;

	case PLOT_FONT_FAMILY_MONOSPACE:
		if (fstyle->weight >= BOLD_WEIGHT) {
			selected_face = FB_FACE_MONOSPACE_BOLD;
		} else {
			selected_face = FB_FACE_MONOSPACE;
                }
		break;

	case PLOT_FONT_FAMILY_CURSIVE:
                selected_face = FB_FACE_CURSIVE;
		break;

	case PLOT_FONT_FAMILY_FANTASY:
                selected_face = FB_FACE_FANTASY;
		break;

	case PLOT_FONT_FAMILY_SANS_SERIF:
	default:
		if ((fstyle->flags & FONTF_ITALIC) || 
		    (fstyle->flags & FONTF_OBLIQUE)) {
			if (fstyle->weight >= BOLD_WEIGHT) {
                                selected_face = FB_FACE_SANS_SERIF_ITALIC_BOLD;
			} else {
                                selected_face = FB_FACE_SANS_SERIF_ITALIC;
			}
		} else {
			if (fstyle->weight >= BOLD_WEIGHT) {
                                selected_face = FB_FACE_SANS_SERIF_BOLD;
                        } else {
                                selected_face = FB_FACE_SANS_SERIF;
			}
                }
	}

        srec->face_id = (FTC_FaceID)fb_faces[selected_face];

	srec->width = srec->height = (fstyle->size * 64) / PLOT_STYLE_SCALE;
	srec->pixel = 0;

	srec->x_res = srec->y_res = browser_get_dpi();
}

/* exported interface documented in framebuffer/freetype_font.h */
FT_Glyph fb_getglyph(const plot_font_style_t *fstyle, uint32_t ucs4)
{
        FT_UInt glyph_index;
        FTC_ScalerRec srec;
        FT_Glyph glyph;
        FT_Error error;
        fb_faceid_t *fb_face; 

        fb_fill_scalar(fstyle, &srec);

        fb_face = (fb_faceid_t *)srec.face_id;

        glyph_index = FTC_CMapCache_Lookup(ft_cmap_cache, srec.face_id,
			fb_face->cidx, ucs4);

        error = FTC_ImageCache_LookupScaler(ft_image_cache, 
                                            &srec, 
                                            FT_LOAD_RENDER | 
                                            FT_LOAD_FORCE_AUTOHINT | 
                                            ft_load_type, 
                                            glyph_index, 
                                            &glyph, 
                                            NULL);
	if (error != 0)
		return NULL;

        return glyph;
}


/* exported interface documented in framebuffer/freetype_font.h */
nserror
fb_font_width(const plot_font_style_t *fstyle,
                         const char *string, size_t length,
                         int *width)
{
        uint32_t ucs4;
        size_t nxtchr = 0;
        FT_Glyph glyph;

        *width = 0;
        while (nxtchr < length) {
                ucs4 = utf8_to_ucs4(string + nxtchr, length - nxtchr);
                nxtchr = utf8_next(string, length, nxtchr);

                glyph = fb_getglyph(fstyle, ucs4);
                if (glyph == NULL)
                        continue;

                *width += glyph->advance.x >> 16;
        }
	return NSERROR_OK;
}


/* exported interface documented in framebuffer/freetype_font.h */
nserror
fb_font_position(const plot_font_style_t *fstyle,
		const char *string, size_t length,
		int x, size_t *char_offset, int *actual_x)
{
        uint32_t ucs4;
        size_t nxtchr = 0;
        FT_Glyph glyph;
        int prev_x = 0;

        *actual_x = 0;
        while (nxtchr < length) {
                ucs4 = utf8_to_ucs4(string + nxtchr, length - nxtchr);

                glyph = fb_getglyph(fstyle, ucs4);
                if (glyph == NULL)
                        continue;

                *actual_x += glyph->advance.x >> 16;
                if (*actual_x > x)
                        break;

                prev_x = *actual_x;
                nxtchr = utf8_next(string, length, nxtchr);
        }

        /* choose nearest of previous and last x */
        if (abs(*actual_x - x) > abs(prev_x - x))
                *actual_x = prev_x;

        *char_offset = nxtchr;
	return NSERROR_OK;
}


/**
 * Find where to split a string to make it fit a width.
 *
 * \param  fstyle       style for this text
 * \param  string       UTF-8 string to measure
 * \param  length       length of string, in bytes
 * \param  x            width available
 * \param  char_offset  updated to offset in string of actual_x, [1..length]
 * \param  actual_x     updated to x coordinate of character closest to x
 * \return  true on success, false on error and error reported
 *
 * On exit, char_offset indicates first character after split point.
 *
 * Note: char_offset of 0 should never be returned.
 *
 *   Returns:
 *     char_offset giving split point closest to x, where actual_x <= x
 *   else
 *     char_offset giving split point closest to x, where actual_x > x
 *
 * Returning char_offset == length means no split possible
 */
static nserror
fb_font_split(const plot_font_style_t *fstyle,
		const char *string, size_t length,
		int x, size_t *char_offset, int *actual_x)
{
        uint32_t ucs4;
        size_t nxtchr = 0;
        int last_space_x = 0;
        int last_space_idx = 0;
        FT_Glyph glyph;

        *actual_x = 0;
        while (nxtchr < length) {
                ucs4 = utf8_to_ucs4(string + nxtchr, length - nxtchr);

                glyph = fb_getglyph(fstyle, ucs4);
                if (glyph == NULL)
                        continue;

                if (ucs4 == 0x20) {
                        last_space_x = *actual_x;
                        last_space_idx = nxtchr;
                }

                *actual_x += glyph->advance.x >> 16;
                if (*actual_x > x && last_space_idx != 0) {
                        /* string has exceeded available width and we've
                         * found a space; return previous space */
                        *actual_x = last_space_x;
                        *char_offset = last_space_idx;
                        return NSERROR_OK;
                }

                nxtchr = utf8_next(string, length, nxtchr);
        }

        *char_offset = nxtchr;

	return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
	.width = fb_font_width,
	.position = fb_font_position,
	.split = fb_font_split,
};

struct gui_layout_table *framebuffer_layout_table = &layout_table;


struct gui_utf8_table *framebuffer_utf8_table = NULL;

/*
 * Local Variables:
 * c-basic-offset:8
 * End:
 */

/* ------------------------------------------------------------------ */
/* Canvas text                                                        */
/* ------------------------------------------------------------------ */

#include "netsurf/canvas.h"

static bool fb_canvas_fill_text(uint8_t *px, int w, int h, size_t stride,
		const struct plot_font_style *fs, int x, int y,
		const char *text, size_t len, uint32_t colour, float alpha,
		const int clip[4])
{
	size_t nxt = 0;
	int cr = colour & 0xff, cg = (colour >> 8) & 0xff;
	int cb = (colour >> 16) & 0xff;
	int cx0 = clip[0] > 0 ? clip[0] : 0, cy0 = clip[1] > 0 ? clip[1] : 0;
	int cx1 = clip[2] < w ? clip[2] : w, cy1 = clip[3] < h ? clip[3] : h;

	while (nxt < len) {
		uint32_t ucs4 = utf8_to_ucs4(text + nxt, len - nxt);
		FT_Glyph glyph;

		nxt = utf8_next(text, len, nxt);
		glyph = fb_getglyph(fs, ucs4);
		if (glyph == NULL)
			continue;
		if (glyph->format == FT_GLYPH_FORMAT_BITMAP) {
			FT_BitmapGlyph bg = (FT_BitmapGlyph)glyph;
			int gx = x + bg->left, gy = y - bg->top, row, col;
			for (row = 0; row < (int)bg->bitmap.rows; row++) {
				int py = gy + row;
				if (py < cy0 || py >= cy1)
					continue;
				for (col = 0; col < (int)bg->bitmap.width; col++) {
					int pxx = gx + col, cov;
					uint8_t *d;
					float sa, da, oa;
					if (pxx < cx0 || pxx >= cx1)
						continue;
					if (bg->bitmap.pixel_mode ==
							FT_PIXEL_MODE_MONO)
						cov = (bg->bitmap.buffer[row *
							bg->bitmap.pitch + col / 8] &
							(0x80 >> (col & 7))) ? 255 : 0;
					else
						cov = bg->bitmap.buffer[row *
							bg->bitmap.pitch + col];
					if (cov == 0)
						continue;
					d = px + py * stride + pxx * 4;
					sa = cov / 255.0f * alpha;
					da = d[3] / 255.0f;
					oa = sa + da * (1 - sa);
					if (oa <= 0)
						continue;
					d[0] = (uint8_t)((cr * sa + d[0] * da *
						(1 - sa)) / oa + 0.5f);
					d[1] = (uint8_t)((cg * sa + d[1] * da *
						(1 - sa)) / oa + 0.5f);
					d[2] = (uint8_t)((cb * sa + d[2] * da *
						(1 - sa)) / oa + 0.5f);
					d[3] = (uint8_t)(oa * 255 + 0.5f);
				}
			}
		}
		x += glyph->advance.x >> 16;
	}
	return true;
}

static struct gui_canvas_table canvas_table = {
	.fill_text = fb_canvas_fill_text,
};

struct gui_canvas_table *framebuffer_canvas_table = &canvas_table;
