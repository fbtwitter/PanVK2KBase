// Semaphores: creation, a binary chain between two submits, and a timeline
// value moved by the GPU.
//
// Until VK_SYNC_FEATURE_GPU_WAIT was advertised, vkCreateSemaphore returned
// VK_ERROR_FEATURE_NOT_PRESENT on this driver - get_semaphore_sync_type() in
// the Vulkan runtime rejects every sync type without it. That is a hard stop
// for essentially any real application, which is why this probe exists
// separately from driver_pipeline_probe: that one proves work runs, this one
// proves work can be *ordered*.
//
// Three things are checked, in increasing strength:
//
//   1. vkCreateSemaphore succeeds, binary and timeline. The headline - it is
//      what changed - but on its own it only proves a feature bit is set.
//
//   2. Two submits chained by a binary semaphore both complete and both
//      write their own buffer. This is where a broken wait shows up: an
//      error out of vkQueueSubmit, a fence that never signals, or a hang.
//
//   3. A timeline semaphore signalled by the GPU reaches the exact value the
//      submit asked for, observed through vkGetSemaphoreCounterValue. The
//      strongest of the three: a SYNC_SET64 that wrote 1 (the binary
//      convention) instead of the requested value would pass check 2 and
//      fail here.
//
// Deliberately not checked: that submit B's work happens *after* submit A's.
// Submissions are serialised in this driver anyway - a kick only lands on an
// idle CS - so ordering would hold with the semaphore removed entirely, and
// a test that passes for the wrong reason is worse than no test. What can be
// checked honestly is that the semaphore does not break anything, which is
// the actual risk here.
//
// Usage: driver_semaphore_probe <path-to-libvulkan_panfrost.so>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Shared with driver_pipeline_probe - same shader, same geometry. Two
 * multipliers so the two submits write distinguishable results; if the
 * second buffer came back holding the first's pattern, one dispatch ran
 * twice or the descriptor set never changed.
 */
#define LOCAL_SIZE   64
#define ELEM_COUNT   1024
#define WORKGROUPS   (ELEM_COUNT / LOCAL_SIZE)
#define MULTIPLIER_A 7u
#define MULTIPLIER_B 11u
#define SENTINEL     0xdeadbeefu

/* The value the GPU is asked to write into the timeline. Not 1: that is what
 * a binary signal writes, so a driver that ignored signal_value entirely
 * would still look correct at 1.
 */
#define TIMELINE_TARGET 42ull

/* Long enough that a slow first dispatch is not mistaken for a hang, short
 * enough that a real hang is reported rather than waited on.
 */
