// Does the driver report external-memory capability truthfully on kbase?
//
// The two directions are NOT symmetric, and this probe exists to make sure
// the driver says so:
//
//   import - WORKS, for dma-bufs. pan_kmod_ops::bo_import_fd dispatches to
//            the backend before any DRM call (patch-pan-kmod-import-fd.py),
//            and kbase_kmod_bo_import_fd() runs KBASE_IOCTL_MEM_IMPORT with
//            BASE_MEM_IMPORT_TYPE_UMM. Measured end to end by
//            tests/dmabuf_import_probe and tests/driver_dmabuf_probe.
//
//   export - IMPOSSIBLE. Nothing in kbase's UAPI turns an allocation into an
//            fd: no PRIME, no dmabuf-out. Not "not yet".
//
// So the expected report on kbase is:
//
//   DMA_BUF    IMPORTABLE set, EXPORTABLE clear,
//              exportFromImportedHandleTypes == 0
//   OPAQUE_FD  nothing at all - an OPAQUE_FD import is a promise with no
//              producer, since the only way to get a PanVK opaque fd is to
//              export one, and this driver cannot.
//
// Getting either direction wrong costs an application real debugging time.
// Claiming export it does not have turns a clean query-time refusal into a
// VK_ERROR_OUT_OF_DEVICE_MEMORY from vkGetMemoryFdKHR(); denying import it
// does have silently gives up the path Android WSI needs.
//
// NOTE: this probe previously asserted the opposite - that NO feature bit
// was set, which was correct when import was unreachable. It failed on
// success the day import started working, which is exactly what a probe
// pinned to a stale contract does. Kept as a reminder to update the probe
// with the behaviour, not after it.
//
// Usage: driver_extmem_probe /data/local/tmp/libvulkan_panfrost.so
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
   VkExternalMemoryHandleTypeFlags from_imported =
      props.externalMemoryProperties.exportFromImportedHandleTypes;

   printf("\n  %s: externalMemoryFeatures = 0x%x%s%s\n", handle_type_name(type),
          f, (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? " EXPORTABLE" : "",
          (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? " IMPORTABLE" : "");
   printf("  %s: exportFromImportedHandleTypes = 0x%x\n",
          handle_type_name(type), from_imported);

   const bool want_import =
      (type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   char msg[160];

   /* Export is impossible on kbase in every case - no PRIME, no dmabuf-out. */
   snprintf(msg, sizeof(msg), "%s: not advertised as EXPORTABLE",
            handle_type_name(type));
   check(!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT), msg);

   /* Import: expected for dma-bufs, and expected ABSENT for OPAQUE_FD, where
    * no application could ever obtain a matching fd from this driver.
    */
   if (want_import) {
      snprintf(msg, sizeof(msg), "%s: advertised as IMPORTABLE (it works)",
               handle_type_name(type));
      check((f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0, msg);
   } else {
      snprintf(msg, sizeof(msg),
               "%s: not advertised as IMPORTABLE (no producer for it)",
               handle_type_name(type));
      check(!(f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT), msg);
   }

   /* "You can re-export what you imported" is false here in both cases, and
    * is the kind of claim that fails at vkGetMemoryFdKHR() rather than at
    * query time.
    */
   snprintf(msg, sizeof(msg), "%s: exportFromImportedHandleTypes is empty",
            handle_type_name(type));
   check(from_imported == 0, msg);
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
      printf("  The driver reports external memory truthfully: dma-buf\n"
             "  import yes, export no, no re-export of imports, and no\n"
             "  OPAQUE_FD import it could never receive.\n");
   else
      printf("  %d check(s) FAILED - the driver's external-memory report and\n"
             "  what it can actually do have diverged. Either it claims a\n"
             "  capability that will fail at vkAllocateMemory/\n"
             "  vkGetMemoryFdKHR(), or it denies one that works and an\n"
             "  application will route around a path it did not need to.\n",
             failures);

   return failures ? 1 : 0;
}
