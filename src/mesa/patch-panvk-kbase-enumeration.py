#!/usr/bin/env python3
"""Teach PanVK to enumerate a kbase device.

The pan_kmod_kbase backend is reachable only if something opens /dev/mali0
and hands the fd to pan_kmod_dev_create(). Nothing does: PanVK enumerates
physical devices purely through libdrm, so on a kbase-only device it finds
nothing and the backend is dead code.

Mesa's vk_instance layer already supports this case. It calls an optional
physical_devices.enumerate callback first, and only falls back to DRM
enumeration if that returns VK_ERROR_INCOMPATIBLE_DRIVER. So a kbase probe
can be added *without* disturbing the DRM path - one binary keeps working on
panfrost/panthor hardware. (Turnip solves the same problem for kgsl by
picking one or the other at build time; this is strictly more flexible.)

Applied as a script rather than a diff because upstream PanVK moves and a
context diff would rot quickly. Idempotent.

Usage: patch-panvk-kbase-enumeration.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"

PHYS = os.path.join(mesa, "src/panfrost/vulkan/panvk_physical_device.c")
INST = os.path.join(mesa, "src/panfrost/vulkan/panvk_instance.c")

# ---------------------------------------------------------------- physical
src = open(PHYS).read()

if "create_kbase_kmod_dev" in src:
    print("    panvk_physical_device.c: already patched")
else:
    anchor = """static VkResult
