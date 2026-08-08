/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 */

/* Does this driver work from inside a real Android app?
 *
 * Everything in src/tests/ is a shell binary, and a shell binary cannot
 * answer that. It runs in the default linker namespace, which can already
 * see /system/lib64 and /vendor/lib64, so the namespace work that a real app
 * needs is invisible there - tests/driver_namespace_probe says so itself, in
 * as many words: "An app is the restricted case, and that is the one that
 * matters. A shell binary structurally cannot tell the two apart... Answering
 * it needs an APK."
 *
 * This is that APK. It is a NativeActivity, so there is no Java at all: the
 * whole app is this file plus the NDK's native_app_glue.
 *
 * MILESTONE 1 (this file, now): load the driver from inside an app through
 * the same linker namespace shape libadrenotools builds, walk the hwvulkan
 * HAL to a real vkGetInstanceProcAddr, create an instance and a device, and
 * report what the ANativeWindow looks like. That is the whole "can an
 * emulator's driver picker actually use this" question, minus presentation.
 *
 * MILESTONE 2 (not yet): actually present. On Android, VK_KHR_swapchain is
 * implemented by the platform's Vulkan loader (libvulkan.so) on top of the
 * driver's VK_ANDROID_native_buffer - drivers do not implement it themselves.
 * An app that loads a driver directly, as this one does and as a driver
 * picker does, therefore has to do the loader's job: dequeue a gralloc buffer
 * from the ANativeWindow, import it as a VkImage, render, then
 * vkQueueSignalReleaseImageANDROID and queue the buffer back. The pieces are
 * all proven individually (tests/driver_android_wsi_probe imports an
 * AHardwareBuffer as VkDeviceMemory; panvk_kbase_sync.c does the sync-fd
 * halves) but they have never been driven against a real window.
 *
 * Results go to logcat under the tag below, one line per step, ending in a
 * PASS/FAIL summary:
 *
 *     adb logcat -s PanVKApp
 */

#include <android/dlext.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define TAG "PanVKApp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Where the driver is expected to live. /data/local/tmp is readable by an
 * app, which is what makes this convenient for testing; a real picker would
 * copy the .so into its own private directory first.
 */
#define DEFAULT_DRIVER "/data/local/tmp/libvulkan_panfrost.so"

static int g_checks_run;
static int g_checks_failed;

static void
check(bool ok, const char *what)
{
   g_checks_run++;
   if (!ok)
      g_checks_failed++;
   LOGI("  [%s] %s", ok ? "ok" : "FAIL", what);
}

/* Reports a measurement without passing judgement on it. Used for the "which
 * directories may an app execute from" survey, where a "no" is a legitimate
 * finding about Android rather than a defect in this driver.
 */
static void
note(bool yes, const char *what)
{
   LOGI("  [%s] %s", yes ? "yes" : "no", what);
}

/* ------------------------------------------------------------------ */
/* Bionic's namespace API.
 *
 * android_dlopen_ext() is NDK-public; android_create_namespace() and
 * android_link_namespaces() are not, and are exported from libdl.so under
 * __loader_-prefixed private names. libadrenotools resolves them exactly
 * this way. Copied from tests/driver_namespace_probe, which measured which
 * spellings exist on this device.
 */
struct android_namespace_t;

typedef struct android_namespace_t *(*create_ns_fn)(
   const char *name, const char *ld_library_path,
   const char *default_library_path, uint64_t type,
   const char *permitted_when_isolated_path,
   struct android_namespace_t *parent);

typedef bool (*link_ns_fn)(struct android_namespace_t *from,
                           struct android_namespace_t *to,
                           const char *shared_libs_sonames);

#ifndef ANDROID_NAMESPACE_TYPE_ISOLATED
#define ANDROID_NAMESPACE_TYPE_ISOLATED 1
#endif
#ifndef ANDROID_NAMESPACE_TYPE_SHARED
#define ANDROID_NAMESPACE_TYPE_SHARED 2
#endif

/* The driver needs libdrm.so and libhardware.so, neither of which is an
 * Android public library, so they cannot be resolved from an app's own
 * namespace. These are the sonames that have to be linked in from the
 * default namespace instead - built up empirically in the namespace probe,
 * one dlopen failure at a time. The last two are the non-obvious ones:
 * libhardware pulls in libvndksupport, which needs libdl_android.so out of
 * the runtime apex, and a namespace cannot reach into the apex itself.
 */
/* The namespace the driver was loaded through. Kept because anything else
 * that needs a non-public system library - libui for IMapper, say - has to
 * go through the same one; the app default namespace cannot see them.
 */
static struct android_namespace_t *g_driver_ns;

static const char *PUBLIC_SONAMES =
   "libc.so:libm.so:libdl.so:liblog.so:libz.so:libnativewindow.so:"
   "libsync.so:libandroid.so:libvulkan.so:"
   "libvndksupport.so:libdl_android.so";

/* ------------------------------------------------------------------ */
/* libhardware's hwvulkan ABI.
 *
 * An Android Vulkan driver does not export vkGetInstanceProcAddr. It exports
 * one symbol, "HMI", an hw_module_t whose open() yields a struct holding the
 * real entrypoints. Same mirrors as src/tests/icd_shim.
 */
struct hw_module_methods_t;

#ifdef __LP64__
typedef uint64_t hw_reserved_t;
#else
typedef uint32_t hw_reserved_t;
#endif

