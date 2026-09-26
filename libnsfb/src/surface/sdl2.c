/*
 * Copyright 2026 NetSurf Switch port contributors
 *
 * This file is part of libnsfb, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 *
 * SDL2 surface driver. Written for the Nintendo Switch (libnx) port but
 * kept portable so desktop builds can use it for testing. Renders the
 * framebuffer through a streaming texture and translates SDL2 events
 * (keyboard, mouse, touch, game controller) into nsfb events. The game
 * controller's left stick drives an emulated pointer, the right stick
 * emits scroll-wheel events.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"

#include "nsfb.h"
#include "surface.h"
#include "palette.h"
#include "plot.h"
#include "cursor.h"

#define SDL2_PENDING_MAX 16

/* active renderer, exposed so a frontend can present its own content
 * (e.g. the Switch port's media player) on the surface's output */
static SDL_Renderer *sdl2_active_renderer;

void *nsfb_sdl2_get_renderer(void)
{
    return sdl2_active_renderer;
}

#define STICK_DEADZONE 10000
#define STICK_CURSOR_PXPS 900.0f /* full deflection cursor speed, px/sec */
#define STICK_SCROLL_MS 90       /* interval between synthetic wheel ticks */
#define STICK_MOVE_EMIT_MS 8     /* min interval between synthetic moves */
#define TRIGGER_THRESHOLD 16000  /* analog trigger press threshold */

typedef struct sdl2_priv_s {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint8_t *fb;
    int fb_pitch;

    SDL_GameController *pad;
    SDL_Joystick *joy; /* raw fallback when no controller mapping exists */

    /* queue for synthesized event sequences (touch tap = move+press etc) */
    nsfb_event_t pending[SDL2_PENDING_MAX];
    int pending_head;
    int pending_count;

    /* emulated pointer state for stick control */
    bool present_pending; /* texture updated since the last present */
    float ptr_x;
    float ptr_y;
    uint32_t last_motion_ticks;
    uint32_t last_scroll_ticks;
    uint32_t last_emit_ticks;
    int last_emit_x;
    int last_emit_y;
    bool ptr_dirty;

    /* trigger edge state: ZL = context click, ZR = primary click */
    bool zl_down;
    bool zr_down;
} sdl2_priv_t;

static void sdl2_queue_push(sdl2_priv_t *priv, const nsfb_event_t *ev)
{
    int slot;
    if (priv->pending_count >= SDL2_PENDING_MAX)
        return;
    slot = (priv->pending_head + priv->pending_count) % SDL2_PENDING_MAX;
    priv->pending[slot] = *ev;
    priv->pending_count++;
}

static bool sdl2_queue_pop(sdl2_priv_t *priv, nsfb_event_t *ev)
{
    if (priv->pending_count == 0)
        return false;
    *ev = priv->pending[priv->pending_head];
    priv->pending_head = (priv->pending_head + 1) % SDL2_PENDING_MAX;
    priv->pending_count--;
    return true;
}

static void sdl2_queue_key(sdl2_priv_t *priv, enum nsfb_event_type_e type,
                           enum nsfb_key_code_e code)
{
    nsfb_event_t ev;
    ev.type = type;
    ev.value.keycode = code;
    sdl2_queue_push(priv, &ev);
}

static void sdl2_queue_move(sdl2_priv_t *priv, int x, int y)
{
    nsfb_event_t ev;
    ev.type = NSFB_EVENT_MOVE_ABSOLUTE;
    ev.value.vector.x = x;
    ev.value.vector.y = y;
    ev.value.vector.z = 0;
    sdl2_queue_push(priv, &ev);
}

/* Map SDL2 keycodes to nsfb key codes. Printable ASCII maps directly. */
static enum nsfb_key_code_e sdl2_translate_key(SDL_Keycode sym)
{
    if (sym >= SDLK_SPACE && sym <= SDLK_z)
        return (enum nsfb_key_code_e)sym;

