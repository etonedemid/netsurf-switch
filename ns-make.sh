#!/bin/bash
# Cross-make wrapper for the NetSurf Switch build (run from WSL archlinux).
# Lets the NetSurf buildsystem derive the aarch64 toolchain from HOST=,
# while host tools (nsgenbind) are sanitized via the top Makefile patch.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env-switch.sh"
unset CC CXX AR RANLIB STRIP LIBS PKG_CONFIG PKGCONFIG

# NetSurf makefiles only use CFLAGS, so fold CPPFLAGS in.
export CFLAGS="$CPPFLAGS $CFLAGS"
export CXXFLAGS="$CPPFLAGS $CXXFLAGS"

# pkg-config must see: in-tree installed NetSurf libs, our local prefix, dkp portlibs.
export PKG_CONFIG_LIBDIR="$NSPORT_ROOT/netsurf-switch/inst-framebuffer/lib/pkgconfig:$PKG_CONFIG_LIBDIR"
unset PKG_CONFIG_PATH

cd "$NSPORT_ROOT/netsurf-switch"
exec make HOST=aarch64-none-elf TARGET=framebuffer "$@"