typedef struct hw_module_t {
   uint32_t tag;
   uint16_t module_api_version;
   uint16_t hal_api_version;
   const char *id;
   const char *name;
   const char *author;
   struct hw_module_methods_t *methods;
   void *dso;
   hw_reserved_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_device_t {
   uint32_t tag;
   uint32_t version;
   struct hw_module_t *module;
   hw_reserved_t reserved[12];
   int (*close)(struct hw_device_t *device);
} hw_device_t;

typedef struct hw_module_methods_t {
   int (*open)(const struct hw_module_t *module, const char *id,
               struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hwvulkan_device_t {
   struct hw_device_t common;
   PFN_vkEnumerateInstanceExtensionProperties
      EnumerateInstanceExtensionProperties;
   PFN_vkCreateInstance CreateInstance;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

#define HWVULKAN_DEVICE_0 "vk0"

/* ------------------------------------------------------------------ */
/* The ANativeWindow producer API and VK_ANDROID_native_buffer: between them,
 * everything the platform's Vulkan loader does to implement a swapchain.
 *
 * None of this is in the NDK. ANativeWindow_dequeueBuffer and friends exist
 * in libnativewindow.so on device (verified with strings) but are VNDK, so
 * they are declared here and resolved with dlsym. VkNativeBufferANDROID is
 * from Mesa's include/vulkan/vk_android_native_buffer.h, spec version 11.
 */

typedef struct native_handle {
   int version;
   int numFds;
   int numInts;
   int data[0];
} native_handle_t;

typedef struct android_native_base_t {
   int magic;
   int version;
   void *reserved[4];
   void (*incRef)(struct android_native_base_t *base);
   void (*decRef)(struct android_native_base_t *base);
} android_native_base_t;

/* Field order and padding must match AOSP's ANativeWindowBuffer exactly -
 * this is read, not allocated, so a wrong offset silently yields garbage
 * rather than failing to compile.
 */
typedef struct ANativeWindowBuffer {
   android_native_base_t common;
   int width;
   int height;
   int stride;
   int format;
   int usage_deprecated;
   uintptr_t layerCount;
   void *reserved[1];
   const native_handle_t *handle;
   uint64_t usage;
   void *reserved_proc[8 - (sizeof(uint64_t) / sizeof(void *))];
} ANativeWindowBuffer_t;

/* The producer side of a window has to be "connected" to an API before it
 * will hand out buffers - without it dequeueBuffer returns -ENODEV, which is
 * exactly what the first attempt hit. There is no ANativeWindow_connect in
 * libnativewindow (checked with strings on device), because connecting goes
 * through the struct's own perform() hook. So the struct layout has to be
 * mirrored from AOSP's system/window.h, in full and in order, to reach it.
 */
struct ANativeWindowFull {
   android_native_base_t common;
   const uint32_t flags;
   const int minSwapInterval;
   const int maxSwapInterval;
   const float xdpi;
   const float ydpi;
   intptr_t oem[4];
   int (*setSwapInterval)(struct ANativeWindowFull *, int);
   int (*dequeueBuffer_DEPRECATED)(struct ANativeWindowFull *, void **);
   int (*lockBuffer_DEPRECATED)(struct ANativeWindowFull *, void *);
   int (*queueBuffer_DEPRECATED)(struct ANativeWindowFull *, void *);
   int (*query)(const struct ANativeWindowFull *, int, int *);
   int (*perform)(struct ANativeWindowFull *, int, ...);
   int (*cancelBuffer_DEPRECATED)(struct ANativeWindowFull *, void *);
   int (*dequeueBuffer)(struct ANativeWindowFull *, ANativeWindowBuffer_t **,
                        int *);
   int (*queueBuffer)(struct ANativeWindowFull *, ANativeWindowBuffer_t *, int);
   int (*cancelBuffer)(struct ANativeWindowFull *, ANativeWindowBuffer_t *, int);
};

#define NATIVE_WINDOW_API_CONNECT 13
#define NATIVE_WINDOW_API_DISCONNECT 14
#define NATIVE_WINDOW_API_EGL 1

typedef int (*anw_dequeue_fn)(ANativeWindow *, ANativeWindowBuffer_t **,
                              int *fence_fd);
typedef int (*anw_queue_fn)(ANativeWindow *, ANativeWindowBuffer_t *,
                            int fence_fd);
typedef int (*anw_set_usage_fn)(ANativeWindow *, uint64_t usage);
typedef int (*anw_set_format_fn)(ANativeWindow *, int format);
typedef int (*anw_set_dims_fn)(ANativeWindow *, uint32_t w, uint32_t h);

static anw_dequeue_fn anw_dequeue;
static anw_queue_fn anw_queue;
static anw_set_usage_fn anw_set_usage;
static anw_set_format_fn anw_set_format;
static anw_set_dims_fn anw_set_dims;

static bool
load_native_window_api(void)
{
   void *h = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
   if (!h)
      return false;

   anw_dequeue = (anw_dequeue_fn)dlsym(h, "ANativeWindow_dequeueBuffer");
   anw_queue = (anw_queue_fn)dlsym(h, "ANativeWindow_queueBuffer");
   anw_set_usage = (anw_set_usage_fn)dlsym(h, "ANativeWindow_setUsage");
   anw_set_format = (anw_set_format_fn)dlsym(h, "ANativeWindow_setBuffersFormat");
   anw_set_dims = (anw_set_dims_fn)dlsym(h, "ANativeWindow_setBuffersDimensions");

   return anw_dequeue && anw_queue && anw_set_usage && anw_set_format &&
          anw_set_dims;
}

#define VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID ((VkStructureType)1000010000)

typedef struct {
   uint64_t consumer;
   uint64_t producer;
} VkNativeBufferUsage2ANDROID;

typedef struct {
   VkStructureType sType;
   const void *pNext;
   const native_handle_t *handle;
   int stride;
   int format;
   int usage;
   VkNativeBufferUsage2ANDROID usage2;
   uint64_t usage3;
   struct AHardwareBuffer *ahb;
} VkNativeBufferANDROID;

typedef VkResult(VKAPI_PTR *PFN_vkGetSwapchainGrallocUsageANDROID)(
   VkDevice device, VkFormat format, VkImageUsageFlags imageUsage,
   int *grallocUsage);
typedef VkResult(VKAPI_PTR *PFN_vkAcquireImageANDROID)(
   VkDevice device, VkImage image, int nativeFenceFd, VkSemaphore semaphore,
   VkFence fence);
typedef VkResult(VKAPI_PTR *PFN_vkQueueSignalReleaseImageANDROID)(
   VkQueue queue, uint32_t waitSemaphoreCount,
   const VkSemaphore *pWaitSemaphores, VkImage image, int *pNativeFenceFd);

/* HAL_PIXEL_FORMAT_RGBA_8888, the gralloc format matching
 * VK_FORMAT_R8G8B8A8_UNORM.
 */
#define HAL_PIXEL_FORMAT_RGBA_8888 1

/* ------------------------------------------------------------------ */

/* Copies a file. Used to try loading the driver out of the app's own private
 * directory, which is what a picker that downloads a driver at runtime would
 * have to do.
 */
static bool
copy_file(const char *src, const char *dst)
{
   FILE *in = fopen(src, "rb");
   if (!in)
      return false;

   FILE *out = fopen(dst, "wb");
   if (!out) {
      fclose(in);
      return false;
   }

   char buf[64 * 1024];
   size_t n;
   bool ok = true;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      if (fwrite(buf, 1, n, out) != n) {
         ok = false;
         break;
      }
   }

   fclose(in);
   fclose(out);
   return ok;
}

/* The directory this app's own .so was loaded from, i.e. the APK's extracted
 * native library directory. Found by asking the dynamic linker where a
 * function in this library lives, which avoids having to guess at the
 * /data/app/~~hash~~/pkg-hash/lib/arm64 layout.
 *
 * That directory is the one place an app is always allowed to execute from,
 * so it is where a bundled driver goes.
 */
static bool
native_lib_dir(char *out, size_t out_sz)
{
   Dl_info info;
   if (!dladdr((const void *)&copy_file, &info) || !info.dli_fname)
      return false;

   snprintf(out, out_sz, "%s", info.dli_fname);
   char *slash = strrchr(out, '/');
   if (!slash)
      return false;

   *slash = '\0';
   return true;
}

/* Loads the driver the way a driver picker does and returns its
 * vkGetInstanceProcAddr, or NULL. The two steps are independent failures and
 * are checked separately, because they fail for completely different reasons:
 * the namespace step fails when the app cannot reach the vendor libraries,
 * the HAL step fails when the .so is not a hwvulkan module.
 */
static PFN_vkGetInstanceProcAddr
load_driver(const char *path)
{
   LOGI("  trying: %s", path);

   /* Split the path: the directory goes on the namespace search path, the
    * basename is what we dlopen through it.
    */
   char dir[512];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   const char *base = path;
   if (slash) {
      *slash = '\0';
      base = slash + 1;
   } else {
      snprintf(dir, sizeof(dir), ".");
   }

   void *libdl = dlopen("libdl.so", RTLD_NOW | RTLD_LOCAL);

   static bool api_checked;
   create_ns_fn create_ns = NULL;
   link_ns_fn link_ns = NULL;
   static const char *create_names[] = {"__loader_android_create_namespace",
                                        "android_create_namespace"};
   static const char *link_names[] = {"__loader_android_link_namespaces",
                                      "android_link_namespaces"};

   for (size_t i = 0; i < sizeof(create_names) / sizeof(create_names[0]); i++) {
      void *s = dlsym(RTLD_DEFAULT, create_names[i]);
      if (!s && libdl)
         s = dlsym(libdl, create_names[i]);
      if (s && !create_ns)
         create_ns = (create_ns_fn)s;
   }
   for (size_t i = 0; i < sizeof(link_names) / sizeof(link_names[0]); i++) {
      void *s = dlsym(RTLD_DEFAULT, link_names[i]);
      if (!s && libdl)
         s = dlsym(libdl, link_names[i]);
      if (s && !link_ns)
         link_ns = (link_ns_fn)s;
   }

   /* Only worth reporting once, however many locations get tried. */
   if (!api_checked) {
      check(create_ns && link_ns, "bionic namespace API reachable from an app");
      api_checked = true;
   }
   if (!create_ns || !link_ns)
      return NULL;

   /* The adrenotools shape: SHARED, no parent, driver directory plus the
    * vendor library directories on the search path. An app cannot see
    * /vendor/lib64 by default, which is the whole reason those extra entries
    * are here and the reason a shell binary cannot test this honestly.
    */
   char search_path[1024];
   snprintf(search_path, sizeof(search_path),
            "%s:/system/lib64:/vendor/lib64:/vendor/lib64/hw", dir);
   LOGI("  search path: %s", search_path);

   struct android_namespace_t *ns = create_ns(
      "panvk-kbase-app", search_path, NULL, ANDROID_NAMESPACE_TYPE_SHARED,
      NULL, NULL);
   if (!ns) {
      LOGE("    android_create_namespace failed");
      return NULL;
   }

   bool linked = link_ns(ns, NULL, PUBLIC_SONAMES);
   if (!linked) {
      LOGE("    android_link_namespaces failed");
      return NULL;
   }

   g_driver_ns = ns;

   android_dlextinfo info = {
      .flags = ANDROID_DLEXT_USE_NAMESPACE,
      .library_namespace = ns,
   };
   void *h = android_dlopen_ext(base, RTLD_NOW | RTLD_LOCAL, &info);
   if (!h) {
      LOGE("    dlopen failed: %s", dlerror());
      return NULL;
   }
   LOGI("    loaded");

   /* Now the HAL walk. */
   hw_module_t *mod = (hw_module_t *)dlsym(h, "HMI");
   if (!mod || !mod->methods || !mod->methods->open) {
      LOGE("    no usable HMI module");
      return NULL;
   }
   LOGI("    HMI: id=%s name=%s", mod->id ? mod->id : "(null)",
        mod->name ? mod->name : "(null)");

   hw_device_t *hwdev = NULL;
   if (mod->methods->open(mod, HWVULKAN_DEVICE_0, &hwdev) != 0 || !hwdev) {
      LOGE("    HAL open(\"vk0\") failed");
      return NULL;
   }

   return ((hwvulkan_device_t *)hwdev)->GetInstanceProcAddr;
}

/* Tries each place a driver could live, in the order a picker would care
 * about, and reports which ones an app is actually allowed to execute from.
 *
 * This is the part worth measuring. Android's W^X policy means an app may
 * not map executable pages out of an arbitrary world-readable directory, and
 * a driver picker that downloads a driver at runtime has to put it somewhere
 * that is both writable by the app and executable - which is not obviously
 * the same place.
 */
static PFN_vkGetInstanceProcAddr
load_driver_from_anywhere(const char *internal_path)
{
   LOGI("=== load the driver from inside an app ===");

   PFN_vkGetInstanceProcAddr first = NULL, gipa;
   char path[512];

   /* All three are tried even after one works. Which locations an app may
    * execute from is the question a driver picker actually needs answered,
    * and stopping at the first success would leave it unanswered.
    */

   /* 1. Bundled in the APK's native library directory. Always executable,
    * and the arrangement a picker that ships a driver would use.
    */
   char libdir[512];
   if (native_lib_dir(libdir, sizeof(libdir))) {
      snprintf(path, sizeof(path), "%s/libvulkan_panfrost.so", libdir);
      gipa = load_driver(path);
      note(gipa != NULL, "executable: the APK's native library dir");
      if (gipa && !first)
         first = gipa;
   }

   /* 2. Copied into the app's own private files directory. This is the case
    * a picker that downloads drivers depends on, and the one Android's W^X
    * policy is most likely to refuse.
    */
   if (internal_path && *internal_path) {
      char src[512];
      const char *env = getenv("PANVK_KBASE_ICD_DRIVER");

      /* Prefer the bundled copy as the source: /data/local/tmp may not exist
       * on a device that only ever had the APK installed.
       */
      if (env)
         snprintf(src, sizeof(src), "%s", env);
      else if (native_lib_dir(libdir, sizeof(libdir)))
         snprintf(src, sizeof(src), "%s/libvulkan_panfrost.so", libdir);
      else
         snprintf(src, sizeof(src), "%s", DEFAULT_DRIVER);

      snprintf(path, sizeof(path), "%s/libvulkan_panfrost.so", internal_path);

      if (copy_file(src, path)) {
         gipa = load_driver(path);
         note(gipa != NULL, "executable: the app's private files dir");
         if (gipa && !first)
            first = gipa;
      } else {
         LOGI("  (could not copy %s -> %s)", src, path);
         note(false, "executable: the app's private files dir (copy failed)");
      }
   }

   /* 3. Straight out of /data/local/tmp, where adb push puts it. Expected to
    * fail in an app - kept because the failure is the informative part.
    */
   gipa = load_driver(DEFAULT_DRIVER);
   note(gipa != NULL, "executable: /data/local/tmp");
   if (gipa && !first)
      first = gipa;

   return first;
}

/* ------------------------------------------------------------------ */

/* Which AFBC modifier did gralloc actually use?
 *
 * The driver cannot ask - u_gralloc IMapper backends are compiled out of an
 * -Dandroid-stub build - and guessing is what produced the LINEAR bug. So
 * solve for it instead, using the driver as the oracle:
 *
 *   for each plausible Mali AFBC modifier, create an image of exactly the
 *   swapchain geometry with that modifier and ask what it would allocate.
 *   The modifier whose size reproduces the gralloc allocation is the one
 *   gralloc used.
 *
 * This is not the LINEAR mistake repeated. That was an assumption with no
 * way to check it; this has an exact numeric test, and a candidate that does
 * not reproduce the measured size is rejected. If two candidates give the
 * same size the answer is ambiguous and this says so rather than pick one -
 * at which point IMapper is genuinely required.
 *
 * Nothing here modifies the driver. It only measures.
 */
#define FOURCC_MOD_ARM_AFBC(mode) (((uint64_t)0x08 << 56) | (uint64_t)(mode))
#define AFBC_BLOCK_16x16 1ull
#define AFBC_BLOCK_32x8  2ull
#define AFBC_YTR    (1ull << 4)
#define AFBC_SPLIT  (1ull << 5)
#define AFBC_SPARSE (1ull << 6)
#define AFBC_CBR    (1ull << 7)
#define AFBC_TILED  (1ull << 8)
#define AFBC_SC     (1ull << 9)

static void
probe_modifiers(VkPhysicalDevice pdev, PFN_vkGetInstanceProcAddr gipa,
                VkInstance instance, VkDevice device,
                PFN_vkGetDeviceProcAddr gdpa, uint32_t width, uint32_t height,
                uint64_t target_size)
{
   LOGI("=== solve for the gralloc modifier ===");
   LOGI("  target allocation: %llu bytes for %ux%u RGBA_8888",
        (unsigned long long)target_size, width, height);

   PFN_vkCreateImage create_image =
      (PFN_vkCreateImage)gdpa(device, "vkCreateImage");
   PFN_vkDestroyImage destroy_image =
      (PFN_vkDestroyImage)gdpa(device, "vkDestroyImage");
   PFN_vkGetImageMemoryRequirements get_reqs =
      (PFN_vkGetImageMemoryRequirements)gdpa(device,
                                             "vkGetImageMemoryRequirements");
   if (!create_image || !destroy_image || !get_reqs) {
      note(false, "resolved the image-size entrypoints");
      return;
   }

   /* Ask the driver which modifiers it supports for this format, rather
    * than trying candidates blind. Creating an image with a modifier the
    * driver does not handle does not fail cleanly - it segfaults inside
    * pan_image_layout_init(), the same null deref that
    * DRM_FORMAT_MOD_INVALID produced. So the list must come from the driver.
    */
   PFN_vkGetPhysicalDeviceFormatProperties2 get_fmt_props2 =
      (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(
         instance, "vkGetPhysicalDeviceFormatProperties2");
   if (!get_fmt_props2) {
      note(false, "resolved vkGetPhysicalDeviceFormatProperties2");
      return;
   }

   VkDrmFormatModifierPropertiesListEXT mod_props = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
   };
   VkFormatProperties2 fmt_props = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &mod_props,
   };
   get_fmt_props2(pdev, VK_FORMAT_R8G8B8A8_UNORM, &fmt_props);

   const uint32_t mod_count = mod_props.drmFormatModifierCount;
   LOGI("  driver supports %u modifiers for R8G8B8A8_UNORM", mod_count);
   if (mod_count == 0)
      return;

   VkDrmFormatModifierPropertiesEXT *props =
      calloc(mod_count, sizeof(*props));
   if (!props)
      return;

   mod_props.pDrmFormatModifierProperties = props;
   get_fmt_props2(pdev, VK_FORMAT_R8G8B8A8_UNORM, &fmt_props);

   uint32_t matches = 0;
   uint64_t match_mod = 0;

   for (uint32_t i = 0; i < mod_count; i++) {
      const uint64_t mod = props[i].drmFormatModifier;

      const VkImageDrmFormatModifierListCreateInfoEXT mod_list = {
         .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
         .drmFormatModifierCount = 1,
         .pDrmFormatModifiers = &mod,
      };
      const VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .pNext = &mod_list,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {width, height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };

      VkImage img = VK_NULL_HANDLE;
      VkResult r = create_image(device, &ici, NULL, &img);
      if (r != VK_SUCCESS) {
         LOGI("  mod=0x%016llx  rejected (%d)",
              (unsigned long long)mod, r);
         continue;
      }

      VkMemoryRequirements reqs = {0};
      get_reqs(device, img, &reqs);
      const bool hit = (uint64_t)reqs.size == target_size;
      LOGI("  mod=0x%016llx planes=%u  size=%llu%s",
           (unsigned long long)mod, props[i].drmFormatModifierPlaneCount,
           (unsigned long long)reqs.size, hit ? "   <== MATCH" : "");
      if (hit) {
         matches++;
         match_mod = mod;
      }
      destroy_image(device, img, NULL);
   }

   free(props);

   if (matches == 1)
      LOGI("  RESULT: unique match - modifier 0x%016llx",
           (unsigned long long)match_mod);
   else if (matches == 0)
      LOGI("  RESULT: no candidate reproduces the allocation - list is "
           "incomplete or the size includes metadata this does not model");
   else
      LOGI("  RESULT: %u candidates match - AMBIGUOUS, do not pick one",
           matches);
}

/* Ask gralloc for the buffer's real format modifier, via IMapper5.
 *
 * The driver cannot do this: u_gralloc's IMapper backends need AOSP-generated
 * HIDL headers and are compiled out of an -Dandroid-stub build, so the ANB
 * import path has been assuming LINEAR. But IMapper5 has a stable *C* entry
 * point, AIMapper_loadIMapper, exported by libui.so and implemented here by
 * /vendor/lib64/hw/mapper.mediatek.so. That is loadable with dlopen and a
 * locally declared ABI - the same technique this app already uses for the
 * ANativeWindow producer functions and the bionic namespace API.
 *
 * The ABI below is hand-written from AOSP's IMapper.h. Getting the vtable
 * order wrong would mean calling the wrong function pointer, so this does not
 * trust it blindly: it queries ALLOCATION_SIZE as well, whose correct answer
 * is already known independently from the gralloc handle (14,394,880). If
 * that matches, the offsets are right and the modifier beside it is real. If
 * it does not, the ABI is wrong and nothing here should be believed.
 */
typedef int32_t AIMapper_Error;

/* StandardMetadataType.aidl ordinals (hardware/interfaces/graphics/common),
 * verified against the actual enum rather than remembered - the first
 * attempt had these wrong (7 and 9), which is why it queried USAGE instead
 * of ALLOCATION_SIZE and got a plausible-looking but meaningless n=77.
 * Only the two used here are named; the full enum runs 0-23.
 */
#define STANDARD_METADATA_PIXEL_FORMAT_MODIFIER 8L
#define STANDARD_METADATA_ALLOCATION_SIZE 10L

struct AIMapperV5 {
   /* Order is load-bearing; see the note above. */
   AIMapper_Error (*importBuffer)(const native_handle_t *, void **);
   AIMapper_Error (*freeBuffer)(void *);
   AIMapper_Error (*getTransportSize)(void *, uint32_t *, uint32_t *);
   AIMapper_Error (*lock)(void *, uint64_t, int32_t[4], int, void **);
   AIMapper_Error (*unlock)(void *, int *);
   AIMapper_Error (*flushLockedBuffer)(void *);
   AIMapper_Error (*rereadLockedBuffer)(void *);
   int32_t (*getMetadata)(void *, uint64_t, void *, size_t);
   int32_t (*getStandardMetadata)(void *buffer, int64_t type, void *dest,
                                  size_t dest_size);
};

struct AIMapper {
   uint32_t version;
   struct AIMapperV5 v5;
};

typedef AIMapper_Error (*load_imapper_fn)(struct AIMapper **out);

static void
probe_imapper(const native_handle_t *handle, uint64_t known_alloc_size)
{
   LOGI("=== ask gralloc for the modifier (IMapper5 stable C) ===");

   /* libui.so is not an Android public library, so a plain dlopen from an
    * app fails exactly as libdrm/libhardware do. android_load_sphal_library()
    * exists for this: it loads out of the sphal namespace, which is how apps
    * are meant to reach vendor libraries. It lives in libvndksupport, which
    * this app already links into its driver namespace.
    */
   /* libui.so is not an Android public library, so neither a plain dlopen
    * nor libvndksupport is reachable from the app default namespace - both
    * were tried and both failed. The driver namespace already has
    * /system/lib64 on its search path, so load through that instead.
    */
   void *lib = NULL;
   if (g_driver_ns) {
      android_dlextinfo ns_info = {
         .flags = ANDROID_DLEXT_USE_NAMESPACE,
         .library_namespace = g_driver_ns,
      };
      lib = android_dlopen_ext("libui.so", RTLD_NOW | RTLD_LOCAL, &ns_info);
      if (!lib)
         lib = android_dlopen_ext("mapper.mediatek.so",
                                  RTLD_NOW | RTLD_LOCAL, &ns_info);
   }
   if (!lib)
      lib = dlopen("libui.so", RTLD_NOW | RTLD_LOCAL);

   if (!lib) {
      LOGE("  could not load a mapper library: %s", dlerror());
      note(false, "loaded a library exporting AIMapper_loadIMapper");
      return;
   }

   /* libui.so *references* AIMapper_loadIMapper (it is the caller); the
    * symbol is exported by the vendor implementation. So if it is not in
    * whichever library loaded first, try the vendor mapper explicitly.
    */
   load_imapper_fn load =
      (load_imapper_fn)dlsym(lib, "AIMapper_loadIMapper");
   if (!load && g_driver_ns) {
      android_dlextinfo ns_info = {
         .flags = ANDROID_DLEXT_USE_NAMESPACE,
         .library_namespace = g_driver_ns,
      };
      static const char *impls[] = {"mapper.mediatek.so",
                                    "android.hardware.graphics.mapper@4.0.so",
                                    "gralloc.default.so"};
      for (uint32_t i = 0; i < ARRAY_LEN(impls) && !load; i++) {
         void *h = android_dlopen_ext(impls[i], RTLD_NOW | RTLD_LOCAL,
                                      &ns_info);
         LOGI("  %s -> %p", impls[i], h);
         if (h)
            load = (load_imapper_fn)dlsym(h, "AIMapper_loadIMapper");
      }
   }
   note(load != NULL, "found AIMapper_loadIMapper");
   if (!load)
      return;

   struct AIMapper *mapper = NULL;
   AIMapper_Error err = load(&mapper);
   LOGI("  AIMapper_loadIMapper -> %d (mapper %p)", err, (void *)mapper);
   note(err == 0 && mapper != NULL, "loaded the vendor IMapper5");
   if (err != 0 || !mapper)
      return;

   LOGI("  mapper version: %u", mapper->version);
   if (!mapper->v5.getStandardMetadata) {
      note(false, "IMapper5 exposes getStandardMetadata");
      return;
   }

   /* getStandardMetadata operates on a buffer that has come through
    * importBuffer(), not on the raw ANativeWindowBuffer handle straight from
    * dequeueBuffer. The first attempt skipped this and got n=77 for an 8-byte
    * ask - a strong sign the handle was simply not valid input for this call,
    * independent of any encoding question. AOSP's IMapper.h: importBuffer
    * takes the const native_handle_t* and hands back an opaque buffer
    * handle; every other v5 call, including getStandardMetadata, takes that
    * opaque handle, not the original one. freeBuffer releases it again.
    */
   if (!mapper->v5.importBuffer || !mapper->v5.freeBuffer) {
      note(false, "IMapper5 exposes importBuffer/freeBuffer");
      return;
   }

   void *imported = NULL;
   AIMapper_Error imp_err = mapper->v5.importBuffer(handle, &imported);
   LOGI("  importBuffer -> %d (buffer %p)", imp_err, imported);
   note(imp_err == 0 && imported != NULL, "imported the buffer");
   if (imp_err != 0 || !imported)
      return;

   /* AOSP's own doc for this call: the return is "the number of bytes
    * written to destBuffer, OR WHICH WOULD HAVE BEEN WRITTEN if
    * destBufferSize was large enough". The first two attempts passed an
    * 8-byte destBuffer and got n=77 both times - not an error, but the
    * driver saying the real payload needs 77 bytes and it only wrote (or
    * would have written) that many. Reading 8 bytes of a 77-byte structure
    * as a scalar was never going to produce the real value; that is what
    * this was actually measuring, and the "ABI mismatch" framing before this
    * was wrong.
    *
    * So: pass a buffer big enough for the real payload, then find the known
    * value inside it by search rather than by guessing the layout. The
    * allocation size (14,394,880 = 0x00dba400) is already known from the
    * gralloc handle int dump, so its exact byte position in the 77-byte
    * blob is discoverable rather than assumed - and once found, the same
    * offset convention very likely applies to PIXEL_FORMAT_MODIFIER's blob,
    * since both come from the same encoder.
    */
   uint8_t buf1[256] = {0};
   int32_t n = mapper->v5.getStandardMetadata(
      imported, STANDARD_METADATA_ALLOCATION_SIZE, buf1, sizeof(buf1));
   LOGI("  ALLOCATION_SIZE -> n=%d", n);

   if (n <= 0 || n > (int32_t)sizeof(buf1)) {
      LOGE("  no usable payload (n=%d) - cannot search for the known value",
           n);
      note(false, "got a usable ALLOCATION_SIZE payload");
      mapper->v5.freeBuffer(imported);
      return;
   }

   for (int32_t i = 0; i + 8 <= n; i++) {
      LOGI("    buf1[%3d..%3d]: %02x %02x %02x %02x %02x %02x %02x %02x", i,
           i + 7, buf1[i], buf1[i + 1], buf1[i + 2], buf1[i + 3], buf1[i + 4],
           buf1[i + 5], buf1[i + 6], buf1[i + 7]);
   }

   int32_t value_offset = -1;
   for (int32_t i = 0; i + 8 <= n; i++) {
      uint64_t candidate;
      memcpy(&candidate, buf1 + i, 8);
      if (candidate == known_alloc_size) {
         value_offset = i;
         break;
      }
   }
   LOGI("  known allocation size 0x%016llx found at byte offset %d",
        (unsigned long long)known_alloc_size, value_offset);
   note(value_offset >= 0, "located the scalar within the encoded payload");
   if (value_offset < 0) {
      mapper->v5.freeBuffer(imported);
      return;
   }

   uint8_t buf2[256] = {0};
   n = mapper->v5.getStandardMetadata(imported,
                                      STANDARD_METADATA_PIXEL_FORMAT_MODIFIER,
                                      buf2, sizeof(buf2));
   LOGI("  PIXEL_FORMAT_MODIFIER -> n=%d", n);
   note(n > 0 && n <= (int32_t)sizeof(buf2), "got a usable modifier payload");

   if (n > 0 && n <= (int32_t)sizeof(buf2) && value_offset + 8 <= n) {
      uint64_t modifier;
      memcpy(&modifier, buf2 + value_offset, 8);
      const uint64_t vendor = modifier >> 56;
      LOGI("  modifier @ offset %d = 0x%016llx  vendor=0x%02llx "
           "payload=0x%012llx%s",
           value_offset, (unsigned long long)modifier,
           (unsigned long long)vendor,
           (unsigned long long)(modifier & 0x00ffffffffffffffull),
           vendor == 0x08 ? "   (ARM - AFBC family)"
                          : (modifier == 0 ? "   (LINEAR)" : ""));
   } else {
      LOGI("  modifier payload shorter than the discovered offset (%d) - "
           "the two metadata types are not encoded the same way, this "
           "offset-reuse approach does not apply",
           value_offset);
   }

   mapper->v5.freeBuffer(imported);
}

/* Milestone 2: be the swapchain.
 *
 * Presents `frames` frames of a solid colour straight to the ANativeWindow,
 * doing what libvulkan.so would do if the app had gone through it: ask the
 * driver what gralloc usage it needs, dequeue a buffer from the window, wrap
 * it as a VkImage via VK_ANDROID_native_buffer, clear it, hand ownership
 * back with vkQueueSignalReleaseImageANDROID, and queue the buffer.
 *
 * Returns the number of frames that made it all the way to queueBuffer.
 */
static int
present_frames(VkDevice device, VkQueue queue, uint32_t queue_family,
               ANativeWindow *window, PFN_vkGetDeviceProcAddr gdpa, int frames)
{
   LOGI("=== present (milestone 2) ===");

#define DEV_FN(var, name)                                                    \
   PFN_##name var = (PFN_##name)gdpa(device, #name);                         \
   if (!var) {                                                               \
      LOGE("  missing device entrypoint: %s", #name);                        \
      check(false, "resolved " #name);                                       \
      return 0;                                                              \
   }

   DEV_FN(get_gralloc_usage, vkGetSwapchainGrallocUsageANDROID);
   DEV_FN(acquire_image, vkAcquireImageANDROID);
   DEV_FN(signal_release, vkQueueSignalReleaseImageANDROID);
   DEV_FN(create_image, vkCreateImage);
   DEV_FN(destroy_image, vkDestroyImage);
   DEV_FN(create_pool, vkCreateCommandPool);
   DEV_FN(destroy_pool, vkDestroyCommandPool);
   DEV_FN(alloc_cbs, vkAllocateCommandBuffers);
   DEV_FN(begin_cb, vkBeginCommandBuffer);
   DEV_FN(end_cb, vkEndCommandBuffer);
   DEV_FN(cmd_barrier, vkCmdPipelineBarrier);
   DEV_FN(cmd_clear, vkCmdClearColorImage);
   DEV_FN(create_sem, vkCreateSemaphore);
   DEV_FN(destroy_sem, vkDestroySemaphore);
   DEV_FN(queue_submit, vkQueueSubmit);
   DEV_FN(queue_wait_idle, vkQueueWaitIdle);
   check(true, "resolved the VK_ANDROID_native_buffer entrypoints");

   const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   /* The driver decides what gralloc usage bits its images need; the window
    * has to be told before it allocates anything.
    */
   int gralloc_usage = 0;
   VkResult r = get_gralloc_usage(device, format, usage, &gralloc_usage);
   LOGI("  vkGetSwapchainGrallocUsageANDROID -> %d (usage 0x%x)", r,
        gralloc_usage);
   check(r == VK_SUCCESS, "driver reported its gralloc usage");
   if (r != VK_SUCCESS)
      return 0;

   const int w = ANativeWindow_getWidth(window);
   const int h = ANativeWindow_getHeight(window);

   /* Connect as the EGL producer. Nothing will dequeue before this; the
    * platform loader does the same thing when it creates a swapchain.
    */
   struct ANativeWindowFull *wnd = (struct ANativeWindowFull *)window;
   int connected = wnd->perform(wnd, NATIVE_WINDOW_API_CONNECT,
                                NATIVE_WINDOW_API_EGL);
   LOGI("  api_connect(EGL) -> %d", connected);
   check(connected == 0, "connected to the window as a producer");
   if (connected != 0)
      return 0;

   int e = anw_set_usage(window, (uint64_t)gralloc_usage);
   e |= anw_set_format(window, HAL_PIXEL_FORMAT_RGBA_8888);
   e |= anw_set_dims(window, (uint32_t)w, (uint32_t)h);
   check(e == 0, "configured the ANativeWindow");
   if (e != 0)
      return 0;

   /* vkQueueSignalReleaseImageANDROID returns VK_ERROR_UNKNOWN on this
    * driver. Mesa's implementation first creates a semaphore exportable as a
    * SYNC_FD (vk_anb_semaphore_init_once) and then exports one from it, and
    * exportable sync-fd is a known kbase limitation. Those are two different
    * failures with two different fixes, so ask which it is directly rather
    * than infer it: create exactly that semaphore here.
    */
   {
      const VkExportSemaphoreCreateInfo export_info = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      const VkSemaphoreCreateInfo sem_ci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
         .pNext = &export_info,
      };
      VkSemaphore probe = VK_NULL_HANDLE;
      VkResult sr = create_sem(device, &sem_ci, NULL, &probe);
      LOGI("  vkCreateSemaphore(export SYNC_FD) -> %d", sr);
      note(sr == VK_SUCCESS, "driver can create a SYNC_FD-exportable semaphore");
      if (sr == VK_SUCCESS)
         destroy_sem(device, probe, NULL);
   }

   VkCommandPool pool = VK_NULL_HANDLE;
   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = queue_family,
   };
   r = create_pool(device, &pci, NULL, &pool);
   check(r == VK_SUCCESS, "created a command pool");
   if (r != VK_SUCCESS)
      return 0;

   int presented = 0;

   const bool sync_every_frame = getenv("PANVK_APP_SYNC_EVERY_FRAME") != NULL;
#define RING 4
   VkImage keep_img[RING];
   VkSemaphore keep_acq[RING], keep_rnd[RING];
   bool ring_used[RING] = {false};

   for (int frame = 0; frame < frames; frame++) {
      ANativeWindowBuffer_t *buf = NULL;
      int fence_fd = -1;

      int rc = anw_dequeue(window, &buf, &fence_fd);
      if (rc != 0 || !buf) {
         LOGE("  frame %d: dequeueBuffer failed: %d", frame, rc);
         break;
      }
      if (frame == 0) {
         LOGI("  buffer: %dx%d stride=%d format=%d handle=%p numFds=%d",
              buf->width, buf->height, buf->stride, buf->format,
              (const void *)buf->handle, buf->handle ? buf->handle->numFds : -1);
         check(buf->handle && buf->handle->numFds > 0,
               "dequeued a gralloc buffer with a usable handle");

         /* Dump the ints in the gralloc handle.
          *
          * The driver cannot ask gralloc for this buffer's format modifier:
          * u_gralloc's IMapper backends, the only ones that can, are
          * compiled out of an -Dandroid-stub build, so the ANB path assumes
          * LINEAR. Arm gralloc keeps an internal/alloc format in the handle
          * with the AFBC bits set in it, so if this buffer is AFBC it should
          * show up here as a 64-bit value with high bits set, split across
          * two adjacent ints.
          *
          * Archaeology, not an API. It exists to confirm or kill the AFBC
          * hypothesis for the Eden freeze, nothing more.
          */
         if (buf->handle)
            /* int[08] of the handle dump below is 0x00dba400. That is
             * 14,394,368 - earlier commits in this session hand-computed it
             * as 14,394,880, which is simply wrong arithmetic (368 read as
             * 880). Using the hex literal directly here so there is no more
             * decimal transcription to get wrong.
             */
            probe_imapper(buf->handle, 0x00dba400ull);

         if (buf->handle) {
            LOGI("  handle: numFds=%d numInts=%d", buf->handle->numFds,
                 buf->handle->numInts);
            const int *ints = &buf->handle->data[buf->handle->numFds];
            for (int i = 0; i < buf->handle->numInts && i < 64; i += 4) {
               int a = ints[i];
               int b = (i + 1 < buf->handle->numInts) ? ints[i + 1] : 0;
               int c = (i + 2 < buf->handle->numInts) ? ints[i + 2] : 0;
               int d = (i + 3 < buf->handle->numInts) ? ints[i + 3] : 0;
               LOGI("    int[%02d] %08x %08x %08x %08x", i, a, b, c, d);
            }
         }
      }

      /* Wrap the gralloc buffer as a VkImage. This is the step that has never
       * run on this driver: it is where the driver turns a native_handle_t
       * into memory it can render to.
       */
      const VkNativeBufferANDROID anb = {
         .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
         .handle = buf->handle,
         .stride = buf->stride,
         .format = buf->format,
         .usage = gralloc_usage,
         .usage3 = (uint64_t)gralloc_usage,
      };
      const VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .pNext = &anb,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = format,
         .extent = {(uint32_t)buf->width, (uint32_t)buf->height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = usage,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };

      VkImage image = VK_NULL_HANDLE;
      r = create_image(device, &ici, NULL, &image);
      if (r != VK_SUCCESS && frame != 0)
         LOGE("  frame %d: vkCreateImage -> %d", frame, r);
      if (frame == 0) {
         LOGI("  vkCreateImage(VkNativeBufferANDROID) -> %d", r);
         check(r == VK_SUCCESS, "wrapped the gralloc buffer as a VkImage");
      }
      if (r != VK_SUCCESS)
         break;

      /* Take ownership. The fence from dequeueBuffer says when the compositor
       * is finished with the buffer; handing it to the driver transfers the
       * wait onto the GPU instead of blocking here.
       */
      VkSemaphore acquire_sem = VK_NULL_HANDLE, render_sem = VK_NULL_HANDLE;
      const VkSemaphoreCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      create_sem(device, &sci, NULL, &acquire_sem);
      create_sem(device, &sci, NULL, &render_sem);

      r = acquire_image(device, image, fence_fd, acquire_sem, VK_NULL_HANDLE);
      if (r != VK_SUCCESS && frame != 0)
         LOGE("  frame %d: vkAcquireImageANDROID -> %d", frame, r);
      if (frame == 0) {
         LOGI("  vkAcquireImageANDROID -> %d", r);
         check(r == VK_SUCCESS, "acquired the image");
      }
      if (r != VK_SUCCESS) {
         destroy_sem(device, acquire_sem, NULL);
         destroy_sem(device, render_sem, NULL);
         destroy_image(device, image, NULL);
         break;
      }

      VkCommandBuffer cb = VK_NULL_HANDLE;
      const VkCommandBufferAllocateInfo cbai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      alloc_cbs(device, &cbai, &cb);

      const VkCommandBufferBeginInfo cbbi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      begin_cb(cb, &cbbi);

      const VkImageSubresourceRange range = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      };

      VkImageMemoryBarrier to_dst = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = range,
      };
      cmd_barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                  &to_dst);

      /* A colour that changes per frame, so a human watching the screen can
       * tell presentation is live rather than one stuck frame.
       */
      const VkClearColorValue colour = {
         .float32 = {frame & 1 ? 0.9f : 0.1f, 0.4f, frame & 1 ? 0.1f : 0.9f,
                     1.0f}};
      cmd_clear(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1,
                &range);

      VkImageMemoryBarrier to_present = to_dst;
      to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_present.dstAccessMask = 0;
      to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      cmd_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                  &to_present);

      end_cb(cb);

      const VkPipelineStageFlags wait_stage =
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      const VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &acquire_sem,
         .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &cb,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &render_sem,
      };
      r = queue_submit(queue, 1, &si, VK_NULL_HANDLE);
      if (frame == 0)
         check(r == VK_SUCCESS, "submitted the clear");
      else if (r != VK_SUCCESS)
         LOGE("  frame %d: vkQueueSubmit -> %d", frame, r);

      /* Give ownership back and get a fence saying when the GPU is done. */
      int release_fd = -1;
      if (r == VK_SUCCESS) {
         r = signal_release(queue, 1, &render_sem, image, &release_fd);
         if (r != VK_SUCCESS && frame != 0)
            LOGE("  frame %d: vkQueueSignalReleaseImageANDROID -> %d", frame, r);
      }
      if (frame == 0) {
         LOGI("  vkQueueSignalReleaseImageANDROID -> %d (fence fd %d)", r,
              release_fd);
         check(r == VK_SUCCESS, "released the image back to the window");
      }

      if (r == VK_SUCCESS) {
         rc = anw_queue(window, buf, release_fd);
         if (frame == 0)
            check(rc == 0, "queued the buffer to the window");
         if (rc == 0)
            presented++;
      }

      /* PANVK_APP_SYNC_EVERY_FRAME=1 restores the original behaviour: block
       * until the GPU is idle before touching the next frame.
       *
       * The default is deliberately NOT to do that. Draining the queue every
       * frame hides whether the release fence handed to queueBuffer actually
       * means anything, and this driver hands back -1 ("already signalled")
       * because kbase cannot export a real fence. An emulator presenting
       * continuously does rely on it - Eden froze the whole device with
       * SurfaceFlinger and hwcomposer stuck in waitForever on unsignalled
       * mali kcpu fences. Presenting without the drain is the smallest thing
       * that reproduces that shape of workload in a harness we control.
       *
       * Per-frame objects therefore have to outlive the iteration; they are
       * kept and destroyed after the loop. Bounded by frame count, so a long
       * run is a deliberate choice rather than a leak.
       */
      if (sync_every_frame) {
         queue_wait_idle(queue);
         destroy_sem(device, acquire_sem, NULL);
         destroy_sem(device, render_sem, NULL);
         destroy_image(device, image, NULL);
      } else {
         /* Recycle through a small ring instead of hoarding every frame.
          * Slot (frame % RING) held a frame RING presents ago; dequeueBuffer
          * blocks until the window hands a buffer back, so by then that
          * frame has been through the compositor and its objects are done.
          * This is what a real app does, and it is the difference that
          * separates "the harness holds too much" from "the driver leaks
          * per present".
          */
         uint32_t slot = (uint32_t)frame % RING;
         if (ring_used[slot]) {
            destroy_sem(device, keep_acq[slot], NULL);
            destroy_sem(device, keep_rnd[slot], NULL);
            destroy_image(device, keep_img[slot], NULL);
         }
         keep_img[slot] = image;
         keep_acq[slot] = acquire_sem;
         keep_rnd[slot] = render_sem;
         ring_used[slot] = true;
      }

      if ((frame % 30) == 0)
         LOGI("  ... frame %d ok", frame);

      if (r != VK_SUCCESS)
         break;
   }

   if (!sync_every_frame) {
      /* One drain at the end, then release everything the loop held. */
      queue_wait_idle(queue);
      for (uint32_t i = 0; i < RING; i++) {
         if (!ring_used[i])
            continue;
         destroy_sem(device, keep_acq[i], NULL);
         destroy_sem(device, keep_rnd[i], NULL);
         destroy_image(device, keep_img[i], NULL);
      }
   }

   LOGI("  presented %d/%d frames", presented, frames);
   check(presented == frames, "presented every frame");

   destroy_pool(device, pool, NULL);
   wnd->perform(wnd, NATIVE_WINDOW_API_DISCONNECT, NATIVE_WINDOW_API_EGL);
   return presented;

