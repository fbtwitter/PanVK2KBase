#!/usr/bin/env python3
"""Give PanVK a non-DRM vk_sync so a kbase physical device can be created.

get_device_sync_types() calls vk_drm_syncobj_get_type(dev->fd)
unconditionally. On /dev/mali0 there is no DRM fd behind that, so it fails
and physical device creation returns VK_ERROR_INITIALIZATION_FAILED - the
blocker that survives even with enumeration and the pan_kmod backend
working.

This routes kbase devices to panvk_kbase_sync (event-memory backed, see
src/mesa/panvk_kbase_sync.c) and leaves the DRM path untouched, so one
binary still works on panfrost/panthor.

Applied as a script rather than a diff because upstream PanVK moves.
Idempotent. Run patch-panvk-kbase-enumeration.py first - this depends on
the is_kbase flag it would otherwise have to invent.

Usage: patch-panvk-kbase-sync.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"

PHYS_C = os.path.join(mesa, "src/panfrost/vulkan/panvk_physical_device.c")
PHYS_H = os.path.join(mesa, "src/panfrost/vulkan/panvk_physical_device.h")
MESON = os.path.join(mesa, "src/panfrost/vulkan/meson.build")

# ------------------------------------------------------------------ header
src = open(PHYS_H).read()

if "panvk_kbase_sync_type" in src:
    print("    panvk_physical_device.h: already patched")
else:
    anchor = "   struct vk_sync_type drm_syncobj_type;"
    assert anchor in src, "drm_syncobj_type anchor not found - PanVK moved"
    src = src.replace(
        anchor,
        anchor + "\n"
        "\n"
        "   /* kbase has no DRM syncobj. When is_kbase is set, sync_types[]\n"
        "    * points at this instead. See src/mesa/panvk_kbase_sync.c.\n"
        "    */\n"
        "   bool is_kbase;\n"
        "   struct panvk_kbase_sync_type kbase_sync_type;", 1)

    inc_anchor = '#include "vk_physical_device.h"'
    assert inc_anchor in src, "physical device include anchor not found"
    src = src.replace(inc_anchor,
                      inc_anchor + '\n#include "panvk_kbase_sync.h"', 1)

    open(PHYS_H, "w").write(src)
    print("    patched panvk_physical_device.h")

# ------------------------------------------------------------------ source
src = open(PHYS_C).read()

if "panvk_kbase_sync_type_init" in src:
    print("    panvk_physical_device.c: already patched")
else:
    # Mark kbase devices, so get_device_sync_types() can branch without
    # re-probing the fd (VERSION_CHECK is once-per-fd - see
    # tests/double_handshake_probe).
    mark_anchor = """   if (PANVK_DEBUG(STARTUP))
      mesa_logi("Found compatible kbase device '%s'.", path);"""
    assert mark_anchor in src, ("kbase probe success anchor not found - run "
                                "patch-panvk-kbase-enumeration.py first")
    src = src.replace(mark_anchor, "   device->is_kbase = true;\n\n" + mark_anchor, 1)

    old = """   device->drm_syncobj_type = vk_drm_syncobj_get_type(device->kmod.dev->fd);
   if (!device->drm_syncobj_type.features) {
      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "failed to query syncobj features");
   }

   device->sync_types[sync_type_count++] = &device->drm_syncobj_type;"""
    assert old in src, "drm_syncobj_get_type block not found - PanVK moved"

    new = """   /* kbase is not a DRM device, so there is no syncobj to query. Use the
    * CSF-event-memory sync type instead; it reports TIMELINE, so the
    * arch >= 10 path below is satisfied without a vk_sync_timeline wrapper.
    */
   if (device->is_kbase) {
      VkResult result =
         panvk_kbase_sync_type_init(&device->kbase_sync_type,
                                    device->kmod.dev->fd);
      if (result != VK_SUCCESS) {
         return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                          "failed to set up kbase event-memory sync");
      }

      device->sync_types[sync_type_count++] = &device->kbase_sync_type.base;
      device->sync_types[sync_type_count] = NULL;
      return VK_SUCCESS;
   }

   device->drm_syncobj_type = vk_drm_syncobj_get_type(device->kmod.dev->fd);
   if (!device->drm_syncobj_type.features) {
      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "failed to query syncobj features");
   }

   device->sync_types[sync_type_count++] = &device->drm_syncobj_type;"""
    src = src.replace(old, new, 1)

    # Release the event page with the physical device.
    fini_anchor = "panvk_physical_device_finish(struct panvk_physical_device *device)\n{"
    assert fini_anchor in src, "physical_device_finish anchor not found"
    src = src.replace(
        fini_anchor,
        fini_anchor + "\n"
        "   if (device->is_kbase)\n"
        "      panvk_kbase_sync_type_finish(&device->kbase_sync_type);\n", 1)

    open(PHYS_C, "w").write(src)
    print("    patched panvk_physical_device.c")

# ------------------------------------------------------------------- meson
src = open(MESON).read()

if "panvk_kbase_sync.c" in src:
    print("    meson.build: already patched")
else:
    anchor = "  'panvk_physical_device.c',"
    assert anchor in src, "meson source-list anchor not found"
    src = src.replace(anchor, anchor + "\n  'panvk_kbase_sync.c',", 1)
    open(MESON, "w").write(src)
    print("    patched src/panfrost/vulkan/meson.build")

print("panvk kbase vk_sync patch applied")

# ---------------------------------------------- copy_sync_payloads on kbase
#
# panvk_vX_device.c unconditionally sets
#
#     device->vk.copy_sync_payloads = vk_drm_syncobj_copy_payloads;
#
# which needs a real DRM syncobj - the one thing kbase does not have, and the
# reason panvk_kbase_sync.c exists at all.
#
# Nothing noticed for months because almost nothing calls it. The path that
# does is presentation: vk_common_QueueSignalReleaseImageANDROID() prefers
# copy_sync_payloads over QueueSubmit2 when the device sets it, so every
# release of a swapchain image went straight into the DRM implementation and
# came back VK_ERROR_UNKNOWN. Measured from inside the APK:
#
#     ANB-RELEASE branch=copy_payloads waits=1
#     ANB-RELEASE submit result=-13
#
# Leaving it NULL on kbase makes that call site take the QueueSubmit2 branch
# instead, which is the ordinary submit path this driver already runs for
# everything else.
DEVICE = os.path.join(mesa, "src/panfrost/vulkan/panvk_vX_device.c")
src = open(DEVICE).read()

if "copy_sync_payloads =\n      to_panvk_physical_device" in src or \
   "is_kbase ? NULL : vk_drm_syncobj_copy_payloads" in src:
    print("    panvk_vX_device.c: copy_sync_payloads already gated")
else:
    anchor = "   device->vk.copy_sync_payloads = vk_drm_syncobj_copy_payloads;"
    assert anchor in src, \
        "panvk_vX_device.c: copy_sync_payloads assignment not found - PanVK moved"

    src = src.replace(anchor, """   /* vk_drm_syncobj_copy_payloads() needs a DRM syncobj, which kbase has
    * not got. Leaving this NULL makes callers that offer a choice - notably
    * vk_common_QueueSignalReleaseImageANDROID(), i.e. presentation - take
    * the ordinary QueueSubmit2 path instead of failing VK_ERROR_UNKNOWN.
    */
   device->vk.copy_sync_payloads =
      to_panvk_physical_device(device->vk.physical)->is_kbase
         ? NULL
         : vk_drm_syncobj_copy_payloads;""", 1)

    open(DEVICE, "w").write(src)
    print("    panvk_vX_device.c: copy_sync_payloads gated off on kbase")
