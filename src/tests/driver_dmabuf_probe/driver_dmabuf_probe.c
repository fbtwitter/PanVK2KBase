// Does a real dma-buf become a VkDeviceMemory the GPU can write?
//
// tests/dmabuf_import_probe answered the kernel-side half: KBASE_IOCTL_MEM_
// IMPORT accepts a dma-buf and hands back a NEED_MMAP cookie that resolves
// to a usable GPU address. This is the other half - the same thing through
// the actual Vulkan entry points, with the GPU doing the writing.
//
// The load-bearing assertion is step 6, and it is deliberately awkward:
// after the GPU writes a pattern into the imported memory, the readback goes
// through a SEPARATE, DIRECT mmap of the dma-buf fd - not through
// vkMapMemory. A vkMapMemory readback would be satisfied by a driver that
// imported nothing and quietly allocated fresh memory instead; only reading
// the dma-buf's own pages proves the import was real.
//
// (vkMapMemory would not work here anyway: an imported region on this kernel
// has no CPU view at all - mmap succeeds and touching it takes SIGBUS. See
// docs/kbase-notes.md. That is fine for the use case this unblocks, since a
// gralloc buffer is written by the GPU, not the CPU.)
//
// No --i-know-it-hangs gate: this runs a transfer/compute submit, which has
// been stable for thousands of submits, and it does not enter a render pass.
//
// Usage:
//   driver_dmabuf_probe <path-to-libvulkan_panfrost.so> [--source=heap|ahb]
//                       [--loop=N]
//
//   --loop=N repeats import/use/free N times. A leaked kernel region per
//   import is invisible in a single run and obvious in fifty.
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "dmabuf_source.h"

/* Same libhardware ABI mirrors as every other driver probe here - see
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

#define BUF_SIZE 4096
#define CPU_PATTERN 0xA5A5A5A5u /* written by the CPU before importing */
#define GPU_PATTERN 0x5EEDF00Du /* written by the GPU through the import */

static int failures;