#undef DEV_FN
}

/* Brings up Vulkan far enough to prove the driver is usable in this process,
 * then hands off to present_frames() to do the platform loader's job.
 */
static void
run_vulkan(ANativeWindow *window, PFN_vkGetInstanceProcAddr gipa)
{
   LOGI("=== bring up Vulkan ===");

#define INST_FN(name)                                                        \
   PFN_##name name = (PFN_##name)gipa(instance, #name);                      \
   if (!name) {                                                              \
      LOGE("  missing entrypoint: %s", #name);                               \
      check(false, "resolved " #name);                                       \
      return;                                                                \
   }

   VkInstance instance = VK_NULL_HANDLE;

   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   check(create_instance != NULL, "resolved vkCreateInstance");
   if (!create_instance)
      return;

   const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-kbase-swapchain-app",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
   };

   VkResult r = create_instance(&ici, NULL, &instance);
   LOGI("  vkCreateInstance -> %d", r);
   check(r == VK_SUCCESS, "created a VkInstance");
   if (r != VK_SUCCESS)
      return;

   INST_FN(vkEnumeratePhysicalDevices);
   INST_FN(vkGetPhysicalDeviceProperties);
   INST_FN(vkGetPhysicalDeviceQueueFamilyProperties);
   INST_FN(vkEnumerateDeviceExtensionProperties);
   INST_FN(vkCreateDevice);
   INST_FN(vkDestroyDevice);
   INST_FN(vkDestroyInstance);

   uint32_t count = 0;
   r = vkEnumeratePhysicalDevices(instance, &count, NULL);
   LOGI("  physical devices: %u (result %d)", count, r);
   check(r == VK_SUCCESS && count > 0, "found at least one physical device");
   if (r != VK_SUCCESS || count == 0) {
      vkDestroyInstance(instance, NULL);
      return;
   }

   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   count = 1;
   vkEnumeratePhysicalDevices(instance, &count, &pdev);

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   LOGI("  device: %s", props.deviceName);
   LOGI("  driver API version: %u.%u.%u", VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
   check(true, "queried device properties");

   /* VK_ANDROID_native_buffer is what milestone 2 needs: it is how a driver
    * turns a gralloc buffer from the ANativeWindow into a VkImage. Check for
    * it now, so this run says whether presentation is even reachable.
    */
   uint32_t ext_count = 0;
   vkEnumerateDeviceExtensionProperties(pdev, NULL, &ext_count, NULL);
   VkExtensionProperties *exts =
      calloc(ext_count ? ext_count : 1, sizeof(*exts));
   vkEnumerateDeviceExtensionProperties(pdev, NULL, &ext_count, exts);

   bool has_native_buffer = false;
   for (uint32_t i = 0; i < ext_count; i++) {
      if (!strcmp(exts[i].extensionName, "VK_ANDROID_native_buffer"))
         has_native_buffer = true;
   }
   LOGI("  device extensions: %u", ext_count);
   check(has_native_buffer,
         "driver advertises VK_ANDROID_native_buffer (needed to present)");
   free(exts);

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
   VkQueueFamilyProperties *qfs = calloc(qf_count ? qf_count : 1, sizeof(*qfs));
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, qfs);

   uint32_t gfx_family = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         gfx_family = i;
         break;
      }
   }
   free(qfs);
   check(gfx_family != UINT32_MAX, "found a graphics queue family");
   if (gfx_family == UINT32_MAX) {
      vkDestroyInstance(instance, NULL);
      return;
   }

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = gfx_family,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   /* VK_ANDROID_native_buffer is what makes presentation possible at all -
    * without it there is no way to turn a window buffer into a VkImage.
    */
   /* Bypassing the platform loader means enabling what it would have enabled.
    * vkQueueSignalReleaseImageANDROID exports a sync fd internally, via
    * vkGetSemaphoreFdKHR, so the external-fd extensions are not optional here
    * even though the app never calls them directly - without them that
    * entrypoint resolves to NULL and the release fails VK_ERROR_UNKNOWN.
    */
   const char *dev_exts[] = {
      "VK_ANDROID_native_buffer",
      "VK_KHR_external_semaphore_fd",
      "VK_KHR_external_fence_fd",
      "VK_KHR_external_memory_fd",
      "VK_EXT_image_drm_format_modifier",
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount =
         has_native_buffer ? (uint32_t)ARRAY_LEN(dev_exts) : 0,
      .ppEnabledExtensionNames = dev_exts,
   };

   VkDevice device = VK_NULL_HANDLE;
   r = vkCreateDevice(pdev, &dci, NULL, &device);
   LOGI("  vkCreateDevice -> %d", r);
   check(r == VK_SUCCESS, "created a VkDevice from inside the app");

   if (window) {
      LOGI("  ANativeWindow: %dx%d format=%d", ANativeWindow_getWidth(window),
           ANativeWindow_getHeight(window), ANativeWindow_getFormat(window));
      check(true, "have a real ANativeWindow to present to");
   } else {
      check(false, "have a real ANativeWindow to present to");
   }

   /* Milestone 2. Everything above is setup; this is the part that has never
    * run against a real window on this driver.
    */
   if (device != VK_NULL_HANDLE && window && has_native_buffer) {
      PFN_vkGetDeviceProcAddr gdpa =
         (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
      PFN_vkGetDeviceQueue get_queue =
         (PFN_vkGetDeviceQueue)gipa(instance, "vkGetDeviceQueue");

      if (gdpa && get_queue && load_native_window_api()) {
         check(true, "resolved the ANativeWindow producer API");
         VkQueue queue = VK_NULL_HANDLE;
         get_queue(device, gfx_family, 0, &queue);

         /* Geometry and allocation size measured from the gralloc handle on
          * this device; see the handle dump in present_frames(). Corrected
          * to the actual value of int[08] (0x00dba400) - earlier commits in
          * this session hand-computed that hex value as 14,394,880, which
          * is wrong arithmetic (368 misread as 880); it is 14,394,368.
          *
          * This candidate-enumeration approach is superseded by
          * probe_imapper() below, which reads the real modifier from gralloc
          * directly instead of guessing candidates and matching sizes. Left
          * in place as a secondary check: probe_imapper()'s answer should
          * also explain this function's allocation-size target exactly.
          */
         probe_modifiers(pdev, gipa, instance, device, gdpa, 1280, 2768,
                         0x00dba400ull);
         /* Long enough to outlast the window's buffer count many times
          * over, which is what makes the release fence load-bearing.
          * Default bumped way up (was 4) to reproduce the fdsan crash seen
          * under Azahar's continuous present loop - that took 20-30+ seconds
          * of real presentation to surface, so a handful of frames here
          * never had a chance to hit it. Override with PANVK_APP_FRAMES for
          * a quick smoke run.
          */
         int frames = 3000;
         const char *frames_env = getenv("PANVK_APP_FRAMES");
         if (frames_env)
            frames = atoi(frames_env);
         present_frames(device, queue, gfx_family, window, gdpa, frames);
      } else {
         check(false, "resolved the ANativeWindow producer API");
      }
   }

   if (device != VK_NULL_HANDLE)
      vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

#undef INST_FN
}

