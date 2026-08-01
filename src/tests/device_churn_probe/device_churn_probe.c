// Root-causes the ~307-case ceiling found running dEQP-VK.api.object_management
// through deqp-vk (docs/kbase-notes.md): after a few hundred VkInstance/
// VkDevice create+destroy cycles in one process, vkCreateDevice starts
// failing with VK_ERROR_OUT_OF_DEVICE_MEMORY. A live /proc/<pid>/fd sample
// during that CTS run showed the mali0 fd count staying small and bounded
// (2-18, not growing) right up to the failure - so it is not a plain
// userspace fd leak. This probe isolates the create/destroy cycle from
// everything else deqp-vk's object_management cases do, to find the real
// raw ceiling and check whether it is a hard leak (ceiling is the same
// regardless of pacing) or a reclaim race (a delay between iterations
// raises or removes the ceiling, meaning the kernel eventually frees
// whatever this is, just not synchronously with the fd close).
//
// Uses the ICD shim (tests/icd_shim/), not the raw HAL path other probes
// here use - the shim is already proven, and this only needs
// vkGetInstanceProcAddr, which keeps this file focused on the churn loop
// rather than re-deriving HAL bootstrap boilerplate.
//
// Usage: device_churn_probe <path-to-icd-shim.so> <iterations>
//                            [--delay-ms N] [--use-queue] [--alloc-buffer]
//                            [--threads N]
//   --use-queue: also vkGetDeviceQueue + create/destroy a command pool and
//                a primary command buffer each cycle (record+end, never
//                submitted) - closer to what real CTS device-management
//                cases do than a bare create+destroy, and the first place
//                kbase-specific CSF group/queue allocation could happen.
//   --alloc-buffer: also vkCreateBuffer + vkAllocateMemory + vkBindBufferMemory
//                   + free each cycle - a real BASE_MEM_ALLOC/MEM_FREE round
//                   trip through kbase_kmod_bo_alloc/bo_free
//                   (src/mesa/pan_kmod_kbase.c), not just the FIXED_VA probe
//                   device creation itself does.
//   --threads N: spawn N threads, each running <iterations> cycles
//                concurrently (bare create+destroy only, ignores
//                --use-queue/--alloc-buffer). Tests whether concurrent
//                device creation - what dEQP-VK.api.object_management's
//                multithreaded_* groups do - is part of the ~307-case
//                ceiling documented in docs/kbase-notes.md.
//   --private-data / --private-data-slots N M: churn devices with
//                VK_EXT_private_data enabled exactly the way
//                dEQP-VK.api.object_management.private_data.* does
//                (vktApiObjectManagementTests.cpp's SingletonDevice) -
//                the group that actually fails in the real run. Plain
//                --private-data uses {0,0} (no slots), matching the
//                actual failing case (buffer_storage_large, index 0);
//                --private-data-slots N M requests N first-level and M
//                chained second-level slots, up to {1,100} (the
//                heaviest config any SingletonDevice index uses).
//   --many-objects N: per device, allocate N 64KB buffers (each with its
//                own real memory allocation, bound) all SIMULTANEOUSLY
//                ALIVE, only freeing them once all N exist (or creation
//                fails) - unlike --alloc-buffer, which creates and frees
//                one at a time. This is architecturally what
//                dEQP-VK.api.object_management's max_concurrent.*/
//                multiple_* groups actually do (stress the concurrent
//                limit on one device).
//   --many-pipelines N: per device, compile one shader module (reusing
//                driver_pipeline_probe's shader_spv.h) and create N compute
//                pipelines from it, all simultaneously alive before
//                destroying them. Pipelines are executable BOs, allocated
//                in kbase's EXEC_VA zone with a kernel-chosen address -
//                kbase_kmod_bo_free (src/mesa/pan_kmod_kbase.c) explicitly
//                does NOT return their address to this backend's own VA
//                heap tracking the way ordinary buffers are, since the
//                kernel picked it. That asymmetry is the leading untested
//                candidate for the ~307-case ceiling: --many-objects (all
//                ordinary buffers) stayed clean at 125,000 cumulative
//                allocations, but nothing so far has stressed repeated
//                executable-BO alloc/free specifically.
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

#include "shader_spv.h"

static int
count_mali_fds(void)
{
   DIR *d = opendir("/proc/self/fd");
   if (!d)
      return -1;

   int count = 0;
   struct dirent *ent;
   char target[256];
   char path[64];
   while ((ent = readdir(d)) != NULL) {
      if (ent->d_name[0] == '.')
         continue;
      snprintf(path, sizeof(path), "/proc/self/fd/%s", ent->d_name);
      ssize_t n = readlink(path, target, sizeof(target) - 1);
      if (n < 0)
         continue;
      target[n] = 0;
      if (strstr(target, "mali0"))
         count++;
   }
   closedir(d);
   return count;
}

static int
count_all_fds(void)
{
   DIR *d = opendir("/proc/self/fd");
   if (!d)
      return -1;
   int count = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL) {
      if (ent->d_name[0] == '.')
         continue;
      count++;
   }
   closedir(d);
   return count;
}

