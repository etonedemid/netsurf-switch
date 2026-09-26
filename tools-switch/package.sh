#!/bin/bash
# Package netsurf/nsfb into switch-dist/netsurf.nro (romfs + icon + nacp).
set -euo pipefail
cd "$(dirname "$0")/../netsurf"
DIST=switch-dist
DKP=${DEVKITPRO:-/opt/devkitpro}
aarch64-none-elf-strip -o $DIST/nsfb-stripped nsfb 2>/dev/null || \
    $DKP/devkitA64/bin/aarch64-none-elf-strip -o $DIST/nsfb-stripped nsfb
# refresh romfs resources that live in the source tree
for f in default.css internal.css quirks.css adblock.css welcome.html; do
    [ -f frontends/framebuffer/res/$f ] && cp -L frontends/framebuffer/res/$f $DIST/romfs/res/ || true
done
[ -f resources/default.css ] && cp resources/default.css $DIST/romfs/res/default.css
$DKP/tools/bin/nacptool --create "NetSurf" "NetSurf Switch port" "3.12" $DIST/netsurf.nacp
$DKP/tools/bin/elf2nro $DIST/nsfb-stripped $DIST/netsurf.nro \
    --icon=$DIST/netsurf-icon.jpg --nacp=$DIST/netsurf.nacp --romfsdir=$DIST/romfs
ls -l $DIST/netsurf.nro