static void
run_everything(ANativeWindow *window, const char *internal_path)
{
   g_checks_run = 0;
   g_checks_failed = 0;

   /* AFBC modifiers are gated behind PANVK_DEBUG=wsi_afbc in
    * panvk_physical_device.c - without it the driver advertises LINEAR only,
    * which is why importing an AFBC gralloc buffer had nothing to describe
    * it with. Set before the driver loads, since it is read during device
    * setup. PANVK_APP_NO_WSI_AFBC=1 turns this back off for comparison.
    */
   if (!getenv("PANVK_APP_NO_WSI_AFBC") && !getenv("PANVK_DEBUG"))
      setenv("PANVK_DEBUG", "wsi_afbc", 1);

   PFN_vkGetInstanceProcAddr gipa = load_driver_from_anywhere(internal_path);
   if (gipa)
      run_vulkan(window, gipa);

   LOGI("=== %d checks, %d failed ===", g_checks_run, g_checks_failed);
   if (g_checks_failed == 0)
      LOGI("RESULT: PASS - the driver is usable from inside an app; "
           "presentation is milestone 2");
   else
      LOGE("RESULT: FAIL - %d check(s) failed", g_checks_failed);
}

/* ------------------------------------------------------------------ */

static void
on_app_cmd(struct android_app *app, int32_t cmd)
{
   /* APP_CMD_INIT_WINDOW is the first moment a real ANativeWindow exists,
    * which is exactly what this app is here to get hold of.
    */
   if (cmd == APP_CMD_INIT_WINDOW && app->window != NULL)
      run_everything(app->window, app->activity->internalDataPath);
}

void
android_main(struct android_app *app)
{
   app->onAppCmd = on_app_cmd;

   for (;;) {
      int events;
      struct android_poll_source *source;

      while (ALooper_pollOnce(-1, NULL, &events, (void **)&source) >= 0) {
         if (source)
            source->process(app, source);
         if (app->destroyRequested)
            return;
      }
   }
}
