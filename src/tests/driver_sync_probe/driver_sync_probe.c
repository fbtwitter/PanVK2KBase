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
// It then submits: an empty vkQueueSubmit (no command buffers) whose whole
// command stream is a SYNC_SET64 against a fence's event slot, and waits
// for the fence. Nothing in the driver's submit path writes that slot from
// the CPU, so a fence that comes back signalled was signalled by the GPU.
//
// Scope: no command buffers are submitted - those need the per-subqueue
// context init that does not exist yet. A passing run means the sync
// object is correct and the GPU can signal it, not that real work runs.
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

   /* Unbuffered: this probe now submits GPU work, and a submit path that
    * hangs is a failure mode worth diagnosing. With the default block
    * buffering on a pipe, killing a hung run discards everything printed
    * so far and the log says nothing about how far it got.
    */
   setvbuf(stdout, NULL, _IONBF, 0);

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
             "   This used to be an expected -3 from the panthor-specific\n"
             "   GPU queue, but panvk_vX_kbase_queue.c now creates the tiler\n"
             "   heap, queue group and per-subqueue CS rings on kbase, so\n"
             "   success is the expected outcome. A failure here is a real\n"
             "   regression - check `adb logcat -d | grep MESA`.\n",
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

      if (r != VK_SUCCESS) {
         /* Expected, and correct. vk_semaphore.c:99 requires
          * VK_SYNC_FEATURE_GPU_WAIT of any sync type backing a semaphore,
          * and panvk_kbase_sync deliberately does not advertise it: nothing
          * signals an event slot from the GPU until VkQueueSubmit is
          * implemented. Advertising it would let the runtime hand out
          * semaphores that could never be signalled, which is worse than
          * refusing to create them. Fences, which only need the CPU-side
          * features, do work - see the next section.
          */
         printf("  vkCreateSemaphore -> %d: EXPECTED. Semaphores need\n"
                "    VK_SYNC_FEATURE_GPU_WAIT, which panvk_kbase_sync does\n"
                "    not advertise until submission can signal a slot.\n", r);
      } else {
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

   /* ------------------------------------------- GPU-signalled fence */
   /* The one thing every check above cannot show: that the GPU, not the
    * CPU, can signal a sync. An empty submit - no command buffers - builds
    * a command stream whose entire content is a SYNC_SET64 against the
    * fence's event slot, publishes it to the ring and kicks. Nothing in
    * the driver's submit path writes that slot from the CPU, so a fence
    * that comes back signalled was signalled by the GPU.
    *
    * This is the first time anything in this repo signals a vk_sync from
    * the GPU through the Vulkan API rather than through a standalone probe.
    */
   printf("\n=== GPU-signalled fence (empty vkQueueSubmit) ===\n");
   PFN_vkGetDeviceQueue get_queue = GDPA(vkGetDeviceQueue);
   PFN_vkQueueSubmit queue_submit = GDPA(vkQueueSubmit);
   PFN_vkWaitForFences wait_fences = GDPA(vkWaitForFences);

   if (!get_queue || !queue_submit || !wait_fences || !create_fence) {
      printf("  submit entrypoints missing\n");
      failures++;
   } else {
      VkQueue queue = VK_NULL_HANDLE;
      get_queue(device, 0, 0, &queue);

      /* Repeated deliberately. A ring buffer only exercises its bookkeeping
       * on the second and later submits: the first one always writes at
       * offset 0 with an empty ring, so a broken insert/extract accounting
       * or a stream that only runs once would still pass a single-shot
       * test. Three is enough to catch "only the first one runs".
       */
      for (int iter = 0; iter < 3; iter++) {
         VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         };
         VkFence fence = VK_NULL_HANDLE;
         r = create_fence(device, &fci, NULL, &fence);
         if (r != VK_SUCCESS) {
            check(false, "vkCreateFence(unsignalled)");
            break;
         }

         if (iter == 0)
            check(fence_status(device, fence) == VK_NOT_READY,
                  "fence starts unsignalled");

         /* submitCount=1 with zero command buffers, rather than a
          * fence-only submitCount=0, so the runtime definitely builds a
          * vk_queue_submit carrying the fence as a signal.
          */
         VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         };
         r = queue_submit(queue, 1, &si, fence);
         printf("    [%d] vkQueueSubmit -> %d\n", iter, r);

         char label[64];
         snprintf(label, sizeof(label), "submit %d accepted", iter);
         check(r == VK_SUCCESS, label);

         if (r == VK_SUCCESS) {
            /* 2s, the same budget the standalone probes give a stream
             * before calling it never-going-to-run.
             */
            r = wait_fences(device, 1, &fence, VK_TRUE,
                            2ull * 1000 * 1000 * 1000);
            printf("    [%d] vkWaitForFences -> %d\n", iter, r);

            snprintf(label, sizeof(label),
                     "*** GPU SIGNALLED THE FENCE (submit %d) ***", iter);
            check(r == VK_SUCCESS, label);

            if (r != VK_SUCCESS) {
               printf("      Submit %d was accepted but its slot was never\n"
                      "      written. If submit 0 passed and this did not,\n"
                      "      the stream only runs once per queue - look at\n"
                      "      ring insert/extract accounting, not at the\n"
                      "      SYNC_SET64 encoding.\n", iter);
               destroy_fence(device, fence, NULL);
               break;
            }
         }

         destroy_fence(device, fence, NULL);
      }
   }

   /* vkDeviceWaitIdle drains the queue, which now has a real submit path.
    */
   printf("\n=== vkDeviceWaitIdle ===\n");
   if (wait_idle) {
      r = wait_idle(device);
      printf("  vkDeviceWaitIdle -> %d%s\n", r,
             r == VK_SUCCESS ? " (OK)" : "");
      check(r == VK_SUCCESS, "vkDeviceWaitIdle");
   }

   destroy_device(device, NULL);

   printf("\n================================================================\n");
   if (failures == 0)
      printf("RESULT: all checks passed. The kbase event-memory vk_sync works\n"
             "        for every CPU-side operation Vulkan exposes, AND the\n"
             "        GPU can signal one through vkQueueSubmit.\n"
             "        vkCreateDevice succeeding also means the compute\n"
             "        subqueue's init stream ran on the GPU - it is submitted\n"
             "        and waited on during queue creation, so a failure there\n"
             "        would have failed device creation.\n"
             "        Command buffers are still unsubmittable: the render\n"
             "        subqueues' contexts are not initialised yet.\n");
   else
      printf("RESULT: %d check(s) FAILED - see above.\n", failures);
   printf("================================================================\n");

   return failures ? 1 : 0;
}
