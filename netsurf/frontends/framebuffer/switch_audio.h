/*
 * Non-blocking audio playback for the NetSurf Switch port.
 *
 * Unlike switch_player (which takes the screen over and blocks until the
 * media ends), this decodes in small slices driven by the browser's own
 * scheduler, so the page stays interactive while audio plays. The UI is
 * an fbtk transport bar built in gui.c.
 */

#ifndef NETSURF_FB_SWITCH_AUDIO_H
#define NETSURF_FB_SWITCH_AUDIO_H

#include <stdbool.h>

/**
 * Begin playing an audio file.
 *
 * \param path   local file to decode (the caller's copy; owned by us and
 *               deleted when playback stops if \a own_file is true)
 * \param title  label for the transport bar (copied)
 * \param own_file delete \a path when playback stops
 * \return true if the stream opened and playback started
 */
bool switch_audio_start(const char *path, const char *title, bool own_file);

/** Stop playback and release everything (safe if not playing). */
void switch_audio_stop(void);

/** Decode a slice and top the audio queue up; call regularly while active. */
void switch_audio_pump(void);

/** Pause/resume. */
void switch_audio_toggle_pause(void);

/** Seek by \a delta seconds, clamped to the file. */
void switch_audio_seek_relative(double delta);

bool switch_audio_active(void);
bool switch_audio_paused(void);

/** Playback position and total length in seconds (duration 0 if unknown). */
double switch_audio_position(void);
double switch_audio_duration(void);

/** Title passed to switch_audio_start, or "" when idle. */
const char *switch_audio_title(void);

#endif
