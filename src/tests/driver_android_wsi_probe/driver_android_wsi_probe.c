// Is the Android presentation surface actually there?
//
// ROADMAP listed "gralloc cannot initialise under -Dandroid-stub=true" as
// the first blocker for Android presentation. That was wrong, and this probe
// is what establishes the real position rather than re-deriving it from the
// build flags:
//
//  - -Dandroid-stub=true only affects LINK time. It builds no-op libhardware
//    /libnativewindow/liblog/libsync .so files so the cross build has
//    something to link against. On a real device the loader resolves the
//    REAL libhardware.so, so hw_get_module() is the genuine article.
//
//  - u_gralloc_create(U_GRALLOC_TYPE_AUTO) tries CrOS -> (IMapper4) ->
//    libdrm -> QCOM -> fallback, and u_gralloc_fallback_create() never
//    returns NULL - it warns and returns a usable object. So
//    vk_android_get_ugralloc() is non-NULL even when no vendor-specific
//    backend matches.
//
// Confirmed on device by logcat, which shows the fallback's *second*
// message ("Gralloc doesn't support lock_ycbcr"), reachable only when
// hw_get_module() succeeded and returned a valid module:
//
//    W MESA: Gralloc doesn't support lock_ycbcr (video buffers won't be
//            supported)
//    I MESA: Using fallback gralloc implementation
//
// So this probe asks the questions that actually decide whether presentation
// is reachable:
//
//   1. which VK_ANDROID_* device extensions does the driver advertise?
//   2. can a device be created with the AHardwareBuffer extension enabled?
//   3. does vkGetAndroidHardwareBufferPropertiesANDROID work on a real AHB?
//   4. can that AHB be imported as VkDeviceMemory?
//
// (4) is the one that matters: it is the same memory path a swapchain image
// takes, and it runs through the dma-buf import this port just gained.
//
// No --i-know-it-hangs gate: no render pass, no draw.
//
// Usage: driver_android_wsi_probe <path-to-libvulkan_panfrost.so>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>

