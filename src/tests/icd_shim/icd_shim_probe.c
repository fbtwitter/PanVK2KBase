// Verifies libpanvk_kbase_icd_shim.so on real hardware, loaded exactly the
// way a standard Vulkan loader (or deqp-vk's tcuAndroidPlatform.cpp) would
// load it: dlopen the shim's path, dlsym "vkGetInstanceProcAddr", and use
// only that - no HMI, no hw_module_t, no HAL-specific code at all. If this
// probe works, the shim's indirection is proven end to end and CTS has a
// real chance of working through it; if it doesn't, that is cheaper to
// find out here than after a full deqp-vk cross-build.
//
// Deliberately not gated behind --i-know-it-hangs the way the render_*
// probes are: this calls the exact same driver entrypoints
// (vkCreateInstance, vkEnumeratePhysicalDevices, vkCreateDevice,
// vkCreateFence) that have already run successfully many times today via
// this repo's own probes. The only new thing under test is the shim's
// indirection, not any new GPU operation.
//
// Usage: icd_shim_probe <path-to-libpanvk_kbase_icd_shim.so>
#include <dlfcn.h>
#include <stdio.h>

#include <vulkan/vulkan_core.h>

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc != 2) {
      fprintf(stderr, "usage: %s <path-to-libpanvk_kbase_icd_shim.so>\n",
              argv[0]);
      return 2;
   }

   void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen(%s) failed: %s\n", argv[1], dlerror());
      return 1;
   }
   printf("dlopen ok\n");

   /* The one and only thing a standard Vulkan loader needs from an ICD -
    * no HAL knowledge past this point.
    */
   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   if (!gipa) {
      printf("dlsym(vkGetInstanceProcAddr) failed: %s\n", dlerror());
      return 1;
   }
   printf("dlsym(vkGetInstanceProcAddr) ok\n");

   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!create_instance) {
      printf("vkGetInstanceProcAddr(NULL, \"vkCreateInstance\") -> NULL\n");
      return 1;
   }
   printf("bootstrapped vkCreateInstance through the shim\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-kbase-icd-shim-probe",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance inst = VK_NULL_HANDLE;
   VkResult r = create_instance(&ici, NULL, &inst);
   printf("vkCreateInstance -> %d\n", r);
   if (r != VK_SUCCESS)
      return 1;

   /* Everything past here is fetched through gipa(inst, ...), same as any
    * real Vulkan loader would, not through anything HAL-specific.
    */
#define GIPA(name) (PFN_##name) gipa(inst, #name)
   PFN_vkEnumeratePhysicalDevices enum_pd = GIPA(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceProperties get_props =
      GIPA(vkGetPhysicalDeviceProperties);
   PFN_vkCreateDevice create_dev = GIPA(vkCreateDevice);
   PFN_vkGetDeviceProcAddr gdpa_bootstrap = GIPA(vkGetDeviceProcAddr);

   if (!enum_pd || !get_props || !create_dev || !gdpa_bootstrap) {
      printf("missing entrypoint fetched through the shim: enum_pd=%p "
             "get_props=%p create_dev=%p gdpa=%p\n",
             (void *)enum_pd, (void *)get_props, (void *)create_dev,
             (void *)gdpa_bootstrap);
      return 1;
   }
   printf("fetched vkEnumeratePhysicalDevices/vkGetPhysicalDeviceProperties/"
          "vkCreateDevice/vkGetDeviceProcAddr through the shim\n");

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      printf("vkEnumeratePhysicalDevices -> %d, count=%u\n", r, count);
      return 1;
   }

   VkPhysicalDeviceProperties props;
   get_props(pd, &props);
   printf("physical device: %s\n", props.deviceName);

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
   };
   VkDevice device = VK_NULL_HANDLE;
   r = create_dev(pd, &dci, NULL, &device);
   printf("vkCreateDevice -> %d\n", r);
   if (r != VK_SUCCESS)
      return 1;

   /* vkCreateFence and vkDestroyFence, fetched via vkGetDeviceProcAddr
    * exactly the way a loader would - proves the device-level dispatch
    * chain works through the shim too, not just the instance-level one.
    */
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gdpa_bootstrap(device, "vkGetDeviceProcAddr");
#define GDPA(name) (PFN_##name) gdpa(device, #name)
   PFN_vkCreateFence create_fence = GDPA(vkCreateFence);
   PFN_vkDestroyFence destroy_fence = GDPA(vkDestroyFence);
   PFN_vkDestroyDevice destroy_device = GDPA(vkDestroyDevice);

   if (!create_fence || !destroy_fence || !destroy_device) {
      printf("missing device-level entrypoint through the shim\n");
      return 1;
   }

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   r = create_fence(device, &fci, NULL, &fence);
   printf("vkCreateFence (via shim, device-level dispatch) -> %d\n", r);
   if (r != VK_SUCCESS)
      return 1;

   destroy_fence(device, fence, NULL);
   destroy_device(device, NULL);

   printf("\n=> The shim works: a program that only ever calls "
          "vkGetInstanceProcAddr - no HAL-specific code - reached this "
          "driver, created an instance, a device, and a fence.\n");
   return 0;
}
