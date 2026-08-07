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

# ---------------------------------------------- panvk_android.c: ANB modifier
#
# Third site, same root cause as the other two: a gralloc that cannot describe
# itself. u_gralloc's fallback backend cannot determine this device's format
# modifier, so vk_android_get_anb_layout() hands back DRM_FORMAT_MOD_INVALID
# while still returning VK_SUCCESS. panvk_image_init() passes that straight to
# pan_image_layout_init(), which looks up a modifier description, gets NULL,
# and dereferences it - a SIGSEGV inside vkCreateImage, measured on device:
#
#     ANB-LAYOUT result=0 modifier=0xffffffffffffff planes=1
#                l0.offset=0 l0.size=0 l0.rowPitch=5120
#
# rowPitch is exactly width * 4 for an RGBA_8888 buffer, i.e. an ordinary
# linear stride, and there is one plane. So the buffer is linear; the fallback
# simply has no way to assert it. Say so, rather than crash.
#
# UPDATE: that LINEAR assumption turned out to be wrong on this exact device -
# see the "real modifier" section below, which replaces it with an actual
# query. Left here as the very-last-resort fallback for a device where the
# query itself cannot be answered.
src = open(ANDROID).read()

if "DRM_FORMAT_MOD_INVALID" in src:
    print("    panvk_android.c: ANB modifier already handled")
else:
    anchor = """   result = vk_android_get_anb_layout(create_info, &mod_info, layouts,
                                      PANVK_MAX_PLANES);
   if (result != VK_SUCCESS)
      return result;
"""
    assert anchor in src, "panvk_android.c: anb layout call not found - PanVK moved"

    src = src.replace(anchor, anchor + """
   /* u_gralloc's fallback backend cannot work out this device's modifier and
    * reports DRM_FORMAT_MOD_INVALID while still returning success. Passing
    * that on null-derefs in pan_image_layout_init(). Measured on device: one
    * plane, rowPitch exactly width * 4 - a linear buffer the fallback cannot
    * name. See docs/kbase-notes.md.
    */
   if (mod_info.drmFormatModifier == DRM_FORMAT_MOD_INVALID)
      mod_info.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
""", 1)

    if "drm_fourcc.h" not in src:
        src = src.replace('#include "panvk_android.h"',
                          '#include "drm-uapi/drm_fourcc.h"\n'
                          '#include "panvk_android.h"', 1)

    open(ANDROID, "w").write(src)
    print("    panvk_android.c: ANB modifier INVALID mapped to LINEAR")

# --------------------------------------------- panvk_android.c: real modifier
#
# Replaces the DRM_FORMAT_MOD_INVALID -> LINEAR guess above with an actual
# query of gralloc's real modifier via IMapper5's stable-C ABI
# (AIMapper_loadIMapper).
#
# Why this exists: u_gralloc's IMapper backends - the only components that
# can normally ask gralloc for a buffer's modifier - need AOSP-generated
# HIDL/AIDL headers and are compiled out of an -Dandroid-stub build, so the
# section above always hit its LINEAR fallback on this build. That guess is
# wrong whenever gralloc actually allocated AFBC - measured on a Poco X8 Pro
# (MediaTek): the buffer was AFBC (BLOCK_SIZE_32x8 | YTR | SPLIT | SPARSE),
# 0x0800000000000072, one of PAN_SUPPORTED_MODIFIERS's own entries and the
# modifier PanVK's own comments call the intended choice for WSI images.
# Rendering LINEAR content into it looked fine on a flat clear (the failure
# mode that made the guess look correct for an entire session) but is the
# leading explanation for a real emulator (Eden) freezing the whole device
# under actual presentation load - the compositor's AFBC decoder choking on
# content described incorrectly.
#
# IMapper5 has a stable *C* ABI, unlike the C++/AIDL object APIs u_gralloc's
# compiled-out backends use, so it is reachable with dlopen and a locally
# declared vtable instead of AOSP-generated headers. The vtable layout,
# metadata ordinals and payload encoding below were all verified against
# real sources or real captured bytes, not assumed - see the commit history
# of src/android/swapchain_app/main.c's probe_imapper() for the derivation,
# including three wrong guesses along the way (metadata ordinals, a
# WebFetch-hallucinated function signature, and an arithmetic slip in the
# "known" comparison value) that this ports the corrected result of, not the
# guesses themselves.
#
# A plain dlopen() (not android_dlopen_ext with an explicit namespace object,
# which the APK probe needed) is enough here: this code runs inside the
# driver's own .so, already loaded through a namespace with /system/lib64 and
# /vendor/lib64/hw on its search path (see tools/package-driver.sh), so it
# inherits that resolution rather than needing to reconstruct it.
#
# Placed BEFORE the vk_android.c section below deliberately: that section's
# "already patched" branch calls sys.exit(0), which would silently skip
# anything appended after it. Same hazard its own comment warns about.
src = open(ANDROID).read()

