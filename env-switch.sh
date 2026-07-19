# Switch (aarch64/libnx) cross-compile environment for the NetSurf port.
# Usage: source env-switch.sh   (from WSL with devkitPro installed)
#
# NSPORT_ROOT is the directory holding netsurf-switch/, prefix-switch/ and
# quickjs/. It defaults to the parent of this script's directory; export it
# yourself to build from a different layout.
export NSPORT_ROOT="${NSPORT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=${DEVKITPRO}/devkitA64
export PORTLIBS_PREFIX=${DEVKITPRO}/portlibs/switch
export PREFIX_LOCAL=${NSPORT_ROOT}/prefix-switch

export PATH=${PORTLIBS_PREFIX}/bin:${DEVKITA64}/bin:${DEVKITPRO}/tools/bin:${PATH}

export TOOL_PREFIX=aarch64-none-elf-
export CC=${TOOL_PREFIX}gcc
export CXX=${TOOL_PREFIX}g++
export AR=${TOOL_PREFIX}gcc-ar
export RANLIB=${TOOL_PREFIX}gcc-ranlib
export STRIP=${TOOL_PREFIX}strip

export ARCH_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec"
export CPPFLAGS="-D__SWITCH__ -I${PREFIX_LOCAL}/include -I${PORTLIBS_PREFIX}/include -I${DEVKITPRO}/libnx/include"
export CFLAGS="${ARCH_FLAGS} -O2 -ffunction-sections -fdata-sections"
export CXXFLAGS="${CFLAGS}"
export LDFLAGS="${ARCH_FLAGS} -L${PREFIX_LOCAL}/lib -L${PORTLIBS_PREFIX}/lib -L${DEVKITPRO}/libnx/lib"
export LIBS="-lnx"

# pkg-config: restrict lookups to switch portlibs + our local prefix
export PKG_CONFIG_LIBDIR=${PREFIX_LOCAL}/lib/pkgconfig:${PORTLIBS_PREFIX}/lib/pkgconfig
export PKG_CONFIG=${PORTLIBS_PREFIX}/bin/${TOOL_PREFIX}pkg-config
export PKGCONFIG=${PKG_CONFIG}
