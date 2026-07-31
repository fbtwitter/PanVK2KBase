// Does the driver still claim it can share dma-bufs?
//
// It should not, on kbase. Neither direction works there:
//
//   import - kbase itself can do it (KBASE_IOCTL_MEM_IMPORT), but
//            pan_kmod_bo_import() turns the fd into a GEM handle with
//            drmPrimeFDToHandle() *before* dispatching to the backend, and
//            that fails on a misc device. The backend hook is unreachable.
//
//   export - kbase has no export path at all. Nothing in the UAPI turns an
//            allocation into an fd.
//
// PanVK advertises OPAQUE_FD and DMA_BUF as EXPORTABLE|IMPORTABLE for every
// device, which on kbase is a promise it cannot keep: an application that
// believes the query gets VK_ERROR_OUT_OF_DEVICE_MEMORY out of
// vkGetMemoryFdKHR() instead of a clean refusal at query time.
// patch-panvk-kbase-external-memory.py gates that on the backend, and this
// probe is what says whether the gate actually took effect.
//
// Checks vkGetPhysicalDeviceExternalBufferProperties for both handle types
// and expects an empty feature mask. Exits non-zero if the driver is still
// claiming a capability it does not have.
//
// Usage: driver_extmem_probe /data/local/tmp/libvulkan_panfrost.so
#define VK_USE_PLATFORM_ANDROID_KHR
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* Minimal mirrors of libhardware's ABI - the NDK does not ship those
 * headers. Kept identical to tests/driver_enum_probe, including the LP64
 * conditional on `reserved`: on 64-bit it is uint64_t, and getting it wrong
 * silently shifts every following field so the Vulkan entrypoints read back
 * as NULL rather than failing loudly.
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

static int failures;

static void
check(bool ok, const char *what)
{
   printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

static const char *
handle_type_name(VkExternalMemoryHandleTypeFlagBits t)
{
   switch (t) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
      return "OPAQUE_FD";
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      return "DMA_BUF";
   default:
      return "?";
   }
}

static void
probe_handle_type(PFN_vkGetPhysicalDeviceExternalBufferProperties fn,
                  VkPhysicalDevice pd, VkExternalMemoryHandleTypeFlagBits type)
{
   VkPhysicalDeviceExternalBufferInfo info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .handleType = type,
   };
   VkExternalBufferProperties props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
   };

   fn(pd, &info, &props);

   VkExternalMemoryFeatureFlags f =
      props.externalMemoryProperties.externalMemoryFeatures;

   printf("\n  %s: externalMemoryFeatures = 0x%x%s%s\n", handle_type_name(type),
          f, (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? " EXPORTABLE" : "",
          (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? " IMPORTABLE" : "");

   char msg[128];
   snprintf(msg, sizeof(msg), "%s: not advertised as EXPORTABLE",
            handle_type_name(type));
   check(!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT), msg);

   snprintf(msg, sizeof(msg), "%s: not advertised as IMPORTABLE",
            handle_type_name(type));
   check(!(f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT), msg);
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc != 2) {
      fprintf(stderr, "usage: %s <path-to-libvulkan_panfrost.so>\n", argv[0]);
      return 2;
   }

   void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen failed: %s\n", dlerror());
      return 1;
   }

   hw_module_t *mod = dlsym(h, "HMI");
   if (!mod || !mod->methods || !mod->methods->open) {
      printf("no usable HMI/open in module\n");
      return 1;
   }

   hw_device_t *dev = NULL;
   if (mod->methods->open(mod, HWVULKAN_DEVICE_0, &dev) != 0 || !dev) {
      printf("HAL open failed\n");
      return 1;
   }

   hwvulkan_device_t *vk = (hwvulkan_device_t *)dev;
   if (!vk->CreateInstance) {
      printf("HAL device has no CreateInstance at the expected offset\n");
      return 1;
   }

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-kbase-extmem-probe",
      /* 1.1 is where GetPhysicalDeviceExternalBufferProperties became core. */
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };

   VkInstance inst = VK_NULL_HANDLE;
   if (vk->CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
      printf("vkCreateInstance failed\n");
      return 1;
   }

   /* Use the HAL's own GetInstanceProcAddr so every call lands in this
    * driver rather than whatever the system loader would pick.
    */
   PFN_vkEnumeratePhysicalDevices enum_pd =
      (PFN_vkEnumeratePhysicalDevices)vk->GetInstanceProcAddr(
         inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceExternalBufferProperties get_ext =
      (PFN_vkGetPhysicalDeviceExternalBufferProperties)vk->GetInstanceProcAddr(
         inst, "vkGetPhysicalDeviceExternalBufferProperties");

   if (!enum_pd || !get_ext) {
      printf("missing entrypoint: enum_pd=%p get_ext=%p\n", (void *)enum_pd,
             (void *)get_ext);
      return 1;
   }

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   if (enum_pd(inst, &count, &pd) != VK_SUCCESS || count == 0) {
      printf("no physical device\n");
      return 1;
   }
   printf("physical device enumerated through the kbase path\n");

   probe_handle_type(get_ext, pd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
   probe_handle_type(get_ext, pd,
                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   printf("\n=== verdict ===\n");
   if (failures == 0)
      printf("  The driver no longer claims dma-buf sharing it cannot do.\n");
   else
      printf("  %d check(s) FAILED - still advertising an unsupported\n"
             "  capability. An app trusting this query will fail at\n"
             "  vkGetMemoryFdKHR() instead of here.\n",
             failures);

   return failures ? 1 : 0;
}
