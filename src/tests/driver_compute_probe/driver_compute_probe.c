// Exercises compute command buffers through the real Vulkan API.
//
// tests/driver_sync_probe got an *empty* vkQueueSubmit signalling a fence
// from the GPU. This goes one step further: a real command buffer, whose
// stream the kbase submit path CALLs where it lies rather than copying into
// the ring (see panvk_vX_kbase_queue.c).
//
// STAGED ON PURPOSE, and the default stage touches no GPU.
//
//   (default)   record a command buffer and end it. No submit. This proves
//               the command pool, the mempools behind it and the CS builders
//               work on kbase, and it cannot hang the device because nothing
//               is ever kicked.
//   --submit    additionally submit that command buffer and wait on a fence.
//               THIS IS THE FIRST TIME A CALLed STREAM RUNS. If the stream
//               or the flush ahead of it is wrong, the GPU faults, and a
//               faulting kbase context has previously needed a reboot to
//               clear - see docs/kbase-notes.md.
//   --fill      additionally vkCmdFillBuffer into a mapped buffer and check
//               the pattern came back. Implies --submit. This is the first
//               stage that runs an actual shader.
//
// Run them in that order, on separate runs, and read the output of each
// before going to the next. Every wait here has a finite timeout so a stuck
// queue reports rather than blocking forever.
//
// Usage: driver_compute_probe <path-to-libvulkan_panfrost.so> [--submit] [--fill]
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

#define FILL_BYTES   4096
#define FILL_PATTERN 0xa5a5a5a5u

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
   bool do_submit = false, do_fill = false;

   if (argc < 2) {
      fprintf(stderr,
              "usage: %s <path-to-libvulkan_panfrost.so> [--submit] [--fill]\n",
              argv[0]);
      return 2;
   }
   for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--submit")) {
         do_submit = true;
      } else if (!strcmp(argv[i], "--fill")) {
         do_fill = true;
         do_submit = true;
      } else {
         fprintf(stderr, "unknown argument: %s\n", argv[i]);
         return 2;
      }
   }

   /* Unbuffered. Both device hangs this repo has caused produced zero
    * diagnostics because the default block buffering on a pipe discards
    * everything printed so far when the run is killed.
    */
   setvbuf(stdout, NULL, _IONBF, 0);

   printf("stage: record%s%s\n", do_submit ? " + submit" : "",
          do_fill ? " + fill" : "");
   if (!do_submit)
      printf("       (no GPU work will be kicked in this stage)\n");

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
      .pApplicationName = "panvk-kbase-compute-probe",
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
   PFN_vkCreateDevice create_dev = GIPA(vkCreateDevice);
   PFN_vkGetDeviceProcAddr gdpa = GIPA(vkGetDeviceProcAddr);
   PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props =
      GIPA(vkGetPhysicalDeviceMemoryProperties);

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      printf("no physical device (%d)\n", r);
      return 1;
   }

   printf("\n=== device ===\n");
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
   if (r != VK_SUCCESS) {
      printf("\n=> device creation failed (%d); nothing below can run.\n"
             "   check `adb logcat -d | grep MESA`\n", r);
      return 1;
   }

