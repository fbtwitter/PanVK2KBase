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
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

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
   for (int i = 3; i < argc; i++) {
      if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc)
         delay_ms = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--use-queue"))
         use_queue = true;
      else if (!strcmp(argv[i], "--alloc-buffer"))
         alloc_buffer = true;
      else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
         nthreads = atoi(argv[++i]);
   }

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
