# NetSurf for Nintendo Switch

A true port of the NetSurf browser engine to Nintendo Switch homebrew, the first
of its kind. Built with devkitA64/libnx in WSL Arch Linux. Verified rendering
real sites over HTTPS on hardware.

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
- **Audio player with transport bar**
- **Bookmarks**: `+Bkm` adds the current page, `Bkms` opens the bookmark
  list as a page. Stored as a standard NetSurf hotlist HTML file at
  `sdmc:/switch/netsurf/Hotlist`.
- SDL2 surface driver: touch input, left stick = pointer, A = click, right
  stick = scroll, L/R = page up/down, B = back a page, Plus = quit, USB
  mouse/keyboard. Raw-joystick fallback if the gamepad mapping fails;
  drift-proof move limiter.
- Diagnostics: everything logs to `sdmc:/switch/netsurf/stderr.log`

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
