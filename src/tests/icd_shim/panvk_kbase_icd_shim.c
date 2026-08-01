// A minimal Vulkan ICD shim, purpose-built to let tools that expect the
// standard Vulkan loader ABI (deqp-vk, in particular) talk to this repo's
// kbase-backed driver, which speaks Android's hwvulkan HAL ABI instead.
//
// Why this exists at all: every probe in this repo (driver_enum_probe.c
// onward) works around the same fact by hand - libvulkan_panfrost.so does
// not export vkGetInstanceProcAddr directly. It exports a single symbol,
// "HMI" (an Android hw_module_t), and getting a real Vulkan entrypoint out
// of it means dlsym-ing HMI, calling its open() method to get an
// hwvulkan_device_t, and reading GetInstanceProcAddr/CreateInstance/
// EnumerateInstanceExtensionProperties off THAT struct instead. That is
// exactly what Android's own real libvulkan.so does internally when an app
// calls into the platform's normal Vulkan loader - this file is a small,
// standalone reimplementation of just that one piece, scoped down to
// loading one specific driver .so by path instead of going through
// Android's hw_get_module() HAL discovery (which cannot be redirected to
// an experimental driver without installing it as the system's real
// Vulkan HAL - not something to do to a real device's system partition
// for test purposes).
//
// Verified against VK-GL-CTS's own source (third_party/VK-GL-CTS,
// framework/platform/android/tcuAndroidPlatform.cpp): its VulkanLibrary
// dlopens whatever path Platform::createLibrary() is given and does
// exactly one thing with it - dlsym("vkGetInstanceProcAddr") - before
// bootstrapping everything else through the returned function pointer,
// per vkPlatform.cpp's PlatformDriver constructor. No ICD manifest, no
// loader-negotiation handshake (vk_icdNegotiateLoaderICDInterfaceVersion)
// is required for this path - CTS's Android platform layer does not call
// it. So the only contract this file has to satisfy is: export a symbol
// literally named vkGetInstanceProcAddr, with the standard signature.
//
// Which driver .so to bridge to is read from the PANVK_KBASE_ICD_DRIVER
// environment variable, defaulting to a path on /data/local/tmp that
// matches where this repo's own probes have been pushing the built driver
// all session. Not hardcoded only so a differently-named or -located
// build can be tested without editing and recompiling this file.
//
// Build as a shared library: -shared -fPIC, linked with -ldl. See
// `make icd_shim` in the root makefile.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>

#define DEFAULT_DRIVER_PATH "/data/local/tmp/libvulkan_panfrost.so"
#define DRIVER_PATH_ENV "PANVK_KBASE_ICD_DRIVER"

/* Same libhardware ABI mirrors as every probe in src/tests/ this session -
 * the NDK does not ship these headers. See driver_enum_probe.c for the
 * long note on the LP64 `reserved` width; getting it wrong silently
 * shifts every later field so the Vulkan entrypoints read back NULL
 * instead of failing loudly.
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

/* Lazily initialised, once, on first call - CTS calls vkGetInstanceProcAddr
 * many times before any instance exists, so this cannot be a constructor
 * that runs before main() has decided anything; it has to be safe to be
 * the very first thing called into this library at all.
 */
static hwvulkan_device_t *g_hwdevice;
static int g_init_attempted;
static int g_init_failed;

static void
init_once(void)
{
   if (g_init_attempted)
      return;
   g_init_attempted = 1;

   const char *path = getenv(DRIVER_PATH_ENV);
   if (!path)
      path = DEFAULT_DRIVER_PATH;

   void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      fprintf(stderr, "panvk_kbase_icd_shim: dlopen(%s) failed: %s\n", path,
              dlerror());
      g_init_failed = 1;
      return;
   }

   hw_module_t *mod = dlsym(h, "HMI");
   if (!mod || !mod->methods || !mod->methods->open) {
      fprintf(stderr,
              "panvk_kbase_icd_shim: %s has no usable HMI/open - not a "
              "hwvulkan HAL module\n",
              path);
      g_init_failed = 1;
      return;
   }

   hw_device_t *hwdev = NULL;
   if (mod->methods->open(mod, HWVULKAN_DEVICE_0, &hwdev) != 0 || !hwdev) {
      fprintf(stderr, "panvk_kbase_icd_shim: HAL open(\"%s\") failed\n",
              HWVULKAN_DEVICE_0);
      g_init_failed = 1;
      return;
   }

   g_hwdevice = (hwvulkan_device_t *)hwdev;
   fprintf(stderr, "panvk_kbase_icd_shim: bridged to %s\n", path);
}

/* The one symbol this entire file exists to export. Everything above is
 * how it gets answered.
 */
PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   init_once();
   if (g_init_failed || !g_hwdevice)
      return NULL;

   /* Per the Vulkan spec, these are the only entrypoints valid to query
    * with instance == NULL - the pre-instance bootstrap set. The HAL
    * struct exposes exactly two of them as direct fields (this device
    * does not support layers, so EnumerateInstanceLayerProperties and
    * EnumerateInstanceVersion are not in the struct - GetInstanceProcAddr
    * itself covers whatever the driver does implement, which the fallthrough
    * below handles).
    */
   if (instance == NULL && pName != NULL) {
      if (strcmp(pName, "vkCreateInstance") == 0)
         return (PFN_vkVoidFunction)g_hwdevice->CreateInstance;
      if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0)
         return (PFN_vkVoidFunction)g_hwdevice->EnumerateInstanceExtensionProperties;
   }

   return g_hwdevice->GetInstanceProcAddr(instance, pName);
}