    switch (sym) {
    case SDLK_BACKSPACE:    return NSFB_KEY_BACKSPACE;
    case SDLK_TAB:          return NSFB_KEY_TAB;
    case SDLK_RETURN:       return NSFB_KEY_RETURN;
    case SDLK_ESCAPE:       return NSFB_KEY_ESCAPE;
    case SDLK_DELETE:       return NSFB_KEY_DELETE;
    case SDLK_UP:           return NSFB_KEY_UP;
    case SDLK_DOWN:         return NSFB_KEY_DOWN;
    case SDLK_RIGHT:        return NSFB_KEY_RIGHT;
    case SDLK_LEFT:         return NSFB_KEY_LEFT;
    case SDLK_INSERT:       return NSFB_KEY_INSERT;
    case SDLK_HOME:         return NSFB_KEY_HOME;
    case SDLK_END:          return NSFB_KEY_END;
    case SDLK_PAGEUP:       return NSFB_KEY_PAGEUP;
    case SDLK_PAGEDOWN:     return NSFB_KEY_PAGEDOWN;
    case SDLK_F1:           return NSFB_KEY_F1;
    case SDLK_F2:           return NSFB_KEY_F2;
    case SDLK_F3:           return NSFB_KEY_F3;
    case SDLK_F4:           return NSFB_KEY_F4;
    case SDLK_F5:           return NSFB_KEY_F5;
    case SDLK_F6:           return NSFB_KEY_F6;
    case SDLK_F7:           return NSFB_KEY_F7;
    case SDLK_F8:           return NSFB_KEY_F8;
    case SDLK_F9:           return NSFB_KEY_F9;
    case SDLK_F10:          return NSFB_KEY_F10;
    case SDLK_F11:          return NSFB_KEY_F11;
    case SDLK_F12:          return NSFB_KEY_F12;
    case SDLK_RSHIFT:       return NSFB_KEY_RSHIFT;
    case SDLK_LSHIFT:       return NSFB_KEY_LSHIFT;
    case SDLK_RCTRL:        return NSFB_KEY_RCTRL;
    case SDLK_LCTRL:        return NSFB_KEY_LCTRL;
    case SDLK_RALT:         return NSFB_KEY_RALT;
    case SDLK_LALT:         return NSFB_KEY_LALT;
    case SDLK_KP_0:         return NSFB_KEY_KP0;
    case SDLK_KP_1:         return NSFB_KEY_KP1;
    case SDLK_KP_2:         return NSFB_KEY_KP2;
    case SDLK_KP_3:         return NSFB_KEY_KP3;
    case SDLK_KP_4:         return NSFB_KEY_KP4;
    case SDLK_KP_5:         return NSFB_KEY_KP5;
    case SDLK_KP_6:         return NSFB_KEY_KP6;
    case SDLK_KP_7:         return NSFB_KEY_KP7;
    case SDLK_KP_8:         return NSFB_KEY_KP8;
    case SDLK_KP_9:         return NSFB_KEY_KP9;
    case SDLK_KP_ENTER:     return NSFB_KEY_KP_ENTER;
    default:                return NSFB_KEY_UNKNOWN;
    }
}

static int sdl2_update(nsfb_t *nsfb, nsfb_bbox_t *box);
static void sdl2_flush_present(sdl2_priv_t *priv);

/* push the current emulated pointer position as a move event */
static void sdl2_flush_pointer(nsfb_t *nsfb, sdl2_priv_t *priv)
{
    int x = (int)priv->ptr_x;
    int y = (int)priv->ptr_y;

    if (x < 0) { x = 0; priv->ptr_x = 0; }
    if (y < 0) { y = 0; priv->ptr_y = 0; }
    if (x >= nsfb->width) { x = nsfb->width - 1; priv->ptr_x = (float)x; }
    if (y >= nsfb->height) { y = nsfb->height - 1; priv->ptr_y = (float)y; }

    priv->ptr_dirty = false;

    /* only emit an actual movement; a stream of no-op moves starves the
     * fbtk event loop (it consolidates moves with timeout=0 forever) */
    if (x == priv->last_emit_x && y == priv->last_emit_y)
        return;

    sdl2_queue_move(priv, x, y);
    priv->last_emit_x = x;
    priv->last_emit_y = y;
}

