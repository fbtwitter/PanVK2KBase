#!/bin/bash
# Fetch a Linux Android NDK into WSL.
#
# The NDK installed on the Windows side ships only windows-x86_64 host
# binaries, which cannot run inside WSL - a separate Linux NDK is required
# for the cross-build.
#
# r27c is the current LTS line and is what Mesa is routinely built against.
set -e

NDK_VER=${NDK_VER:-r27c}
DEST=${DEST:-/opt/android-ndk}
ZIP=/tmp/android-ndk-$NDK_VER-linux.zip

if [ -d "$DEST/toolchains/llvm/prebuilt/linux-x86_64" ]; then
  echo "NDK already present at $DEST"
  "$DEST/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version | head -1
  exit 0
fi

command -v unzip >/dev/null 2>&1 || {
  echo "=== installing unzip ==="
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq unzip
}

echo "=== downloading NDK $NDK_VER ==="
[ -f "$ZIP" ] || curl -fL --progress-bar \
  -o "$ZIP" \
  "https://dl.google.com/android/repository/android-ndk-$NDK_VER-linux.zip"

echo "=== extracting ==="
rm -rf /tmp/ndk-extract "$DEST"
mkdir -p /tmp/ndk-extract
unzip -q "$ZIP" -d /tmp/ndk-extract
mv /tmp/ndk-extract/android-ndk-"$NDK_VER" "$DEST"
rm -rf /tmp/ndk-extract

echo "=== installed ==="
"$DEST/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version | head -1