/* Same libhardware ABI mirrors as the other driver probes - see
 * driver_enum_probe.c for the note on the LP64 `reserved` width.
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

static void check(bool ok, const char *what) {
   printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

int main(int argc, char **argv) {
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 2) {
      fprintf(stderr, "usage: %s <path-to-libvulkan_panfrost.so>\n", argv[0]);
      return 2;
   }

   void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen failed: %s\n", dlerror());
      return 1;
   }

   hw_module_t *mod = dlsym(h, "HMI");
   hw_device_t *hwdev = NULL;
   if (!mod || !mod->methods ||
       mod->methods->open(mod, HWVULKAN_DEVICE_0, &hwdev) != 0 || !hwdev) {
      printf("HAL open failed\n");
      return 1;
   }
   hwvulkan_device_t *vk = (hwvulkan_device_t *)hwdev;

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-kbase-android-wsi-probe",
      .apiVersion = VK_API_VERSION_1_3,
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

#define GIPA(n) (PFN_##n) vk->GetInstanceProcAddr(inst, #n)
   PFN_vkEnumeratePhysicalDevices enum_pd = GIPA(vkEnumeratePhysicalDevices);
   PFN_vkEnumerateDeviceExtensionProperties enum_ext =
      GIPA(vkEnumerateDeviceExtensionProperties);
   PFN_vkCreateDevice create_dev = GIPA(vkCreateDevice);
   PFN_vkGetDeviceProcAddr gdpa = GIPA(vkGetDeviceProcAddr);

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   VkResult r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      printf("no physical device (%d)\n", r);
      return 1;
   }

   /* ------------------------------------------------- 1. what is advertised */
   printf("\n=== device extensions ===\n");
   uint32_t n = 0;
   enum_ext(pd, NULL, &n, NULL);
   VkExtensionProperties *exts = calloc(n, sizeof(*exts));
   enum_ext(pd, NULL, &n, exts);
   printf("  %u device extensions advertised\n", n);

   bool has_anb = false, has_ahb = false;
   for (uint32_t i = 0; i < n; i++) {
      if (!strcmp(exts[i].extensionName, "VK_ANDROID_native_buffer"))
         has_anb = true;
      if (!strcmp(exts[i].extensionName,
                  VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME))
         has_ahb = true;
      if (!strncmp(exts[i].extensionName, "VK_ANDROID", 10))
         printf("    %s (rev %u)\n", exts[i].extensionName,
                exts[i].specVersion);
   }

   check(has_anb, "VK_ANDROID_native_buffer advertised");
   check(has_ahb, "VK_ANDROID_external_memory_android_hardware_buffer "
                  "advertised");

   if (!has_ahb) {
      printf("\n  Without the AHardwareBuffer extension there is nothing\n"
             "  further to test here - gralloc did not initialise.\n");
      return failures ? 1 : 0;
   }

   /* -------------------------------------------- 2. create a device with it */
   printf("\n=== device creation with the AHB extension ===\n");
   const char *want[] = {
      VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
      VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
   };
   /* Only ask for the ones this driver actually has, so a missing optional
    * dependency reports as itself rather than as a device-creation failure.
    */
   const char *enable[8];
   uint32_t n_enable = 0;
   for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
      for (uint32_t i = 0; i < n; i++) {
         if (!strcmp(exts[i].extensionName, want[w])) {
            enable[n_enable++] = want[w];
            break;
         }
      }
   }
   printf("  enabling %u of %zu wanted extensions\n", n_enable,
          sizeof(want) / sizeof(want[0]));

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = n_enable,
      .ppEnabledExtensionNames = enable,
   };
   VkDevice device = VK_NULL_HANDLE;
   r = create_dev(pd, &dci, NULL, &device);
   printf("  vkCreateDevice -> %d\n", r);
   check(r == VK_SUCCESS, "device created with AHardwareBuffer support");
   if (r != VK_SUCCESS)
      return 1;

   PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_props =
      (PFN_vkGetAndroidHardwareBufferPropertiesANDROID) gdpa(
         device, "vkGetAndroidHardwareBufferPropertiesANDROID");
   PFN_vkAllocateMemory alloc_mem =
      (PFN_vkAllocateMemory) gdpa(device, "vkAllocateMemory");
   PFN_vkFreeMemory free_mem = (PFN_vkFreeMemory) gdpa(device, "vkFreeMemory");

   check(get_ahb_props != NULL,
         "vkGetAndroidHardwareBufferPropertiesANDROID resolved");
   if (!get_ahb_props)
      return 1;

   /* ------------------------------------------- 3. query a real AHB */
   printf("\n=== a real AHardwareBuffer ===\n");
   AHardwareBuffer_Desc desc = {
      .width = 64,
      .height = 64,
      .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
   };
   AHardwareBuffer *ahb = NULL;
   int rc = AHardwareBuffer_allocate(&desc, &ahb);
   printf("  AHardwareBuffer_allocate(64x64 RGBA8, GPU usage) -> %d\n", rc);
   check(rc == 0 && ahb != NULL, "allocated a GPU-usable AHardwareBuffer");
   if (rc != 0 || !ahb)
      return 1;

   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
   };
   r = get_ahb_props(device, ahb, &ahb_props);
   printf("  vkGetAndroidHardwareBufferPropertiesANDROID -> %d\n", r);
   if (r == VK_SUCCESS)
      printf("    allocationSize=%" PRIu64 " memoryTypeBits=0x%x\n",
             (uint64_t)ahb_props.allocationSize, ahb_props.memoryTypeBits);
   check(r == VK_SUCCESS, "driver described the AHardwareBuffer");

   /* ------------------------------- 4. import it as VkDeviceMemory */
   if (r == VK_SUCCESS && ahb_props.memoryTypeBits) {
      printf("\n=== import it as VkDeviceMemory ===\n");
      uint32_t type_idx = 0;
      for (uint32_t i = 0; i < 32; i++) {
         if (ahb_props.memoryTypeBits & (1u << i)) {
            type_idx = i;
            break;
         }
      }

      VkImportAndroidHardwareBufferInfoANDROID import = {
         .sType =
            VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
         .buffer = ahb,
      };
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .pNext = &import,
         .allocationSize = ahb_props.allocationSize,
         .memoryTypeIndex = type_idx,
      };
      VkDeviceMemory mem = VK_NULL_HANDLE;
      r = alloc_mem(device, &mai, NULL, &mem);
      printf("  vkAllocateMemory(VkImportAndroidHardwareBufferInfoANDROID) "
             "-> %d\n", r);
      check(r == VK_SUCCESS,
            "imported an AHardwareBuffer as VkDeviceMemory");
      if (r == VK_SUCCESS)
         free_mem(device, mem, NULL);
   }

   AHardwareBuffer_release(ahb);

   printf("\n=== %d failure(s) ===\n", failures);
   if (failures == 0)
      printf("\n=> The Android external-memory surface is present and an\n"
             "   AHardwareBuffer imports as VkDeviceMemory. That is the same\n"
             "   memory path a swapchain image takes.\n");
   return failures ? 1 : 0;
}
