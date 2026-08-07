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

/* Loads the driver the way a driver picker does and returns its
 * vkGetInstanceProcAddr, or NULL. The two steps are independent failures and
 * are checked separately, because they fail for completely different reasons:
 * the namespace step fails when the app cannot reach the vendor libraries,
 * the HAL step fails when the .so is not a hwvulkan module.
 */
static PFN_vkGetInstanceProcAddr
load_driver(const char *path)
{
   LOGI("=== load the driver from inside an app ===");
   LOGI("  driver: %s", path);

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

   check(create_ns && link_ns, "bionic namespace API reachable from an app");
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
   check(ns != NULL, "created a linker namespace");
   if (!ns)
      return NULL;

   bool linked = link_ns(ns, NULL, PUBLIC_SONAMES);
   check(linked, "linked it to the platform's public libraries");
   if (!linked)
      return NULL;

   android_dlextinfo info = {
      .flags = ANDROID_DLEXT_USE_NAMESPACE,
      .library_namespace = ns,
   };
   void *h = android_dlopen_ext(base, RTLD_NOW | RTLD_LOCAL, &info);
   if (!h) {
      LOGE("  android_dlopen_ext(%s) failed: %s", base, dlerror());
      check(false, "driver loaded through that namespace");
      return NULL;
   }
   check(true, "driver loaded through that namespace");

   /* Now the HAL walk. */
   hw_module_t *mod = (hw_module_t *)dlsym(h, "HMI");
   if (!mod || !mod->methods || !mod->methods->open) {
      check(false, "driver exports a usable hwvulkan HMI module");
      return NULL;
   }
   check(true, "driver exports a usable hwvulkan HMI module");
   LOGI("  HMI: id=%s name=%s", mod->id ? mod->id : "(null)",
        mod->name ? mod->name : "(null)");

   hw_device_t *hwdev = NULL;
   if (mod->methods->open(mod, HWVULKAN_DEVICE_0, &hwdev) != 0 || !hwdev) {
      check(false, "HAL open(\"vk0\") succeeded");
      return NULL;
   }
   check(true, "HAL open(\"vk0\") succeeded");

   PFN_vkGetInstanceProcAddr gipa =
      ((hwvulkan_device_t *)hwdev)->GetInstanceProcAddr;
   check(gipa != NULL, "got a real vkGetInstanceProcAddr");
   return gipa;
}

/* ------------------------------------------------------------------ */

/* Brings up Vulkan far enough to prove the driver is usable in this process,
 * and reports what would be needed to present. Deliberately stops short of a
 * swapchain - see the milestone note at the top of this file.
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
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
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

   if (device != VK_NULL_HANDLE)
      vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

#undef INST_FN
}

static void
run_everything(ANativeWindow *window)
{
   g_checks_run = 0;
   g_checks_failed = 0;

   const char *path = getenv("PANVK_KBASE_ICD_DRIVER");
   if (!path)
      path = DEFAULT_DRIVER;

   PFN_vkGetInstanceProcAddr gipa = load_driver(path);
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
      run_everything(app->window);
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
