// A real compute pipeline, from application SPIR-V.
//
// tests/driver_compute_probe --fill got a shader running, but through
// vkCmdFillBuffer, which uses PanVK's own *precompiled* shaders. That leaves
// the whole application-facing path untouched: SPIR-V -> NIR -> Bifrost in
// this driver's compiler, descriptor set layouts, descriptor pools and sets,
// pipeline layouts, push constants, and vkCmdDispatch.
//
// This exercises that path. The shader (shader.comp, embedded as SPIR-V in
// shader_spv.h) writes `gl_GlobalInvocationID.x * multiplier` to a storage
// buffer, with the multiplier arriving as a push constant. A buffer holding
// i * MULTIPLIER for every i can only have been written by a dispatch that
// read its descriptor set, read its push constants, and knew its invocation
// id - none of which a zeroed or stale buffer would produce.
//
// The buffer is seeded with a sentinel first, so "all zeroes" cannot pass:
// element 0 is legitimately 0, and only checking the rest would let a
// completely dead dispatch through if MULTIPLIER were ever 0.
//
// Usage: driver_pipeline_probe <path-to-libvulkan_panfrost.so>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>

#include "shader_spv.h"

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

/* Must match local_size_x in shader.comp. */
#define LOCAL_SIZE  64
#define ELEM_COUNT  1024
#define WORKGROUPS  (ELEM_COUNT / LOCAL_SIZE)
#define MULTIPLIER  7u
#define SENTINEL    0xdeadbeefu

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
   if (argc < 2 || argc > 3) {
      fprintf(stderr,
              "usage: %s <path-to-libvulkan_panfrost.so> [--burst=N]\n",
              argv[0]);
      return 2;
   }

   /* Optional: after the correctness check, submit N times back to back and
    * wait once. Off by default because it measures rather than verifies.
    */
   uint32_t burst_count = 0;
   if (argc == 3) {
      if (strncmp(argv[2], "--burst=", 8)) {
         fprintf(stderr, "unknown option: %s\n", argv[2]);
         return 2;
      }
      burst_count = (uint32_t)strtoul(argv[2] + 8, NULL, 0);
   }

   /* Unbuffered - a submit path that hangs is a failure mode worth
    * diagnosing, and block buffering on a pipe discards everything printed
    * so far when a hung run is killed.
    */
   setvbuf(stdout, NULL, _IONBF, 0);

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
      .pApplicationName = "panvk-kbase-pipeline-probe",
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
   if (r != VK_SUCCESS)
      return 1;

