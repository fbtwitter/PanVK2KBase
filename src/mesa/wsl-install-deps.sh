#!/bin/bash
# Installs the toolchain needed to build Mesa's panfrost targets under WSL.
# Run as root (wsl -u root); no sudo/password involved.
set -e

export DEBIAN_FRONTEND=noninteractive

echo "=== apt-get update ==="
apt-get update -qq

echo "=== installing build deps ==="
apt-get install -y -qq --no-install-recommends \
  build-essential \
  ninja-build \
  pkg-config \
  cmake \
  flex \
  bison \
  git \
  python3-pip \
  python3-venv \
  python3-mako \
  python3-yaml \
  llvm-dev \
  libclang-cpp-dev \
  clang \
  libdrm-dev \
  libexpat1-dev \
  zlib1g-dev \
  glslang-tools \
  libclc-18-dev \
  libllvmspirvlib-18-dev

echo "=== meson venv (apt meson 1.3.2 is older than Mesa's >= 1.4.0) ==="
if [ ! -x /opt/mesa-venv/bin/meson ]; then
  python3 -m venv /opt/mesa-venv
  /opt/mesa-venv/bin/pip install --quiet --upgrade pip
  /opt/mesa-venv/bin/pip install --quiet "meson>=1.4.0"
fi

echo "=== versions ==="
/opt/mesa-venv/bin/meson --version
ninja --version
llvm-config --version
clang --version | head -1
echo "install done"