if "panvk_android_query_gralloc_modifier" in src:
    print("    panvk_android.c: real-modifier query already added")
else:
    if "#include <dlfcn.h>" not in src:
        src = src.replace("#include <unistd.h>",
                          "#include <dlfcn.h>\n#include <string.h>\n"
                          "#include <unistd.h>", 1)

    query_code = '''typedef int32_t AIMapper_Error;

/* StandardMetadataType.aidl (hardware/interfaces/graphics/common), verified
 * against the real enum rather than remembered - an earlier attempt at this
 * had these wrong (7 and 9) and silently queried an unrelated field.
 */
#define PANVK_ANDROID_STANDARD_METADATA_PIXEL_FORMAT_MODIFIER 8L

struct panvk_android_aimapper_v5 {
   /* Field order is load-bearing - verified against AOSP's stable-c
    * IMapper.h source directly (curl + base64 decode, not a paraphrase),
    * after a fetch tool's summary of the same header hallucinated a wrong
    * return type for importBuffer. Only the fields this code calls are
    * named for their real purpose; the rest exist purely to keep every
    * later field, in particular getStandardMetadata, at its real offset.
    */
   AIMapper_Error (*importBuffer)(const native_handle_t *handle,
                                  void **outBufferHandle);
   AIMapper_Error (*freeBuffer)(void *buffer);
   AIMapper_Error (*getTransportSize)(void *buffer, uint32_t *outNumFds,
                                      uint32_t *outNumInts);
   AIMapper_Error (*lock)(void *buffer, uint64_t cpuUsage,
                          int32_t accessRegion[4], int acquireFence,
                          void **outData);
   AIMapper_Error (*unlock)(void *buffer, int *outReleaseFence);
   AIMapper_Error (*flushLockedBuffer)(void *buffer);
   AIMapper_Error (*rereadLockedBuffer)(void *buffer);
   int32_t (*getMetadata)(void *buffer, uint64_t metadataType,
                          void *destBuffer, size_t destBufferSize);
   int32_t (*getStandardMetadata)(void *buffer, int64_t standardMetadataType,
                                  void *destBuffer, size_t destBufferSize);
};

struct panvk_android_aimapper {
   uint32_t version;
   struct panvk_android_aimapper_v5 v5;
};

typedef AIMapper_Error (*panvk_android_load_imapper_fn)(
   struct panvk_android_aimapper **out);

/* Decodes one getStandardMetadata() payload. This vendor mapper wraps even a
 * plain scalar in a self-describing envelope - an int32 name length, 4 bytes
 * padding, the UTF-8 type name
 * "android.hardware.graphics.common.StandardMetadataType", then an int64
 * holding the StandardMetadataType ordinal itself - before the actual value.
 * Found by capturing a full payload and searching it for an independently
 * known value (the gralloc allocation size) rather than parsing the
 * envelope from documentation; the return doc for getStandardMetadata
 * explains why a naive 8-byte read fails ("the number of bytes written, OR
 * WHICH WOULD HAVE BEEN WRITTEN if destBufferSize was large enough" - not an
 * error, a size query answer).
 *
 * The header length is read from the buffer rather than hardcoded, since
 * only the type-name string is guaranteed constant for a given vendor
 * build, not the envelope's total size.
 */
static bool
panvk_android_decode_metadata_scalar(const uint8_t *payload, int32_t len,
                                     uint64_t *out_value)
{
   if (len < 8)
      return false;

   int32_t name_len;
   memcpy(&name_len, payload, sizeof(name_len));

   const int64_t header_size = 4 + 4 + (int64_t)name_len + 8;
   if (name_len < 0 || header_size < 0 || header_size + 8 != (int64_t)len)
      return false;

   memcpy(out_value, payload + header_size, 8);
   return true;
}

/* Returns the real DRM format modifier for an Android gralloc buffer, or
 * DRM_FORMAT_MOD_INVALID if it could not be determined. Callers must treat
 * that exactly like vk_android_get_anb_layout() itself reporting it - not
 * every device is guaranteed to expose a reachable vendor IMapper5, and this
 * function failing is that case, not a hard error.
 */
static uint64_t
panvk_android_query_gralloc_modifier(const native_handle_t *handle)
{
   /* libui.so is where AOSP intends AIMapper_loadIMapper to be found, since
    * it is the one that calls it; the vendor implementation exporting the
    * actual symbol varies, so the same small candidate search the APK probe
    * used is repeated here rather than hardcoding one vendor's filename.
    */
   static const char *libs[] = {
      "libui.so",
      "mapper.mediatek.so",
      "android.hardware.graphics.mapper@4.0.so",
      "gralloc.default.so",
   };

   panvk_android_load_imapper_fn load = NULL;
   for (uint32_t i = 0; i < sizeof(libs) / sizeof(libs[0]) && !load; i++) {
      void *lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
      if (lib)
         load = (panvk_android_load_imapper_fn)dlsym(
            lib, "AIMapper_loadIMapper");
   }
   if (!load)
      return DRM_FORMAT_MOD_INVALID;

   struct panvk_android_aimapper *mapper = NULL;
   if (load(&mapper) != 0 || !mapper || mapper->version != 5 ||
       !mapper->v5.importBuffer || !mapper->v5.freeBuffer ||
       !mapper->v5.getStandardMetadata)
      return DRM_FORMAT_MOD_INVALID;

   void *imported = NULL;
   if (mapper->v5.importBuffer(handle, &imported) != 0 || !imported)
      return DRM_FORMAT_MOD_INVALID;

   uint8_t buf[256];
   int32_t n = mapper->v5.getStandardMetadata(
      imported, PANVK_ANDROID_STANDARD_METADATA_PIXEL_FORMAT_MODIFIER, buf,
      sizeof(buf));

   uint64_t modifier = DRM_FORMAT_MOD_INVALID;
   if (n > 0 && n <= (int32_t)sizeof(buf))
      panvk_android_decode_metadata_scalar(buf, n, &modifier);

   mapper->v5.freeBuffer(imported);
   return modifier;
}

'''

    fn_anchor = "panvk_android_anb_init("
    idx = src.find(fn_anchor)
    assert idx != -1, "panvk_android.c: panvk_android_anb_init not found"
    start = src.rfind("\nstatic VkResult", 0, idx)
    assert start != -1, \
        "panvk_android.c: could not find panvk_android_anb_init's start"
    src = src[:start + 1] + query_code + src[start + 1:]

    assert_anchor = (
        "   assert(vk_find_struct_const(create_info->pNext, "
        "NATIVE_BUFFER_ANDROID));")
    assert assert_anchor in src, \
        "panvk_android.c: NATIVE_BUFFER_ANDROID assert not found - PanVK moved"
    src = src.replace(assert_anchor,
                      "   const VkNativeBufferANDROID *native_buffer =\n"
                      "      vk_find_struct_const(create_info->pNext, "
                      "NATIVE_BUFFER_ANDROID);\n"
                      "   assert(native_buffer);", 1)

    invalid_anchor = (
        "   if (mod_info.drmFormatModifier == DRM_FORMAT_MOD_INVALID)\n"
        "      mod_info.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;")
    assert invalid_anchor in src, \
        "panvk_android.c: DRM_FORMAT_MOD_INVALID fallback not found - moved"
    src = src.replace(invalid_anchor,
                      "   if (mod_info.drmFormatModifier == "
                      "DRM_FORMAT_MOD_INVALID) {\n"
                      "      const uint64_t queried =\n"
                      "         panvk_android_query_gralloc_modifier("
                      "native_buffer->handle);\n"
                      "      mod_info.drmFormatModifier =\n"
                      "         queried != DRM_FORMAT_MOD_INVALID ? queried\n"
                      "                                           : "
                      "DRM_FORMAT_MOD_LINEAR;\n"
                      "   }", 1)

    open(ANDROID, "w").write(src)
    print("    panvk_android.c: real modifier queried from gralloc via "
          "IMapper5, LINEAR is now last resort")

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

