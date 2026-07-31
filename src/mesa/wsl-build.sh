#!/bin/bash
# Build Mesa's libpankmod_lib with the kbase backend included, natively on
# Linux/x86_64. A native build is the point here: it proves pan_kmod_kbase.c
# compiles and links as part of Mesa's real build system. Android
# cross-compilation is a separate concern handled elsewhere.
set -e

MESA=/opt/mesa-src
REPO=/mnt/c/Users/User/dev-workspace/projects/PanVK2KBase
KBASE_UAPI=$REPO/third_party/kbase-uapi-r49p1
SHIM=$REPO/src/utils/kconfig_shim.h
MESON=/opt/mesa-venv/bin/meson

cd "$MESA"

echo "=== 1. sync backend source ==="
cp "$REPO/src/mesa/pan_kmod_kbase.c" src/panfrost/lib/kmod/
cp "$REPO/src/mesa/pan_kmod_kbase.h" src/panfrost/lib/kmod/
# Not part of the vendored uapi header set (firmware-interface offsets, not
# ioctl uapi), so it travels next to the backend rather than via -I.
cp "$REPO/src/utils/csf_user_regs.h" src/panfrost/lib/kmod/

echo "=== 2. apply dispatch change to pan_kmod.c ==="
if grep -q kbase_kmod_ops src/panfrost/lib/kmod/pan_kmod.c; then
  echo "    already applied"
else
  # Applied programmatically rather than via `git apply`, because the
  # checked-in patch was generated against a different Mesa commit and this
  # tree has moved. Same edit, resilient to surrounding churn.
  python3 - src/panfrost/lib/kmod/pan_kmod.c <<'PYEOF'
import sys
path = sys.argv[1]
src = open(path).read()

decl_anchor = "extern const struct pan_kmod_ops panthor_kmod_ops;"
assert decl_anchor in src, "declaration anchor not found - Mesa moved"
src = src.replace(decl_anchor, decl_anchor + """

/* kbase is not a DRM driver, so it cannot be identified by drmGetVersion()
 * like the backends above. pan_kmod_kbase.h declares the probe and the ops.
 */
#include "pan_kmod_kbase.h\"""", 1)

old = """   drmVersionPtr version = drmGetVersion(fd);
   struct pan_kmod_dev *dev = NULL;

   if (!version)
      return NULL;

   if (!allocator)
      allocator = &default_allocator;
"""
assert old in src, "dev_create body not found - Mesa moved"
new = """   drmVersionPtr version;
   struct pan_kmod_dev *dev = NULL;

   if (!allocator)
      allocator = &default_allocator;

   /* kbase is not a DRM driver - it's a misc character device (/dev/mali0)
    * with its own ioctl surface, so drmGetVersion() fails on it. Probe for
    * it before falling through to DRM enumeration. This mirrors how Turnip
    * special-cases kgsl (/dev/kgsl-3d0).
    */
   uint16_t kbase_uk_major = 0, kbase_uk_minor = 0;
   if (pan_kmod_fd_is_kbase(fd, &kbase_uk_major, &kbase_uk_minor)) {
      /* kbase reports a UK interface version rather than a DRM driver
       * version. Use it as the pan_kmod_driver version so the usual
       * pan_kmod_driver_version_at_least() gating keeps working.
       */
      const struct pan_kmod_driver kbase_drv_info = {
         .version = {.major = kbase_uk_major, .minor = kbase_uk_minor},
      };

      return kbase_kmod_ops.dev_create(fd, flags, &kbase_drv_info, allocator);
   }

   version = drmGetVersion(fd);
   if (!version)
      return NULL;
"""
src = src.replace(old, new, 1)
open(path, "w").write(src)
print("    patched pan_kmod.c")
PYEOF
fi

echo "=== 3. add backend to meson.build ==="
KMOD_MESON=src/panfrost/lib/kmod/meson.build
if grep -q pan_kmod_kbase "$KMOD_MESON"; then
  echo "    already added"
else
  python3 - "$KMOD_MESON" "$KBASE_UAPI" "$SHIM" <<'PYEOF'
import sys
path, kbase_uapi, shim = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path).read()

src = src.replace(
    "  'panthor_kmod.c',\n)",
    "  'panthor_kmod.c',\n  'pan_kmod_kbase.c',\n)", 1)

# Point at the vendored kbase UAPI headers and select CSF, matching every
# other consumer of these headers in the PanVK2KBase repo. kconfig_shim.h
# resolves IS_ENABLED() for MTK-derived header sets outside a kernel tree.
src = src.replace(
    "  c_args : [no_override_init_args],",
    "  c_args : [no_override_init_args, '-DMALI_USE_CSF=1',\n"
    "            '-include', '%s',\n"
    "            '-I%s']," % (shim, kbase_uapi), 1)

open(path, "w").write(src)
print("    patched", path)
PYEOF
fi

echo "=== 4. meson setup ==="
if [ ! -d build-native ]; then
  "$MESON" setup build-native \
    -Dgallium-drivers= \
    -Dvulkan-drivers=panfrost \
    -Dplatforms= \
    -Dglx=disabled \
    -Dbuildtype=debug
fi

echo "=== 5. build libpankmod_lib ==="
ninja -C build-native src/panfrost/lib/kmod/libpankmod_lib.a

echo ""
echo "=== result ==="
ls -la build-native/src/panfrost/lib/kmod/libpankmod_lib.a
echo "--- archive members ---"
ar t build-native/src/panfrost/lib/kmod/libpankmod_lib.a
echo "--- kbase symbols ---"
nm --defined-only build-native/src/panfrost/lib/kmod/libpankmod_lib.a 2>/dev/null | grep -i kbase_kmod_ops || true
nm --defined-only build-native/src/panfrost/lib/kmod/libpankmod_lib.a 2>/dev/null | grep -i fd_is_kbase || true
