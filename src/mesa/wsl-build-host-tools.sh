#!/bin/bash
# Build and install Mesa's host-side compilers, which an Android cross-build
# needs prebuilt (a cross build can't run the aarch64 binaries it produces).
#
# Per Mesa's docs/android.rst: mesa_clc is needed by any driver using CLC,
# and panvk additionally needs the panfrost precompiled-shader compiler
# (-Dtools=panfrost -Dinstall-precomp-compiler=true).
#
# Run as root under WSL (wsl -u root). Slow - these are LLVM-linked host
# tools.
set -e

MESA=${MESA:-/opt/mesa-src}
PREFIX=${PREFIX:-/opt/mesa-compiler}
MESON=${MESON:-/opt/mesa-venv/bin/meson}

cd "$MESA"

if [ -x "$PREFIX/bin/mesa_clc" ]; then
  echo "host tools already installed at $PREFIX"
  ls "$PREFIX/bin"
  exit 0
fi

echo "=== configure host tools ==="
rm -rf build-compiler
"$MESON" setup build-compiler \
  -Dprefix="$PREFIX" \
  -Dbuildtype=release \
  -Dstrip=true \
  -Dplatforms= \
  -Dgallium-drivers= \
  -Dvulkan-drivers= \
  -Dmesa-clc=enabled \
  -Dinstall-mesa-clc=true \
  -Dtools=panfrost \
  -Dinstall-precomp-compiler=true

echo "=== build + install host tools ==="
"$MESON" install -C build-compiler

echo "=== installed ==="
ls -la "$PREFIX/bin"