#define GDPA(name) (PFN_##name) gdpa(device, #name)
   PFN_vkGetDeviceQueue get_queue = GDPA(vkGetDeviceQueue);
   PFN_vkCreateCommandPool create_pool = GDPA(vkCreateCommandPool);
   PFN_vkDestroyCommandPool destroy_pool = GDPA(vkDestroyCommandPool);
   PFN_vkAllocateCommandBuffers alloc_cmdbufs = GDPA(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_cmdbuf = GDPA(vkBeginCommandBuffer);
   PFN_vkEndCommandBuffer end_cmdbuf = GDPA(vkEndCommandBuffer);
   PFN_vkCmdPipelineBarrier2 cmd_barrier2 = GDPA(vkCmdPipelineBarrier2);
   PFN_vkCmdFillBuffer cmd_fill = GDPA(vkCmdFillBuffer);
   PFN_vkQueueSubmit queue_submit = GDPA(vkQueueSubmit);
   PFN_vkCreateFence create_fence = GDPA(vkCreateFence);
   PFN_vkDestroyFence destroy_fence = GDPA(vkDestroyFence);
   PFN_vkWaitForFences wait_fences = GDPA(vkWaitForFences);
   PFN_vkCreateBuffer create_buffer = GDPA(vkCreateBuffer);
   PFN_vkDestroyBuffer destroy_buffer = GDPA(vkDestroyBuffer);
   PFN_vkGetBufferMemoryRequirements get_buf_reqs =
      GDPA(vkGetBufferMemoryRequirements);
   PFN_vkAllocateMemory alloc_mem = GDPA(vkAllocateMemory);
   PFN_vkFreeMemory free_mem = GDPA(vkFreeMemory);
   PFN_vkBindBufferMemory bind_buf_mem = GDPA(vkBindBufferMemory);
   PFN_vkMapMemory map_mem = GDPA(vkMapMemory);

   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);
   check(queue != VK_NULL_HANDLE, "vkGetDeviceQueue");

   /* ------------------------------------------------------ command pool */
   printf("\n=== command buffer recording ===\n");
   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   r = create_pool(device, &cpci, NULL, &pool);
   check(r == VK_SUCCESS, "vkCreateCommandPool");
   if (r != VK_SUCCESS)
      return 1;

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
   r = alloc_cmdbufs(device, &cbai, &cmdbuf);
   check(r == VK_SUCCESS, "vkAllocateCommandBuffers");
   if (r != VK_SUCCESS)
      return 1;

   /* A buffer to fill, allocated before recording so the fill can reference
    * it. Only actually used in the --fill stage, but allocating it in every
    * stage means vkCreateBuffer/vkAllocateMemory get exercised by the stage
    * that cannot hang.
    */
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   void *mapped = NULL;

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = FILL_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   r = create_buffer(device, &bci, NULL, &buffer);
   check(r == VK_SUCCESS, "vkCreateBuffer");

   if (r == VK_SUCCESS) {
      VkMemoryRequirements reqs;
      get_buf_reqs(device, buffer, &reqs);

      VkPhysicalDeviceMemoryProperties mem_props;
      get_mem_props(pd, &mem_props);

      /* HOST_VISIBLE | HOST_COHERENT so the pattern can be read back without
       * an explicit invalidate on the CPU side.
       */
      const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      uint32_t type_idx = UINT32_MAX;
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
         if (!(reqs.memoryTypeBits & (1u << i)))
            continue;
         if ((mem_props.memoryTypes[i].propertyFlags & want) == want) {
            type_idx = i;
            break;
         }
      }
      check(type_idx != UINT32_MAX, "host-visible memory type found");

      if (type_idx != UINT32_MAX) {
         VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = type_idx,
         };
         r = alloc_mem(device, &mai, NULL, &memory);
         check(r == VK_SUCCESS, "vkAllocateMemory");

         if (r == VK_SUCCESS) {
            r = bind_buf_mem(device, buffer, memory, 0);
            check(r == VK_SUCCESS, "vkBindBufferMemory");

            r = map_mem(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
            check(r == VK_SUCCESS, "vkMapMemory");
         }
      }
   }

   /* Seed the buffer with the complement of the pattern, so a readback that
    * matches cannot be the memory happening to already hold it.
    */
   if (mapped)
      memset(mapped, (int)(~FILL_PATTERN & 0xff), FILL_BYTES);

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = begin_cmdbuf(cmdbuf, &cbbi);
   check(r == VK_SUCCESS, "vkBeginCommandBuffer");

   if (do_fill && mapped) {
      cmd_fill(cmdbuf, buffer, 0, FILL_BYTES, FILL_PATTERN);
      printf("  recorded vkCmdFillBuffer(%u bytes, 0x%08x)\n", FILL_BYTES,
             FILL_PATTERN);
   }

   /* A compute-to-compute barrier. Deliberately the narrowest dependency
    * that still produces a non-empty compute stream: anything naming another
    * stage could make PanVK emit a cross-subqueue wait, and the vertex-tiler
    * and fragment subqueues have no GPU-side context here, so their syncobjs
    * never advance and such a wait would never be satisfied.
    */
   VkMemoryBarrier2 mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
   };
   VkDependencyInfo dep = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &mb,
   };
   cmd_barrier2(cmdbuf, &dep);
   printf("  recorded a compute->compute VkMemoryBarrier2\n");

   r = end_cmdbuf(cmdbuf);
   check(r == VK_SUCCESS, "vkEndCommandBuffer");

   if (!do_submit) {
      printf("\n=> recording stage passed with %d failure(s).\n"
             "   Nothing was submitted. Re-run with --submit to kick it,\n"
             "   which is the step that can fault the GPU.\n",
             failures);
      goto done;
   }

   /* ------------------------------------------------------------ submit */
   printf("\n=== submit (GPU work starts here) ===\n");

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence = VK_NULL_HANDLE;
   r = create_fence(device, &fci, NULL, &fence);
   check(r == VK_SUCCESS, "vkCreateFence");

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmdbuf,
   };

   printf("  submitting...\n");
   r = queue_submit(queue, 1, &si, fence);
   printf("  vkQueueSubmit -> %d\n", r);
   check(r == VK_SUCCESS, "vkQueueSubmit");

   if (r == VK_SUCCESS) {
      /* 2s, the budget every probe in this repo uses before calling a stream
       * never-going-to-run. VK_TIMEOUT here means the GPU took the work and
       * never finished it, which is the interesting failure.
       */
      printf("  waiting on the fence (2s budget)...\n");
      r = wait_fences(device, 1, &fence, VK_TRUE, 2000000000ull);
      printf("  vkWaitForFences -> %d%s\n", r,
             r == VK_TIMEOUT ? " (TIMEOUT - the GPU did not finish)" : "");
      check(r == VK_SUCCESS, "fence signalled by the GPU");
   }

   if (do_fill && mapped && r == VK_SUCCESS) {
      printf("\n=== readback ===\n");
      const uint32_t *words = mapped;
      uint32_t bad = 0, first_bad = 0;
      for (uint32_t i = 0; i < FILL_BYTES / 4; i++) {
         if (words[i] != FILL_PATTERN) {
            if (!bad)
               first_bad = i;
            bad++;
         }
      }
      if (bad) {
         printf("  %u of %u words wrong; first at [%u] = 0x%08x\n", bad,
                FILL_BYTES / 4, first_bad, words[first_bad]);
      }
      check(bad == 0, "buffer holds the fill pattern");
   }

   if (fence != VK_NULL_HANDLE)
      destroy_fence(device, fence, NULL);

done:
   if (mapped)
      free_mem(device, memory, NULL);
   if (buffer != VK_NULL_HANDLE)
      destroy_buffer(device, buffer, NULL);
   destroy_pool(device, pool, NULL);

   printf("\n=== %d failure(s) ===\n", failures);
   return failures ? 1 : 0;
}