/* integrate game controller sticks; returns true if pointer is active */
static bool sdl2_pump_sticks(nsfb_t *nsfb, sdl2_priv_t *priv)
{
    Sint16 lx, ly, ry;
    uint32_t now;
    float dt;
    bool active = false;

    if (priv->pad == NULL && priv->joy == NULL)
        return false;

    now = SDL_GetTicks();
    dt = (now - priv->last_motion_ticks) / 1000.0f;
    if (dt > 0.1f)
        dt = 0.1f;

    if (priv->pad != NULL) {
        lx = SDL_GameControllerGetAxis(priv->pad, SDL_CONTROLLER_AXIS_LEFTX);
        ly = SDL_GameControllerGetAxis(priv->pad, SDL_CONTROLLER_AXIS_LEFTY);
        ry = SDL_GameControllerGetAxis(priv->pad, SDL_CONTROLLER_AXIS_RIGHTY);
    } else {
        lx = SDL_JoystickGetAxis(priv->joy, 0);
        ly = SDL_JoystickGetAxis(priv->joy, 1);
        ry = SDL_JoystickGetAxis(priv->joy, 3);
    }

    if (abs(lx) > STICK_DEADZONE || abs(ly) > STICK_DEADZONE) {
        priv->ptr_x += (lx / 32767.0f) * STICK_CURSOR_PXPS * dt *
            (nsfb->width / 1280.0f);
        priv->ptr_y += (ly / 32767.0f) * STICK_CURSOR_PXPS * dt *
            (nsfb->width / 1280.0f);
        priv->ptr_dirty = true;
        active = true;
    }

    if (abs(ry) > STICK_DEADZONE * 2) {
        if (now - priv->last_scroll_ticks > STICK_SCROLL_MS) {
            enum nsfb_key_code_e wheel =
                (ry < 0) ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
            sdl2_queue_key(priv, NSFB_EVENT_KEY_DOWN, wheel);
            sdl2_queue_key(priv, NSFB_EVENT_KEY_UP, wheel);
            priv->last_scroll_ticks = now;
        }
        active = true;
    }

    /* trigger edges: ZR = primary click, ZL = context click */
    {
        bool zl, zr;

        if (priv->pad != NULL) {
            zl = SDL_GameControllerGetAxis(priv->pad,
                    SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TRIGGER_THRESHOLD;
            zr = SDL_GameControllerGetAxis(priv->pad,
                    SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_THRESHOLD;
        } else {
            zl = SDL_JoystickGetButton(priv->joy, 8) != 0; /* ZL */
            zr = SDL_JoystickGetButton(priv->joy, 9) != 0; /* ZR */
        }

        if (zr != priv->zr_down) {
            priv->zr_down = zr;
            if (zr)
                sdl2_queue_move(priv, (int)priv->ptr_x, (int)priv->ptr_y);
            sdl2_queue_key(priv,
                           zr ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP,
                           NSFB_KEY_MOUSE_1);
            active = true;
        }
        if (zl != priv->zl_down) {
            priv->zl_down = zl;
            if (zl)
                sdl2_queue_move(priv, (int)priv->ptr_x, (int)priv->ptr_y);
            sdl2_queue_key(priv,
                           zl ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP,
                           NSFB_KEY_MOUSE_3);
            active = true;
        }
    }

    priv->last_motion_ticks = now;

    /* rate-limit synthetic moves so consolidation loops always terminate */
    if (priv->ptr_dirty && (now - priv->last_emit_ticks) >= STICK_MOVE_EMIT_MS) {
        sdl2_flush_pointer(nsfb, priv);
        priv->last_emit_ticks = now;
    }

    return active;
}

static bool
sdl2_translate_event(nsfb_t *nsfb, sdl2_priv_t *priv,
                     const SDL_Event *sdlevent, nsfb_event_t *event)
{
    event->type = NSFB_EVENT_NONE;

    switch (sdlevent->type) {
    case SDL_KEYDOWN:
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = sdl2_translate_key(sdlevent->key.keysym.sym);
        break;

    case SDL_KEYUP:
        event->type = NSFB_EVENT_KEY_UP;
        event->value.keycode = sdl2_translate_key(sdlevent->key.keysym.sym);
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (sdlevent->button.which == SDL_TOUCH_MOUSEID)
            return false; /* handled via finger events */
        event->type = (sdlevent->type == SDL_MOUSEBUTTONDOWN) ?
            NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        switch (sdlevent->button.button) {
        case SDL_BUTTON_LEFT:
            event->value.keycode = NSFB_KEY_MOUSE_1;
            break;
        case SDL_BUTTON_MIDDLE:
            event->value.keycode = NSFB_KEY_MOUSE_2;
            break;
        case SDL_BUTTON_RIGHT:
            event->value.keycode = NSFB_KEY_MOUSE_3;
            break;
        default:
            return false;
        }
        break;

    case SDL_MOUSEWHEEL:
        if (sdlevent->wheel.y == 0)
            return false;
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = (sdlevent->wheel.y > 0) ?
            NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
        sdl2_queue_key(priv, NSFB_EVENT_KEY_UP, event->value.keycode);
        break;

    case SDL_MOUSEMOTION:
        if (sdlevent->motion.which == SDL_TOUCH_MOUSEID)
            return false;
        priv->ptr_x = (float)sdlevent->motion.x;
        priv->ptr_y = (float)sdlevent->motion.y;
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = sdlevent->motion.x;
        event->value.vector.y = sdlevent->motion.y;
        event->value.vector.z = 0;
        break;

    case SDL_FINGERDOWN:
        priv->ptr_x = sdlevent->tfinger.x * nsfb->width;
        priv->ptr_y = sdlevent->tfinger.y * nsfb->height;
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = (int)priv->ptr_x;
        event->value.vector.y = (int)priv->ptr_y;
        event->value.vector.z = 0;
        sdl2_queue_key(priv, NSFB_EVENT_KEY_DOWN, NSFB_KEY_MOUSE_1);
        break;

    case SDL_FINGERMOTION:
        priv->ptr_x = sdlevent->tfinger.x * nsfb->width;
        priv->ptr_y = sdlevent->tfinger.y * nsfb->height;
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = (int)priv->ptr_x;
        event->value.vector.y = (int)priv->ptr_y;
        event->value.vector.z = 0;
        break;

    case SDL_FINGERUP:
        event->type = NSFB_EVENT_KEY_UP;
        event->value.keycode = NSFB_KEY_MOUSE_1;
        break;

    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP: {
        enum nsfb_key_code_e jcode;
        if (priv->pad != NULL)
            return false; /* controller API handles it */
        switch (sdlevent->jbutton.button) {
        case 0:  jcode = NSFB_KEY_MOUSE_1; break;   /* A: click */
        case 1:  jcode = NSFB_KEY_ESCAPE;  break;   /* B */
        case 6:  jcode = NSFB_KEY_PAGEUP;  break;   /* L */
        case 7:  jcode = NSFB_KEY_PAGEDOWN; break;  /* R */
        case 12: jcode = NSFB_KEY_LEFT;    break;
        case 13: jcode = NSFB_KEY_UP;      break;
        case 14: jcode = NSFB_KEY_RIGHT;   break;
        case 15: jcode = NSFB_KEY_DOWN;    break;
        case 10: /* Plus: quit */
            if (sdlevent->type == SDL_JOYBUTTONDOWN) {
                event->type = NSFB_EVENT_CONTROL;
                event->value.controlcode = NSFB_CONTROL_QUIT;
                return true;
            }
            return false;
        default:
            return false;
        }
        if (jcode == NSFB_KEY_MOUSE_1 &&
            sdlevent->type == SDL_JOYBUTTONDOWN) {
            event->type = NSFB_EVENT_MOVE_ABSOLUTE;
            event->value.vector.x = (int)priv->ptr_x;
            event->value.vector.y = (int)priv->ptr_y;
            event->value.vector.z = 0;
            sdl2_queue_key(priv, NSFB_EVENT_KEY_DOWN, jcode);
            return true;
        }
        event->type = (sdlevent->type == SDL_JOYBUTTONDOWN) ?
            NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        event->value.keycode = jcode;
        break;
    }

    case SDL_CONTROLLERDEVICEADDED:
        if (priv->pad == NULL)
            priv->pad = SDL_GameControllerOpen(sdlevent->cdevice.which);
        return false;

    case SDL_CONTROLLERDEVICEREMOVED:
        if (priv->pad != NULL) {
            SDL_GameControllerClose(priv->pad);
            priv->pad = NULL;
        }
        return false;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        enum nsfb_key_code_e code;
        switch (sdlevent->cbutton.button) {
        /* SDL names buttons by position, so its A is the bottom button,
         * which on a Switch pad is physically B. Keep Nintendo
         * semantics: A (right) confirms, B (bottom) goes back. */
        case SDL_CONTROLLER_BUTTON_A:   /* physical B */
            code = NSFB_KEY_ESCAPE;     /* browser maps this to back */
            break;
        case SDL_CONTROLLER_BUTTON_B:   /* physical A */
            code = NSFB_KEY_MOUSE_1;    /* click at pointer */
            break;
        case SDL_CONTROLLER_BUTTON_X:
            code = NSFB_KEY_F5; /* reserved: reload */
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            code = NSFB_KEY_F2; /* reserved: url bar / osk */
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            code = NSFB_KEY_UP;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            code = NSFB_KEY_DOWN;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            code = NSFB_KEY_LEFT;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            code = NSFB_KEY_RIGHT;
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            code = NSFB_KEY_PAGEUP;
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            code = NSFB_KEY_PAGEDOWN;
            break;
        case SDL_CONTROLLER_BUTTON_START:
            if (sdlevent->type == SDL_CONTROLLERBUTTONDOWN) {
                event->type = NSFB_EVENT_CONTROL;
                event->value.controlcode = NSFB_CONTROL_QUIT;
                return true;
            }
            return false;
        default:
            return false;
        }

        /* clicking at the emulated pointer: make sure browser knows where */
        if (code == NSFB_KEY_MOUSE_1 &&
            sdlevent->type == SDL_CONTROLLERBUTTONDOWN) {
            event->type = NSFB_EVENT_MOVE_ABSOLUTE;
            event->value.vector.x = (int)priv->ptr_x;
            event->value.vector.y = (int)priv->ptr_y;
            event->value.vector.z = 0;
            sdl2_queue_key(priv, NSFB_EVENT_KEY_DOWN, code);
            return true;
        }

        event->type = (sdlevent->type == SDL_CONTROLLERBUTTONDOWN) ?
            NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        event->value.keycode = code;
        break;
    }

    case SDL_WINDOWEVENT:
        if (sdlevent->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            event->type = NSFB_EVENT_RESIZE;
            event->value.resize.w = sdlevent->window.data1;
            event->value.resize.h = sdlevent->window.data2;
            break;
        }
        return false;

    case SDL_QUIT:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        break;

    default:
        return false;
    }

    return true;
}

static bool sdl2_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    sdl2_priv_t *priv = nsfb->surface_priv;
    SDL_Event sdlevent;
    int got_event;
    bool stick_active;

    if (priv == NULL)
        return false;

    /* show everything drawn since the last input poll */
    sdl2_flush_present(priv);

    {
        static unsigned int calls;
        calls++;
        if (calls <= 8 || (calls & 1023) == 0)
            fprintf(stderr, "sdl2: input #%u timeout=%d\n", calls, timeout);
    }

    if (sdl2_queue_pop(priv, event))
        return true;

    stick_active = sdl2_pump_sticks(nsfb, priv);
    if (sdl2_queue_pop(priv, event))
        return true;

    /* while a stick drives the pointer, poll quickly to keep integrating */
    if (stick_active && (timeout < 0 || timeout > 8))
        timeout = 8;

    if (timeout == 0) {
        got_event = SDL_PollEvent(&sdlevent);
    } else if (timeout > 0) {
        got_event = SDL_WaitEventTimeout(&sdlevent, timeout);
    } else {
        got_event = SDL_WaitEvent(&sdlevent);
    }

    if (got_event == 0) {
        if (timeout == 0)
            return false; /* empty poll, mirror stock sdl surface */
        /* timeout expired with no event */
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_TIMEOUT;
        return true;
    }

    if (!sdl2_translate_event(nsfb, priv, &sdlevent, event)) {
        /* swallowed event: report as none so caller loops again */
        event->type = NSFB_EVENT_NONE;
    }

    return true;
}

