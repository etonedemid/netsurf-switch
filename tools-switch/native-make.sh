#!/bin/bash
# Native Linux build of the same tree (SDL2 surface) for desktop testing.
# QuickJS-ng must be installed in $NATIVE_PREFIX (see build-prefix.sh).
set -e
cd "$(dirname "$0")/.."
NATIVE_PREFIX=${NATIVE_PREFIX:-$(cd .. && pwd)/prefix-native}
export CFLAGS="-I$NATIVE_PREFIX/include -O1 -g"
export LDFLAGS="-L$NATIVE_PREFIX/lib"
rm -f inst-native/build-stamp netsurf/nsfb
exec make TARGET=framebuffer TMP_PREFIX="$PWD/inst-native" "$@"
