#!/usr/bin/env bash
#
# Package the built driver in the Adrenotools convention that custom-driver
# pickers in Winlator / Eden / Azahar / Skyline / PPSSPP already understand:
# a zip with meta.json at the root next to the .so.
#
# Producing the package is the easy half. The half that actually matters is
# recorded in docs/kbase-notes.md and demonstrated by
# tests/driver_namespace_probe: this driver cannot be loaded by a plain
# dlopen() from an app, because two of its DT_NEEDED entries are not Android
# public libraries -
#
#     libdrm.so        not public, present in /system/lib64 and /vendor/lib64
#     libhardware.so   not public, present in /system/lib64
#
# so it needs a linker namespace whose search path covers those directories,
# and which links libvndksupport.so and libdl_android.so in from the default
# namespace. That is exactly what libadrenotools does for Adreno, and it was
# measured to work unchanged for this Mali driver.
#
# Usage: bash tools/package-driver.sh [--so PATH] [--out DIR] [--version STR]

set -euo pipefail
export MSYS_NO_PATHCONV=1

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SO="$REPO/build/libvulkan_panfrost.so"
OUT="$REPO/build/package"
VERSION=""

while [ $# -gt 0 ]; do
  case "$1" in
    --so)      SO="$2"; shift 2 ;;
    --out)     OUT="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

[ -f "$SO" ] || { echo "no driver at $SO - build it first" >&2; exit 1; }

# Derive the version from the Mesa tree if not given, so the package cannot
# silently claim a version it was not built from.
if [ -z "$VERSION" ]; then
  if [ -f /opt/mesa-src/VERSION ]; then
    VERSION="$(tr -d '\r\n' < /opt/mesa-src/VERSION)"
  else
    VERSION="unknown"
  fi
fi

GIT_SHA="$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo nogit)"

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$SO" "$OUT/libvulkan_panfrost.so"

cat > "$OUT/meta.json" <<EOF
{
  "schemaVersion": 1,
  "name": "PanVK (kbase) $VERSION",
  "description": "Mesa PanVK for Arm Mali, running on the proprietary kbase kernel driver instead of panthor. Experimental: compute, rendering and presentation all work; not conformant, and untested in real games. Built from PanVK2KBase $GIT_SHA.",
  "author": "PanVK2KBase",
  "packageVersion": "1",
  "vendor": "Mesa",
  "driverVersion": "$VERSION",
  "minApi": 27,
  "libraryName": "libvulkan_panfrost.so"
}
EOF

ZIP="$REPO/build/panvk-kbase-$VERSION-$GIT_SHA.zip"
rm -f "$ZIP"
( cd "$OUT" && zip -q -r "$ZIP" . )

echo "packaged: $ZIP"
echo
echo "contents:"
( cd "$OUT" && ls -la )
echo
cat "$OUT/meta.json"
echo
echo "NOTE: a picker that loads this with a plain dlopen() will fail on"
echo "libdrm.so / libhardware.so - neither is an Android public library."
echo "It needs the namespace recipe in docs/kbase-notes.md; see"
echo "tests/driver_namespace_probe for a working implementation, and"
echo "src/android/swapchain_app for the same recipe driven from inside a"
echo "real app, which is the case that actually matters."
echo
echo "The driver must also end up somewhere the app may execute from."
echo "Measured on device (src/android/swapchain_app):"
echo "  the APK's native library dir   yes"
echo "  the app's private files dir    yes   <- for a downloaded driver"
echo "  /data/local/tmp                NO    - readable, never executable"