static int sdl2_set_geometry(nsfb_t *nsfb, int width, int height,
                             enum nsfb_format_e format)
{
    sdl2_priv_t *priv = nsfb->surface_priv;

    if (width <= 0 || height <= 0)
        return -1;

    /* this surface only renders 32bpp */
    if ((format != NSFB_FMT_XRGB8888) && (format != NSFB_FMT_XBGR8888))
        format = NSFB_FMT_XRGB8888;

    nsfb->width = width;
    nsfb->height = height;
    nsfb->format = format;

    select_plotters(nsfb);

    if (priv != NULL) {
        uint8_t *newfb;
        SDL_Texture *newtex;

        newfb = realloc(priv->fb, (size_t)width * height * 4);
        if (newfb == NULL)
            return -1;
        priv->fb = newfb;
        priv->fb_pitch = width * 4;

        newtex = SDL_CreateTexture(priv->renderer,
                                   (format == NSFB_FMT_XBGR8888) ?
                                       SDL_PIXELFORMAT_ABGR8888 :
                                       SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   width, height);
        if (newtex == NULL) {
            fprintf(stderr, "sdl2: texture create failed: %s\n",
                    SDL_GetError());
            return -1;
        }
        if (priv->texture != NULL)
            SDL_DestroyTexture(priv->texture);
        priv->texture = newtex;

#ifndef __SWITCH__
        SDL_SetWindowSize(priv->window, width, height);
#endif
        /* on the Switch the window keeps the console's output size and
         * the renderer scales the backing texture to fit */

        nsfb->ptr = priv->fb;
        nsfb->linelen = priv->fb_pitch;
    }