struct thread_ctx {
   PFN_vkGetInstanceProcAddr gipa;
   int iterations;
   int thread_id;
   int completed;
   VkResult fail_result;
   int fail_iteration;
};

static void *
churn_thread(void *arg)
{
   struct thread_ctx *ctx = arg;
   PFN_vkGetInstanceProcAddr gipa = ctx->gipa;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

   for (int i = 0; i < ctx->iterations; i++) {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .pApplicationName = "device-churn-probe-mt",
         .apiVersion = VK_API_VERSION_1_3,
      };
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app,
      };
      VkInstance inst = VK_NULL_HANDLE;
      VkResult r = create_instance(&ici, NULL, &inst);
      if (r != VK_SUCCESS) {
         ctx->fail_result = r;
         ctx->fail_iteration = i;
         return NULL;
      }

#define TGIPA(name) (PFN_##name)gipa(inst, #name)
      PFN_vkEnumeratePhysicalDevices enum_pd = TGIPA(vkEnumeratePhysicalDevices);
      PFN_vkCreateDevice create_dev = TGIPA(vkCreateDevice);
      PFN_vkDestroyDevice destroy_dev = TGIPA(vkDestroyDevice);
      PFN_vkDestroyInstance destroy_inst = TGIPA(vkDestroyInstance);

      uint32_t count = 1;
      VkPhysicalDevice pd = VK_NULL_HANDLE;
      r = enum_pd(inst, &count, &pd);
      if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
         ctx->fail_result = r;
         ctx->fail_iteration = i;
         destroy_inst(inst, NULL);
         return NULL;
      }

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
      VkDevice dev = VK_NULL_HANDLE;
      r = create_dev(pd, &dci, NULL, &dev);
      if (r != VK_SUCCESS) {
         ctx->fail_result = r;
         ctx->fail_iteration = i;
         destroy_inst(inst, NULL);
         return NULL;
      }

      destroy_dev(dev, NULL);
      destroy_inst(inst, NULL);
      ctx->completed = i + 1;
   }
   return NULL;
}

static int
run_threaded(const char *shim_path, int iterations, int nthreads)
{
   void *h = dlopen(shim_path, RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen(%s) failed: %s\n", shim_path, dlerror());
      return 1;
   }
   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   if (!gipa) {
      printf("dlsym(vkGetInstanceProcAddr) failed: %s\n", dlerror());
      return 1;
   }

   printf("threads=%d iterations_per_thread=%d (total=%d)\n", nthreads,
          iterations, nthreads * iterations);

   /* The shim's lazy init (tests/icd_shim/panvk_kbase_icd_shim.c) has no
    * locking around its first-caller-wins guard - fine for every
    * single-threaded probe that's used it so far, but a real race if
    * multiple threads call vkGetInstanceProcAddr(NULL, ...) as their very
    * first call simultaneously. Real deqp-vk never hits this: it
    * bootstraps the loader once, single-threaded, before any test spawns
    * worker threads. Match that here rather than let an artifact of this
    * test harness get mistaken for a driver/kernel finding.
    */
   PFN_vkCreateInstance warm = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   (void)warm;

   pthread_t *tids = calloc(nthreads, sizeof(*tids));
   struct thread_ctx *ctxs = calloc(nthreads, sizeof(*ctxs));
   for (int t = 0; t < nthreads; t++) {
      ctxs[t] = (struct thread_ctx){
         .gipa = gipa, .iterations = iterations, .thread_id = t};
      pthread_create(&tids[t], NULL, churn_thread, &ctxs[t]);
   }

   int total_completed = 0;
   int any_failed = 0;
   for (int t = 0; t < nthreads; t++) {
      pthread_join(tids[t], NULL);
      total_completed += ctxs[t].completed;
      if (ctxs[t].completed < iterations) {
         any_failed = 1;
         printf("thread %d: completed %d/%d, failed at iteration %d with "
                "VkResult %d\n",
                t, ctxs[t].completed, iterations, ctxs[t].fail_iteration,
                ctxs[t].fail_result);
      } else {
         printf("thread %d: completed %d/%d\n", t, ctxs[t].completed,
                iterations);
      }
   }

   printf("\n=== summary ===\n");
   printf("total completed across all threads: %d/%d\n", total_completed,
          nthreads * iterations);
   free(tids);
   free(ctxs);
   return any_failed;
}

/* Matches dEQP-VK.api.object_management.private_data.* exactly - the
 * group that actually fails in the real run (docs/kbase-notes.md,
 * "root-cause dig, third pass"). slot_count/slot_count2 mirror
 * SingletonDevice::createPrivateDataDevice's requestedSlots table
 * (vktApiObjectManagementTests.cpp): {0,0} is what the failing case
 * (buffer_storage_large, SingletonDevice index 0) actually uses - the
 * private data FEATURE is requested and the extension enabled, but no
 * slots. {1,100} is the heaviest config any other SingletonDevice index
 * uses, included for comparison.
 */
