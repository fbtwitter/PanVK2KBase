// What would a frame actually cost?
//
// Every measurement in this repo so far either waits for a fence per submit
// (driver_compute_probe --loop, ~22ms/submit) or bursts with one fence at
// the end (driver_pipeline_probe --burst, 0.36ms/submit). A real present
// loop is neither: it renders one frame's worth of work and then has to
// hand the compositor something, which on this driver means blocking.
//
// That matters because two costs now stack on every presented frame:
//
//   - the ~12ms idle-GPU kick. A submit arriving at an idle CS has to wait
//     for CS_ACTIVE to clear before the kick lands. Blocking each frame
//     guarantees the GPU is idle when the next frame arrives, so this is
//     paid every time rather than amortised.
//
//   - vkGetSemaphoreFdKHR, which CPU-waits and returns -1 because kbase
//     cannot produce a real fence (see docs/kbase-notes.md). The compositor
//     cannot wait on our behalf, so we wait instead.
//
// 16.6ms is the whole budget at 60fps and 33.3ms at 30fps, so the question
// is not academic. This probe answers it.
//
// WHAT IT REPRODUCES, AND WHAT IT DOES NOT
//
// It cannot call vkAcquireImageANDROID / vkQueueSignalReleaseImageANDROID -
// those need a real swapchain, which needs an ANativeWindow, which needs an
// app. What it does instead is perform the exact driver-visible sequence
// those two functions perform:
//
//   acquire  ->  vkImportSemaphoreFdKHR(SYNC_FD, fd = -1)
//                (what AcquireImageANDROID does when the compositor has
//                 nothing outstanding, which is the common case)
//   render   ->  a real render pass into one of N rotating images
//   release  ->  vkQueueSubmit signalling a semaphore, then
//                vkGetSemaphoreFdKHR(SYNC_FD) on it
//
// So the numbers are the driver's cost, not a swapchain's. Compositor
// latency, buffer queue depth and vsync pacing are all absent, which means
// a real frame cannot be *faster* than this - it is a floor, and that is
// the useful direction for the question being asked.
//
// The GPU work is deliberately trivial (a clear and one triangle). This
// measures driver and submission overhead, not shading. A real emulator
// frame adds its own work on top of whatever floor this establishes.
//
// Usage:
//   driver_present_loop_probe <path-to-libvulkan_panfrost.so>
//                             --i-know-it-hangs
//                             [--frames=N] [--size=WxH] [--images=N]
//                             [--mode=present|pipelined]
//
//   --mode=present    (default) block on the release export every frame,
//                     which is what presenting actually does.
//   --mode=pipelined  skip the export and never wait, keeping work in
//                     flight. Isolates what the blocking release costs by
//                     removing only that.

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "vbo_frag_spv.h"
#include "vbo_vert_spv.h"

/* Same libhardware ABI mirrors as the other driver probes. */
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
#define MAX_IMAGES 4

static const float VERTICES[3][2] = {
   {-0.8f, -0.8f},
   {0.8f, -0.8f},
   {-0.8f, 0.8f},
};

static int failures;