    return 0;
}

static int sdl2_initialise(nsfb_t *nsfb)
{
    sdl2_priv_t *priv;

    if (nsfb->surface_priv != NULL)
        return -1;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER |
                 SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "sdl2: init failed: %s\n", SDL_GetError());
        return -1;
    }

    priv = calloc(1, sizeof(*priv));
    if (priv == NULL)
        return -1;

    priv->window = SDL_CreateWindow("NetSurf",
                                    SDL_WINDOWPOS_UNDEFINED,
                                    SDL_WINDOWPOS_UNDEFINED,
                                    nsfb->width, nsfb->height,
                                    SDL_WINDOW_SHOWN);
    if (priv->window == NULL) {
        fprintf(stderr, "sdl2: window create failed: %s\n", SDL_GetError());
        free(priv);
        return -1;
    }

    /* no vsync: presents happen per damage region and a vblank wait on
     * every small cursor update makes the pointer feel laggy */
    priv->renderer = SDL_CreateRenderer(priv->window, -1,
                                        SDL_RENDERER_ACCELERATED);
    if (priv->renderer == NULL) {
        /* fall back to software rendering */
        priv->renderer = SDL_CreateRenderer(priv->window, -1, 0);
    }
    if (priv->renderer == NULL) {
        fprintf(stderr, "sdl2: renderer create failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(priv->window);
        free(priv);
        return -1;
    }

    if (SDL_NumJoysticks() > 0) {
        priv->pad = SDL_GameControllerOpen(0);
        if (priv->pad == NULL)
            priv->joy = SDL_JoystickOpen(0);
    }
    fprintf(stderr, "sdl2: joysticks=%d pad=%p joy=%p\n",
            SDL_NumJoysticks(), (void *)priv->pad, (void *)priv->joy);

    priv->ptr_x = nsfb->width / 2.0f;
    priv->ptr_y = nsfb->height / 2.0f;
    priv->last_motion_ticks = SDL_GetTicks();

    sdl2_active_renderer = priv->renderer;

    nsfb->surface_priv = priv;

    if (sdl2_set_geometry(nsfb, nsfb->width, nsfb->height,
                          nsfb->format) != 0) {
        SDL_DestroyRenderer(priv->renderer);
        SDL_DestroyWindow(priv->window);
        free(priv);
        nsfb->surface_priv = NULL;
        return -1;
    }

    SDL_ShowCursor(SDL_DISABLE);

    return 0;
}