static int
run_private_data(const char *shim_path, int iterations, int slot_count,
                 int slot_count2)
{
   void *h = dlopen(shim_path, RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen(%s) failed: %s\n", shim_path, dlerror());
      return 1;
   }
   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   if (!gipa) {
      printf("dlsym(vkGetInstanceProcAddr) failed: %s\n", dlerror());
      return 1;
   }
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!create_instance) {
      printf("missing pre-instance entrypoints through the shim\n");
      return 1;
   }

   printf("private-data mode: iterations=%d slots=%d,%d\n", iterations,
          slot_count, slot_count2);

   int completed = 0;
   for (int i = 0; i < iterations; i++) {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .pApplicationName = "device-churn-probe-privdata",
         .apiVersion = VK_API_VERSION_1_3,
      };
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app,
      };
      VkInstance inst = VK_NULL_HANDLE;
      VkResult r = create_instance(&ici, NULL, &inst);
      if (r != VK_SUCCESS) {
         printf("iteration %d: vkCreateInstance -> %d, stopping\n", i, r);
         break;
      }

#define PGIPA(name) (PFN_##name)gipa(inst, #name)
      PFN_vkEnumeratePhysicalDevices enum_pd = PGIPA(vkEnumeratePhysicalDevices);
      PFN_vkGetPhysicalDeviceFeatures get_features =
         PGIPA(vkGetPhysicalDeviceFeatures);
      PFN_vkCreateDevice create_dev = PGIPA(vkCreateDevice);
      PFN_vkDestroyDevice destroy_dev = PGIPA(vkDestroyDevice);
      PFN_vkDestroyInstance destroy_inst = PGIPA(vkDestroyInstance);

      uint32_t count = 1;
      VkPhysicalDevice pd = VK_NULL_HANDLE;
      r = enum_pd(inst, &count, &pd);
      if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
         printf("iteration %d: vkEnumeratePhysicalDevices -> %d, "
                "stopping\n",
                i, r);
         destroy_inst(inst, NULL);
         break;
      }

      VkPhysicalDeviceFeatures enabled_features;
      get_features(pd, &enabled_features);

      /* Mirrors SingletonDevice::createPrivateDataDevice exactly: a
       * VkDevicePrivateDataCreateInfoEXT chain (only built if slots are
       * requested - the failing case requests none), then the feature
       * struct, then the device create info with the extension named
       * and pEnabledFeatures set - all of which this repo's other probes
       * leave untouched (no pNext, no extensions, no explicit features).
       */
      VkDevicePrivateDataCreateInfoEXT pdci0 = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO_EXT,
         .privateDataSlotRequestCount = (uint32_t)slot_count,
      };
      VkDevicePrivateDataCreateInfoEXT pdci1 = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO_EXT,
         .privateDataSlotRequestCount = (uint32_t)slot_count2,
      };
      void *pnext = NULL;
      if (slot_count) {
         pnext = &pdci0;
         if (slot_count2)
            pdci0.pNext = &pdci1;
      }

      VkPhysicalDevicePrivateDataFeaturesEXT priv_features = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES_EXT,
         .pNext = pnext,
         .privateData = VK_TRUE,
      };

      const char *ext_name = "VK_EXT_private_data";
      float prio = 1.0f;
      VkDeviceQueueCreateInfo qci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &prio,
      };
      VkDeviceCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .pNext = &priv_features,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &qci,
         .enabledExtensionCount = 1,
         .ppEnabledExtensionNames = &ext_name,
         .pEnabledFeatures = &enabled_features,
      };
      VkDevice dev = VK_NULL_HANDLE;
      r = create_dev(pd, &dci, NULL, &dev);
      if (r != VK_SUCCESS) {
         printf("iteration %d: vkCreateDevice (VK_EXT_private_data) -> "
                "%d, stopping\n",
                i, r);
         destroy_inst(inst, NULL);
         break;
      }

      destroy_dev(dev, NULL);
      destroy_inst(inst, NULL);
      completed = i + 1;

      if (i % 20 == 0 || i == iterations - 1)
         printf("iteration %d ok\n", i);
   }

   printf("\n=== summary ===\n");
   printf("completed %d/%d private-data device cycles\n", completed,
          iterations);
   return completed == iterations ? 0 : 1;
}

/* Matches dEQP-VK.api.object_management.multithreaded_shared_resources
 * exactly in shape (vktApiObjectManagementTests.cpp's
 * multithreadedCreateSharedResourcesTest): N threads all create+destroy
 * objects on ONE ALREADY-EXISTING shared device concurrently, synced with
 * a barrier every few iterations "to make entering driver at the same
 * time more likely" (CTS's own comment) - not each thread owning its own
 * device, which is what device_churn_probe's --threads mode tests and
 * already came up clean. This is the one pattern from the real failing
 * run nothing so far has replicated: concurrent kbase_kmod_bo_alloc/free
 * on the SAME device's VA heap (protected only by kbase_dev->va.lock
 * around the userspace bookkeeping - the underlying ioctls' own
 * concurrency safety on one fd is untested). If a race there leaks real
 * kernel/GPU memory rather than just corrupting this backend's own VA
 * heap tracking, that would explain why a LATER, completely unrelated
 * device's allocation fails afterward - the shared device stays alive the
 * whole time, so any corruption confined to its own state wouldn't
 * explain a fresh device failing, but a real leaked kernel resource
 * would.
 */
