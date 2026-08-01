// A real triangle: vertex shader through this driver's compiler, IDVS
// primitive assembly, tiling, and a fragment shader running per-pixel.
//
// tests/render_clear_probe proved render-pass entry/exit works on kbase for
// non-SIMULTANEOUS_USE command buffers - it deliberately recorded zero draw
// calls, to isolate that from rasterization. This is the next, separate
// step: the actual draw. Nothing here has run on this device before -
// vertex shader compilation for a graphics stage, IDVS, IDVS-related
// registers, IDVS-mode IDVS, real IDVS drawing - none of it shares code
// with the compute path driver_pipeline_probe already proved.
//
// Deliberately partial coverage, not a fullscreen triangle. A fullscreen
// triangle (vertices at (-1,-1),(3,-1),(-1,3), the classic "cover the whole
// viewport" trick) cannot distinguish "the GPU rasterized my geometry" from
// "a bug paints every pixel regardless of geometry" - both produce a
// uniformly-coloured image. This triangle covers roughly the lower-left
// half of an 8x8 render target with margin, so a correct result has BOTH
// colours present, and nothing else - not blended edge pixels (MSAA is
// off), not stale memory, not the wrong colour.
//
// No vertex buffers (positions are hardcoded in the shader, indexed by
// gl_VertexIndex), no descriptor sets (the fragment colour is hardcoded
// too), no dynamic state (static viewport/scissor baked into the
// pipeline), no depth/stencil attachment. Everything not under test is
// removed, the same discipline render_clear_probe used.
//
// Mirrors tests/alias_cs_probe's safety convention: unbuffered stdout,
// refuses to run without --i-know-it-hangs. This is materially more novel
// than render_clear_probe - real shader compilation and IDVS, not just
// entering a render pass - so treat a hang here as at least as likely as
// it was there, not less.
//
// Usage: render_triangle_probe <path-to-libvulkan_panfrost.so> --i-know-it-hangs
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "triangle_frag_spv.h"
#include "triangle_vert_spv.h"

/* Same libhardware ABI mirrors as every other driver_*_probe / render_*_probe
 * in this repo - see driver_enum_probe.c for the long note on the LP64
 * `reserved` width.
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

/* Bigger than render_clear_probe's 4x4 - a triangle needs enough pixels
 * that partial coverage is unambiguous rather than an accident of
 * quantization at very low resolution.
 */
#define IMG_W 16
#define IMG_H 16
#define IMG_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define BYTES_PER_PIXEL 4

static const uint8_t CLEAR_RGBA[4] = {0x2a, 0xb3, 0x5c, 0xff};
static const uint8_t TRI_RGBA[4] = {0xff, 0x00, 0xff, 0xff}; /* magenta */

static int failures;