get_drm_device_ids(struct panvk_physical_device *device,"""
    assert anchor in src, "get_drm_device_ids anchor not found - PanVK moved"

    kbase_fn = '''/* Default kbase device node. Overridable so a non-standard node can be
 * pointed at without a rebuild.
 */
#define PANVK_KBASE_DEFAULT_PATH "/dev/mali0"

/* kbase is a misc character device, not a DRM node, so there is no
 * drmDevice to describe it and it cannot be found by drmGetDevices2(). Open
 * the node directly and let pan_kmod identify it.
 *
 * Returns VK_ERROR_INCOMPATIBLE_DRIVER (rather than an error) when there is
 * no kbase device here, so the caller falls through to DRM enumeration.
 */
static VkResult
create_kbase_kmod_dev(struct panvk_physical_device *device,
                      const struct panvk_instance *instance)
{
   const char *path = os_get_option("PANVK_KBASE_DEVICE");

   if (!path)
      path = PANVK_KBASE_DEFAULT_PATH;

   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   uint32_t flags = PAN_KMOD_DEV_FLAG_OWNS_FD;

   if (PANVK_DEBUG(NO_USER_MMAP_SYNC))
      flags |= PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL;

   /* Call pan_kmod_fd_is_kbase() directly here, exactly once, instead of
    * going through pan_kmod_dev_create()'s dispatcher (which would call it
    * again internally for the kbase branch). KBASE_IOCTL_VERSION_CHECK may
    * only be issued once per fd - a second call returns -EPERM - so this
    * is the *only* place that probe may happen for this fd. Calling
    * kbase_kmod_ops.dev_create() directly afterward, rather than through
    * the generic dispatcher, is what makes that possible: it is the exact
    * same pair of calls pan_kmod_dev_create() makes internally for the
    * kbase branch, just relocated to the caller.
    *
    * That relocation is also the fix for a real bug found via CTS
    * (dEQP-VK.api.device_init.create_instance_device_intentional_alloc_fail):
    * going through the generic dispatcher collapsed two different failure
    * reasons - "this fd genuinely is not kbase" and "this fd is kbase but
    * kbase_kmod_dev_create() failed for a real reason (host allocation,
    * SET_FLAGS, GET_GPUPROPS, FIXED_VA heap init)" - into the same NULL
    * return, indistinguishable by the caller. Both were reported as
    * VK_ERROR_INCOMPATIBLE_DRIVER, which Mesa's vk_instance layer treats as
    * "try DRM enumeration instead" - correct for the first case, but for
    * the second it silently turned a genuine allocation failure into
    * vkEnumeratePhysicalDevices succeeding with zero devices, which then
    * crashed the CTS test (and would crash any real application) on the
    * very next physical-device access. Calling both steps directly here
    * lets the two cases be told apart and reported correctly.
    *
    * See tests/double_handshake_probe/ for the probe that established the
    * once-per-fd rule.
    */
   uint16_t uk_major = 0, uk_minor = 0;
   if (!pan_kmod_fd_is_kbase(fd, &uk_major, &uk_minor)) {
      close(fd);
      /* Genuinely not a kbase device - let DRM enumeration have a turn. */
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   const struct pan_kmod_driver kbase_drv_info = {
      .version = {.major = uk_major, .minor = uk_minor},
   };
   device->kmod.dev = kbase_kmod_ops.dev_create(fd, flags, &kbase_drv_info,
                                                &instance->kmod.allocator);

   if (!device->kmod.dev) {
      close(fd);
      /* fd was already confirmed kbase above, so this is a genuine
       * internal failure - not "wrong device". Must NOT be
       * VK_ERROR_INCOMPATIBLE_DRIVER; see the comment above.
       */
      return panvk_errorf(instance, VK_ERROR_OUT_OF_HOST_MEMORY,
                          "kbase device init failed");
   }

   if (PANVK_DEBUG(STARTUP))
      mesa_logi("Found compatible kbase device '%s'.", path);

   return VK_SUCCESS;
}

'''
    src = src.replace(anchor, kbase_fn + anchor, 1)

    # Route device creation: NULL drm_device means "kbase".
    old_create = """   result = create_kmod_dev(device, instance, drm_device);
   if (result != VK_SUCCESS)
      return result;"""
    assert old_create in src, "create_kmod_dev call not found"
    new_create = """   /* A NULL drm_device means this is a kbase (non-DRM) device. */
   result = drm_device ? create_kmod_dev(device, instance, drm_device)
                       : create_kbase_kmod_dev(device, instance);
   if (result != VK_SUCCESS)
      return result;"""
    src = src.replace(old_create, new_create, 1)

    # drm render/primary rdevs only exist for real DRM nodes. They feed
    # VK_EXT_physical_device_drm, which already null-guards them.
    old_ids = """   result = get_drm_device_ids(device, instance, drm_device);"""
    assert old_ids in src, "get_drm_device_ids call not found"
    new_ids = """   result = drm_device ? get_drm_device_ids(device, instance, drm_device)
                       : VK_SUCCESS;"""
    src = src.replace(old_ids, new_ids, 1)

    # pan_kmod_fd_is_kbase() lives with the kbase backend.
    inc_anchor = '#include "util/os_misc.h"'
    assert inc_anchor in src, "include anchor not found"
    src = src.replace(inc_anchor, inc_anchor + '\n\n#include "kmod/pan_kmod_kbase.h"', 1)

    open(PHYS, "w").write(src)
    print("    patched panvk_physical_device.c")

# ---------------------------------------------------------------- instance
src = open(INST).read()

if "panvk_enumerate_devices" in src:
    print("    panvk_instance.c: already patched")
else:
    anchor = """static void
panvk_destroy_physical_device(struct vk_physical_device *device)"""
    assert anchor in src, "destroy_physical_device anchor not found"

    enum_fn = '''/* Enumeration entrypoint for non-DRM (kbase) devices.
 *
 * vk_instance calls this before DRM enumeration and, if it returns
 * VK_ERROR_INCOMPATIBLE_DRIVER, falls through to the DRM path. Returning
 * that error when there is no kbase device is therefore what keeps a single
 * binary working on panfrost/panthor hardware too.
 *
 * static: only referenced by the hook assignment below, and Mesa builds
 * with -Werror=missing-prototypes.
 */
static VkResult
panvk_enumerate_devices(struct vk_instance *vk_instance)
{
   struct panvk_instance *instance =
      container_of(vk_instance, struct panvk_instance, vk);

   struct panvk_physical_device *device =
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return panvk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* NULL drm_device selects the kbase path. */
   VkResult result = panvk_physical_device_init(device, instance, NULL);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   list_addtail(&device->vk.link, &vk_instance->physical_devices.list);

   return VK_SUCCESS;
}

'''
    src = src.replace(anchor, enum_fn + anchor, 1)

    hook_anchor = "   instance->vk.physical_devices.try_create_for_drm ="
    assert hook_anchor in src, "instance hook anchor not found"
    src = src.replace(
        hook_anchor,
        "   instance->vk.physical_devices.enumerate = panvk_enumerate_devices;\n"
        + hook_anchor, 1)

    open(INST, "w").write(src)
    print("    patched panvk_instance.c")

print("panvk kbase enumeration patch applied")