static int sdl2_finalise(nsfb_t *nsfb)
{
    sdl2_priv_t *priv = nsfb->surface_priv;

    if (priv != NULL) {
        sdl2_active_renderer = NULL;
        if (priv->pad != NULL)
            SDL_GameControllerClose(priv->pad);
        if (priv->joy != NULL)
            SDL_JoystickClose(priv->joy);
        if (priv->texture != NULL)
            SDL_DestroyTexture(priv->texture);
        if (priv->renderer != NULL)
            SDL_DestroyRenderer(priv->renderer);
        if (priv->window != NULL)
            SDL_DestroyWindow(priv->window);
        free(priv->fb);
        free(priv);
        nsfb->surface_priv = NULL;
    }

    SDL_Quit();
    return 0;
}

static int sdl2_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;

    if ((cursor != NULL) &&
        (cursor->plotted == true) &&
        (nsfb_plot_bbox_intersect(box, &cursor->loc))) {
        nsfb_cursor_clear(nsfb, cursor);
    }
    return 0;
}

/* present the backing store; box is in framebuffer coordinates */
static int sdl2_present(nsfb_t *nsfb, const nsfb_bbox_t *box)
{
    sdl2_priv_t *priv = nsfb->surface_priv;
    SDL_Rect rect;

    if (priv == NULL || priv->texture == NULL)
        return -1;

    rect.x = box->x0;
    rect.y = box->y0;
    rect.w = box->x1 - box->x0;
    rect.h = box->y1 - box->y0;

    if (rect.x < 0) { rect.w += rect.x; rect.x = 0; }
    if (rect.y < 0) { rect.h += rect.y; rect.y = 0; }
    if (rect.x + rect.w > nsfb->width)
        rect.w = nsfb->width - rect.x;
    if (rect.y + rect.h > nsfb->height)
        rect.h = nsfb->height - rect.y;
    if (rect.w <= 0 || rect.h <= 0)
        return 0;

    SDL_UpdateTexture(priv->texture, &rect,
                      priv->fb + (size_t)rect.y * priv->fb_pitch +
                          (size_t)rect.x * 4,
                      priv->fb_pitch);

    /* the GPU copy and flip happen once per main loop iteration, however
     * many regions were updated (see sdl2_flush_present) */
    priv->present_pending = true;

    return 0;
}

