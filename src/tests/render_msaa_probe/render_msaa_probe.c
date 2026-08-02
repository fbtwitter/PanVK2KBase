// Flagged in ROADMAP.md's Phase 5 (2026-08-02): every render probe up to
// this one is single-sample, so none of them exercise multisampling or
// resolve at all. That gap got sharper once the vendored Mesa clone
// (third_party/MESA-KMOD) was confirmed to already contain PanVK's
// rewritten framebuffer abstraction (src/panfrost/lib/pan_fb.{c,h}:
// pan_fb_layout/pan_fb_load/pan_fb_store/pan_fb_resolves/pan_fb_desc_info -
// see Collabora's write-up of MR mesa/mesa!39759) - the rewrite's headline
// feature is doing MSAA resolve entirely in tile memory when the
// multisampled attachment uses VK_ATTACHMENT_STORE_OP_NONE, which is
// exactly the path this probe exercises and nothing here had touched
// before.
//
// One variable changed from tests/render_vbo_probe, which proved a plain
// colour-only draw with a real vertex buffer: the colour attachment is now
// multisampled (VK_SAMPLE_COUNT_2/4_BIT, whichever the device's
// framebufferColorSampleCounts actually supports), storeOp is NONE on the
// multisampled attachment, and a second, single-sample "resolve" image is
// what actually gets read back - same shaders, same geometry, same clear
// and triangle colours as every earlier probe.
//
// What "correct" looks like is NOT the same 190/66/0 split every prior
// probe got. Antialiasing is the point of MSAA: pixels the triangle's
// diagonal edge only partially covers should resolve to a colour BETWEEN
// the clear and triangle colours, not snap to one or the other the way a
// single centre-sample test does. So this probe classifies every pixel
// into three buckets - exact clear, exact triangle, or a valid AVERAGE
// blend of the two (each channel between the two reference values,
// alpha exactly 0xff) - and the actual pass condition is that blended
// edge pixels exist and nothing falls outside all three buckets. A
// resolve that silently no-ops (e.g. quietly falling back to sample 0)
// would reproduce the old exact 190/66/0 split with zero blended pixels,
// which is exactly what this probe would catch as a failure.
//
// Same 16x16 target, same partial-coverage triangle, same safety
// convention: unbuffered stdout, refuses to run without
// --i-know-it-hangs.
//
// Usage: render_msaa_probe <path-to-libvulkan_panfrost.so> --i-know-it-hangs
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "msaa_frag_spv.h"
#include "msaa_vert_spv.h"

/* Same libhardware ABI mirrors as every other probe in this repo - see
 * driver_enum_probe.c for the long note on the LP64 `reserved` width.
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

#define IMG_W 16
#define IMG_H 16
#define IMG_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define BYTES_PER_PIXEL 4

static const uint8_t CLEAR_RGBA[4] = {0x2a, 0xb3, 0x5c, 0xff};
static const uint8_t TRI_RGBA[4] = {0xff, 0x00, 0xff, 0xff};

static const float VERTICES[3][2] = {
   {-0.8f, -0.8f},
   {0.8f, -0.8f},
   {-0.8f, 0.8f},
};

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

/* A resolved AVERAGE blend of two known reference colours must land
 * componentwise between them (inclusive - a sample set that's mostly one
 * colour can still average to very close to it). Order-independent since
 * clear/tri aren't sorted per channel. */