struct shared_ctx {
   PFN_vkCreateBuffer create_buf;
   PFN_vkDestroyBuffer destroy_buf;
   PFN_vkGetBufferMemoryRequirements get_reqs;
   PFN_vkAllocateMemory alloc_mem;
   PFN_vkFreeMemory free_mem;
   PFN_vkBindBufferMemory bind_mem;
   VkDevice dev;
   uint32_t mem_type;
   int iters_per_thread;
   int iters_between_syncs;
   atomic_int barrier_count;
   atomic_int barrier_gen;
   int nthreads;
};

static void
shared_barrier(struct shared_ctx *ctx)
{
   int gen = atomic_load(&ctx->barrier_gen);
   int reached = atomic_fetch_add(&ctx->barrier_count, 1) + 1;
   if (reached == ctx->nthreads) {
      atomic_store(&ctx->barrier_count, 0);
      atomic_fetch_add(&ctx->barrier_gen, 1);
   } else {
      while (atomic_load(&ctx->barrier_gen) == gen)
         ; /* spin - this is a diagnostic probe, not production code */
   }
}

static void *
shared_thread(void *arg)
{
   struct shared_ctx *ctx = arg;

   for (int i = 0; i < ctx->iters_per_thread; i++) {
      if (i % ctx->iters_between_syncs == 0)
         shared_barrier(ctx);

      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = 65536,
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkBuffer buf = VK_NULL_HANDLE;
      if (ctx->create_buf(ctx->dev, &bci, NULL, &buf) != VK_SUCCESS)
         return NULL;

      VkMemoryRequirements reqs;
      ctx->get_reqs(ctx->dev, buf, &reqs);

      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = reqs.size,
         .memoryTypeIndex = ctx->mem_type,
      };
      VkDeviceMemory mem = VK_NULL_HANDLE;
      if (ctx->alloc_mem(ctx->dev, &mai, NULL, &mem) != VK_SUCCESS) {
         ctx->destroy_buf(ctx->dev, buf, NULL);
         return NULL;
      }

      ctx->bind_mem(ctx->dev, buf, mem, 0);
      ctx->free_mem(ctx->dev, mem, NULL);
      ctx->destroy_buf(ctx->dev, buf, NULL);
   }
   return NULL;
}

static int
run_shared_threads(const char *shim_path, int outer_iterations, int nthreads,
                   int iters_per_thread)
{
   void *h = dlopen(shim_path, RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen(%s) failed: %s\n", shim_path, dlerror());
      return 1;
   }
   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!create_instance) {
      printf("missing pre-instance entrypoints through the shim\n");
      return 1;
   }

   printf("shared-threads mode: outer_iterations=%d nthreads=%d "
          "iters_per_thread=%d\n",
          outer_iterations, nthreads, iters_per_thread);

   int completed = 0;
   for (int outer = 0; outer < outer_iterations; outer++) {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .pApplicationName = "device-churn-probe-shared",
         .apiVersion = VK_API_VERSION_1_3,
      };
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app,
      };
      VkInstance inst = VK_NULL_HANDLE;
      if (create_instance(&ici, NULL, &inst) != VK_SUCCESS) {
         printf("outer %d: vkCreateInstance failed, stopping\n", outer);
         break;
      }

