#!/usr/bin/env python3
"""Make PanVK tell the truth about dma-buf import/export on kbase.

PanVK reports VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT and
..._DMA_BUF_BIT_EXT as both EXPORTABLE and IMPORTABLE for every device. On
panthor that is true. On kbase the two directions differ, and this script
reports each one honestly rather than collapsing them:

  import - WORKS, via pan_kmod_ops::bo_import_fd (KBASE_IOCTL_MEM_IMPORT
           with BASE_MEM_IMPORT_TYPE_UMM). It used to be unreachable because
           pan_kmod_bo_import() called drmPrimeFDToHandle() before
           dispatching to the backend; patch-pan-kmod-import-fd.py adds the
           fd-taking hook that runs first. Verified on hardware by
           tests/dmabuf_import_probe and tests/driver_dmabuf_probe.

  export - IMPOSSIBLE. kbase has no export path at all: no PRIME, no
           dmabuf-out, nothing in the UAPI that turns an allocation into an
           fd. And pan_kmod_bo_export() is a static inline calling
           drmPrimeHandleToFD() itself, so a backend cannot override it.

Getting either direction wrong is worse than not supporting it. An
application that trusts vkGetPhysicalDeviceExternal*Properties and is told
it can export gets a runtime VK_ERROR_OUT_OF_DEVICE_MEMORY out of
vkGetMemoryFdKHR() instead of a clean "unsupported" at query time; one told
it cannot import silently gives up a path that works. Both are spec
violations and both are hard to diagnose from the application side.

This also fixes exportFromImportedHandleTypes, which claims "you can
re-export what you imported" and is false on kbase in both queries.

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

if "panvk_supports_external_import" in src:
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
 * The two directions are no longer symmetric on kbase, which is why there
 * are two predicates rather than one.
 */

/* Import works on kbase through pan_kmod_ops::bo_import_fd
 * (KBASE_IOCTL_MEM_IMPORT with BASE_MEM_IMPORT_TYPE_UMM), verified on
 * hardware by tests/dmabuf_import_probe and tests/driver_dmabuf_probe.
 *
 * But only for dma-bufs. An OPAQUE_FD import is a promise with no producer
 * here: the only way to obtain a PanVK opaque fd is to export one, and
 * kbase cannot export at all. Advertising it would be vacuous at best and,
 * if an application ever did present an fd under that handle type, would
 * import it with dma-buf semantics it never agreed to.
 */
static bool
panvk_supports_external_import(const struct panvk_physical_device *phys_dev,
                               VkExternalMemoryHandleTypeFlagBits handle_type)
{
   if (phys_dev->is_kbase)
      return handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

   return true;
}

/* BELONGS-UPSTREAM(kernel): export is not "not yet" on kbase, it is not
 * expressible - there is no PRIME ioctl and no dmabuf-out anywhere in the
 * UAPI. Unlike import, no amount of userspace work closes this.
 */
static bool
panvk_supports_dma_buf_export(const struct panvk_physical_device *phys_dev)
{
   return !phys_dev->is_kbase;
}

'''

src = src.replace(anchor, helper + anchor, 1)

# ------------------------------------------------------- image format query
#
# Mask the assembled features rather than refusing outright. Refusing was
# right when neither direction worked; now that import does, a blanket
# refusal would under-report - and under-reporting a capability the driver
# has is just as much a lie as over-reporting one it does not.
#
# Note the LINEAR branch upstream sets EXPORTABLE only, so on kbase it
# collapses to features == 0 and the existing "if (!features)" below refuses
# it. That is correct and needs no extra code.
img_anchor = """   if (!features) {
      return panvk_errorf(
         physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,
         "VkExternalMemoryTypeFlagBits(0x%x) unsupported for VkImageTiling(%d)",
         handleType, pImageFormatInfo->tiling);
   }"""
assert img_anchor in src, "image features check not found - PanVK moved"

img_gate = """   if (!panvk_supports_external_import(physical_device, handleType))
      features &= ~VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
   if (!panvk_supports_dma_buf_export(physical_device))
      features &= ~VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;

""" + img_anchor

src = src.replace(img_anchor, img_gate, 1)

# exportFromImportedHandleTypes claims "you can re-export what you imported",
# which on kbase is false - and is the kind of lie that surfaces as a runtime
# VK_ERROR_OUT_OF_DEVICE_MEMORY instead of a query-time refusal.
exp_anchor = """   *external_properties = (VkExternalMemoryProperties){
      .externalMemoryFeatures = features,
      .exportFromImportedHandleTypes = supported_handle_types,
      .compatibleHandleTypes = supported_handle_types,
   };"""
assert exp_anchor in src, "image external_properties assignment not found - PanVK moved"

exp_gate = """   *external_properties = (VkExternalMemoryProperties){
      .externalMemoryFeatures = features,
      .exportFromImportedHandleTypes =
         panvk_supports_dma_buf_export(physical_device) ? supported_handle_types
                                                        : 0,
      .compatibleHandleTypes = supported_handle_types,
   };"""

src = src.replace(exp_anchor, exp_gate, 1)

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

   if (pExternalBufferInfo->handleType & supported_handle_types) {
      handle_types |= supported_handle_types;
      if (panvk_supports_dma_buf_export(phys_dev))
         features |= VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
      if (panvk_supports_external_import(phys_dev,
                                        pExternalBufferInfo->handleType))
         features |= VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
   }"""

src = src.replace(buf_anchor, buf_gate, 1)

# Same correction as the image path: do not claim re-export of an import.
buf_exp_anchor = """      .exportFromImportedHandleTypes = handle_types,"""
if buf_exp_anchor in src:
    src = src.replace(
        buf_exp_anchor,
        """      .exportFromImportedHandleTypes =
         panvk_supports_dma_buf_export(phys_dev) ? handle_types : 0,""",
        1)

open(PHYS, "w").write(src)
print("    panvk_physical_device.c: external memory gated on backend support")
