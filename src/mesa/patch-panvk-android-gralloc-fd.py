#!/usr/bin/env python3
"""Find the dma-buf in a gralloc handle instead of assuming it is data[0].

Not kbase-specific. This is a portability bug in shared code that happens to
bite here first, and it is an upstreaming candidate rather than a local
workaround - hence the name without "kbase" in it, matching
patch-panvk-null-device-destroy.py.

PanVK does:

    const native_handle_t *handle = AHardwareBuffer_getNativeHandle(ahb);
    assert(handle && handle->numFds > 0);
    int dma_buf_fd = handle->data[0];          <- panvk_android.c

and Mesa's fallback gralloc backend does the same:

    out->fds[0] = hnd->handle->data[0];        <- u_gralloc_fallback.c

data[0] is a widespread convention, not a guarantee. It does not hold on
this MediaTek device (Poco X8 Pro / mt6899). A 4 KiB BLOB AHardwareBuffer
comes back as:

    native_handle: version=12 numFds=3 numInts=62
      data[0] = fd 7, lseek(SEEK_END) = -1     <- NOT a dma-buf
      data[1] = fd 8, lseek(SEEK_END) = 4096   <- the actual buffer
      data[2] = fd 9, lseek(SEEK_END) = 6480   <- metadata, presumably

Measured by tests/dmabuf_import_probe --source=ahb, which imports data[1]
successfully and is refused ENOMEM on data[0].

The symptom this produces is badly misleading. Reading data[0] makes
vkGetAndroidHardwareBufferPropertiesANDROID fail
VK_ERROR_INVALID_EXTERNAL_HANDLE, with the only log line being "invalid
dmabuf size" from pan_kmod_bo_import - several layers below the actual
mistake, and pointing at the importer rather than at the handle.

The fix: pick the first fd in the handle that behaves like a dma-buf.
dma-bufs implement llseek and report their size; the other fds a gralloc
handle carries (metadata, fences) do not. Fall back to the historical index
rather than failing, so a platform where data[0] was always right cannot
regress.

Applied as a script rather than a diff because upstream moves. Idempotent.

Usage: patch-panvk-android-gralloc-fd.py [<mesa-src-dir>]
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"
ANDROID = os.path.join(mesa, "src/panfrost/vulkan/panvk_android.c")
FALLBACK = os.path.join(mesa, "src/util/u_gralloc/u_gralloc_fallback.c")
VK_ANDROID = os.path.join(mesa, "src/vulkan/runtime/vk_android.c")

HELPER = '''/* The dma-buf is not necessarily handle->data[0].
 *
 * That index is a convention, not a guarantee, and it does not hold on every
 * vendor's gralloc - on at least one MediaTek device data[0] is not a
 * dma-buf at all (lseek fails, mmap gives EACCES) and data[1] is the buffer.
 * Reading data[0] there surfaces as VK_ERROR_INVALID_EXTERNAL_HANDLE with an
 * "invalid dmabuf size" log from the kmod importer, several layers away from
 * the actual mistake.
 *
 * dma-bufs implement llseek and report their size; the other fds a gralloc
 * handle carries (metadata, fences) do not. Fall back to the historical
 * index rather than failing, so a platform where data[0] was always correct
 * cannot regress.
 */
static int
panvk_android_dmabuf_fd(const native_handle_t *handle)
{
   for (int i = 0; i < handle->numFds; i++) {
      if (lseek(handle->data[i], 0, SEEK_END) > 0)
         return handle->data[i];
   }

   return handle->data[0];
}

'''

# ------------------------------------------------------------- panvk_android.c
src = open(ANDROID).read()

if "panvk_android_dmabuf_fd" in src:
    print("    panvk_android.c: already patched")
else:
    inc_anchor = '#include "vk_util.h"'
    assert inc_anchor in src, "panvk_android.c: include block not found - PanVK moved"
    src = src.replace(inc_anchor, inc_anchor + "\n\n#include <unistd.h>", 1)

    call_anchor = """   const native_handle_t *handle = AHardwareBuffer_getNativeHandle(ahb);
   assert(handle && handle->numFds > 0);
   int dma_buf_fd = handle->data[0];"""
    assert call_anchor in src, \
        "panvk_android.c: AHardwareBuffer_getNativeHandle call site not found - PanVK moved"

    src = src.replace(call_anchor, """   const native_handle_t *handle = AHardwareBuffer_getNativeHandle(ahb);
   assert(handle && handle->numFds > 0);
   int dma_buf_fd = panvk_android_dmabuf_fd(handle);""", 1)

    # Put the helper immediately before the function that uses it. Anchoring on
    # the enclosing function's opening rather than a line number so this
    # survives upstream moving code around it.
    fn_anchor = "panvk_android_allocate_ahb_memory("
    idx = src.find(fn_anchor)
    assert idx != -1, "panvk_android.c: panvk_android_allocate_ahb_memory not found"
    # walk back to the start of that function's return-type line
    start = src.rfind("\nstatic ", 0, idx)
    if start == -1:
        start = src.rfind("\nVkResult", 0, idx)
    assert start != -1, "panvk_android.c: could not find the function's start"
    src = src[:start + 1] + HELPER + src[start + 1:]

    open(ANDROID, "w").write(src)
    print("    panvk_android.c: dma-buf located in the handle, not assumed at data[0]")

# --------------------------------------------------------- u_gralloc_fallback.c
src = open(FALLBACK).read()

# NB: each file's "already patched" branch must fall through to the next
# file, not sys.exit(). An earlier version exited here, which silently
# skipped the vk_android.c section below whenever this file happened to be
# patched already - and that section is the one that actually fixes the
# failing path.
if "u_gralloc_fallback_dmabuf_fd" in src:
    print("    u_gralloc_fallback.c: already patched")
else:
    anchor = "   out->fds[0] = hnd->handle->data[0];"
    assert anchor in src, \
        "u_gralloc_fallback.c: fds[0] assignment not found - Mesa moved"

    src = src.replace(
        anchor, "   out->fds[0] = u_gralloc_fallback_dmabuf_fd(hnd->handle);", 1)

    fb_helper = '''/* See panvk_android.c's copy of this reasoning: handle->data[0] is a
 * convention rather than a guarantee, and on some vendors' gralloc the
 * dma-buf is at a later index. Probe for the fd that behaves like one.
 */
static int
u_gralloc_fallback_dmabuf_fd(const native_handle_t *handle)
{
   for (int i = 0; i < handle->numFds; i++) {
      if (lseek(handle->data[i], 0, SEEK_END) > 0)
         return handle->data[i];
   }

   return handle->data[0];
}

'''

    fn_anchor = "fallback_gralloc_get_buffer_info("
    idx = src.find(fn_anchor)
    assert idx != -1, \
        "u_gralloc_fallback.c: fallback_gralloc_get_buffer_info not found"
    start = src.rfind("\nstatic ", 0, idx)
    assert start != -1, "u_gralloc_fallback.c: could not find the function's start"
    src = src[:start + 1] + fb_helper + src[start + 1:]

    if "#include <unistd.h>" not in src:
        src = src.replace('#include "util/u_memory.h"',
                          '#include "util/u_memory.h"\n\n#include <unistd.h>', 1)

    open(FALLBACK, "w").write(src)
    print("    u_gralloc_fallback.c: dma-buf located in the handle, "
          "not assumed at data[0]")

# ------------------------------------------------------------- vk_android.c
#
# The widest of the three. This is Mesa's shared Vulkan runtime, not PanVK
# and not panfrost - every Mesa Vulkan driver on Android goes through it, so
# the same assumption would bite anyone running on a gralloc that does not
# put the dma-buf first. It is also the site that actually fails here:
# vkGetAndroidHardwareBufferPropertiesANDROID lseek()s data[0] for the
# allocation size and hands data[0] to GetMemoryFdPropertiesKHR.
src = open(VK_ANDROID).read()

if "vk_android_dmabuf_fd" in src:
    print("    vk_android.c: already patched")
    sys.exit(0)

anchor = """   const native_handle_t *handle = AHardwareBuffer_getNativeHandle(buffer);
   assert(handle && handle->numFds > 0);
   pProperties->allocationSize = lseek(handle->data[0], 0, SEEK_END);

   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   result = device->dispatch_table.GetMemoryFdPropertiesKHR(
      device_h, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, handle->data[0],
      &fd_props);"""

assert anchor in src, \
    "vk_android.c: GetAndroidHardwareBufferProperties fd handling not found - Mesa moved"

replacement = """   const native_handle_t *handle = AHardwareBuffer_getNativeHandle(buffer);
   assert(handle && handle->numFds > 0);
   const int ahb_dmabuf_fd = vk_android_dmabuf_fd(handle);
   pProperties->allocationSize = lseek(ahb_dmabuf_fd, 0, SEEK_END);

   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   result = device->dispatch_table.GetMemoryFdPropertiesKHR(
      device_h, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, ahb_dmabuf_fd,
      &fd_props);"""

src = src.replace(anchor, replacement, 1)

vk_helper = '''/* The dma-buf is not necessarily handle->data[0].
 *
 * That index is a convention, not a guarantee. On at least one MediaTek
 * device an AHardwareBuffer comes back with numFds=3 where data[0] is not a
 * dma-buf at all - lseek() fails on it and mmap() returns EACCES - and
 * data[1] is the buffer. Using data[0] makes the lseek() below return -1 and
 * the import fail, surfacing as VK_ERROR_INVALID_EXTERNAL_HANDLE from
 * vkGetAndroidHardwareBufferPropertiesANDROID with no indication that the
 * handle index was the problem.
 *
 * dma-bufs implement llseek and report their size; the other fds a gralloc
 * handle carries (metadata, fences) do not. Fall back to the historical
 * index rather than failing, so a platform where data[0] was always correct
 * cannot regress.
 */
static int
vk_android_dmabuf_fd(const native_handle_t *handle)
{
   for (int i = 0; i < handle->numFds; i++) {
      if (lseek(handle->data[i], 0, SEEK_END) > 0)
         return handle->data[i];
   }

   return handle->data[0];
}

'''

fn_anchor = "vk_common_GetAndroidHardwareBufferPropertiesANDROID("
idx = src.find(fn_anchor)
assert idx != -1, "vk_android.c: vk_common_GetAndroidHardwareBufferPropertiesANDROID not found"
start = src.rfind("\nVkResult", 0, idx)
assert start != -1, "vk_android.c: could not find the function's start"
src = src[:start + 1] + vk_helper + src[start + 1:]

open(VK_ANDROID, "w").write(src)
print("    vk_android.c: dma-buf located in the handle, not assumed at data[0] (2 sites)")