#define SGIPA(name) (PFN_##name)gipa(inst, #name)
      PFN_vkEnumeratePhysicalDevices enum_pd = SGIPA(vkEnumeratePhysicalDevices);
      PFN_vkCreateDevice create_dev = SGIPA(vkCreateDevice);
      PFN_vkDestroyDevice destroy_dev = SGIPA(vkDestroyDevice);
      PFN_vkDestroyInstance destroy_inst = SGIPA(vkDestroyInstance);
      PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props =
         SGIPA(vkGetPhysicalDeviceMemoryProperties);

      uint32_t count = 1;
      VkPhysicalDevice pd = VK_NULL_HANDLE;
      enum_pd(inst, &count, &pd);

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
      VkDevice dev = VK_NULL_HANDLE;
      VkResult r = create_dev(pd, &dci, NULL, &dev);
      if (r != VK_SUCCESS) {
         printf("outer %d: vkCreateDevice (the shared device) -> %d, "
                "stopping\n",
                outer, r);
         destroy_inst(inst, NULL);
         break;
      }

      VkPhysicalDeviceMemoryProperties mem_props;
      get_mem_props(pd, &mem_props);
      VkMemoryRequirements dummy_reqs = {.memoryTypeBits = ~0u};
      uint32_t mem_type = 0;
      for (uint32_t m = 0; m < mem_props.memoryTypeCount; m++) {
         if (dummy_reqs.memoryTypeBits & (1u << m)) {
            mem_type = m;
            break;
         }
      }

      struct shared_ctx ctx = {
         .create_buf = SGIPA(vkCreateBuffer),
         .destroy_buf = SGIPA(vkDestroyBuffer),
         .get_reqs = SGIPA(vkGetBufferMemoryRequirements),
         .alloc_mem = SGIPA(vkAllocateMemory),
         .free_mem = SGIPA(vkFreeMemory),
         .bind_mem = SGIPA(vkBindBufferMemory),
         .dev = dev,
         .mem_type = mem_type,
         .iters_per_thread = iters_per_thread,
         .iters_between_syncs = iters_per_thread / 5 > 0 ? iters_per_thread / 5 : 1,
         .nthreads = nthreads,
      };
      atomic_init(&ctx.barrier_count, 0);
      atomic_init(&ctx.barrier_gen, 0);

      pthread_t *tids = calloc(nthreads, sizeof(*tids));
      for (int t = 0; t < nthreads; t++)
         pthread_create(&tids[t], NULL, shared_thread, &ctx);
      for (int t = 0; t < nthreads; t++)
         pthread_join(tids[t], NULL);
      free(tids);

      destroy_dev(dev, NULL);
      destroy_inst(inst, NULL);
      completed = outer + 1;

      printf("outer %d: shared-threads round done (all_fds=%d "
             "mali0_fds=%d)\n",
             outer, count_all_fds(), count_mali_fds());

      /* The actual test being run: after this shared-device round tears
       * down, does a completely fresh, unrelated device still work?
       */
      VkInstance inst2 = VK_NULL_HANDLE;
      if (create_instance(&ici, NULL, &inst2) != VK_SUCCESS) {
         printf("outer %d: POST-CHECK vkCreateInstance failed\n", outer);
         break;
      }
      PFN_vkEnumeratePhysicalDevices enum_pd2 = SGIPA(vkEnumeratePhysicalDevices);
      PFN_vkCreateDevice create_dev2 = SGIPA(vkCreateDevice);
      PFN_vkDestroyDevice destroy_dev2 = SGIPA(vkDestroyDevice);
      PFN_vkDestroyInstance destroy_inst2 = SGIPA(vkDestroyInstance);
      VkPhysicalDevice pd2 = VK_NULL_HANDLE;
      uint32_t count2 = 1;
      enum_pd2(inst2, &count2, &pd2);
      VkDevice dev2 = VK_NULL_HANDLE;
      r = create_dev2(pd2, &dci, NULL, &dev2);
      if (r != VK_SUCCESS) {
         printf("outer %d: POST-CHECK fresh device creation -> %d "
                "<<<<<< REPRODUCED\n",
                outer, r);
         destroy_inst2(inst2, NULL);
         break;
      }
      destroy_dev2(dev2, NULL);
      destroy_inst2(inst2, NULL);
   }

   printf("\n=== summary ===\n");
   printf("completed %d/%d shared-threads rounds\n", completed,
          outer_iterations);
   return completed == outer_iterations ? 0 : 1;
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 3) {
      fprintf(stderr,
              "usage: %s <path-to-icd-shim.so> <iterations> "
              "[--delay-ms N] [--use-queue] [--threads N]\n",
              argv[0]);
      return 2;
   }

   const char *shim_path = argv[1];
   int iterations = atoi(argv[2]);
   int delay_ms = 0;
   bool use_queue = false;
   bool alloc_buffer = false;
   int nthreads = 0;
   int many_objects = 0;
   int many_pipelines = 0;
   bool private_data = false;
   int private_data_slots = 0;
   int private_data_slots2 = 0;
   int shared_threads_n = 0;
   int shared_iters_per_thread = 100;
   for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc)
         delay_ms = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--use-queue"))
         use_queue = true;
      else if (!strcmp(argv[i], "--alloc-buffer"))
         alloc_buffer = true;
      else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
         nthreads = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--many-objects") && i + 1 < argc)
         many_objects = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--many-pipelines") && i + 1 < argc)
         many_pipelines = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--private-data"))
         private_data = true;
      else if (!strcmp(argv[i], "--private-data-slots") && i + 2 < argc) {
         private_data = true;
         private_data_slots = atoi(argv[++i]);
         private_data_slots2 = atoi(argv[++i]);
      } else if (!strcmp(argv[i], "--shared-threads") && i + 1 < argc)
         shared_threads_n = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--shared-iters") && i + 1 < argc)
         shared_iters_per_thread = atoi(argv[++i]);
   }

   if (private_data)
      return run_private_data(shim_path, iterations, private_data_slots,
                              private_data_slots2);

   if (shared_threads_n > 0)
      return run_shared_threads(shim_path, iterations, shared_threads_n,
                                shared_iters_per_thread);

   if (nthreads > 0)
      return run_threaded(shim_path, iterations, nthreads);

   void *h = dlopen(shim_path, RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("dlopen(%s) failed: %s\n", shim_path, dlerror());
      return 1;
   }

   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   if (!gipa) {
      printf("dlsym(vkGetInstanceProcAddr) failed: %s\n", dlerror());
      return 1;
   }

   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!create_instance) {
      printf("missing pre-instance entrypoints through the shim\n");
      return 1;
   }

   printf("iterations=%d delay_ms=%d use_queue=%d\n", iterations, delay_ms,
          use_queue);
   printf("baseline: all_fds=%d mali0_fds=%d\n", count_all_fds(),
          count_mali_fds());

   int max_mali_fds = 0;
   int completed = 0;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   for (int i = 0; i < iterations; i++) {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .pApplicationName = "device-churn-probe",
         .apiVersion = VK_API_VERSION_1_3,
      };
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app,
      };
      VkInstance inst = VK_NULL_HANDLE;
      VkResult r = create_instance(&ici, NULL, &inst);
      if (r != VK_SUCCESS) {
         printf("iteration %d: vkCreateInstance -> %d, stopping. "
                "all_fds=%d mali0_fds=%d\n",
                i, r, count_all_fds(), count_mali_fds());
         break;
      }