static void sdl2_flush_present(sdl2_priv_t *priv)
{
    if (priv == NULL || !priv->present_pending || priv->texture == NULL)
        return;
    priv->present_pending = false;
    SDL_RenderClear(priv->renderer);
    SDL_RenderCopy(priv->renderer, priv->texture, NULL, NULL);
    SDL_RenderPresent(priv->renderer);
}

static int sdl2_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    nsfb_bbox_t redraw;
    nsfb_bbox_t fbarea;

    if ((cursor != NULL) && (cursor->plotted == true)) {
        nsfb_bbox_t loc_shift = cursor->loc;
        loc_shift.x0 -= cursor->hotspot_x;
        loc_shift.y0 -= cursor->hotspot_y;
        loc_shift.x1 -= cursor->hotspot_x;
        loc_shift.y1 -= cursor->hotspot_y;

        nsfb_plot_add_rect(&cursor->savloc, &loc_shift, &redraw);

        fbarea.x0 = 0;
        fbarea.y0 = 0;
        fbarea.x1 = nsfb->width;
        fbarea.y1 = nsfb->height;
        nsfb_plot_clip(&fbarea, &redraw);

        nsfb_cursor_clear(nsfb, cursor);
        nsfb_cursor_plot(nsfb, cursor);

        sdl2_present(nsfb, &redraw);
    }
    return true;
}

static int sdl2_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;

    if ((cursor != NULL) && (cursor->plotted == false)) {
        nsfb_cursor_plot(nsfb, cursor);
    }

    return sdl2_present(nsfb, box);
}

const nsfb_surface_rtns_t sdl2_rtns = {
    .initialise = sdl2_initialise,
    .finalise = sdl2_finalise,
    .input = sdl2_input,
    .claim = sdl2_claim,
    .update = sdl2_update,
    .cursor = sdl2_cursor,
    .geometry = sdl2_set_geometry,
};

NSFB_SURFACE_DEF(sdl2, NSFB_SURFACE_SDL2, &sdl2_rtns)

/*
 * Local variables:
 *  c-basic-offset: 4
 *  tab-width: 8
 * End:
 */
