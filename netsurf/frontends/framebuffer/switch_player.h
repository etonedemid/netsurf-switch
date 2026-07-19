/*
 * Minimal media player for the NetSurf Nintendo Switch port.
 *
 * Plays a URL (http/file) with ffmpeg decode onto the SDL2 renderer that
 * the libnsfb sdl2 surface already owns. Blocks until playback ends or
 * the user backs out (B / Escape / Plus).
 */

#ifndef NETSURF_FB_SWITCH_PLAYER_H
#define NETSURF_FB_SWITCH_PLAYER_H

/* returns 0 on played-to-end or user exit, negative on setup failure */
int switch_player_play_url(const char *url);

/* true if the mime type or url extension looks like playable media */
int switch_player_is_media(const char *mime, const char *url);

#endif
