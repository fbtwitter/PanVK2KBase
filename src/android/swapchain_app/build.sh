#!/usr/bin/env bash
#
# Build and sign the NativeActivity APK that tests this driver from inside a
# real app. See main.c for what it measures and why a shell binary cannot.
#
# Runs from Git Bash on Windows, because the Android SDK build-tools
# (aapt2/zipalign/apksigner) live on the Windows side while the Mesa build
# lives in WSL. Nothing here needs WSL.
#
# Usage: bash src/android/swapchain_app/build.sh [--install]
set -euo pipefail

# NOTE: deliberately NOT setting MSYS_NO_PATHCONV=1 here, unlike
# tools/run-probes.sh. Every tool below is a native Windows binary being
# handed local paths, so Git Bash's POSIX->Windows rewriting is exactly what
# is wanted. It is only turned off further down, around the one adb command
# whose argument is a component name that must not be mistaken for a path.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="$REPO/build/swapchain_app"

SDK="${ANDROID_SDK:-C:/Users/User/AppData/Local/Android/Sdk}"
BUILD_TOOLS="${BUILD_TOOLS:-$SDK/build-tools/35.0.0}"
PLATFORM="${PLATFORM:-$SDK/platforms/android-35/android.jar}"
NDK="${ANDROID_NDK:-$SDK/ndk/29.0.14206865}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
# API 29 is the floor for the namespace API this app leans on; the manifest
# and aapt2 below agree with it.
CC="$TOOLCHAIN/bin/aarch64-linux-android29-clang.cmd"
[ -x "$CC" ] || CC="$TOOLCHAIN/bin/aarch64-linux-android29-clang"

GLUE="$NDK/sources/android/native_app_glue"

INSTALL=0
[ "${1:-}" = "--install" ] && INSTALL=1

for f in "$PLATFORM" "$GLUE/android_native_app_glue.c"; do
  [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT/lib/arm64-v8a"

echo "=== 1. compile the native library ==="
# -u ANativeActivity_onCreate keeps the glue's entry symbol from being
# dropped by --gc-sections; without it the platform finds no entry point and
# the activity dies at startup with a singularly unhelpful message.
"$CC" -shared -fPIC -O2 \
  -o "$OUT/lib/arm64-v8a/libpanvk_swapchain_app.so" \
  "$HERE/main.c" "$GLUE/android_native_app_glue.c" \
  -I"$GLUE" \
  -u ANativeActivity_onCreate \
  -landroid -llog -ldl -lnativewindow

echo "=== 1b. bundle the driver ==="
# The APK's native library directory is the one place an app may always
# execute from, so the driver rides along in it. Without this the app can
# still be built, and will report that it could not load a driver from
# anywhere - which is a legitimate thing to test, hence the warning rather
# than a hard failure.
DRIVER="${DRIVER:-$REPO/build/libvulkan_panfrost.so}"
if [ -f "$DRIVER" ]; then
  cp "$DRIVER" "$OUT/lib/arm64-v8a/libvulkan_panfrost.so"
  echo "  bundled: $DRIVER"
else
  echo "  WARNING: no driver at $DRIVER - the app will have nothing to load"
fi

echo "=== 2. link resources into a base APK ==="
"$BUILD_TOOLS/aapt2.exe" link \
  -I "$PLATFORM" \
  --manifest "$HERE/AndroidManifest.xml" \
  --min-sdk-version 29 \
  --target-sdk-version 35 \
  -o "$OUT/base.apk"

echo "=== 3. add the native library ==="
( cd "$OUT" && "$BUILD_TOOLS/aapt2.exe" version >/dev/null && zip -q -r base.apk lib )

echo "=== 4. align ==="
"$BUILD_TOOLS/zipalign.exe" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"

echo "=== 5. sign ==="
# A throwaway debug key, generated once and kept out of git. Android refuses
# to install an unsigned APK, and the key's identity is irrelevant here.
KEYSTORE="$OUT/../debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
  echo "  generating a debug keystore"
  keytool -genkeypair -keystore "$KEYSTORE" -alias androiddebugkey \
    -storepass android -keypass android -keyalg RSA -keysize 2048 \
    -validity 10000 -dname "CN=PanVK2KBase Debug,O=PanVK2KBase,C=US" >/dev/null
fi

"$BUILD_TOOLS/apksigner.bat" sign \
  --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/panvk-swapchain-app.apk" "$OUT/aligned.apk"

echo
echo "built: $OUT/panvk-swapchain-app.apk"

if [ "$INSTALL" = "1" ]; then
  echo
  echo "=== install and run ==="
  adb install -r -t "$OUT/panvk-swapchain-app.apk"
  MSYS_NO_PATHCONV=1 adb shell am start \
    -n org.panvk2kbase.swapchainapp/android.app.NativeActivity
  echo
  echo "results:  adb logcat -s PanVKApp"
fi