#define GIPA(name) (PFN_##name)gipa(inst, #name)
      PFN_vkEnumeratePhysicalDevices enum_pd = GIPA(vkEnumeratePhysicalDevices);
      PFN_vkCreateDevice create_dev = GIPA(vkCreateDevice);
      PFN_vkDestroyDevice destroy_dev = GIPA(vkDestroyDevice);
      PFN_vkDestroyInstance destroy_inst = GIPA(vkDestroyInstance);

      uint32_t count = 1;
      VkPhysicalDevice pd = VK_NULL_HANDLE;
      r = enum_pd(inst, &count, &pd);
      if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
         printf("iteration %d: vkEnumeratePhysicalDevices -> %d count=%u, "
                "stopping. all_fds=%d mali0_fds=%d\n",
                i, r, count, count_all_fds(), count_mali_fds());
         destroy_inst(inst, NULL);
         break;
      }

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
      VkDevice dev = VK_NULL_HANDLE;
      r = create_dev(pd, &dci, NULL, &dev);
      if (r != VK_SUCCESS) {
         printf("iteration %d: vkCreateDevice -> %d, stopping. "
                "all_fds=%d mali0_fds=%d\n",
                i, r, count_all_fds(), count_mali_fds());
         destroy_inst(inst, NULL);
         break;
      }

      if (use_queue) {
#define GDPA(name) (PFN_##name)gipa(inst, #name) /* HAL forwards these too */
         PFN_vkGetDeviceQueue get_queue = GDPA(vkGetDeviceQueue);
         PFN_vkCreateCommandPool create_pool = GDPA(vkCreateCommandPool);
         PFN_vkDestroyCommandPool destroy_pool = GDPA(vkDestroyCommandPool);
         PFN_vkAllocateCommandBuffers alloc_cmdbuf = GDPA(vkAllocateCommandBuffers);
         PFN_vkFreeCommandBuffers free_cmdbuf = GDPA(vkFreeCommandBuffers);
         PFN_vkBeginCommandBuffer begin_cmdbuf = GDPA(vkBeginCommandBuffer);
         PFN_vkEndCommandBuffer end_cmdbuf = GDPA(vkEndCommandBuffer);

         if (!get_queue || !create_pool || !destroy_pool || !alloc_cmdbuf ||
             !free_cmdbuf || !begin_cmdbuf || !end_cmdbuf) {
            printf("iteration %d: missing device-level entrypoint, "
                   "stopping\n",
                   i);
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkQueue queue = VK_NULL_HANDLE;
         get_queue(dev, 0, 0, &queue);

         VkCommandPoolCreateInfo cpci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = 0,
         };
         VkCommandPool pool = VK_NULL_HANDLE;
         r = create_pool(dev, &cpci, NULL, &pool);
         if (r != VK_SUCCESS) {
            printf("iteration %d: vkCreateCommandPool -> %d, stopping. "
                   "all_fds=%d mali0_fds=%d\n",
                   i, r, count_all_fds(), count_mali_fds());
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
         };
         VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
         r = alloc_cmdbuf(dev, &cbai, &cmdbuf);
         if (r != VK_SUCCESS) {
            printf("iteration %d: vkAllocateCommandBuffers -> %d, "
                   "stopping. all_fds=%d mali0_fds=%d\n",
                   i, r, count_all_fds(), count_mali_fds());
            destroy_pool(dev, pool, NULL);
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkCommandBufferBeginInfo cbbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         };
         begin_cmdbuf(cmdbuf, &cbbi);
         end_cmdbuf(cmdbuf);

         free_cmdbuf(dev, pool, 1, &cmdbuf);
         destroy_pool(dev, pool, NULL);
      }

      if (alloc_buffer) {
         PFN_vkCreateBuffer create_buf = GIPA(vkCreateBuffer);
         PFN_vkDestroyBuffer destroy_buf = GIPA(vkDestroyBuffer);
         PFN_vkGetBufferMemoryRequirements get_reqs =
            GIPA(vkGetBufferMemoryRequirements);
         PFN_vkAllocateMemory alloc_mem = GIPA(vkAllocateMemory);
         PFN_vkFreeMemory free_mem = GIPA(vkFreeMemory);
         PFN_vkBindBufferMemory bind_mem = GIPA(vkBindBufferMemory);
         PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props =
            GIPA(vkGetPhysicalDeviceMemoryProperties);

         if (!create_buf || !destroy_buf || !get_reqs || !alloc_mem ||
             !free_mem || !bind_mem || !get_mem_props) {
            printf("iteration %d: missing memory-related entrypoint, "
                   "stopping\n",
                   i);
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 65536,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         };
         VkBuffer buf = VK_NULL_HANDLE;
         r = create_buf(dev, &bci, NULL, &buf);
         if (r != VK_SUCCESS) {
            printf("iteration %d: vkCreateBuffer -> %d, stopping. "
                   "all_fds=%d mali0_fds=%d\n",
                   i, r, count_all_fds(), count_mali_fds());
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkMemoryRequirements reqs;
         get_reqs(dev, buf, &reqs);

         VkPhysicalDeviceMemoryProperties mem_props;
         get_mem_props(pd, &mem_props);
         uint32_t mem_type = UINT32_MAX;
         for (uint32_t m = 0; m < mem_props.memoryTypeCount; m++) {
            if (reqs.memoryTypeBits & (1u << m)) {
               mem_type = m;
               break;
            }
         }

         VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = mem_type,
         };
         VkDeviceMemory mem = VK_NULL_HANDLE;
         r = alloc_mem(dev, &mai, NULL, &mem);
         if (r != VK_SUCCESS) {
            printf("iteration %d: vkAllocateMemory -> %d, stopping. "
                   "all_fds=%d mali0_fds=%d\n",
                   i, r, count_all_fds(), count_mali_fds());
            destroy_buf(dev, buf, NULL);
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         bind_mem(dev, buf, mem, 0);
         free_mem(dev, mem, NULL);
         destroy_buf(dev, buf, NULL);
      }

      if (many_objects > 0) {
         PFN_vkCreateBuffer create_buf = GIPA(vkCreateBuffer);
         PFN_vkDestroyBuffer destroy_buf = GIPA(vkDestroyBuffer);
         PFN_vkGetBufferMemoryRequirements get_reqs =
            GIPA(vkGetBufferMemoryRequirements);
         PFN_vkAllocateMemory alloc_mem = GIPA(vkAllocateMemory);
         PFN_vkFreeMemory free_mem = GIPA(vkFreeMemory);
         PFN_vkBindBufferMemory bind_mem = GIPA(vkBindBufferMemory);
         PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props =
            GIPA(vkGetPhysicalDeviceMemoryProperties);

         if (!create_buf || !destroy_buf || !get_reqs || !alloc_mem ||
             !free_mem || !bind_mem || !get_mem_props) {
            printf("iteration %d: missing memory-related entrypoint, "
                   "stopping\n",
                   i);
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkPhysicalDeviceMemoryProperties mem_props;
         get_mem_props(pd, &mem_props);

         VkBuffer *bufs = calloc(many_objects, sizeof(*bufs));
         VkDeviceMemory *mems = calloc(many_objects, sizeof(*mems));
         int live = 0;
         VkResult stop_result = VK_SUCCESS;

         for (int n = 0; n < many_objects; n++) {
            VkBufferCreateInfo bci = {
               .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
               .size = 65536,
               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };
            r = create_buf(dev, &bci, NULL, &bufs[n]);
            if (r != VK_SUCCESS) {
               stop_result = r;
               printf("iteration %d: vkCreateBuffer failed at object %d/%d "
                      "-> %d\n",
                      i, n, many_objects, r);
               break;
            }

            VkMemoryRequirements reqs;
            get_reqs(dev, bufs[n], &reqs);
            uint32_t mem_type = UINT32_MAX;
            for (uint32_t m = 0; m < mem_props.memoryTypeCount; m++) {
               if (reqs.memoryTypeBits & (1u << m)) {
                  mem_type = m;
                  break;
               }
            }
            VkMemoryAllocateInfo mai = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
               .allocationSize = reqs.size,
               .memoryTypeIndex = mem_type,
            };
            r = alloc_mem(dev, &mai, NULL, &mems[n]);
            if (r != VK_SUCCESS) {
               stop_result = r;
               printf("iteration %d: vkAllocateMemory failed at object "
                      "%d/%d -> %d (all_fds=%d mali0_fds=%d)\n",
                      i, n, many_objects, r, count_all_fds(),
                      count_mali_fds());
               destroy_buf(dev, bufs[n], NULL);
               break;
            }
            bind_mem(dev, bufs[n], mems[n], 0);
            live = n + 1;
         }

         if (i % 5 == 0 || live < many_objects) {
            printf("iteration %d: held %d/%d buffers simultaneously alive "
                   "(64KB each = %.1fMB), all_fds=%d mali0_fds=%d\n",
                   i, live, many_objects, live * 65536.0 / (1024 * 1024),
                   count_all_fds(), count_mali_fds());
         }

         for (int n = 0; n < live; n++) {
            free_mem(dev, mems[n], NULL);
            destroy_buf(dev, bufs[n], NULL);
         }
         free(bufs);
         free(mems);

         if (stop_result != VK_SUCCESS) {
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }
      }

      if (many_pipelines > 0) {
         PFN_vkCreateShaderModule create_module = GIPA(vkCreateShaderModule);
         PFN_vkDestroyShaderModule destroy_module = GIPA(vkDestroyShaderModule);
         PFN_vkCreateDescriptorSetLayout create_dsl =
            GIPA(vkCreateDescriptorSetLayout);
         PFN_vkDestroyDescriptorSetLayout destroy_dsl =
            GIPA(vkDestroyDescriptorSetLayout);
         PFN_vkCreatePipelineLayout create_pl = GIPA(vkCreatePipelineLayout);
         PFN_vkDestroyPipelineLayout destroy_pl = GIPA(vkDestroyPipelineLayout);
         PFN_vkCreateComputePipelines create_pipelines =
            GIPA(vkCreateComputePipelines);
         PFN_vkDestroyPipeline destroy_pipeline = GIPA(vkDestroyPipeline);

         if (!create_module || !destroy_module || !create_dsl ||
             !destroy_dsl || !create_pl || !destroy_pl || !create_pipelines ||
             !destroy_pipeline) {
            printf("iteration %d: missing pipeline-related entrypoint, "
                   "stopping\n",
                   i);
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkShaderModuleCreateInfo smci = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(pipeline_probe_shader),
            .pCode = pipeline_probe_shader,
         };
         VkShaderModule module = VK_NULL_HANDLE;
         r = create_module(dev, &smci, NULL, &module);
         if (r != VK_SUCCESS) {
            printf("iteration %d: vkCreateShaderModule -> %d, stopping\n", i,
                   r);
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }

         VkDescriptorSetLayoutBinding binding = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         };
         VkDescriptorSetLayoutCreateInfo dslci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding,
         };
         VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
         create_dsl(dev, &dslci, NULL, &dsl);

         VkPushConstantRange pcr = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(uint32_t),
         };
         VkPipelineLayoutCreateInfo plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &dsl,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr,
         };
         VkPipelineLayout layout = VK_NULL_HANDLE;
         create_pl(dev, &plci, NULL, &layout);

         VkPipeline *pipelines = calloc(many_pipelines, sizeof(*pipelines));
         int live = 0;
         VkResult stop_result = VK_SUCCESS;

         for (int n = 0; n < many_pipelines; n++) {
            VkComputePipelineCreateInfo cpci = {
               .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
               .stage = {
                  .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main",
               },
               .layout = layout,
            };
            r = create_pipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL,
                                  &pipelines[n]);
            if (r != VK_SUCCESS) {
               stop_result = r;
               printf("iteration %d: vkCreateComputePipelines failed at "
                      "pipeline %d/%d -> %d (all_fds=%d mali0_fds=%d)\n",
                      i, n, many_pipelines, r, count_all_fds(),
                      count_mali_fds());
               break;
            }
            live = n + 1;
         }

         if (i % 5 == 0 || live < many_pipelines) {
            printf("iteration %d: held %d/%d compute pipelines "
                   "simultaneously alive, all_fds=%d mali0_fds=%d\n",
                   i, live, many_pipelines, count_all_fds(),
                   count_mali_fds());
         }

         for (int n = 0; n < live; n++)
            destroy_pipeline(dev, pipelines[n], NULL);
         free(pipelines);

         destroy_pl(dev, layout, NULL);
         destroy_dsl(dev, dsl, NULL);
         destroy_module(dev, module, NULL);

         if (stop_result != VK_SUCCESS) {
            destroy_dev(dev, NULL);
            destroy_inst(inst, NULL);
            break;
         }
      }

      destroy_dev(dev, NULL);
      destroy_inst(inst, NULL);
      completed = i + 1;

      int mali_fds = count_mali_fds();
      if (mali_fds > max_mali_fds)
         max_mali_fds = mali_fds;

      if (i % 20 == 0 || i == iterations - 1) {
         printf("iteration %d ok: all_fds=%d mali0_fds=%d\n", i,
                count_all_fds(), mali_fds);
      }

      if (delay_ms > 0) {
         struct timespec ts = {.tv_sec = delay_ms / 1000,
                                .tv_nsec = (delay_ms % 1000) * 1000000L};
         nanosleep(&ts, NULL);
      }
   }

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

   printf("\n=== summary ===\n");
   printf("completed %d/%d full create+destroy cycles in %.2fs\n", completed,
          iterations, elapsed);
   printf("max mali0 fds observed at any point: %d\n", max_mali_fds);
   printf("final: all_fds=%d mali0_fds=%d\n", count_all_fds(),
          count_mali_fds());

   return completed == iterations ? 0 : 1;
}
