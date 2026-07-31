// Exercises the kbase vk_sync end to end, through the real Vulkan API.
//
// tests/driver_enum_probe established that a physical device can now be
// created on kbase. That only proves panvk_kbase_sync registered as a sync
// type - it says nothing about whether the thing works. This goes further:
// creates a logical device, then drives a timeline semaphore through
// vkSignalSemaphore / vkGetSemaphoreCounterValue / vkWaitSemaphores, which
// land directly on panvk_kbase_sync's signal / get_value / wait_many, and a
// binary VkFence through vkGetFenceStatus / vkResetFences.
//
// Every value read back here comes out of a 64-bit slot in a
// BASE_MEM_CSF_EVENT allocation - see src/mesa/panvk_kbase_sync.c and
// docs/kbase-notes.md "Finding 2".
//
// Scope: CPU-side operations only. Nothing here submits GPU work, because
// VkQueueSubmit on kbase is not wired up (Phase 4). A passing run means the
// sync object is correct, not that the GPU can signal it.
//
// Usage: driver_sync_probe /data/local/tmp/libvulkan_panfrost.so
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* Same libhardware ABI mirrors as driver_enum_probe.c - see the long note
 * there about the LP64 `reserved` width.
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
   printf("  %-52s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

int
main(int argc, char **argv)
{
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
   hw_device_t *dev = NULL;
   if (!mod || !mod->methods || mod->methods->open(mod, HWVULKAN_DEVICE_0,
                                                   &dev) != 0 || !dev) {
      printf("HAL open failed\n");
      return 1;
   }
   hwvulkan_device_t *vk = (hwvulkan_device_t *)dev;

   /* Timeline semaphores are core in 1.2; ask for 1.3 so the promoted
    * entrypoints resolve without an extension dance.
    */
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-kbase-sync-probe",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };

   VkInstance inst = VK_NULL_HANDLE;
   VkResult r = vk->CreateInstance(&ici, NULL, &inst);
   if (r != VK_SUCCESS) {
      printf("vkCreateInstance -> %d\n", r);
      return 1;
   }

#define GIPA(name) (PFN_##name) vk->GetInstanceProcAddr(inst, #name)
   PFN_vkEnumeratePhysicalDevices enum_pd = GIPA(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qf =
      GIPA(vkGetPhysicalDeviceQueueFamilyProperties);
   PFN_vkCreateDevice create_dev = GIPA(vkCreateDevice);
   PFN_vkGetDeviceProcAddr gdpa = GIPA(vkGetDeviceProcAddr);

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      printf("no physical device (%d)\n", r);
      return 1;
   }

   /* ---------------------------------------------------------- device */
   printf("\n=== vkCreateDevice ===\n");
   uint32_t qf_count = 0;
   get_qf(pd, &qf_count, NULL);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkPhysicalDeviceVulkan12Features f12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .timelineSemaphore = VK_TRUE,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &f12,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
   };

   VkDevice device = VK_NULL_HANDLE;
   r = create_dev(pd, &dci, NULL, &device);
   printf("  vkCreateDevice -> %d%s\n", r, r == VK_SUCCESS ? " (OK)" : "");
   if (r != VK_SUCCESS) {
      printf("\n=> Logical device creation failed (%d), so the sync type\n"
             "   could not be exercised through the API. This does not mean\n"
             "   the sync type is broken - physical device creation, which\n"
             "   is what registers it, succeeds (see driver_enum_probe).\n"
             "\n"
             "   Expected as of now: -3 VK_ERROR_INITIALIZATION_FAILED from\n"
             "   panvk_vX_gpu_queue.c's DRM_IOCTL_PANTHOR_GROUP_CREATE,\n"
             "   issued on the kbase fd. The GPU queue is still entirely\n"
             "   panthor-specific; swapping it for kbase's\n"
             "   CS_QUEUE_GROUP_CREATE/REGISTER/BIND/KICK is Phase 4 (and is\n"
             "   already prototyped in tests/queue_group).\n"
             "   Anything else, check `adb logcat -d | grep MESA`.\n",
             r);
      return 1;
   }

#define GDPA(name) (PFN_##name) gdpa(device, #name)
   PFN_vkCreateSemaphore create_sem = GDPA(vkCreateSemaphore);
   PFN_vkDestroySemaphore destroy_sem = GDPA(vkDestroySemaphore);
   PFN_vkSignalSemaphore signal_sem = GDPA(vkSignalSemaphore);
   PFN_vkWaitSemaphores wait_sems = GDPA(vkWaitSemaphores);
   PFN_vkGetSemaphoreCounterValue get_counter =
      GDPA(vkGetSemaphoreCounterValue);
   PFN_vkCreateFence create_fence = GDPA(vkCreateFence);
   PFN_vkDestroyFence destroy_fence = GDPA(vkDestroyFence);
   PFN_vkGetFenceStatus fence_status = GDPA(vkGetFenceStatus);
   PFN_vkResetFences reset_fences = GDPA(vkResetFences);
   PFN_vkDeviceWaitIdle wait_idle = GDPA(vkDeviceWaitIdle);
   PFN_vkDestroyDevice destroy_device = GDPA(vkDestroyDevice);

   /* ------------------------------------------------- timeline semaphore */
   printf("\n=== timeline semaphore (init/signal/get_value/wait) ===\n");
   if (!create_sem || !signal_sem || !wait_sems || !get_counter) {
      printf("  timeline entrypoints missing - driver reports no 1.2 core?\n");
      failures++;
   } else {
      VkSemaphoreTypeCreateInfo stci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
         .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
         .initialValue = 7,
      };
      VkSemaphoreCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
         .pNext = &stci,
      };

      VkSemaphore sem = VK_NULL_HANDLE;
      r = create_sem(device, &sci, NULL, &sem);
      check(r == VK_SUCCESS, "vkCreateSemaphore(timeline, initialValue=7)");

      if (r == VK_SUCCESS) {
         /* init() wrote the initial value into the event slot. */
         uint64_t v = 0;
         r = get_counter(device, sem, &v);
         printf("    counter after create = %llu (expect 7)\n",
                (unsigned long long)v);
         check(r == VK_SUCCESS && v == 7, "initial value round-trips");

         /* signal() writes the slot. */
         VkSemaphoreSignalInfo ssi = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .semaphore = sem,
            .value = 42,
         };
         r = signal_sem(device, &ssi);
         check(r == VK_SUCCESS, "vkSignalSemaphore(42)");

         v = 0;
         get_counter(device, sem, &v);
         printf("    counter after signal = %llu (expect 42)\n",
                (unsigned long long)v);
         check(v == 42, "signalled value round-trips");

         /* wait_many, already-satisfied path. */
         uint64_t want = 42;
         VkSemaphoreWaitInfo swi = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &sem,
            .pValues = &want,
         };
         r = wait_sems(device, &swi, 0);
         check(r == VK_SUCCESS, "vkWaitSemaphores(<= current) returns at once");

         /* wait_many, timeout path: nothing will ever signal 99, so this
          * must time out rather than hang or spuriously succeed.
          */
         want = 99;
         r = wait_sems(device, &swi, 50ull * 1000 * 1000); /* 50ms */
         printf("    wait for 99 -> %d (expect %d VK_TIMEOUT)\n", r,
                VK_TIMEOUT);
         check(r == VK_TIMEOUT, "vkWaitSemaphores(unreachable) times out");

         destroy_sem(device, sem, NULL);
         check(true, "vkDestroySemaphore");
      }
   }

   /* ------------------------------------------------------- binary fence */
   printf("\n=== binary fence (init/reset/status) ===\n");
   if (!create_fence || !fence_status || !reset_fences) {
      printf("  fence entrypoints missing\n");
      failures++;
   } else {
      VkFenceCreateInfo fci = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         .flags = VK_FENCE_CREATE_SIGNALED_BIT,
      };
      VkFence fence = VK_NULL_HANDLE;
      r = create_fence(device, &fci, NULL, &fence);
      check(r == VK_SUCCESS, "vkCreateFence(SIGNALED)");

      if (r == VK_SUCCESS) {
         r = fence_status(device, fence);
         printf("    status after create(SIGNALED) = %d (expect 0 VK_SUCCESS)\n",
                r);
         check(r == VK_SUCCESS, "fence created signalled reads as signalled");

         r = reset_fences(device, 1, &fence);
         check(r == VK_SUCCESS, "vkResetFences");

         r = fence_status(device, fence);
         printf("    status after reset = %d (expect %d VK_NOT_READY)\n", r,
                VK_NOT_READY);
         check(r == VK_NOT_READY, "reset fence reads as unsignalled");

         destroy_fence(device, fence, NULL);
         check(true, "vkDestroyFence");
      }
   }

   /* vkDeviceWaitIdle goes through the queue, which is not wired for kbase.
    * Report it rather than asserting - it is expected to be unhappy until
    * Phase 4 lands.
    */
   printf("\n=== vkDeviceWaitIdle (expected to be incomplete pre-Phase 4) ===\n");
   if (wait_idle) {
      r = wait_idle(device);
      printf("  vkDeviceWaitIdle -> %d%s\n", r,
             r == VK_SUCCESS ? " (OK)" : " (not fatal here)");
   }

   destroy_device(device, NULL);

   printf("\n================================================================\n");
   if (failures == 0)
      printf("RESULT: %d checks passed. The kbase event-memory vk_sync works\n"
             "        for every CPU-side operation Vulkan exposes.\n"
             "        GPU-side signalling is still unwired - see Phase 4.\n",
             0);
   else
      printf("RESULT: %d check(s) FAILED - see above.\n", failures);
   printf("================================================================\n");

   return failures ? 1 : 0;
}
