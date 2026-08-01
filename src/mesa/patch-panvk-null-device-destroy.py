#!/usr/bin/env python3
"""Stop panvk_DestroyDevice() crashing on vkDestroyDevice(VK_NULL_HANDLE, ...).

Per the Vulkan spec every vkDestroy*/vkFree* command accepts VK_NULL_HANDLE
for the object being destroyed as a no-op - vkDestroyDevice is no exception.
dEQP-VK.api.null_handle.destroy_device exercises exactly this and segfaults
this driver: panvk_DestroyDevice() (src/panfrost/vulkan/panvk_physical_device.c)
dereferences the device handle via VK_FROM_HANDLE()/device->vk.physical
before checking whether _device is VK_NULL_HANDLE, a null pointer
dereference at device->vk.physical's offset (confirmed on hardware -
tombstone fault addr 0x70, symbolized to this exact line against an
unstripped local deqp-vk/libvulkan_panfrost.so build - see
docs/kbase-notes.md's CTS-coverage findings).

Not kbase-specific: this file and function are shared PanVK code, used by
the panthor and panfrost backends too - unlike every other
patch-panvk-kbase-*.py script in this directory, which exists to make
kbase work at all. This one exists to verify a real Mesa correctness bug
on real hardware before it's worth reporting upstream (see
docs/kbase-notes.md / this repo's own discipline: verify on hardware
first, a PR is a conclusion not an opinion) - applied as a script rather
than a diff because upstream moves. Idempotent.

Usage: patch-panvk-null-device-destroy.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"
SRC = os.path.join(mesa, "src/panfrost/vulkan/panvk_physical_device.c")

src = open(SRC).read()

if "_device == VK_NULL_HANDLE" in src:
    print("    panvk_physical_device.c: already patched")
    sys.exit(0)

anchor = """VKAPI_ATTR void VKAPI_CALL
panvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);
   unsigned arch = pan_arch(physical_device->kmod.dev->props.gpu_id);

   panvk_arch_dispatch(arch, destroy_device, device, pAllocator);
}"""

assert anchor in src, "panvk_DestroyDevice body not found - upstream moved"

replacement = """VKAPI_ATTR void VKAPI_CALL
panvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   /* Every vkDestroy-family command accepts VK_NULL_HANDLE as a no-op,
    * vkDestroyDevice included - dEQP-VK.api.null_handle.destroy_device
    * exercises exactly this. Without this check, VK_FROM_HANDLE() below
    * yields a null panvk_device and device->vk.physical segfaults.
    */
   if (_device == VK_NULL_HANDLE)
      return;

   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);
   unsigned arch = pan_arch(physical_device->kmod.dev->props.gpu_id);

   panvk_arch_dispatch(arch, destroy_device, device, pAllocator);
}"""

src = src.replace(anchor, replacement, 1)
open(SRC, "w").write(src)
print("    patched panvk_physical_device.c (null-handle vkDestroyDevice)")
print("panvk null-device-destroy patch applied")