#define FENCE_TIMEOUT_NS 2000000000ull

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
      .pApplicationName = "panvk-kbase-semaphore-probe",
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
   /* timelineSemaphore has to be asked for explicitly even at API 1.3, and
    * without it vkGetSemaphoreCounterValue is not dispatchable - check 3
    * would fail on a missing entrypoint rather than on driver behaviour.
    */
   VkPhysicalDeviceVulkan12Features vk12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .timelineSemaphore = VK_TRUE,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &vk12,
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
   PFN_vkCreateSemaphore create_sem = GDPA(vkCreateSemaphore);
   PFN_vkDestroySemaphore destroy_sem = GDPA(vkDestroySemaphore);
   PFN_vkGetSemaphoreCounterValue get_sem_value =
      GDPA(vkGetSemaphoreCounterValue);
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
   PFN_vkCmdBindDescriptorSets cmd_bind_dsets = GDPA(vkCmdBindDescriptorSets);
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

   /* ------------------------------------------------------- 1. creation */
   printf("\n=== semaphore creation ===\n");
   VkSemaphoreCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
   };
   VkSemaphore binary_sem = VK_NULL_HANDLE;
   r = create_sem(device, &sci, NULL, &binary_sem);
   printf("  vkCreateSemaphore (binary) -> %d\n", r);
   check(r == VK_SUCCESS, "binary semaphore created");
   if (r != VK_SUCCESS) {
      printf("\n=> no sync type advertises VK_SYNC_FEATURE_GPU_WAIT.\n"
             "   nothing below can run. check `adb logcat -d -s MESA`\n");
      return 1;
   }

   VkSemaphoreTypeCreateInfo stci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0,
   };
   VkSemaphoreCreateInfo tsci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &stci,
   };
   VkSemaphore timeline_sem = VK_NULL_HANDLE;
   r = create_sem(device, &tsci, NULL, &timeline_sem);
   printf("  vkCreateSemaphore (timeline) -> %d\n", r);
   check(r == VK_SUCCESS, "timeline semaphore created");

   /* --------------------------------------------------------- pipeline */
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
   check(r == VK_SUCCESS, "vkCreateComputePipelines");
   if (r != VK_SUCCESS)
      return 1;

   /* -------------------------------------------------------- resources */
   printf("\n=== resources ===\n");
   VkPhysicalDeviceMemoryProperties mem_props;
   get_mem_props(pd, &mem_props);

   VkBuffer buffer[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   VkDeviceMemory memory[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   uint32_t *mapped[2] = { NULL, NULL };

   for (unsigned i = 0; i < 2; i++) {
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = ELEM_COUNT * sizeof(uint32_t),
         .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      r = create_buffer(device, &bci, NULL, &buffer[i]);
      if (r != VK_SUCCESS) {
         check(false, "vkCreateBuffer");
         return 1;
      }

      VkMemoryRequirements reqs;
      get_buf_reqs(device, buffer[i], &reqs);

      const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      uint32_t type_idx = UINT32_MAX;
      for (uint32_t t = 0; t < mem_props.memoryTypeCount; t++) {
         if ((reqs.memoryTypeBits & (1u << t)) &&
             (mem_props.memoryTypes[t].propertyFlags & want) == want) {
            type_idx = t;
            break;
         }
      }
      if (type_idx == UINT32_MAX) {
         check(false, "host-visible memory type found");
         return 1;
      }

      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = reqs.size,
         .memoryTypeIndex = type_idx,
      };
      r = alloc_mem(device, &mai, NULL, &memory[i]);
      if (r == VK_SUCCESS)
         r = bind_buf_mem(device, buffer[i], memory[i], 0);
      if (r == VK_SUCCESS)
         r = map_mem(device, memory[i], 0, VK_WHOLE_SIZE, 0,
                     (void **)&mapped[i]);
      if (r != VK_SUCCESS) {
         check(false, "buffer allocated, bound and mapped");
         return 1;
      }

      /* Sentinel everywhere, including element 0 - whose correct result is
       * 0, so seeding with zero would make a dead dispatch look right there.
       */
      for (uint32_t e = 0; e < ELEM_COUNT; e++)
         mapped[i][e] = SENTINEL;
   }
   check(true, "two buffers allocated, bound, mapped and seeded");

   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 2,
   };
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 2,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   r = create_dpool(device, &dpci, NULL, &dpool);
   check(r == VK_SUCCESS, "vkCreateDescriptorPool");

   VkDescriptorSetLayout set_layouts[2] = { dsl, dsl };
   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool,
      .descriptorSetCount = 2,
      .pSetLayouts = set_layouts,
   };
   VkDescriptorSet dset[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   r = alloc_dsets(device, &dsai, dset);
   check(r == VK_SUCCESS, "vkAllocateDescriptorSets");

   VkDescriptorBufferInfo dbi[2];
   VkWriteDescriptorSet writes[2];
   for (unsigned i = 0; i < 2; i++) {
      dbi[i] = (VkDescriptorBufferInfo){
         .buffer = buffer[i],
         .offset = 0,
         .range = VK_WHOLE_SIZE,
      };
      writes[i] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = dset[i],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &dbi[i],
      };
   }
   update_dsets(device, 2, writes, 0, NULL);

   /* ----------------------------------------------------------- record */
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
      .commandBufferCount = 2,
   };
   VkCommandBuffer cmdbuf[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   r = alloc_cmdbufs(device, &cbai, cmdbuf);
   check(r == VK_SUCCESS, "vkAllocateCommandBuffers");

   const uint32_t multiplier[2] = { MULTIPLIER_A, MULTIPLIER_B };
   for (unsigned i = 0; i < 2; i++) {
      VkCommandBufferBeginInfo cbbi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      r = begin_cmdbuf(cmdbuf[i], &cbbi);
      if (r != VK_SUCCESS) {
         check(false, "vkBeginCommandBuffer");
         return 1;
      }

      cmd_bind_pipeline(cmdbuf[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      cmd_bind_dsets(cmdbuf[i], VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                     &dset[i], 0, NULL);
      cmd_push(cmdbuf[i], layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
               sizeof(multiplier[i]), &multiplier[i]);
      cmd_dispatch(cmdbuf[i], WORKGROUPS, 1, 1);

      r = end_cmdbuf(cmdbuf[i]);
      if (r != VK_SUCCESS) {
         check(false, "vkEndCommandBuffer");
         return 1;
      }
   }
   check(true, "two dispatches recorded");

   /* -------------------------------------------------- 2. binary chain */
   printf("\n=== binary semaphore between two submits ===\n");
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence = VK_NULL_HANDLE;
   r = create_fence(device, &fci, NULL, &fence);
   check(r == VK_SUCCESS, "vkCreateFence");

   /* A signals the semaphore, B waits on it. Submitted in one call so the
    * ordering requirement - a binary semaphore's signal must be submitted
    * before its wait - is met by construction rather than by luck.
    */
   VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
   VkSubmitInfo si[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmdbuf[0],
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &binary_sem,
      },
      {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &binary_sem,
         .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmdbuf[1],
      },
   };

   r = queue_submit(queue, 2, si, fence);
   printf("  vkQueueSubmit (2 batches, chained) -> %d\n", r);
   check(r == VK_SUCCESS, "chained submit accepted");

   if (r == VK_SUCCESS) {
      r = wait_fences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);
      printf("  vkWaitForFences -> %d%s\n", r,
             r == VK_TIMEOUT ? " (TIMEOUT - the GPU did not finish)" : "");
      check(r == VK_SUCCESS, "both batches completed");
   }

   if (r == VK_SUCCESS) {
      for (unsigned i = 0; i < 2; i++) {
         uint32_t bad = 0, first_bad = 0, still_sentinel = 0;

         for (uint32_t e = 0; e < ELEM_COUNT; e++) {
            if (mapped[i][e] == SENTINEL)
               still_sentinel++;
            if (mapped[i][e] != e * multiplier[i]) {
               if (!bad)
                  first_bad = e;
               bad++;
            }
         }

         if (bad) {
            printf("  buffer %c: %u of %u wrong; first at [%u] = 0x%08x, "
                   "expected 0x%08x\n",
                   'A' + i, bad, ELEM_COUNT, first_bad, mapped[i][first_bad],
                   first_bad * multiplier[i]);
            printf("            %u still hold the sentinel (never written)\n",
                   still_sentinel);
         }

         char what[64];
         snprintf(what, sizeof(what), "buffer %c holds i * %u", 'A' + i,
                  multiplier[i]);
         check(bad == 0, what);
      }
   }

   /* ------------------------------------------------------- 3. timeline */
   printf("\n=== timeline semaphore signalled by the GPU ===\n");
   if (timeline_sem == VK_NULL_HANDLE || !get_sem_value) {
      check(false, "timeline semaphore and entrypoint available");
   } else {
      uint64_t before = UINT64_MAX;
      r = get_sem_value(device, timeline_sem, &before);
      printf("  counter before submit: %" PRIu64 " (vkGetSemaphoreCounterValue"
             " -> %d)\n", before, r);
      check(r == VK_SUCCESS && before == 0, "timeline starts at 0");

      /* An empty submit: no command buffers, only the timeline signal. The
       * point is the signal path, and an empty submit isolates it from
       * anything a dispatch might incidentally do.
       */
      const uint64_t target = TIMELINE_TARGET;
      VkTimelineSemaphoreSubmitInfo tssi = {
         .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
         .signalSemaphoreValueCount = 1,
         .pSignalSemaphoreValues = &target,
      };
      VkSubmitInfo tsi = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = &tssi,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &timeline_sem,
      };

      VkFence tfence = VK_NULL_HANDLE;
      r = create_fence(device, &fci, NULL, &tfence);
      check(r == VK_SUCCESS, "vkCreateFence (timeline batch)");

      r = queue_submit(queue, 1, &tsi, tfence);
      printf("  vkQueueSubmit (timeline signal to %" PRIu64 ") -> %d\n",
             target, r);
      check(r == VK_SUCCESS, "timeline-signalling submit accepted");

      if (r == VK_SUCCESS) {
         r = wait_fences(device, 1, &tfence, VK_TRUE, FENCE_TIMEOUT_NS);
         check(r == VK_SUCCESS, "timeline batch completed");
      }

      if (r == VK_SUCCESS) {
         uint64_t after = UINT64_MAX;
         r = get_sem_value(device, timeline_sem, &after);
         printf("  counter after submit:  %" PRIu64 "\n", after);
         check(r == VK_SUCCESS && after == target,
               "GPU wrote the requested timeline value");
         if (r == VK_SUCCESS && after == 1 && target != 1) {
            printf("  => wrote 1, not %" PRIu64 ": the signal took the binary"
                   " path\n", target);
         }
      }

      destroy_fence(device, tfence, NULL);
   }

   /* ---------------------------------------------------------- teardown */
   destroy_fence(device, fence, NULL);
   destroy_pool(device, pool, NULL);
   destroy_dpool(device, dpool, NULL);
   for (unsigned i = 0; i < 2; i++) {
      free_mem(device, memory[i], NULL);
      destroy_buffer(device, buffer[i], NULL);
   }
   destroy_pipeline(device, pipeline, NULL);
   destroy_pl(device, layout, NULL);
   destroy_dsl(device, dsl, NULL);
   destroy_module(device, module, NULL);
   destroy_sem(device, timeline_sem, NULL);
   destroy_sem(device, binary_sem, NULL);

   printf("\n=== %d failure(s) ===\n", failures);
   return failures ? 1 : 0;
}