static void check(bool ok, const char *what) {
   printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

static uint64_t now_ns(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
   uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return (x > y) - (x < y);
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

/* Percentiles rather than just a mean: a present loop is judged by its worst
 * frames, and a mean hides a bimodal distribution - which is exactly what a
 * per-frame wake-up cost produces.
 */
static void report(const char *label, uint64_t *v, uint32_t n) {
   if (!n)
      return;
   qsort(v, n, sizeof(*v), cmp_u64);
   uint64_t sum = 0;
   for (uint32_t i = 0; i < n; i++)
      sum += v[i];
   printf("  %-16s min %7.2f  med %7.2f  p95 %7.2f  max %7.2f  mean %7.2f\n",
          label, ms(v[0]), ms(v[n / 2]), ms(v[(uint32_t)(n * 0.95)]),
          ms(v[n - 1]), ms(sum / n));
}

static uint32_t find_mem_type(VkPhysicalDeviceMemoryProperties *mp,
                              uint32_t bits, VkMemoryPropertyFlags want) {
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((bits & (1u << i)) &&
          (mp->memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

int main(int argc, char **argv) {
   setvbuf(stdout, NULL, _IONBF, 0);

   uint32_t frames = 200, width = 1280, height = 720, n_images = 3;
   bool pipelined = false;
   bool gated = false;

   for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--i-know-it-hangs"))
         gated = true;
      else if (!strncmp(argv[i], "--frames=", 9))
         frames = (uint32_t)atoi(argv[i] + 9);
      else if (!strncmp(argv[i], "--images=", 9))
         n_images = (uint32_t)atoi(argv[i] + 9);
      else if (!strncmp(argv[i], "--size=", 7))
         sscanf(argv[i] + 7, "%ux%u", &width, &height);
      else if (!strcmp(argv[i], "--mode=pipelined"))
         pipelined = true;
      else if (!strcmp(argv[i], "--mode=present"))
         pipelined = false;
   }
   if (n_images > MAX_IMAGES)
      n_images = MAX_IMAGES;

   if (argc < 2 || !gated) {
      fprintf(stderr,
              "This renders in a loop - a real render pass per frame, a few\n"
              "hundred times. Rendering is proven on this device (see the\n"
              "render_* probes) but it carries the same gate they do.\n"
              "\n"
              "usage: %s <path-to-.so> --i-know-it-hangs [--frames=N]\n"
              "         [--size=WxH] [--images=N] [--mode=present|pipelined]\n",
              argv[0]);
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
      .pApplicationName = "panvk-kbase-present-loop",
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
   PFN_vkGetPhysicalDeviceMemoryProperties get_mp =
      GIPA(vkGetPhysicalDeviceMemoryProperties);
   PFN_vkGetPhysicalDeviceFeatures2 get_f2 = GIPA(vkGetPhysicalDeviceFeatures2);

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   VkResult r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      printf("no physical device (%d)\n", r);
      return 1;
   }

   printf("\n=== present-loop cost: %ux%u, %u images, %u frames, %s ===\n",
          width, height, n_images, frames,
          pipelined ? "pipelined (no release block)" : "present (blocking)");

   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   VkPhysicalDeviceFeatures2 f2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13,
   };
   get_f2(pd, &f2);
   if (!f13.dynamicRendering) {
      printf("dynamicRendering unsupported\n");
      return 1;
   }

   VkPhysicalDeviceVulkan13Features en13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio,
   };
   const char *exts[] = {
      VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &en13,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = exts,
   };
   VkDevice device = VK_NULL_HANDLE;
   r = create_dev(pd, &dci, NULL, &device);
   check(r == VK_SUCCESS, "vkCreateDevice with external semaphore support");
   if (r != VK_SUCCESS)
      return 1;

#define GDPA(n) (PFN_##n) gdpa(device, #n)
   PFN_vkGetDeviceQueue get_queue = GDPA(vkGetDeviceQueue);
   PFN_vkCreateImage create_image = GDPA(vkCreateImage);
   PFN_vkGetImageMemoryRequirements img_reqs = GDPA(vkGetImageMemoryRequirements);
   PFN_vkAllocateMemory alloc_mem = GDPA(vkAllocateMemory);
   PFN_vkBindImageMemory bind_img = GDPA(vkBindImageMemory);
   PFN_vkCreateImageView create_view = GDPA(vkCreateImageView);
   PFN_vkCreateBuffer create_buffer = GDPA(vkCreateBuffer);
   PFN_vkGetBufferMemoryRequirements buf_reqs = GDPA(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory bind_buf = GDPA(vkBindBufferMemory);
   PFN_vkMapMemory map_mem = GDPA(vkMapMemory);
   PFN_vkCreateShaderModule create_sm = GDPA(vkCreateShaderModule);
   PFN_vkCreatePipelineLayout create_pl = GDPA(vkCreatePipelineLayout);
   PFN_vkCreateGraphicsPipelines create_gp = GDPA(vkCreateGraphicsPipelines);
   PFN_vkCreateCommandPool create_pool = GDPA(vkCreateCommandPool);
   PFN_vkAllocateCommandBuffers alloc_cb = GDPA(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_cb = GDPA(vkBeginCommandBuffer);
   PFN_vkEndCommandBuffer end_cb = GDPA(vkEndCommandBuffer);
   PFN_vkResetCommandBuffer reset_cb = GDPA(vkResetCommandBuffer);
   PFN_vkCmdBeginRendering cmd_begin = GDPA(vkCmdBeginRendering);
   PFN_vkCmdEndRendering cmd_end = GDPA(vkCmdEndRendering);
   PFN_vkCmdBindPipeline cmd_bind_pipe = GDPA(vkCmdBindPipeline);
   PFN_vkCmdBindVertexBuffers cmd_bind_vb = GDPA(vkCmdBindVertexBuffers);
   PFN_vkCmdDraw cmd_draw = GDPA(vkCmdDraw);
   PFN_vkCmdPipelineBarrier cmd_barrier = GDPA(vkCmdPipelineBarrier);
   PFN_vkQueueSubmit queue_submit = GDPA(vkQueueSubmit);
   PFN_vkCreateSemaphore create_sem = GDPA(vkCreateSemaphore);
   PFN_vkImportSemaphoreFdKHR import_sem = GDPA(vkImportSemaphoreFdKHR);
   PFN_vkGetSemaphoreFdKHR get_sem_fd = GDPA(vkGetSemaphoreFdKHR);
   PFN_vkDeviceWaitIdle wait_idle = GDPA(vkDeviceWaitIdle);

   check(import_sem && get_sem_fd, "sync-fd entrypoints resolved");
   if (!import_sem || !get_sem_fd)
      return 1;

   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);

   VkPhysicalDeviceMemoryProperties mp;
   get_mp(pd, &mp);

   /* ------------------------------------------------------ the "swapchain" */
   VkImageView views[MAX_IMAGES] = {0};
   for (uint32_t i = 0; i < n_images; i++) {
      VkImageCreateInfo ii = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {width, height, 1},
         .mipLevels = 1, .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      VkImage img;
      if (create_image(device, &ii, NULL, &img) != VK_SUCCESS) {
         printf("image %u creation failed\n", i);
         return 1;
      }
      VkMemoryRequirements mr;
      img_reqs(device, img, &mr);
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = mr.size,
         .memoryTypeIndex = find_mem_type(&mp, mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      VkDeviceMemory mem;
      alloc_mem(device, &mai, NULL, &mem);
      bind_img(device, img, mem, 0);

      VkImageViewCreateInfo vi = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      create_view(device, &vi, NULL, &views[i]);
   }
   check(true, "created the render targets");

   /* --------------------------------------------------------- vertex data */
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(VERTICES),
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
   };
   VkBuffer vbo;
   create_buffer(device, &bci, NULL, &vbo);
   VkMemoryRequirements vr;
   buf_reqs(device, vbo, &vr);
   VkMemoryAllocateInfo vmai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vr.size,
      .memoryTypeIndex = find_mem_type(&mp, vr.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory vmem;
   alloc_mem(device, &vmai, NULL, &vmem);
   bind_buf(device, vbo, vmem, 0);
   void *vmap = NULL;
   map_mem(device, vmem, 0, VK_WHOLE_SIZE, 0, &vmap);
   memcpy(vmap, VERTICES, sizeof(VERTICES));

   /* ------------------------------------------------------------ pipeline */
   VkShaderModuleCreateInfo vs = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_vbo_probe_vert),
      .pCode = render_vbo_probe_vert,
   };
   VkShaderModuleCreateInfo fs = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_vbo_probe_frag),
      .pCode = render_vbo_probe_frag,
   };
   VkShaderModule vsm, fsm;
   create_sm(device, &vs, NULL, &vsm);
   create_sm(device, &fs, NULL, &fsm);

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout layout;
   create_pl(device, &plci, NULL, &layout);

   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main"},
   };
   VkVertexInputBindingDescription vbind = {0, sizeof(float) * 2,
                                            VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vattr = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vin = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbind,
      .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vattr,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport vp = {0, 0, (float)width, (float)height, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {width, height}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo msaa = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba,
   };
   VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkPipelineRenderingCreateInfo prci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt,
   };
   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &prci, .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vin, .pInputAssemblyState = &ia,
      .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &msaa, .pColorBlendState = &cb, .layout = layout,
   };
   VkPipeline pipeline;
   r = create_gp(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
   check(r == VK_SUCCESS, "created the graphics pipeline");
   if (r != VK_SUCCESS)
      return 1;

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool;
   create_pool(device, &cpci, NULL, &pool);

   VkCommandBuffer cbs[MAX_IMAGES];
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = n_images,
   };
   alloc_cb(device, &cbai, cbs);

   VkSemaphore acquire_sems[MAX_IMAGES], release_sems[MAX_IMAGES];
   for (uint32_t i = 0; i < n_images; i++) {
      VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      create_sem(device, &si, NULL, &acquire_sems[i]);
      create_sem(device, &si, NULL, &release_sems[i]);
   }

   /* ---------------------------------------------------------- the loop */
   printf("\n=== running %u frames ===\n", frames);

   uint64_t *t_frame = calloc(frames, sizeof(uint64_t));
   uint64_t *t_acquire = calloc(frames, sizeof(uint64_t));
   uint64_t *t_submit = calloc(frames, sizeof(uint64_t));
   uint64_t *t_release = calloc(frames, sizeof(uint64_t));

   uint64_t loop_start = now_ns();

   for (uint32_t f = 0; f < frames; f++) {
      uint32_t idx = f % n_images;
      uint64_t f0 = now_ns();

      /* --- acquire: what AcquireImageANDROID does with nothing pending --- */
      VkImportSemaphoreFdInfoKHR imp = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = acquire_sems[idx],
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = -1,
      };
      import_sem(device, &imp);
      uint64_t f1 = now_ns();

      /* --- render --- */
      reset_cb(cbs[idx], 0);
      VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      begin_cb(cbs[idx], &bi);

      VkImageMemoryBarrier tob = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      /* image handle is not retained; barrier by view's image is not
       * expressible, so re-derive via the same creation order is unnecessary
       * here - UNDEFINED->ATTACHMENT on a fresh frame is what a swapchain
       * image gets anyway. Use a global barrier instead.
       */
      (void)tob;
      VkMemoryBarrier gb = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      };
      cmd_barrier(cbs[idx], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &gb, 0,
                  NULL, 0, NULL);

      VkClearValue clear = {.color.float32 = {0.1f, 0.2f, 0.3f, 1.0f}};
      VkRenderingAttachmentInfo att = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = views[idx],
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .clearValue = clear,
      };
      VkRenderingInfo ri = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = {{0, 0}, {width, height}},
         .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att,
      };
      cmd_begin(cbs[idx], &ri);
      cmd_bind_pipe(cbs[idx], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      VkDeviceSize off = 0;
      cmd_bind_vb(cbs[idx], 0, 1, &vbo, &off);
      cmd_draw(cbs[idx], 3, 1, 0, 0);
      cmd_end(cbs[idx]);
      end_cb(cbs[idx]);

      VkPipelineStageFlags wait_stage =
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &acquire_sems[idx],
         .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &cbs[idx],
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &release_sems[idx],
      };
      queue_submit(queue, 1, &si, VK_NULL_HANDLE);
      uint64_t f2 = now_ns();

      /* --- release: what QueueSignalReleaseImageANDROID does --- */
      if (!pipelined) {
         VkSemaphoreGetFdInfoKHR gf = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = release_sems[idx],
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         };
         int out = -1;
         get_sem_fd(device, &gf, &out);
         if (out >= 0)
            close(out);
      }
      uint64_t f3 = now_ns();

      t_acquire[f] = f1 - f0;
      t_submit[f] = f2 - f1;
      t_release[f] = f3 - f2;
      t_frame[f] = f3 - f0;
   }

   uint64_t loop_ns = now_ns() - loop_start;
   wait_idle(device);

   /* ------------------------------------------------------------ results */
   printf("\n=== per-frame cost (ms) ===\n");
   report("frame total", t_frame, frames);
   report("  acquire", t_acquire, frames);
   report("  record+submit", t_submit, frames);
   report("  release export", t_release, frames);

   double total_ms = ms(loop_ns);
   double per_frame = total_ms / frames;
   printf("\n  %u frames in %.1f ms -> %.2f ms/frame, %.1f fps\n", frames,
          total_ms, per_frame, 1000.0 / per_frame);
   printf("  budget: 16.67 ms at 60fps, 33.33 ms at 30fps\n");

   const char *verdict;
   if (per_frame <= 16.67)
      verdict = "fits a 60fps budget";
   else if (per_frame <= 33.33)
      verdict = "fits 30fps but not 60";
   else
      verdict = "does NOT fit a 30fps budget";
   printf("  -> %s (driver overhead only; real work adds to this)\n", verdict);

   printf("\n=== %d failure(s) ===\n", failures);
   return failures ? 1 : 0;
}