static void
check(bool ok, const char *what)
{
   printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

static uint32_t
find_memory_type(VkPhysicalDeviceMemoryProperties *mem_props,
                 uint32_t type_bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mem_props->memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) &&
          (mem_props->memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   return UINT32_MAX;
}

int
main(int argc, char **argv)
{
   /* Unbuffered, and first: a hang here ends in uninterruptible D state
    * (kill -9 will not reap it), the same failure mode tests/alias_cs_probe
    * hit twice. Block buffering on a pipe discards everything printed so
    * far when that happens. Do not remove.
    */
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 3 || strcmp(argv[2], "--i-know-it-hangs") != 0) {
      fprintf(stderr,
              "This draws a real triangle - vertex shader, IDVS, tiling, a\n"
              "fragment shader - none of which has run on this device in\n"
              "this repo before. tests/render_clear_probe proved entering\n"
              "and leaving a render pass works, but recorded zero draw\n"
              "calls on purpose. This is the draw call. tests/alias_cs_probe\n"
              "hung the kbase context twice on adjacent GPU-fault territory\n"
              "- uninterruptible D state, needs a device reboot - and this\n"
              "is at least as novel a path as that was.\n"
              "\n"
              "Run tests/render_clear_probe first if you have not already.\n"
              "\n"
              "If you really mean it: %s <path-to-.so> --i-know-it-hangs\n",
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
      .pApplicationName = "panvk-kbase-render-triangle-probe",
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
   PFN_vkGetPhysicalDeviceFeatures2 get_features2 =
      GIPA(vkGetPhysicalDeviceFeatures2);

   uint32_t count = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      printf("no physical device (%d)\n", r);
      return 1;
   }

   printf("\n=== device ===\n");

   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features13,
   };
   get_features2(pd, &features2);
   check(features13.dynamicRendering, "dynamicRendering feature supported");
   if (!features13.dynamicRendering)
      return 1;

   VkPhysicalDeviceVulkan13Features enable13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
   };
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &enable13,
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
   PFN_vkCreateImage create_image = GDPA(vkCreateImage);
   PFN_vkDestroyImage destroy_image = GDPA(vkDestroyImage);
   PFN_vkGetImageMemoryRequirements get_img_reqs =
      GDPA(vkGetImageMemoryRequirements);
   PFN_vkAllocateMemory alloc_mem = GDPA(vkAllocateMemory);
   PFN_vkFreeMemory free_mem = GDPA(vkFreeMemory);
   PFN_vkBindImageMemory bind_img_mem = GDPA(vkBindImageMemory);
   PFN_vkCreateImageView create_view = GDPA(vkCreateImageView);
   PFN_vkDestroyImageView destroy_view = GDPA(vkDestroyImageView);
   PFN_vkCreateBuffer create_buffer = GDPA(vkCreateBuffer);
   PFN_vkDestroyBuffer destroy_buffer = GDPA(vkDestroyBuffer);
   PFN_vkGetBufferMemoryRequirements get_buf_reqs =
      GDPA(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory bind_buf_mem = GDPA(vkBindBufferMemory);
   PFN_vkMapMemory map_mem = GDPA(vkMapMemory);
   PFN_vkCreateShaderModule create_module = GDPA(vkCreateShaderModule);
   PFN_vkDestroyShaderModule destroy_module = GDPA(vkDestroyShaderModule);
   PFN_vkCreatePipelineLayout create_pl = GDPA(vkCreatePipelineLayout);
   PFN_vkDestroyPipelineLayout destroy_pl = GDPA(vkDestroyPipelineLayout);
   PFN_vkCreateGraphicsPipelines create_gfx_pipelines =
      GDPA(vkCreateGraphicsPipelines);
   PFN_vkDestroyPipeline destroy_pipeline = GDPA(vkDestroyPipeline);
   PFN_vkCreateCommandPool create_pool = GDPA(vkCreateCommandPool);
   PFN_vkDestroyCommandPool destroy_pool = GDPA(vkDestroyCommandPool);
   PFN_vkAllocateCommandBuffers alloc_cmdbufs = GDPA(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_cmdbuf = GDPA(vkBeginCommandBuffer);
   PFN_vkEndCommandBuffer end_cmdbuf = GDPA(vkEndCommandBuffer);
   PFN_vkCmdPipelineBarrier cmd_barrier = GDPA(vkCmdPipelineBarrier);
   PFN_vkCmdBeginRendering cmd_begin_rendering = GDPA(vkCmdBeginRendering);
   PFN_vkCmdEndRendering cmd_end_rendering = GDPA(vkCmdEndRendering);
   PFN_vkCmdBindPipeline cmd_bind_pipeline = GDPA(vkCmdBindPipeline);
   PFN_vkCmdDraw cmd_draw = GDPA(vkCmdDraw);
   PFN_vkCmdCopyImageToBuffer cmd_copy_img_to_buf =
      GDPA(vkCmdCopyImageToBuffer);
   PFN_vkQueueSubmit queue_submit = GDPA(vkQueueSubmit);
   PFN_vkCreateFence create_fence = GDPA(vkCreateFence);
   PFN_vkDestroyFence destroy_fence = GDPA(vkDestroyFence);
   PFN_vkWaitForFences wait_fences = GDPA(vkWaitForFences);

   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);

   /* --------------------------------------------------------- resources */
   printf("\n=== resources: a %ux%u render target ===\n", IMG_W, IMG_H);

   VkImageCreateInfo ici2 = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = IMG_FORMAT,
      .extent = {IMG_W, IMG_H, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage =
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image = VK_NULL_HANDLE;
   r = create_image(device, &ici2, NULL, &image);
   check(r == VK_SUCCESS, "vkCreateImage (colour attachment)");
   if (r != VK_SUCCESS)
      return 1;

   VkMemoryRequirements img_reqs;
   get_img_reqs(device, image, &img_reqs);

   VkPhysicalDeviceMemoryProperties mem_props;
   get_mem_props(pd, &mem_props);

   uint32_t img_type = find_memory_type(&mem_props, img_reqs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   check(img_type != UINT32_MAX, "device-local memory type found for image");
   if (img_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo img_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = img_reqs.size,
      .memoryTypeIndex = img_type,
   };
   VkDeviceMemory img_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &img_mai, NULL, &img_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (image)");

   r = bind_img_mem(device, image, img_memory, 0);
   check(r == VK_SUCCESS, "vkBindImageMemory");

   VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = IMG_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView view = VK_NULL_HANDLE;
   r = create_view(device, &ivci, NULL, &view);
   check(r == VK_SUCCESS, "vkCreateImageView");

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = IMG_W * IMG_H * BYTES_PER_PIXEL,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer readback = VK_NULL_HANDLE;
   r = create_buffer(device, &bci, NULL, &readback);
   check(r == VK_SUCCESS, "vkCreateBuffer (readback)");

   VkMemoryRequirements buf_reqs;
   get_buf_reqs(device, readback, &buf_reqs);

   const VkMemoryPropertyFlags host_want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t buf_type =
      find_memory_type(&mem_props, buf_reqs.memoryTypeBits, host_want);
   check(buf_type != UINT32_MAX, "host-visible memory type found for readback");
   if (buf_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo buf_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buf_reqs.size,
      .memoryTypeIndex = buf_type,
   };
   VkDeviceMemory buf_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &buf_mai, NULL, &buf_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (readback)");

   r = bind_buf_mem(device, readback, buf_memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory (readback)");

   uint8_t *mapped = NULL;
   r = map_mem(device, buf_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped);
   check(r == VK_SUCCESS, "vkMapMemory (readback)");
   if (r != VK_SUCCESS)
      return 1;

   /* Neither the clear colour nor the triangle colour, so "the copy never
    * ran" is distinguishable from either real outcome.
    */
   memset(mapped, 0x11, IMG_W * IMG_H * BYTES_PER_PIXEL);

   /* --------------------------------------------------------- pipeline */
   printf("\n=== pipeline: vertex + fragment, no vertex buffers, no "
          "descriptors ===\n");

   VkShaderModuleCreateInfo vs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_triangle_probe_vert),
      .pCode = render_triangle_probe_vert,
   };
   VkShaderModule vs_module = VK_NULL_HANDLE;
   r = create_module(device, &vs_smci, NULL, &vs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (vertex)");
   if (r != VK_SUCCESS) {
      printf("\n=> vertex SPIR-V never became a Bifrost binary; check "
             "`adb logcat -d -s MESA`\n");
      return 1;
   }

   VkShaderModuleCreateInfo fs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_triangle_probe_frag),
      .pCode = render_triangle_probe_frag,
   };
   VkShaderModule fs_module = VK_NULL_HANDLE;
   r = create_module(device, &fs_smci, NULL, &fs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (fragment)");
   if (r != VK_SUCCESS) {
      printf("\n=> fragment SPIR-V never became a Bifrost binary; check "
             "`adb logcat -d -s MESA`\n");
      return 1;
   }

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout layout = VK_NULL_HANDLE;
   r = create_pl(device, &plci, NULL, &layout);
   check(r == VK_SUCCESS, "vkCreatePipelineLayout (empty)");

   VkPipelineShaderStageCreateInfo stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs_module,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs_module,
         .pName = "main",
      },
   };

   VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };

   VkViewport viewport = {
      .x = 0, .y = 0, .width = IMG_W, .height = IMG_H,
      .minDepth = 0.0f, .maxDepth = 1.0f,
   };
   VkRect2D scissor = {.offset = {0, 0}, .extent = {IMG_W, IMG_H}};
   VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };

   VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };

   VkPipelineColorBlendAttachmentState blend_attachment = {
      .blendEnable = VK_FALSE,
      .colorWriteMask =
         VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };

   VkFormat color_format = IMG_FORMAT;
   VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &color_format,
   };

   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_create_info,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .layout = layout,
   };
   VkPipeline pipeline = VK_NULL_HANDLE;
   r = create_gfx_pipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
   printf("  vkCreateGraphicsPipelines -> %d\n", r);
   check(r == VK_SUCCESS, "vkCreateGraphicsPipelines");
   if (r != VK_SUCCESS) {
      printf("\n=> the pipeline never became runnable; nothing below can "
             "run. check `adb logcat -d -s MESA`\n");
      return 1;
   }

   /* ------------------------------------------------------------ record */
   printf("\n=== record: draw one triangle, partial coverage ===\n");

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   r = create_pool(device, &cpci, NULL, &pool);
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

   /* Deliberately NOT SIMULTANEOUS_USE - render_clear_probe already
    * established that is the boundary the submit gate now checks.
    */
   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = begin_cmdbuf(cmdbuf, &cbbi);
   check(r == VK_SUCCESS, "vkBeginCommandBuffer");

   VkImageMemoryBarrier to_color_attachment = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
              NULL, 1, &to_color_attachment);

   VkClearValue clear_value = {
      .color.float32 = {CLEAR_RGBA[0] / 255.0f, CLEAR_RGBA[1] / 255.0f,
                        CLEAR_RGBA[2] / 255.0f, CLEAR_RGBA[3] / 255.0f},
   };
   VkRenderingAttachmentInfo color_attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = clear_value,
   };
   VkRenderingInfo rendering_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.offset = {0, 0}, .extent = {IMG_W, IMG_H}},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
   };

   cmd_begin_rendering(cmdbuf, &rendering_info);
   printf("  vkCmdBeginRendering recorded\n");

   /* The operation under test. Everything else in this file is the same
    * scaffolding render_clear_probe already proved.
    */
   cmd_bind_pipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   cmd_draw(cmdbuf, 3, 1, 0, 0);
   printf("  vkCmdDraw(3, 1, 0, 0) recorded - one triangle, no vertex "
          "buffers\n");

   cmd_end_rendering(cmdbuf);
   printf("  vkCmdEndRendering recorded\n");

   VkImageMemoryBarrier to_transfer_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
              &to_transfer_src);

   VkBufferImageCopy region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1},
      .imageExtent = {IMG_W, IMG_H, 1},
   };
   cmd_copy_img_to_buf(cmdbuf, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       readback, 1, &region);
   printf("  vkCmdCopyImageToBuffer recorded (readback)\n");

   r = end_cmdbuf(cmdbuf);
   check(r == VK_SUCCESS, "vkEndCommandBuffer");

   /* ----------------------------------------------------------- submit */
   printf("\n=== submit ===\n");

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
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
   check(r == VK_SUCCESS, "vkQueueSubmit accepted");

   if (r == VK_SUCCESS) {
      r = wait_fences(device, 1, &fence, VK_TRUE, 5000000000ull);
      printf("  vkWaitForFences -> %d%s\n", r,
             r == VK_TIMEOUT ? " (TIMEOUT - the GPU did not finish)" : "");
      check(r == VK_SUCCESS, "fence signalled by the GPU");
   }

   /* --------------------------------------------------------- readback */
   if (r == VK_SUCCESS) {
      printf("\n=== readback ===\n");
      uint32_t clear_px = 0, tri_px = 0, other_px = 0;
      uint32_t first_other = 0;

      for (uint32_t px = 0; px < IMG_W * IMG_H; px++) {
         const uint8_t *p = mapped + px * BYTES_PER_PIXEL;
         if (memcmp(p, CLEAR_RGBA, BYTES_PER_PIXEL) == 0)
            clear_px++;
         else if (memcmp(p, TRI_RGBA, BYTES_PER_PIXEL) == 0)
            tri_px++;
         else {
            if (!other_px)
               first_other = px;
            other_px++;
         }
      }

      printf("  %u clear-colour, %u triangle-colour, %u other (of %u "
             "total)\n",
             clear_px, tri_px, other_px, IMG_W * IMG_H);
      if (other_px) {
         const uint8_t *p = mapped + first_other * BYTES_PER_PIXEL;
         printf("  first unexpected pixel [%u] = %02x%02x%02x%02x\n",
                first_other, p[0], p[1], p[2], p[3]);
      }

      check(clear_px > 0, "some pixels are still the clear colour "
                          "(not everything painted over)");
      check(tri_px > 0, "some pixels are the triangle colour "
                        "(the draw actually rasterized something)");
      check(other_px == 0, "no pixel is anything other than clear or "
                           "triangle colour");
   }

   destroy_fence(device, fence, NULL);
   destroy_pool(device, pool, NULL);
   destroy_buffer(device, readback, NULL);
   free_mem(device, buf_memory, NULL);
   destroy_view(device, view, NULL);
   destroy_image(device, image, NULL);
   free_mem(device, img_memory, NULL);
   destroy_pipeline(device, pipeline, NULL);
   destroy_pl(device, layout, NULL);
   destroy_module(device, fs_module, NULL);
   destroy_module(device, vs_module, NULL);

   printf("\n=== %d failure(s) ===\n", failures);
   if (failures == 0)
      printf("\n=> A real triangle rasterized correctly on kbase: vertex\n"
             "   shader, IDVS, tiling, and a fragment shader all ran and\n"
             "   produced the expected partial-coverage result.\n");
   return failures ? 1 : 0;
}
