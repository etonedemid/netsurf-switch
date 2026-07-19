# NetSurf for Nintendo Switch

A true port of the NetSurf browser engine to Nintendo Switch homebrew, the first
of its kind. Built with devkitA64/libnx in WSL Arch Linux. Verified rendering
real sites over HTTPS on hardware.

## Artifact

- `netsurf.nro` (~32 MB), deployed to `sdmc:/switch/netsurf.nro`. **Launch via
  title takeover** (hold R while opening a game) for application-mode memory
  (~3.2 GB); album-applet mode works but has far less RAM.

## What's in

- Engine at upstream git HEAD ("3.12 Dev"): HTML5 parsing (hubbub), CSS
  (libcss), DOM (libdom)
- **JavaScript via QuickJS**
- HTTP/HTTPS via curl + mbedtls (CA bundle in romfs)
- Images: PNG, JPEG, GIF, BMP, ICO, WebP, SVG
- **Freetype text rendering** (DejaVu fonts bundled in romfs), family-name
  aliasing (Arial/Times/etc map to the right generic faces)
- **@font-face webfonts**: font families declared by page CSS are fetched
  (ttf/otf/woff; not woff2) and rendered, one face per family
- **Native Switch keyboard (swkbd)**: tap/click the URL bar or any text field
- **Audio player with transport bar**: clicking an audio link (mp3/ogg/
  flac/m4a/opus/wav) plays it *without blocking the browser* - a bar
  appears above the status bar with `<<10` / Pause / `10>>` / Stop and a
  title plus time readout, and you can keep browsing while it plays.
  https audio downloads to the SD through NetSurf's own TLS first (the
  ffmpeg portlib has no TLS) with progress shown in the status bar;
  http audio streams straight from ffmpeg.
- **Video** still uses the fullscreen modal player (B/Plus exits).
- **Bookmarks**: `+Bkm` adds the current page, `Bkms` opens the bookmark
  list as a page. Stored as a standard NetSurf hotlist HTML file at
  `sdmc:/switch/netsurf/Hotlist`.
- SDL2 surface driver: touch input, left stick = pointer, A = click, right
  stick = scroll, L/R = page up/down, B = back a page, Plus = quit, USB
  mouse/keyboard. Raw-joystick fallback if the gamepad mapping fails;
  drift-proof move limiter.
- Diagnostics: everything logs to `sdmc:/switch/netsurf/stderr.log`

## JavaScript

- Engine: **QuickJS-ng** (ES2023, MIT), the only JS backend - Duktape has
  been dropped. Bindings are hand-written over libdom and live in
  `netsurf/content/handlers/javascript/quickjs/`: document, Node, Element,
  Text, Event, addEventListener plus `on*` attributes, querySelector for
  simple selectors, innerHTML injection, timers, location/navigator, and
  HTMLMediaElement (`<audio>`, `new Audio()`, `.play()`) wired to the
  Switch audio player.
- **Off by default**: script-gated sites can render blank when their
  scripts hit an API this DOM does not implement yet, and the no-JS
  fallbacks are usually more usable. Flip it with the toolbar's
  "JS off/JS on" button (persists), or `enable_javascript:1` in
  `sdmc:/switch/netsurf/Choices`.
- Chosen over Moddable XS: XS is marginally more standards-conformant but its
  LGPL-3.0 license is FSF-incompatible with NetSurf's GPL-2.0-only in a single
  statically-linked NRO; QuickJS-ng is MIT.

## Not yet done

- Inline `<video>`/`<audio>` playback
- Docked 1080p mode
- CSS grid, gradients, re-layout after load

## Controls

- Touch: tap = click, drag = pointer
- Left stick: pointer / Right stick: scroll
- A or **ZR**: click / **ZL**: alternate/context click (NetSurf's action-2)
- L/R: page up/down / B: escape / Plus: quit
- Tap URL bar or any HTML text field: native keyboard
