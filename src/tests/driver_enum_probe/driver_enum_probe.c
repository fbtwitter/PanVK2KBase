// Drives a built PanVK Android driver through its hwvulkan HAL entrypoint
// and asks it to enumerate physical devices - WITHOUT installing it as the
// system driver.
//
// This is the test that actually exercises the kbase backend end to end:
//
//   vkEnumeratePhysicalDevices
//     -> panvk_enumerate_devices        (our enumeration hook)
//        -> open("/dev/mali0")
//        -> pan_kmod_fd_is_kbase()      -> KBASE_IOCTL_VERSION_CHECK
//        -> pan_kmod_dev_create()
//           -> kbase_kmod_dev_create()  (our backend)
//              -> KBASE_IOCTL_SET_FLAGS
//              -> KBASE_IOCTL_GET_GPUPROPS + decode into pan_kmod_dev_props
//        -> pan_get_model()             (is this GPU one PanVK supports?)
//
// Android's Vulkan loader would normally do the HAL open for us, but that
// requires the driver to be installed at /vendor/lib64/hw/vulkan.<hw>.so.
// Doing it by hand keeps the running graphics stack untouched.
//
// Driver diagnostics go through liblog, so run with PANVK_DEBUG=startup and
// check `adb logcat -s mesa` alongside this output.
//
// Usage: driver_enum_probe /data/local/tmp/libvulkan_panfrost.so
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* Minimal mirrors of libhardware's ABI. The NDK doesn't ship these headers,
 * and only the layout matters here - it must match AOSP's
 * hardware/libhardware/include/hardware/hardware.h exactly.
 *
 * NOTE the LP64 conditional on `reserved`: on 64-bit it is uint64_t, not
 * uint32_t. Getting this wrong silently shifts every following field - it
 * put `close` at offset 64 instead of 112 here, so the Vulkan entrypoints
 * read back as NULL rather than failing loudly.
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