static bool
is_valid_blend(const uint8_t *p, const uint8_t *a, const uint8_t *b)
{
   for (int c = 0; c < 3; c++) {
      uint8_t lo = a[c] < b[c] ? a[c] : b[c];
      uint8_t hi = a[c] < b[c] ? b[c] : a[c];
      if (p[c] < lo || p[c] > hi)
         return false;
   }
   /* Both reference colours are fully opaque, and AVERAGE-resolving
    * identical alpha values must reproduce that same value exactly. */
   return p[3] == 0xff;
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 3 || strcmp(argv[2], "--i-know-it-hangs") != 0) {
      fprintf(stderr,
              "This draws the same triangle tests/render_vbo_probe already\n"
              "proved, but the colour attachment is multisampled and\n"
              "resolves to a second, single-sample image - one new\n"
              "variable, and a genuinely different hardware path (the\n"
              "resolve unit, not just more descriptor plumbing). Nothing\n"
              "about multisampling or resolve has run on this device\n"
              "before. tests/render_texture_probe's first attempt at a\n"
              "different new mechanism failed silently (wrong data, no\n"
              "error, no hang) before being fixed - see\n"
              "docs/kbase-notes.md. tests/alias_cs_probe hung the kbase\n"
              "context twice on adjacent GPU-fault territory -\n"
              "uninterruptible D state, needs a device reboot.\n"
              "\n"
              "Run tests/render_vbo_probe first if you have not already.\n"
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
      .pApplicationName = "panvk-kbase-render-msaa-probe",
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
   PFN_vkGetPhysicalDeviceProperties get_props =
      GIPA(vkGetPhysicalDeviceProperties);
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

   VkPhysicalDeviceProperties props;
   get_props(pd, &props);
   VkSampleCountFlags color_counts = props.limits.framebufferColorSampleCounts;
   VkSampleCountFlagBits samples;
   if (color_counts & VK_SAMPLE_COUNT_4_BIT)
      samples = VK_SAMPLE_COUNT_4_BIT;
   else if (color_counts & VK_SAMPLE_COUNT_2_BIT)
      samples = VK_SAMPLE_COUNT_2_BIT;
   else
      samples = VK_SAMPLE_COUNT_1_BIT;
   check(samples != VK_SAMPLE_COUNT_1_BIT,
         "framebufferColorSampleCounts offers 2x or 4x MSAA");
   if (samples == VK_SAMPLE_COUNT_1_BIT) {
      printf("  framebufferColorSampleCounts = 0x%x - nothing above 1x to "
             "test\n",
             color_counts);
      return 1;
   }
   printf("  testing at %ux MSAA\n", samples == VK_SAMPLE_COUNT_4_BIT ? 4 : 2);

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
   PFN_vkCmdBindVertexBuffers cmd_bind_vbos = GDPA(vkCmdBindVertexBuffers);
   PFN_vkCmdDraw cmd_draw = GDPA(vkCmdDraw);
   PFN_vkCmdCopyImageToBuffer cmd_copy_img_to_buf =
      GDPA(vkCmdCopyImageToBuffer);
   PFN_vkQueueSubmit queue_submit = GDPA(vkQueueSubmit);
   PFN_vkCreateFence create_fence = GDPA(vkCreateFence);
   PFN_vkDestroyFence destroy_fence = GDPA(vkDestroyFence);
   PFN_vkWaitForFences wait_fences = GDPA(vkWaitForFences);

   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);

   VkPhysicalDeviceMemoryProperties mem_props;
   get_mem_props(pd, &mem_props);
   const VkMemoryPropertyFlags host_want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

   /* -------------------------------------------------------- vertex buffer */
   printf("\n=== vertex buffer (same as every earlier probe) ===\n");

   VkBufferCreateInfo vbo_bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(VERTICES),
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer vbo = VK_NULL_HANDLE;
   r = create_buffer(device, &vbo_bci, NULL, &vbo);
   check(r == VK_SUCCESS, "vkCreateBuffer (vertex buffer)");

   VkMemoryRequirements vbo_reqs;
   get_buf_reqs(device, vbo, &vbo_reqs);
   uint32_t vbo_type =
      find_memory_type(&mem_props, vbo_reqs.memoryTypeBits, host_want);
   check(vbo_type != UINT32_MAX, "host-visible memory type found for VBO");
   if (vbo_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo vbo_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vbo_reqs.size,
      .memoryTypeIndex = vbo_type,
   };
   VkDeviceMemory vbo_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &vbo_mai, NULL, &vbo_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (VBO)");
   r = bind_buf_mem(device, vbo, vbo_memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory (VBO)");

   void *vbo_mapped = NULL;
   r = map_mem(device, vbo_memory, 0, VK_WHOLE_SIZE, 0, &vbo_mapped);
   check(r == VK_SUCCESS, "vkMapMemory (VBO)");
   if (r != VK_SUCCESS)
      return 1;
   memcpy(vbo_mapped, VERTICES, sizeof(VERTICES));

   /* -------------------------------------------------- multisample target */
   printf("\n=== resources: %ux MSAA colour + single-sample resolve "
          "target ===\n",
          samples == VK_SAMPLE_COUNT_4_BIT ? 4 : 2);

   VkImageCreateInfo msaa_ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = IMG_FORMAT,
      .extent = {IMG_W, IMG_H, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage msaa_image = VK_NULL_HANDLE;
   r = create_image(device, &msaa_ici, NULL, &msaa_image);
   check(r == VK_SUCCESS, "vkCreateImage (multisampled colour)");
   if (r != VK_SUCCESS)
      return 1;

   VkMemoryRequirements msaa_reqs;
   get_img_reqs(device, msaa_image, &msaa_reqs);
   uint32_t msaa_type = find_memory_type(&mem_props, msaa_reqs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   check(msaa_type != UINT32_MAX, "device-local memory type found for "
                                  "multisampled image");
   if (msaa_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo msaa_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = msaa_reqs.size,
      .memoryTypeIndex = msaa_type,
   };
   VkDeviceMemory msaa_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &msaa_mai, NULL, &msaa_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (multisampled colour)");
   r = bind_img_mem(device, msaa_image, msaa_memory, 0);
   check(r == VK_SUCCESS, "vkBindImageMemory (multisampled colour)");

   VkImageViewCreateInfo msaa_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = msaa_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = IMG_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView msaa_view = VK_NULL_HANDLE;
   r = create_view(device, &msaa_ivci, NULL, &msaa_view);
   check(r == VK_SUCCESS, "vkCreateImageView (multisampled colour)");

   /* ------------------------------------------------------ resolve target */
   VkImageCreateInfo resolve_ici = {
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
   VkImage resolve_image = VK_NULL_HANDLE;
   r = create_image(device, &resolve_ici, NULL, &resolve_image);
   check(r == VK_SUCCESS, "vkCreateImage (resolve target)");
   if (r != VK_SUCCESS)
      return 1;

   VkMemoryRequirements resolve_reqs;
   get_img_reqs(device, resolve_image, &resolve_reqs);
   uint32_t resolve_type =
      find_memory_type(&mem_props, resolve_reqs.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   check(resolve_type != UINT32_MAX, "device-local memory type found for "
                                     "resolve image");
   if (resolve_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo resolve_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = resolve_reqs.size,
      .memoryTypeIndex = resolve_type,
   };
   VkDeviceMemory resolve_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &resolve_mai, NULL, &resolve_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (resolve target)");
   r = bind_img_mem(device, resolve_image, resolve_memory, 0);
   check(r == VK_SUCCESS, "vkBindImageMemory (resolve target)");

   VkImageViewCreateInfo resolve_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = resolve_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = IMG_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView resolve_view = VK_NULL_HANDLE;
   r = create_view(device, &resolve_ivci, NULL, &resolve_view);
   check(r == VK_SUCCESS, "vkCreateImageView (resolve target)");

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
   uint32_t buf_type =
      find_memory_type(&mem_props, buf_reqs.memoryTypeBits, host_want);
   check(buf_type != UINT32_MAX, "host-visible memory type found for "
                                 "readback");
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
   memset(mapped, 0x11, IMG_W * IMG_H * BYTES_PER_PIXEL);

   /* --------------------------------------------------------- pipeline */
   printf("\n=== pipeline: rasterizationSamples = %ux ===\n",
          samples == VK_SAMPLE_COUNT_4_BIT ? 4 : 2);

   VkShaderModuleCreateInfo vs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_msaa_probe_vert),
      .pCode = render_msaa_probe_vert,
   };
   VkShaderModule vs_module = VK_NULL_HANDLE;
   r = create_module(device, &vs_smci, NULL, &vs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (vertex)");
   if (r != VK_SUCCESS)
      return 1;

   VkShaderModuleCreateInfo fs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_msaa_probe_frag),
      .pCode = render_msaa_probe_frag,
   };
   VkShaderModule fs_module = VK_NULL_HANDLE;
   r = create_module(device, &fs_smci, NULL, &fs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (fragment)");
   if (r != VK_SUCCESS)
      return 1;

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

   VkVertexInputBindingDescription vbo_binding = {
      .binding = 0,
      .stride = sizeof(float) * 2,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   VkVertexInputAttributeDescription vbo_attr = {
      .location = 0,
      .binding = 0,
      .format = VK_FORMAT_R32G32_SFLOAT,
      .offset = 0,
   };
   VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vbo_binding,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &vbo_attr,
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

   /* The actual thing under test: rasterizationSamples > 1, where every
    * earlier probe left this at VK_SAMPLE_COUNT_1_BIT.
    */
   VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = samples,
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
   if (r != VK_SUCCESS)
      return 1;

   /* ------------------------------------------------------------ record */
   printf("\n=== record: draw into the multisampled attachment, resolve "
          "on end-rendering ===\n");

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

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = begin_cmdbuf(cmdbuf, &cbbi);
   check(r == VK_SUCCESS, "vkBeginCommandBuffer");

   VkImageMemoryBarrier msaa_to_attachment = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = msaa_image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageMemoryBarrier resolve_to_attachment = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resolve_image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageMemoryBarrier both_to_attachment[2] = {msaa_to_attachment,
                                                 resolve_to_attachment};
   cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
              NULL, 2, both_to_attachment);

   VkClearValue clear_value = {
      .color.float32 = {CLEAR_RGBA[0] / 255.0f, CLEAR_RGBA[1] / 255.0f,
                        CLEAR_RGBA[2] / 255.0f, CLEAR_RGBA[3] / 255.0f},
   };

   /* storeOp = NONE on the multisampled attachment is the exact trigger for
    * PanVK's in-tile-memory resolve path (see the top-of-file note): the
    * samples never need to leave tile memory at all, only the resolved
    * result does.
    */
   VkRenderingAttachmentInfo color_attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = msaa_view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
      .resolveImageView = resolve_view,
      .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_NONE,
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
   cmd_bind_pipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   VkDeviceSize vbo_offset = 0;
   cmd_bind_vbos(cmdbuf, 0, 1, &vbo, &vbo_offset);

   cmd_draw(cmdbuf, 3, 1, 0, 0);
   printf("  vkCmdDraw(3, 1, 0, 0) recorded (%ux MSAA, storeOp=NONE, "
          "resolveMode=AVERAGE)\n",
          samples == VK_SAMPLE_COUNT_4_BIT ? 4 : 2);

   cmd_end_rendering(cmdbuf);

   /* Only the resolve target needs a post-render transition - storeOp=NONE
    * on the multisampled attachment means its contents are explicitly
    * don't-care, so nothing reads it back.
    */
   VkImageMemoryBarrier resolve_to_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resolve_image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
              &resolve_to_src);

   VkBufferImageCopy region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1},
      .imageExtent = {IMG_W, IMG_H, 1},
   };
   cmd_copy_img_to_buf(cmdbuf, resolve_image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                       &region);
   printf("  resolve target copied to a host-visible buffer for readback\n");

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
      printf("\n=== resolve readback ===\n");
      uint32_t clear_px = 0, tri_px = 0, blend_px = 0, corrupt_px = 0;
      uint32_t first_corrupt = 0;

      for (uint32_t px = 0; px < IMG_W * IMG_H; px++) {
         const uint8_t *p = mapped + px * BYTES_PER_PIXEL;
         if (memcmp(p, CLEAR_RGBA, BYTES_PER_PIXEL) == 0)
            clear_px++;
         else if (memcmp(p, TRI_RGBA, BYTES_PER_PIXEL) == 0)
            tri_px++;
         else if (is_valid_blend(p, CLEAR_RGBA, TRI_RGBA))
            blend_px++;
         else {
            if (!corrupt_px)
               first_corrupt = px;
            corrupt_px++;
         }
      }

      printf("  %u clear-colour, %u triangle-colour, %u blended-edge, %u "
             "corrupt (of %u total)\n",
             clear_px, tri_px, blend_px, corrupt_px, IMG_W * IMG_H);
      printf("  single-sample probes on this geometry got 190/66/0 with no "
             "blend bucket - MSAA is expected to move some of those into "
             "blended-edge, not reproduce the same split\n");
      if (corrupt_px) {
         const uint8_t *p = mapped + first_corrupt * BYTES_PER_PIXEL;
         printf("  first corrupt pixel [%u] = %02x%02x%02x%02x (not clear, "
                "not triangle, not a valid AVERAGE blend of the two)\n",
                first_corrupt, p[0], p[1], p[2], p[3]);
      }

      check(clear_px + tri_px + blend_px + corrupt_px == IMG_W * IMG_H,
            "every pixel classified into exactly one bucket");
      check(clear_px <= 190, "clear-colour pixels did not exceed the "
                             "single-sample interior count");
      check(tri_px <= 66, "triangle-colour pixels did not exceed the "
                          "single-sample interior count");
      check(blend_px > 0, "at least one pixel resolved to a real "
                          "AVERAGE blend - proof the resolve unit ran, "
                          "not a silent single-sample fallback");
      check(corrupt_px == 0, "no pixel fell outside clear / triangle / "
                             "valid-blend");
   }

   destroy_fence(device, fence, NULL);
   destroy_pool(device, pool, NULL);
   destroy_buffer(device, readback, NULL);
   free_mem(device, buf_memory, NULL);
   destroy_view(device, resolve_view, NULL);
   destroy_image(device, resolve_image, NULL);
   free_mem(device, resolve_memory, NULL);
   destroy_view(device, msaa_view, NULL);
   destroy_image(device, msaa_image, NULL);
   free_mem(device, msaa_memory, NULL);
   destroy_pipeline(device, pipeline, NULL);
   destroy_pl(device, layout, NULL);
   destroy_module(device, fs_module, NULL);
   destroy_module(device, vs_module, NULL);
   destroy_buffer(device, vbo, NULL);
   free_mem(device, vbo_memory, NULL);

   printf("\n=== %d failure(s) ===\n", failures);
   if (failures == 0)
      printf("\n=> MSAA resolve works on kbase: the multisampled "
             "attachment used storeOp=NONE (the in-tile-memory resolve "
             "path) and the resolve target holds a real antialiased "
             "blend along the triangle's edge, not a stale or corrupt "
             "value.\n");
   return failures ? 1 : 0;
}