# The same assumption again, in vk_android_import_anb_memory() - the path that
# turns an ANativeWindow buffer into VkDeviceMemory, i.e. presentation.
#
# It was missed the first time round for a reason worth recording: nothing had
# ever driven it. The AHardwareBuffer path above is reachable from a shell
# binary (tests/driver_android_wsi_probe), so it got found and fixed; this one
# needs a real swapchain, which needs a real window, which needs an APK. The
# app in src/android/swapchain_app/ is what finally reached it - it dequeues a
# buffer with numFds=3 and then dies here.
anb_anchor = "   int dma_buf_fd = anb->handle->data[0];"
assert anb_anchor in src, \
    "vk_android.c: ANB import fd handling not found - Mesa moved"
src = src.replace(anb_anchor,
                  "   int dma_buf_fd = vk_android_dmabuf_fd(anb->handle);", 1)

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

# Insert the helper above its *earliest* caller, which is the ANB import path
# near the top of the file - not the AHardwareBuffer one much further down.
# C needs the definition first, and Mesa builds with
# -Werror=missing-prototypes, so a forward declaration alone would not do.
fn_anchor = "vk_android_import_anb_memory("
idx = src.find(fn_anchor)
assert idx != -1, "vk_android.c: vk_android_import_anb_memory not found"
start = src.rfind("\nVkResult", 0, idx)
assert start != -1, "vk_android.c: could not find the function's start"
src = src[:start + 1] + vk_helper + src[start + 1:]

open(VK_ANDROID, "w").write(src)
print("    vk_android.c: dma-buf located in the handle, not assumed at data[0] (2 sites)")
