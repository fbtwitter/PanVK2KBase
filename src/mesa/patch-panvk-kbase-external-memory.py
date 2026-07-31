#!/usr/bin/env python3
"""Stop PanVK advertising dma-buf import/export on kbase, where neither works.

PanVK reports VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT and
..._DMA_BUF_BIT_EXT as both EXPORTABLE and IMPORTABLE for every device. On
panthor that is true. On kbase both directions are broken, for two different
reasons:

  import - kbase itself can do it (KBASE_IOCTL_MEM_IMPORT with
           BASE_MEM_IMPORT_TYPE_UMM), but pan_kmod_bo_import() converts the
           fd with drmPrimeFDToHandle(dev->fd, ...) before dispatching to the
           backend, and that fails on a misc device. The backend hook is
           never reached.

  export - kbase has no export path at all. No PRIME, no dmabuf-out, nothing
           in the UAPI that turns an allocation into an fd. And
           pan_kmod_bo_export() is a static inline calling
           drmPrimeHandleToFD() itself, so a backend cannot override it.

Advertising them anyway is worse than not supporting them. An application
that trusts vkGetPhysicalDeviceExternal*Properties gets a runtime
VK_ERROR_OUT_OF_DEVICE_MEMORY out of vkGetMemoryFdKHR() instead of a clean
"unsupported" at query time, which is both a spec violation and much harder
to diagnose from the application side.

This is a correctness fix, not a feature: it makes the driver tell the truth
about what it can do.

Applied as a script rather than a diff because upstream PanVK moves and a
context diff would rot quickly. Idempotent.

Usage: patch-panvk-kbase-external-memory.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"

PHYS = os.path.join(mesa, "src/panfrost/vulkan/panvk_physical_device.c")

src = open(PHYS).read()

if "panvk_supports_dma_buf_sharing" in src:
    print("    panvk_physical_device.c: external memory already patched")
    sys.exit(0)

# ------------------------------------------------------------------ helper
#
# One predicate, asked in both places, so the two capability reports cannot
# drift apart. Named for what it answers rather than for the backend, so the
# call sites read as a capability question and not as a kbase special case.
anchor = """static VkResult
panvk_get_external_image_format_properties("""
assert anchor in src, \
    "panvk_get_external_image_format_properties anchor not found - PanVK moved"

helper = '''/* BELONGS-UPSTREAM(panvk): this should ask pan_kmod whether the backend can
 * share dma-bufs, rather than being answered here. pan_kmod has no such
 * query today - pan_kmod_ops carries bo_import/bo_export hooks but nothing
 * that reports whether they are reachable - so the answer is derived from
 * the one backend where they are not. When pan_kmod grows a real capability
 * bit, delete this and ask it instead.
 *
 * Both dma-buf directions are unavailable on kbase: import is unreachable
 * because pan_kmod_bo_import() does drmPrimeFDToHandle() before dispatching
 * to the backend, and export does not exist in kbase's UAPI at all. See
 * src/mesa/README.md and ROADMAP.md Phase 3.
 */
static bool
panvk_supports_dma_buf_sharing(const struct panvk_physical_device *phys_dev)
{
   return !phys_dev->is_kbase;
}

'''

src = src.replace(anchor, helper + anchor, 1)

# ------------------------------------------------------- image format query
#
# Refuse before the tiling checks, so the reason reported is "this device
# cannot share dma-bufs" rather than "this tiling cannot".
img_anchor = """   if (!(handleType & supported_handle_types)) {
      return panvk_errorf(physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                          "VkExternalMemoryTypeFlagBits(0x%x) unsupported",
                          handleType);
   }"""
assert img_anchor in src, "image handle-type check not found - PanVK moved"

img_gate = img_anchor + """

   if (!panvk_supports_dma_buf_sharing(physical_device)) {
      return panvk_errorf(physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                          "external memory unsupported on this kernel driver");
   }"""

src = src.replace(img_anchor, img_gate, 1)

# ------------------------------------------------------------ buffer query
#
# No error path here - the entry point returns void and reports capability
# through the features mask, so "unsupported" is an empty mask. Leaving
# handle_types alone keeps the spec's "compatibleHandleTypes must include at
# least handleType" requirement satisfied.
buf_anchor = """   if (pExternalBufferInfo->handleType & supported_handle_types) {
      handle_types |= supported_handle_types;
      features |= VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                  VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
   }"""
assert buf_anchor in src, "buffer handle-type check not found - PanVK moved"

buf_gate = """   VK_FROM_HANDLE(panvk_physical_device, phys_dev, physicalDevice);

   if ((pExternalBufferInfo->handleType & supported_handle_types) &&
       panvk_supports_dma_buf_sharing(phys_dev)) {
      handle_types |= supported_handle_types;
      features |= VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                  VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
   }"""

src = src.replace(buf_anchor, buf_gate, 1)

open(PHYS, "w").write(src)
print("    panvk_physical_device.c: external memory gated on backend support")
