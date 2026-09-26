# NetSurf for Nintendo Switch

A true port of the NetSurf browser engine to Nintendo Switch homebrew, the first
of its kind. Built with devkitA64/libnx in WSL Arch Linux. Verified rendering
real sites over HTTPS on hardware.

## What's in

- Engine at upstream git HEAD ("3.12 Dev"): HTML5 parsing (hubbub), CSS
  (libcss), DOM (libdom)
- **Modern CSS**: flexbox with `gap`, **CSS grid** (templates, areas,
  `minmax`/`fr`/`repeat`), `border-radius`, `box-shadow`/`text-shadow`,
  linear and radial **gradients**, `opacity`, `filter`, `transform`
  (translate), `mask-image` icons, `display: contents`, custom properties
  (`var()`), `calc()`/`min()`/`max()`/`clamp()`, nesting, `@layer`,
  `@supports`, `@container`, range media queries, `:is()`/`:where()`,
  modern colour syntax, `::before`/`::after` with text, `attr()`, quotes and
  images, and the end state of CSS entry animations
- **Declarative shadow DOM** (`<template shadowrootmode>`, slots, scoped
  styles) for web-component sites
- **Inline SVG** and SVG images, drawn by an anti-aliased path rasteriser
- **`<canvas>` 2D**: paths, arcs and curves, fills and strokes,
  transforms, linear/radial gradients, compositing, text, `drawImage`,
  `getImageData`/`putImageData`, `Path2D`, animation
- **JavaScript via QuickJS, on by default**: a large web platform layer
  (DOM, events, `fetch`/XHR, storage, `MutationObserver`,
  `IntersectionObserver`, `matchMedia`, timers, `history`), **ES modules**
  with import maps and dynamic `import()`, and live re-layout when scripts
  change the page. Clicks, `input`/`change` and `submit` reach scripts and
  `preventDefault()` is honoured.
- **Inline `<video>` and `<audio>`**: ffmpeg playback inside the page with
  built-in controls, posters, autoplay/loop/muted and the HTMLMediaElement
  API and events. Streams over HTTPS; on Switch, video uses the **hardware
  (nvtegra) decoder** where possible. Direct media links open an inline
  player.
- **Docked 1080p**: renders natively at 1920x1080 with a 1.5x page scale when
  docked, 720p handheld, switching live; GPU (SDL2/GLES) presentation
- HTTP/HTTPS via curl + mbedtls (CA bundle in romfs)
- Images: PNG, JPEG, GIF, BMP, ICO, WebP, SVG
- **Freetype text rendering** (DejaVu fonts bundled in romfs), family-name
  aliasing (Arial/Times/etc map to the right generic faces)
- **@font-face webfonts**: font families declared by page CSS are fetched
  (ttf/otf/woff; not woff2) and rendered, one face per family
- **Native Switch keyboard (swkbd)**: tap/click the URL bar or any text field
- **Audio player with transport bar**
- **Bookmarks**: `+Bkm` adds the current page, `Bkms` opens the bookmark
  list as a page. Stored as a standard NetSurf hotlist HTML file at
  `sdmc:/switch/netsurf/Hotlist`.
- SDL2 surface driver: touch input, left stick = pointer, A = click, right
  stick = scroll, L/R = page up/down, B = back a page, Plus = quit, USB
  mouse/keyboard. Raw-joystick fallback if the gamepad mapping fails;
  drift-proof move limiter.
- Diagnostics: everything logs to `sdmc:/switch/netsurf/stderr.log`

JavaScript can be turned off with `enable_javascript:0` in
`sdmc:/switch/netsurf/Choices` or the `JS on/off` toolbar button.

## Not yet done

- Media Source Extensions / HLS / DASH (YouTube-style adaptive streaming)
- CSS counters, `position: sticky`, 3D/rotating transforms, animated
  transitions (final states only)
- WebGL; canvas shadows, dashes and patterns

## Development

- `tools-switch/build.sh` / `ns-make.sh`: cross build in the devkitA64
  Docker image; `tools-switch/package.sh` makes `switch-dist/netsurf.nro`
- `tools-switch/native-make.sh`: native Linux build of the same tree with
  the framebuffer frontend, for headless testing:
  `NS_SCREENSHOT=out.ppm NS_SCREENSHOT_DELAY=ms ./nsfb -f ram -w W -h H URL`
  (plus `NS_DUMPBOX=file` for the box tree, `NS_CLICK=x,y` to click)

## Controls

- Touch: tap = click, drag = pointer
- Left stick: pointer / Right stick: scroll
- A or **ZR**: click / **ZL**: alternate/context click (NetSurf's action-2)
- L/R: page up/down / B: escape / Plus: quit
- Tap URL bar or any HTML text field: native keyboard