#define GDPA(name) (PFN_##name) gdpa(device, #name)
   PFN_vkGetDeviceQueue get_queue = GDPA(vkGetDeviceQueue);
   PFN_vkCreateShaderModule create_module = GDPA(vkCreateShaderModule);
   PFN_vkDestroyShaderModule destroy_module = GDPA(vkDestroyShaderModule);
   PFN_vkCreateDescriptorSetLayout create_dsl =
      GDPA(vkCreateDescriptorSetLayout);
   PFN_vkDestroyDescriptorSetLayout destroy_dsl =
      GDPA(vkDestroyDescriptorSetLayout);
   PFN_vkCreatePipelineLayout create_pl = GDPA(vkCreatePipelineLayout);
   PFN_vkDestroyPipelineLayout destroy_pl = GDPA(vkDestroyPipelineLayout);
   PFN_vkCreateComputePipelines create_pipelines =
      GDPA(vkCreateComputePipelines);
   PFN_vkDestroyPipeline destroy_pipeline = GDPA(vkDestroyPipeline);
   PFN_vkCreateDescriptorPool create_dpool = GDPA(vkCreateDescriptorPool);
   PFN_vkDestroyDescriptorPool destroy_dpool = GDPA(vkDestroyDescriptorPool);
   PFN_vkAllocateDescriptorSets alloc_dsets = GDPA(vkAllocateDescriptorSets);
   PFN_vkUpdateDescriptorSets update_dsets = GDPA(vkUpdateDescriptorSets);
   PFN_vkCreateCommandPool create_pool = GDPA(vkCreateCommandPool);
   PFN_vkDestroyCommandPool destroy_pool = GDPA(vkDestroyCommandPool);
   PFN_vkAllocateCommandBuffers alloc_cmdbufs = GDPA(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_cmdbuf = GDPA(vkBeginCommandBuffer);
   PFN_vkEndCommandBuffer end_cmdbuf = GDPA(vkEndCommandBuffer);
   PFN_vkCmdBindPipeline cmd_bind_pipeline = GDPA(vkCmdBindPipeline);
   PFN_vkCmdBindDescriptorSets cmd_bind_dsets =
      GDPA(vkCmdBindDescriptorSets);
   PFN_vkCmdPushConstants cmd_push = GDPA(vkCmdPushConstants);
   PFN_vkCmdDispatch cmd_dispatch = GDPA(vkCmdDispatch);
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

   /* ------------------------------------------------------------ shader */
   printf("\n=== pipeline ===\n");
   VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(pipeline_probe_shader),
      .pCode = pipeline_probe_shader,
   };
   VkShaderModule module = VK_NULL_HANDLE;
   r = create_module(device, &smci, NULL, &module);
   check(r == VK_SUCCESS, "vkCreateShaderModule");
   if (r != VK_SUCCESS)
      return 1;

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
   r = create_dsl(device, &dslci, NULL, &dsl);
   check(r == VK_SUCCESS, "vkCreateDescriptorSetLayout");

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
   r = create_pl(device, &plci, NULL, &layout);
   check(r == VK_SUCCESS, "vkCreatePipelineLayout");

   /* The interesting one: this is where SPIR-V goes through this driver's
    * NIR and Bifrost compiler for the first time in this port.
    */
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
   VkPipeline pipeline = VK_NULL_HANDLE;
   r = create_pipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline);
   printf("  vkCreateComputePipelines -> %d\n", r);
   check(r == VK_SUCCESS, "vkCreateComputePipelines");
   if (r != VK_SUCCESS) {
      printf("\n=> SPIR-V never became a Bifrost binary; nothing below can\n"
             "   run. check `adb logcat -d -s MESA`\n");
      return 1;
   }

   /* ------------------------------------------------------------ buffer */
   printf("\n=== resources ===\n");
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = ELEM_COUNT * sizeof(uint32_t),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer = VK_NULL_HANDLE;
   r = create_buffer(device, &bci, NULL, &buffer);
   check(r == VK_SUCCESS, "vkCreateBuffer");

   VkMemoryRequirements reqs;
   get_buf_reqs(device, buffer, &reqs);

   VkPhysicalDeviceMemoryProperties mem_props;
   get_mem_props(pd, &mem_props);

   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t type_idx = UINT32_MAX;
   for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & want) == want) {
         type_idx = i;
         break;
      }
   }
   check(type_idx != UINT32_MAX, "host-visible memory type found");
   if (type_idx == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = type_idx,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &mai, NULL, &memory);
   check(r == VK_SUCCESS, "vkAllocateMemory");

   r = bind_buf_mem(device, buffer, memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory");

   uint32_t *mapped = NULL;
   r = map_mem(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped);
   check(r == VK_SUCCESS, "vkMapMemory");
   if (r != VK_SUCCESS)
      return 1;

   /* Sentinel everywhere, including element 0 - whose correct result is 0,
    * so seeding with zero would make a completely dead dispatch look right
    * there.
    */
   for (uint32_t i = 0; i < ELEM_COUNT; i++)
      mapped[i] = SENTINEL;

   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
   };
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   r = create_dpool(device, &dpci, NULL, &dpool);
   check(r == VK_SUCCESS, "vkCreateDescriptorPool");

   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet dset = VK_NULL_HANDLE;
   r = alloc_dsets(device, &dsai, &dset);
   check(r == VK_SUCCESS, "vkAllocateDescriptorSets");

   VkDescriptorBufferInfo dbi = {
      .buffer = buffer,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = dset,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &dbi,
   };
   update_dsets(device, 1, &write, 0, NULL);
   printf("  vkUpdateDescriptorSets returned\n");

   /* ----------------------------------------------------------- record */
   printf("\n=== record and dispatch ===\n");
   VkCommandPoolCreateInfo cpci2 = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   r = create_pool(device, &cpci2, NULL, &pool);
   check(r == VK_SUCCESS, "vkCreateCommandPool");

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
   r = alloc_cmdbufs(device, &cbai, &cmdbuf);
   check(r == VK_SUCCESS, "vkAllocateCommandBuffers");

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = begin_cmdbuf(cmdbuf, &cbbi);
   check(r == VK_SUCCESS, "vkBeginCommandBuffer");

   cmd_bind_pipeline(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   cmd_bind_dsets(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset,
                  0, NULL);
   const uint32_t multiplier = MULTIPLIER;
   cmd_push(cmdbuf, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(multiplier),
            &multiplier);
   cmd_dispatch(cmdbuf, WORKGROUPS, 1, 1);
   printf("  recorded vkCmdDispatch(%u, 1, 1)\n", WORKGROUPS);

   r = end_cmdbuf(cmdbuf);
   check(r == VK_SUCCESS, "vkEndCommandBuffer");

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence = VK_NULL_HANDLE;
   r = create_fence(device, &fci, NULL, &fence);
   check(r == VK_SUCCESS, "vkCreateFence");

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmdbuf,
   };
   r = queue_submit(queue, 1, &si, fence);
   printf("  vkQueueSubmit -> %d\n", r);
   check(r == VK_SUCCESS, "vkQueueSubmit");

   if (r == VK_SUCCESS) {
      r = wait_fences(device, 1, &fence, VK_TRUE, 2000000000ull);
      printf("  vkWaitForFences -> %d%s\n", r,
             r == VK_TIMEOUT ? " (TIMEOUT - the GPU did not finish)" : "");
      check(r == VK_SUCCESS, "fence signalled by the GPU");
   }

   /* --------------------------------------------------------- readback */
   if (r == VK_SUCCESS) {
      printf("\n=== readback ===\n");
      uint32_t bad = 0, first_bad = 0, still_sentinel = 0;

      for (uint32_t i = 0; i < ELEM_COUNT; i++) {
         if (mapped[i] == SENTINEL)
            still_sentinel++;
         if (mapped[i] != i * MULTIPLIER) {
            if (!bad)
               first_bad = i;
            bad++;
         }
      }

      if (bad) {
         printf("  %u of %u wrong; first at [%u] = 0x%08x, expected 0x%08x\n",
                bad, ELEM_COUNT, first_bad, mapped[first_bad],
                first_bad * MULTIPLIER);
         printf("  %u still hold the sentinel (never written)\n",
                still_sentinel);
      } else {
         printf("  all %u elements hold i * %u\n", ELEM_COUNT, MULTIPLIER);
      }
      check(bad == 0, "dispatch wrote i * multiplier everywhere");
   }

   /* ------------------------------------------------------------- burst */
   /* Submissions back to back, with no wait in between, which is the only
    * shape that can show whether submits overlap. Every other probe here
    * waits for a fence per submit, so the CS is always idle by the next one
    * and the queue is serialised by the application rather than the driver.
    *
    * N separate vkQueueSubmit calls, not one call with N batches. Batches
    * within a single call get merged by the runtime into one driver submit,
    * which is a different thing entirely - it produces one ring stream with
    * N CALLs in it and a single kick, and it silently blows past
    * PANVK_KBASE_MAX_CALLS_PER_SUBQUEUE once N > 32.
    */
   if (r == VK_SUCCESS && burst_count > 0) {
      printf("\n=== burst of %u submits, one wait at the end ===\n",
             burst_count);

      /* A second command buffer, recorded *without* ONE_TIME_SUBMIT so it
       * can legally be submitted repeatedly.
       */
      VkCommandBuffer burst_cmdbuf = VK_NULL_HANDLE;
      VkCommandBufferAllocateInfo bcbai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      r = alloc_cmdbufs(device, &bcbai, &burst_cmdbuf);
      check(r == VK_SUCCESS, "vkAllocateCommandBuffers (burst)");

      /* SIMULTANEOUS_USE, because the same command buffer is deliberately in
       * flight several times at once - that is the point of the burst.
       */
      VkCommandBufferBeginInfo bcbbi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
      };
      begin_cmdbuf(burst_cmdbuf, &bcbbi);
      cmd_bind_pipeline(burst_cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipeline);
      cmd_bind_dsets(burst_cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                     1, &dset, 0, NULL);
      cmd_push(burst_cmdbuf, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
               sizeof(multiplier), &multiplier);
      cmd_dispatch(burst_cmdbuf, WORKGROUPS, 1, 1);
      r = end_cmdbuf(burst_cmdbuf);
      check(r == VK_SUCCESS, "vkEndCommandBuffer (burst)");

      VkSubmitInfo bsi = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &burst_cmdbuf,
      };

      VkFence bfence = VK_NULL_HANDLE;
      create_fence(device, &fci, NULL, &bfence);

      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);

      /* Fence only on the last one, so nothing waits in between. */
      for (uint32_t i = 0; i < burst_count && r == VK_SUCCESS; i++)
         r = queue_submit(queue, 1, &bsi,
                          i + 1 == burst_count ? bfence : VK_NULL_HANDLE);

      clock_gettime(CLOCK_MONOTONIC, &t1);
      double submit_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                         (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
      check(r == VK_SUCCESS, "burst submitted");

      if (r == VK_SUCCESS) {
         r = wait_fences(device, 1, &bfence, VK_TRUE, 60000000000ull);
         clock_gettime(CLOCK_MONOTONIC, &t1);

         double total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                           (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
         printf("  %u submits issued in %.1f ms (%.2f ms each)\n",
                burst_count, submit_ms, submit_ms / burst_count);
         printf("  all complete after %.1f ms (%.2f ms each)\n", total_ms,
                total_ms / burst_count);
         check(r == VK_SUCCESS, "burst completed");
      }

      destroy_fence(device, bfence, NULL);
   }

   destroy_fence(device, fence, NULL);
   destroy_pool(device, pool, NULL);
   destroy_dpool(device, dpool, NULL);
   free_mem(device, memory, NULL);
   destroy_buffer(device, buffer, NULL);
   destroy_pipeline(device, pipeline, NULL);
   destroy_pl(device, layout, NULL);
   destroy_dsl(device, dsl, NULL);
   destroy_module(device, module, NULL);

   printf("\n=== %d failure(s) ===\n", failures);
   return failures ? 1 : 0;
}
