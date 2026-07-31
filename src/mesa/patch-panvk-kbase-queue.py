#!/usr/bin/env python3
"""Route GPU queue creation to the kbase sibling on kbase devices.

csf/panvk_vX_gpu_queue.c is panthor-only - GROUP_CREATE/_DESTROY/_SUBMIT,
TILER_HEAP_CREATE/_DESTROY and libdrm syncobjs, all on dev->drm_fd. On a
kbase device those ioctls do not exist, so vkCreateDevice fails -3 at
panvk_vX_gpu_queue.c's DRM_IOCTL_PANTHOR_GROUP_CREATE.

csf/panvk_vX_kbase_queue.c is the kbase implementation of the same four
entry points, under different names so both can be linked. This patch
declares them and makes panvk_vX_device.c pick between the two.

Applied as a script rather than a diff because upstream moves. Idempotent.
Run after patch-panvk-kbase-enumeration.py, which introduces is_kbase.

Usage: patch-panvk-kbase-queue.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"

QUEUE_H = os.path.join(mesa, "src/panfrost/vulkan/csf/panvk_queue.h")
DEVICE_C = os.path.join(mesa, "src/panfrost/vulkan/panvk_vX_device.c")
MESON = os.path.join(mesa, "src/panfrost/vulkan/meson.build")

# ------------------------------------------------------------------ header
src = open(QUEUE_H).read()

if "create_kbase_queue" in src:
    print("    panvk_queue.h: already patched")
else:
    anchor = "VkResult panvk_per_arch(gpu_queue_check_status)(struct vk_queue *vk_queue);"
    assert anchor in src, "gpu_queue_check_status declaration not found"

    decls = anchor + """

/* kbase equivalents, implemented in csf/panvk_vX_kbase_queue.c. Selected by
 * panvk_vX_device.c when the physical device came up on /dev/mali0.
 */
VkResult panvk_per_arch(create_kbase_queue)(
   struct panvk_device *dev, const VkDeviceQueueCreateInfo *create_info,
   uint32_t queue_idx, struct vk_queue **out_queue);
void panvk_per_arch(destroy_kbase_queue)(struct vk_queue *vk_queue);
VkResult panvk_per_arch(kbase_queue_submit)(struct vk_queue *vk_queue,
                                            struct vk_queue_submit *vk_submit);
VkResult panvk_per_arch(kbase_queue_check_status)(struct vk_queue *vk_queue);"""

    src = src.replace(anchor, decls, 1)
    open(QUEUE_H, "w").write(src)
    print("    patched csf/panvk_queue.h")

# ------------------------------------------------------------------ device
src = open(DEVICE_C).read()

if "create_kbase_queue" in src:
    print("    panvk_vX_device.c: already patched")
else:
    # Queue creation.
    old_create = "      return panvk_per_arch(create_gpu_queue)("
    assert old_create in src, "create_gpu_queue call not found"
    # Guarded: arch < 10 is JM, which uses jm/panvk_queue.h and has no CSF
    # queue at all, so the kbase declarations are not even visible there.
    new_create = """#if PAN_ARCH >= 10
      if (to_panvk_physical_device(dev->vk.physical)->is_kbase)
         return panvk_per_arch(create_kbase_queue)(
            dev, create_info, queue_idx, out_queue);
#endif

      return panvk_per_arch(create_gpu_queue)("""
    src = src.replace(old_create, new_create, 1)

    # Queue destruction.
    old_destroy = "      panvk_per_arch(destroy_gpu_queue)(queue);"
    assert old_destroy in src, "destroy_gpu_queue call not found"
    # No `dev` in scope here - panvk_queue_destroy() only takes the queue.
    # The dangling `else` before #endif is deliberate: for arch < 10 the
    # preprocessor leaves just the destroy_gpu_queue() call.
    new_destroy = """#if PAN_ARCH >= 10
      if (to_panvk_physical_device(queue->base.device->physical)->is_kbase)
         panvk_per_arch(destroy_kbase_queue)(queue);
      else
#endif
         panvk_per_arch(destroy_gpu_queue)(queue);"""
    src = src.replace(old_destroy, new_destroy, 1)

    # Status check.
    old_status = "      return panvk_per_arch(gpu_queue_check_status)(queue);"
    assert old_status in src, "gpu_queue_check_status call not found"
    new_status = """#if PAN_ARCH >= 10
      if (to_panvk_physical_device(queue->base.device->physical)->is_kbase)
         return panvk_per_arch(kbase_queue_check_status)(queue);
#endif

      return panvk_per_arch(gpu_queue_check_status)(queue);"""
    src = src.replace(old_status, new_status, 1)

    open(DEVICE_C, "w").write(src)
    print("    patched panvk_vX_device.c")

# ------------------------------------------------------------------- meson
src = open(MESON).read()

if "panvk_vX_kbase_queue.c" in src:
    print("    meson.build: already patched")
else:
    # Goes in the per-arch source list, next to the panthor queue it
    # replaces, so it is compiled once per PAN_ARCH like its sibling.
    anchor = "'csf/panvk_vX_gpu_queue.c',"
    assert anchor in src, "csf/panvk_vX_gpu_queue.c not in the source list"
    src = src.replace(anchor, anchor + "\n  'csf/panvk_vX_kbase_queue.c',", 1)
    open(MESON, "w").write(src)
    print("    patched src/panfrost/vulkan/meson.build")

print("panvk kbase queue patch applied")
