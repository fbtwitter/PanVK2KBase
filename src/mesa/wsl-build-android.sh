#!/bin/bash
# Cross-build PanVK (with the kbase backend) for Android/aarch64.
#
# Prerequisites, in order:
#   1. bash wsl-install-deps.sh        toolchain, LLVM, libclc
#   2. bash wsl-fetch-ndk.sh           Linux NDK (the Windows one won't run here)
#   3. bash wsl-build-host-tools.sh    mesa_clc + panfrost precomp compiler
#
# A cross build cannot run the aarch64 binaries it produces, so Mesa's
# host-side shader compilers must exist as native binaries first; that's what
# -Dmesa-clc=system and -Dprecomp-compiler=system consume. See
# docs/android.rst in the Mesa tree.
set -e

MESA=${MESA:-/opt/mesa-src}
REPO=${REPO:-/mnt/c/Users/User/dev-workspace/projects/PanVK2KBase}
KBASE_UAPI=${KBASE_UAPI:-$REPO/third_party/kbase-uapi-r49p1}
SHIM=${SHIM:-$REPO/src/utils/kconfig_shim.h}
MESON=${MESON:-/opt/mesa-venv/bin/meson}
HOST_TOOLS=${HOST_TOOLS:-/opt/mesa-compiler}
BUILD=${BUILD:-build-android}

export PATH="$HOST_TOOLS/bin:$PATH"

for t in mesa_clc panfrost_compile; do
  command -v "$t" >/dev/null 2>&1 || echo "note: $t not on PATH (may be named differently)"
done

cd "$MESA"

echo "=== sync backend + patches (same as the native build) ==="
bash "$REPO/src/mesa/wsl-build.sh" --sync-only 2>/dev/null || {
  cp "$REPO/src/mesa/pan_kmod_kbase.c" src/panfrost/lib/kmod/
  cp "$REPO/src/mesa/pan_kmod_kbase.h" src/panfrost/lib/kmod/
}

echo "=== configure Android cross build ==="
if [ ! -f "$BUILD/build.ninja" ]; then
  rm -rf "$BUILD"
  "$MESON" setup "$BUILD" \
    --cross-file "$REPO/src/mesa/android-aarch64-wsl.cross" \
    -Dplatforms=android \
    -Dplatform-sdk-version=34 \
    -Dandroid-stub=true \
    -Dandroid-libbacktrace=disabled \
    -Degl=disabled \
    -Dgallium-drivers= \
    -Dvulkan-drivers=panfrost \
    -Dallow-fallback-for=libdrm \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dbuildtype=release
fi

echo "=== build the Vulkan driver ==="
ninja -C "$BUILD" src/panfrost/vulkan/libvulkan_panfrost.so

echo ""
echo "=== result ==="
find "$BUILD" -name "libvulkan_panfrost.so" -exec ls -la {} \;
echo "--- confirm it's aarch64 and contains our backend ---"
SO=$(find "$BUILD" -name "libvulkan_panfrost.so" | head -1)
file "$SO"
nm -C "$SO" 2>/dev/null | grep -i kbase | head || \
  echo "(symbols hidden - built with -fvisibility=hidden, expected)"
