#!/bin/bash
# Build the port-local dependencies that devkitPro does not ship:
#  - GNU libiconv (newlib has no iconv)
#  - QuickJS-ng (JavaScript engine)
set -euo pipefail
PREFIX=${1:-/src/prefix-switch}
ICONV_VER=1.17
QJS_TAG=v0.17.0
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec"
CFLAGS="$ARCH -O2 -ffunction-sections -fdata-sections -D__SWITCH__ -I$DEVKITPRO/libnx/include"
mkdir -p "$PREFIX/include/quickjs" "$PREFIX/lib" /tmp/pfx && cd /tmp/pfx

curl -fsSL https://ftp.gnu.org/pub/gnu/libiconv/libiconv-$ICONV_VER.tar.gz | tar xz
cd libiconv-$ICONV_VER
CC=aarch64-none-elf-gcc AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
CFLAGS="$CFLAGS -std=gnu17" ./configure --host=aarch64-none-elf --prefix="$PREFIX" \
    --disable-shared --enable-static --disable-nls >/dev/null
make -j"$(nproc)" -C libcharset >/dev/null
make -C libcharset install-lib libdir="$PWD/lib" includedir="$PWD/lib" >/dev/null
make -j"$(nproc)" -C lib >/dev/null
make -C lib install >/dev/null
cp include/iconv.h "$PREFIX/include/"
cd ..

git clone -q --depth 1 --branch $QJS_TAG https://github.com/quickjs-ng/quickjs.git
cd quickjs
SRCS=""
for f in quickjs.c libregexp.c libunicode.c cutils.c dtoa.c xsum.c libbf.c; do
    [ -f $f ] && SRCS="$SRCS $f"
done
for f in $SRCS; do
    aarch64-none-elf-gcc $CFLAGS -std=gnu11 -D_GNU_SOURCE -DCONFIG_VERSION="\"$QJS_TAG\"" \
        -Wno-implicit-fallthrough -c $f -o ${f%.c}.o
done
aarch64-none-elf-ar rcs "$PREFIX/lib/libquickjs.a" *.o
cp quickjs.h "$PREFIX/include/quickjs/"
cd / && rm -rf /tmp/pfx
