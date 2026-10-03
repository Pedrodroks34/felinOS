#!/bin/bash
# FelinOS Cross-Compilation Environment
# =====================================
# Source this file to set up cross-compilation for FelinOS userspace
#
# Usage: source tools/cross-env.sh

FELINOS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORTS_DIR="$FELINOS_ROOT/felinos-ports"
TOOLCHAIN_DIR="$PORTS_DIR/toolchain"
SYSROOT="$FELINOS_ROOT/userspace"
TARGET="x86_64-felinos"

# Check if toolchain exists
if [ ! -d "$TOOLCHAIN_DIR/bin" ]; then
    echo "Error: Toolchain not found at $TOOLCHAIN_DIR/bin"
    echo "Run 'make ports-toolchain' first"
    return 1 2>/dev/null || exit 1
fi

# Check if sysroot exists
if [ ! -d "$SYSROOT/usr/include" ]; then
    echo "Error: Sysroot not populated at $SYSROOT/usr/include"
    echo "Run 'make ports-base' first"
    return 1 2>/dev/null || exit 1
fi

# Add toolchain to PATH
export PATH="$TOOLCHAIN_DIR/bin:$PATH"

# Cross-compiler variables
export CC="${TARGET}-gcc"
export CXX="${TARGET}-g++"
export AS="${TARGET}-as"
export LD="${TARGET}-ld"
export AR="${TARGET}-ar"
export RANLIB="${TARGET}-ranlib"
export STRIP="${TARGET}-strip"
export OBJCOPY="${TARGET}-objcopy"
export OBJDUMP="${TARGET}-objdump"
export NM="${TARGET}-nm"
export READELF="${TARGET}-readelf"

# Compiler flags for FelinOS target
export CFLAGS="-m64 -mcmodel=small -mno-red-zone -mgeneral-regs-only \
    -std=gnu11 -O2 -pipe \
    -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
    -fcf-protection=none -fno-asynchronous-unwind-tables \
    -fno-builtin -fno-strict-aliasing \
    -Wall -Wextra -Wno-unused-parameter \
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
    -nostdinc -isystem $SYSROOT/usr/include"

export CXXFLAGS="$CFLAGS -std=gnu++20 -fno-exceptions -fno-rtti"

export LDFLAGS="-nostdlib -static -Wl,--build-id=none -Wl,-z,max-page-size=0x1000"

# pkg-config
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"

# Meson
export CROSS_FILE="$FELINOS_ROOT/felinos.cross"

echo "FelinOS cross-compilation environment loaded"
echo "  Target: $TARGET"
echo "  Toolchain: $TOOLCHAIN_DIR"
echo "  Sysroot: $SYSROOT"
echo ""
echo "Example usage:"
echo "  \$CC -o hello hello.c"
echo "  \$CC \$CFLAGS -o myprog myprog.c"
echo "  meson setup builddir --cross-file \$CROSS_FILE"

# Create meson cross file if it doesn't exist
if [ ! -f "$CROSS_FILE" ]; then
    cat > "$CROSS_FILE" << EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
sys_root = '$SYSROOT'
c_args = ['$CFLAGS']
c_link_args = ['$LDFLAGS']
cpp_args = ['$CXXFLAGS']
cpp_link_args = ['$LDFLAGS']

[built-in options]
pic = false
pie = false
static = true
EOF
    echo "Created meson cross file: $CROSS_FILE"
fi