/* hardware/libhardware/include/hardware/hwvulkan.h */
typedef struct hwvulkan_device_t {
   struct hw_device_t common;
   PFN_vkEnumerateInstanceExtensionProperties
      EnumerateInstanceExtensionProperties;
   PFN_vkCreateInstance CreateInstance;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

#define HWVULKAN_DEVICE_0 "vk0"

int main(int argc, char **argv) {
   if (argc != 2) {
      fprintf(stderr, "usage: %s <path-to-libvulkan_panfrost.so>\n", argv[0]);
      return 2;
   }

   void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen failed: %s\n", dlerror());
      return 1;
   }
   printf("loaded %s\n", argv[1]);

   hw_module_t *mod = dlsym(h, "HMI");
   if (!mod || !mod->methods || !mod->methods->open) {
      printf("no usable HMI/open in module\n");
      return 1;
   }

   hw_device_t *dev = NULL;
   int ret = mod->methods->open(mod, HWVULKAN_DEVICE_0, &dev);
   if (ret != 0 || !dev) {
      printf("HAL open(\"%s\") failed: %d\n", HWVULKAN_DEVICE_0, ret);
      return 1;
   }
   printf("HAL open OK\n");

   hwvulkan_device_t *vk = (hwvulkan_device_t *)dev;

   printf("  common.tag     = 0x%08x (expect 0x48574454 HWDT)\n",
          dev->tag);
   printf("  common.version = 0x%08x\n", dev->version);

   if (!vk->CreateInstance) {
      /* Struct layout mismatch is far more likely than the driver genuinely
       * publishing NULL entrypoints, so dump the object and locate the
       * function pointers by scanning for addresses inside this .so's
       * mapping rather than guessing at libhardware's field order.
       */
      printf("  CreateInstance is NULL at the assumed offset - dumping\n");
      void **words = (void **)dev;
      for (int i = 0; i < 20; i++) {
         if (words[i])
            printf("    [%2d] off=%3zu  %p\n", i, i * sizeof(void *),
                   words[i]);
      }
      printf("HAL device has no CreateInstance at the expected offset\n");
      return 1;
   }

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-kbase-enum-probe",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };

   VkInstance inst = VK_NULL_HANDLE;
   VkResult r = vk->CreateInstance(&ici, NULL, &inst);
   printf("vkCreateInstance -> %d%s\n", r, r == VK_SUCCESS ? " (OK)" : "");
   if (r != VK_SUCCESS)
      return 1;

   /* The HAL hands back its own GetInstanceProcAddr; use it rather than the
    * system loader's, so every call lands in this driver.
    */
   PFN_vkEnumeratePhysicalDevices enum_pd =
      (PFN_vkEnumeratePhysicalDevices)vk->GetInstanceProcAddr(
         inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties get_props =
      (PFN_vkGetPhysicalDeviceProperties)vk->GetInstanceProcAddr(
         inst, "vkGetPhysicalDeviceProperties");
   PFN_vkEnumerateDeviceExtensionProperties enum_ext =
      (PFN_vkEnumerateDeviceExtensionProperties)vk->GetInstanceProcAddr(
         inst, "vkEnumerateDeviceExtensionProperties");
   PFN_vkGetPhysicalDeviceFeatures2 get_features2 =
      (PFN_vkGetPhysicalDeviceFeatures2)vk->GetInstanceProcAddr(
         inst, "vkGetPhysicalDeviceFeatures2");

   if (!enum_pd) {
      printf("no vkEnumeratePhysicalDevices\n");
      return 1;
   }

   uint32_t count = 0;
   r = enum_pd(inst, &count, NULL);
   printf("vkEnumeratePhysicalDevices -> %d, count=%u\n", r, count);

   if (r != VK_SUCCESS || count == 0) {
      printf("\n=> No physical device. The kbase enumeration hook ran but\n"
             "   did not produce a usable device - check `adb logcat -s mesa`\n"
             "   with PANVK_DEBUG=startup for the reason.\n");
      return 1;
   }

   VkPhysicalDevice pds[8];
   if (count > 8)
      count = 8;
   r = enum_pd(inst, &count, pds);
   if (r != VK_SUCCESS) {
      printf("second enumerate failed: %d\n", r);
      return 1;
   }

   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceProperties p;
      memset(&p, 0, sizeof(p));
      if (get_props)
         get_props(pds[i], &p);
      printf("\n  device[%u]: %s\n", i, p.deviceName);
      printf("    apiVersion    = %u.%u.%u\n", VK_VERSION_MAJOR(p.apiVersion),
             VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
      printf("    driverVersion = 0x%08x\n", p.driverVersion);
      printf("    vendorID      = 0x%04x  deviceID = 0x%08x\n", p.vendorID,
             p.deviceID);
      printf("    type          = %d\n", p.deviceType);

      /* Eden's log claimed VK_EXT_vertex_attribute_divisor is missing on
       * this device, which contradicts panvk_vX_physical_device.c setting
       * it unconditionally true - check the driver's own answer directly
       * rather than trust a third party's suitability check.
       */
      if (enum_ext) {
         uint32_t ext_count = 0;
         enum_ext(pds[i], NULL, &ext_count, NULL);
         VkExtensionProperties *exts =
            calloc(ext_count, sizeof(VkExtensionProperties));
         enum_ext(pds[i], NULL, &ext_count, exts);
         bool has_divisor_ext = false, has_khr_divisor_ext = false;
         for (uint32_t e = 0; e < ext_count; e++) {
            if (!strcmp(exts[e].extensionName, "VK_EXT_vertex_attribute_divisor"))
               has_divisor_ext = true;
            if (!strcmp(exts[e].extensionName, "VK_KHR_vertex_attribute_divisor"))
               has_khr_divisor_ext = true;
         }
         printf("    device extension count = %u\n", ext_count);
         printf("    VK_EXT_vertex_attribute_divisor present = %s\n",
                has_divisor_ext ? "yes" : "NO");
         printf("    VK_KHR_vertex_attribute_divisor present = %s\n",
                has_khr_divisor_ext ? "yes" : "NO");
         free(exts);
      }

      if (get_features2) {
         VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR div_feat = {
            .sType =
               VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR,
         };
         VkPhysicalDeviceFeatures2 feat2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &div_feat,
         };
         get_features2(pds[i], &feat2);
         printf("    fillModeNonSolid = %d, multiViewport = %d,\n"
                "    shaderClipDistance = %d, shaderCullDistance = %d,\n"
                "    vertexPipelineStoresAndAtomics = %d,\n"
                "    vertexAttributeInstanceRateDivisor (KHR feature) = %d\n",
                feat2.features.fillModeNonSolid, feat2.features.multiViewport,
                feat2.features.shaderClipDistance,
                feat2.features.shaderCullDistance,
                feat2.features.vertexPipelineStoresAndAtomics,
                div_feat.vertexAttributeInstanceRateDivisor);
      }
   }

   printf("\n=> A physical device was created through the kbase path.\n"
          "   That means open(/dev/mali0), VERSION_CHECK, SET_FLAGS and\n"
          "   GET_GPUPROPS all ran inside pan_kmod_kbase.\n");
   return 0;
}