static void check(bool ok, const char *what) {
   printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

int main(int argc, char **argv) {
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 2) {
      fprintf(stderr, "usage: %s <path-to-libvulkan_panfrost.so> "
                      "[--source=heap|ahb] [--loop=N]\n", argv[0]);
      return 2;
   }

   const char *source = "heap";
   int loops = 1;
   for (int i = 2; i < argc; i++) {
      if (!strncmp(argv[i], "--source=", 9))
         source = argv[i] + 9;
      else if (!strncmp(argv[i], "--loop=", 7))
         loops = atoi(argv[i] + 7);
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
      .pApplicationName = "panvk-kbase-dmabuf-probe",
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
   PFN_vkCreateDevice create_dev = GIPA(vkCreateDevice);
   PFN_vkGetDeviceProcAddr gdpa = GIPA(vkGetDeviceProcAddr);
   PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props =
      GIPA(vkGetPhysicalDeviceMemoryProperties);
   PFN_vkGetPhysicalDeviceExternalBufferProperties get_ext_buf =
      GIPA(vkGetPhysicalDeviceExternalBufferProperties);

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   VkResult r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      printf("no physical device (%d)\n", r);
      return 1;
   }

   /* ------------------------------------------- the query half of the proof */
   printf("\n=== capability query ===\n");
   {
      VkPhysicalDeviceExternalBufferInfo info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      VkExternalBufferProperties props = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
      };
      get_ext_buf(pd, &info, &props);
      VkExternalMemoryFeatureFlags f =
         props.externalMemoryProperties.externalMemoryFeatures;
      printf("  DMA_BUF externalMemoryFeatures = 0x%x\n", f);
      /* Catches a driver built without the patch scripts re-run. */
      check((f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0,
            "driver advertises DMA_BUF as IMPORTABLE");
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
   VkDevice device = VK_NULL_HANDLE;
   r = create_dev(pd, &dci, NULL, &device);
   check(r == VK_SUCCESS, "vkCreateDevice");
   if (r != VK_SUCCESS)
      return 1;

#define GDPA(n) (PFN_##n) gdpa(device, #n)
   PFN_vkGetDeviceQueue get_queue = GDPA(vkGetDeviceQueue);
   PFN_vkAllocateMemory alloc_mem = GDPA(vkAllocateMemory);
   PFN_vkFreeMemory free_mem = GDPA(vkFreeMemory);
   PFN_vkCreateBuffer create_buffer = GDPA(vkCreateBuffer);
   PFN_vkDestroyBuffer destroy_buffer = GDPA(vkDestroyBuffer);
   PFN_vkGetBufferMemoryRequirements get_buf_reqs =
      GDPA(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory bind_buf = GDPA(vkBindBufferMemory);
   PFN_vkCreateCommandPool create_pool = GDPA(vkCreateCommandPool);
   PFN_vkDestroyCommandPool destroy_pool = GDPA(vkDestroyCommandPool);
   PFN_vkAllocateCommandBuffers alloc_cb = GDPA(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_cb = GDPA(vkBeginCommandBuffer);
   PFN_vkEndCommandBuffer end_cb = GDPA(vkEndCommandBuffer);
   PFN_vkCmdFillBuffer cmd_fill = GDPA(vkCmdFillBuffer);
   PFN_vkCmdPipelineBarrier cmd_barrier = GDPA(vkCmdPipelineBarrier);
   PFN_vkQueueSubmit queue_submit = GDPA(vkQueueSubmit);
   PFN_vkCreateFence create_fence = GDPA(vkCreateFence);
   PFN_vkDestroyFence destroy_fence = GDPA(vkDestroyFence);
   PFN_vkWaitForFences wait_fences = GDPA(vkWaitForFences);

   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);

   VkPhysicalDeviceMemoryProperties mem_props;
   get_mem_props(pd, &mem_props);

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   create_pool(device, &cpci, NULL, &pool);

   int ok_rounds = 0;

   for (int round = 0; round < loops; round++) {
      bool verbose = (round == 0 || round == loops - 1);
      if (verbose)
         printf("\n=== round %d/%d ===\n", round + 1, loops);

      /* 1. a real dma-buf */
      struct dmabuf_src src;
      if (dmabuf_source_open(&src, source, "system", BUF_SIZE) != 0) {
         printf("  dma-buf source unavailable: %s\n", src.err);
         failures++;
         break;
      }

      /* On gralloc handles the dma-buf is not necessarily data[0] - on this
       * device it is data[1]. Pick the fd that actually looks like one.
       */
      int dfd = src.fd;
      for (int i = 0; i < src.n_handle_fds; i++) {
         if (lseek(src.handle_fds[i], 0, SEEK_END) > 0) {
            dfd = src.handle_fds[i];
            break;
         }
      }

      /* 2. CPU writes a pattern through the dma-buf's own mapping */
      void *dmap = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                        dfd, 0);
      if (dmap == MAP_FAILED) {
         printf("  mmap(dma-buf) failed: %s\n", strerror(errno));
         dmabuf_source_close(&src);
         failures++;
         break;
      }
      dmabuf_cpu_write_begin(dfd);
      for (unsigned i = 0; i < BUF_SIZE / 4; i++)
         ((volatile uint32_t *)dmap)[i] = CPU_PATTERN;
      dmabuf_cpu_write_end(dfd);

      /* 3. import it as VkDeviceMemory - the exact struct panvk_android.c
       * builds for an AHardwareBuffer.
       */
      int import_fd = dup(dfd);
      VkImportMemoryFdInfoKHR import_info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         .fd = import_fd,
      };

      VkExternalMemoryBufferCreateInfo ext_buf = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .pNext = &ext_buf,
         .size = BUF_SIZE,
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkBuffer buf = VK_NULL_HANDLE;
      r = create_buffer(device, &bci, NULL, &buf);
      if (verbose)
         check(r == VK_SUCCESS, "vkCreateBuffer (external)");

      VkMemoryRequirements reqs;
      get_buf_reqs(device, buf, &reqs);

      uint32_t type_idx = UINT32_MAX;
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
         if (reqs.memoryTypeBits & (1u << i)) {
            type_idx = i;
            break;
         }
      }

      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .pNext = &import_info,
         .allocationSize = reqs.size < BUF_SIZE ? BUF_SIZE : reqs.size,
         .memoryTypeIndex = type_idx,
      };
      VkDeviceMemory mem = VK_NULL_HANDLE;
      r = alloc_mem(device, &mai, NULL, &mem);
      if (verbose) {
         printf("  vkAllocateMemory(VkImportMemoryFdInfoKHR{DMA_BUF}) -> %d\n",
                r);
         check(r == VK_SUCCESS, "imported a dma-buf as VkDeviceMemory");
      } else if (r != VK_SUCCESS) {
         printf("  round %d: import failed (%d)\n", round + 1, r);
         failures++;
      }
      if (r != VK_SUCCESS) {
         close(import_fd);
         destroy_buffer(device, buf, NULL);
         munmap(dmap, BUF_SIZE);
         dmabuf_source_close(&src);
         break;
      }

      r = bind_buf(device, buf, mem, 0);
      if (verbose)
         check(r == VK_SUCCESS, "vkBindBufferMemory onto imported memory");

      /* 4. the GPU writes into it */
      VkCommandBufferAllocateInfo cbai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      VkCommandBuffer cb = VK_NULL_HANDLE;
      alloc_cb(device, &cbai, &cb);

      VkCommandBufferBeginInfo cbbi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      begin_cb(cb, &cbbi);
      cmd_fill(cb, buf, 0, BUF_SIZE, GPU_PATTERN);

      VkMemoryBarrier mb = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      cmd_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
      end_cb(cb);

      VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkFence fence = VK_NULL_HANDLE;
      create_fence(device, &fci, NULL, &fence);

      VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cb,
      };
      r = queue_submit(queue, 1, &si, fence);
      if (verbose)
         check(r == VK_SUCCESS, "vkQueueSubmit (GPU fills the imported buffer)");

      if (r == VK_SUCCESS) {
         r = wait_fences(device, 1, &fence, VK_TRUE, 5000000000ull);
         if (verbose)
            check(r == VK_SUCCESS, "fence signalled by the GPU");
      }

      /* 5. THE LOAD-BEARING CHECK: read back through the dma-buf's own
       * mapping, never through vkMapMemory. If the driver had imported
       * nothing and allocated fresh memory, this would still hold
       * CPU_PATTERN.
       */
      /* The invalidate here is load-bearing, not ceremony: the imported
       * region is CACHED_CPU, so without it this read can return the
       * pre-import cache lines and a working import looks broken.
       */
      int sync_rc = dmabuf_cpu_read_begin(dfd);
      uint32_t first = ((volatile uint32_t *)dmap)[0];
      uint32_t mid = ((volatile uint32_t *)dmap)[BUF_SIZE / 8];
      uint32_t last = ((volatile uint32_t *)dmap)[BUF_SIZE / 4 - 1];
      dmabuf_cpu_read_end(dfd);
      bool wrote = (first == GPU_PATTERN && mid == GPU_PATTERN &&
                    last == GPU_PATTERN);
      if (verbose && sync_rc != 0)
         printf("  (DMA_BUF_IOCTL_SYNC returned %d: %s)\n", sync_rc,
                strerror(errno));

      if (verbose) {
         printf("  dma-buf mapping now reads [0]=0x%08x [mid]=0x%08x "
                "[last]=0x%08x\n", first, mid, last);
         printf("  (CPU wrote 0x%08x before the import; GPU wrote 0x%08x)\n",
                CPU_PATTERN, GPU_PATTERN);
         check(wrote, "GPU write landed in the dma-buf's OWN pages");
      } else if (!wrote) {
         printf("  round %d: readback mismatch (0x%08x)\n", round + 1, first);
         failures++;
      }

      if (wrote)
         ok_rounds++;

      destroy_fence(device, fence, NULL);
      free_mem(device, mem, NULL);
      destroy_buffer(device, buf, NULL);
      close(import_fd);
      munmap(dmap, BUF_SIZE);
      dmabuf_source_close(&src);

      if (!wrote)
         break;
   }

   destroy_pool(device, pool, NULL);

   printf("\n=== %d/%d rounds ok, %d failure(s) ===\n", ok_rounds, loops,
          failures);
   if (failures == 0)
      printf("\n=> A dma-buf imports as VkDeviceMemory on kbase and the GPU\n"
             "   writes through it: the pattern was read back through an\n"
             "   independent mapping of the dma-buf, so the driver really\n"
             "   bound the imported pages rather than allocating its own.\n");
   return failures ? 1 : 0;
}
