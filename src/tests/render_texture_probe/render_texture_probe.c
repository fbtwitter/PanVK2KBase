// One variable changed from tests/render_ubo_probe, which proved
// descriptor sets work in a graphics pipeline: the descriptor is now a
// combined image sampler bound to a real texture, instead of a uniform
// buffer.
//
// This is a genuinely different hardware path from render_ubo_probe, not
// just "another descriptor type" - image sampling touches texture cache
// and sampler hardware that a plain buffer read does not, so nothing about
// the UBO result implies this works. Treated with the same from-scratch
// caution as every other first-time-on-this-device mechanism this session.
//
// The texture is 1x1 on purpose. texture() on a 1x1 image returns that one
// texel regardless of the UV passed to it (wrapped or clamped, it is the
// only texel there is), so this isolates "does sampling a bound texture
// work at all" from "is UV/varying interpolation correct," which would
// otherwise be two unknowns tested at once. UV correctness is a separate,
// later question.
//
// Same cross-check discipline as the last two probes: checked against the
// SPECIFIC colour uploaded into the texture (a fourth distinct value from
// every earlier probe), not a hardcoded expectation - so a sampler
// resolving to garbage, a stale image, or a shader falling back to some
// other value would show up as a mismatch.
//
// Same vertex shader as every render_*_probe so far (unchanged, already
// proven), same 16x16 target, same partial-coverage triangle, same safety
// convention: unbuffered stdout, refuses to run without --i-know-it-hangs.
//
// Usage: render_texture_probe <path-to-libvulkan_panfrost.so> --i-know-it-hangs
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "texture_frag_spv.h"
#include "texture_vert_spv.h"

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

/* A fourth distinct colour - not the UBO probe's 0x3399cc, not the
 * push-constant probe's 0x66cc33, not the earlier hardcoded magenta. A
 * sampler resolving to any of those (a stale binding, a shader falling
 * back to a cached value) would be caught by this being different from
 * all of them.
 */
static const uint8_t TEX_RGBA[4] = {0xe6, 0x33, 0x99, 0xff};

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

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 3 || strcmp(argv[2], "--i-know-it-hangs") != 0) {
      fprintf(stderr,
              "This draws the same triangle tests/render_ubo_probe already\n"
              "proved, but the fragment colour comes from sampling a real\n"
              "texture through a combined image sampler instead of reading\n"
              "a uniform buffer - one new variable, and a genuinely\n"
              "different hardware path (image sampling, not a buffer\n"
              "read). Nothing about that path has run on this device\n"
              "before. tests/alias_cs_probe hung the kbase context twice on\n"
              "adjacent GPU-fault territory - uninterruptible D state,\n"
              "needs a device reboot.\n"
              "\n"
              "Run tests/render_ubo_probe first if you have not already.\n"
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
      .pApplicationName = "panvk-kbase-render-texture-probe",
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
   PFN_vkCreateSampler create_sampler = GDPA(vkCreateSampler);
   PFN_vkDestroySampler destroy_sampler = GDPA(vkDestroySampler);
   PFN_vkCreateBuffer create_buffer = GDPA(vkCreateBuffer);
   PFN_vkDestroyBuffer destroy_buffer = GDPA(vkDestroyBuffer);
   PFN_vkGetBufferMemoryRequirements get_buf_reqs =
      GDPA(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory bind_buf_mem = GDPA(vkBindBufferMemory);
   PFN_vkMapMemory map_mem = GDPA(vkMapMemory);
   PFN_vkCreateShaderModule create_module = GDPA(vkCreateShaderModule);
   PFN_vkDestroyShaderModule destroy_module = GDPA(vkDestroyShaderModule);
   PFN_vkCreateDescriptorSetLayout create_dsl =
      GDPA(vkCreateDescriptorSetLayout);
   PFN_vkDestroyDescriptorSetLayout destroy_dsl =
      GDPA(vkDestroyDescriptorSetLayout);
   PFN_vkCreateDescriptorPool create_dpool = GDPA(vkCreateDescriptorPool);
   PFN_vkDestroyDescriptorPool destroy_dpool = GDPA(vkDestroyDescriptorPool);
   PFN_vkAllocateDescriptorSets alloc_dsets = GDPA(vkAllocateDescriptorSets);
   PFN_vkUpdateDescriptorSets update_dsets = GDPA(vkUpdateDescriptorSets);
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
   PFN_vkCmdCopyBufferToImage cmd_copy_buf_to_img =
      GDPA(vkCmdCopyBufferToImage);
   PFN_vkCmdBeginRendering cmd_begin_rendering = GDPA(vkCmdBeginRendering);
   PFN_vkCmdEndRendering cmd_end_rendering = GDPA(vkCmdEndRendering);
   PFN_vkCmdBindPipeline cmd_bind_pipeline = GDPA(vkCmdBindPipeline);
   PFN_vkCmdBindVertexBuffers cmd_bind_vbos = GDPA(vkCmdBindVertexBuffers);
   PFN_vkCmdBindDescriptorSets cmd_bind_dsets =
      GDPA(vkCmdBindDescriptorSets);
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

   /* ------------------------------------------------------------- texture */
   printf("\n=== texture: 1x1, uploaded through a staging buffer ===\n");

   VkBufferCreateInfo staging_bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BYTES_PER_PIXEL,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer staging = VK_NULL_HANDLE;
   r = create_buffer(device, &staging_bci, NULL, &staging);
   check(r == VK_SUCCESS, "vkCreateBuffer (staging)");

   VkMemoryRequirements staging_reqs;
   get_buf_reqs(device, staging, &staging_reqs);
   uint32_t staging_type =
      find_memory_type(&mem_props, staging_reqs.memoryTypeBits, host_want);
   check(staging_type != UINT32_MAX,
        "host-visible memory type found for staging buffer");
   if (staging_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo staging_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = staging_reqs.size,
      .memoryTypeIndex = staging_type,
   };
   VkDeviceMemory staging_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &staging_mai, NULL, &staging_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (staging)");
   r = bind_buf_mem(device, staging, staging_memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory (staging)");

   void *staging_mapped = NULL;
   r = map_mem(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &staging_mapped);
   check(r == VK_SUCCESS, "vkMapMemory (staging)");
   if (r != VK_SUCCESS)
      return 1;
   memcpy(staging_mapped, TEX_RGBA, BYTES_PER_PIXEL);
   printf("  wrote texel %02x%02x%02x%02x into the staging buffer\n",
          TEX_RGBA[0], TEX_RGBA[1], TEX_RGBA[2], TEX_RGBA[3]);

   VkImageCreateInfo tex_ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = IMG_FORMAT,
      .extent = {1, 1, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage tex_image = VK_NULL_HANDLE;
   r = create_image(device, &tex_ici, NULL, &tex_image);
   check(r == VK_SUCCESS, "vkCreateImage (texture, 1x1)");
   if (r != VK_SUCCESS)
      return 1;

   VkMemoryRequirements tex_reqs;
   get_img_reqs(device, tex_image, &tex_reqs);
   uint32_t tex_type = find_memory_type(&mem_props, tex_reqs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   check(tex_type != UINT32_MAX, "device-local memory type found for texture");
   if (tex_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo tex_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = tex_reqs.size,
      .memoryTypeIndex = tex_type,
   };
   VkDeviceMemory tex_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &tex_mai, NULL, &tex_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (texture)");
   r = bind_img_mem(device, tex_image, tex_memory, 0);
   check(r == VK_SUCCESS, "vkBindImageMemory (texture)");

   VkImageViewCreateInfo tex_ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = tex_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = IMG_FORMAT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView tex_view = VK_NULL_HANDLE;
   r = create_view(device, &tex_ivci, NULL, &tex_view);
   check(r == VK_SUCCESS, "vkCreateImageView (texture)");

   VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
   };
   VkSampler sampler = VK_NULL_HANDLE;
   r = create_sampler(device, &sci, NULL, &sampler);
   check(r == VK_SUCCESS, "vkCreateSampler (nearest)");

   /* Diagnostic only: reads the texture image straight back, before it is
    * ever sampled, to bisect "the upload did not reach the image" from
    * "the upload reached the image but sampling it is wrong." Not part of
    * what this probe is trying to prove - only present because the first
    * run came back 0x00000000 and this is the cheapest way to localise
    * that without guessing.
    */
   VkBufferCreateInfo tex_rb_bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BYTES_PER_PIXEL,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer tex_readback = VK_NULL_HANDLE;
   r = create_buffer(device, &tex_rb_bci, NULL, &tex_readback);
   check(r == VK_SUCCESS, "vkCreateBuffer (texture readback, diagnostic)");

   VkMemoryRequirements tex_rb_reqs;
   get_buf_reqs(device, tex_readback, &tex_rb_reqs);
   uint32_t tex_rb_type =
      find_memory_type(&mem_props, tex_rb_reqs.memoryTypeBits, host_want);
   VkMemoryAllocateInfo tex_rb_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = tex_rb_reqs.size,
      .memoryTypeIndex = tex_rb_type,
   };
   VkDeviceMemory tex_rb_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &tex_rb_mai, NULL, &tex_rb_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (texture readback)");
   r = bind_buf_mem(device, tex_readback, tex_rb_memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory (texture readback)");

   uint8_t *tex_rb_mapped = NULL;
   r = map_mem(device, tex_rb_memory, 0, VK_WHOLE_SIZE, 0,
              (void **)&tex_rb_mapped);
   check(r == VK_SUCCESS, "vkMapMemory (texture readback)");
   memset(tex_rb_mapped, 0x22, BYTES_PER_PIXEL);

   /* --------------------------------------------------------- render target */
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
   memset(mapped, 0x11, IMG_W * IMG_H * BYTES_PER_PIXEL);

   /* --------------------------------------------------- descriptor set */
   printf("\n=== descriptor set: one combined image sampler binding ===\n");

   VkDescriptorSetLayoutBinding dsl_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &dsl_binding,
   };
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   r = create_dsl(device, &dslci, NULL, &dsl);
   check(r == VK_SUCCESS, "vkCreateDescriptorSetLayout");

   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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

   VkDescriptorImageInfo dii = {
      .sampler = sampler,
      .imageView = tex_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = dset,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &dii,
   };
   update_dsets(device, 1, &write, 0, NULL);
   printf("  vkUpdateDescriptorSets returned - set points at the texture\n");

   /* --------------------------------------------------------- pipeline */
   printf("\n=== pipeline ===\n");

   VkShaderModuleCreateInfo vs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_texture_probe_vert),
      .pCode = render_texture_probe_vert,
   };
   VkShaderModule vs_module = VK_NULL_HANDLE;
   r = create_module(device, &vs_smci, NULL, &vs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (vertex)");
   if (r != VK_SUCCESS)
      return 1;

   VkShaderModuleCreateInfo fs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_texture_probe_frag),
      .pCode = render_texture_probe_frag,
   };
   VkShaderModule fs_module = VK_NULL_HANDLE;
   r = create_module(device, &fs_smci, NULL, &fs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (fragment)");
   if (r != VK_SUCCESS)
      return 1;

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
   };
   VkPipelineLayout layout = VK_NULL_HANDLE;
   r = create_pl(device, &plci, NULL, &layout);
   check(r == VK_SUCCESS, "vkCreatePipelineLayout");

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
   if (r != VK_SUCCESS)
      return 1;

   /* ------------------------------------------------------------ record */
   printf("\n=== record: upload the texture, bind it, draw ===\n");

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

   /* Texture: UNDEFINED -> TRANSFER_DST_OPTIMAL, copy, ->
    * SHADER_READ_ONLY_OPTIMAL. All recorded before the render pass so the
    * texture is ready before the draw that samples it.
    */
   VkImageMemoryBarrier tex_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = tex_image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
              &tex_to_dst);

   VkBufferImageCopy tex_region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1},
      .imageExtent = {1, 1, 1},
   };
   cmd_copy_buf_to_img(cmdbuf, staging, tex_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tex_region);
   printf("  vkCmdCopyBufferToImage recorded (staging -> texture)\n");

   /* Diagnostic: transition to TRANSFER_SRC and copy the texture straight
    * back out, before it is ever sampled. Bisects "upload did not reach
    * the image" from "sampling is wrong" - see the comment where
    * tex_readback was created.
    */
   VkImageMemoryBarrier tex_to_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = tex_image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
              &tex_to_src);

   VkBufferImageCopy tex_rb_region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1},
      .imageExtent = {1, 1, 1},
   };
   cmd_copy_img_to_buf(cmdbuf, tex_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tex_readback, 1, &tex_rb_region);
   printf("  vkCmdCopyImageToBuffer recorded (texture -> diagnostic "
          "readback)\n");

   VkImageMemoryBarrier tex_to_shader_read = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = tex_image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
              &tex_to_shader_read);

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
   cmd_bind_pipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   VkDeviceSize vbo_offset = 0;
   cmd_bind_vbos(cmdbuf, 0, 1, &vbo, &vbo_offset);

   cmd_bind_dsets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                  &dset, 0, NULL);
   printf("  vkCmdBindDescriptorSets recorded\n");

   cmd_draw(cmdbuf, 3, 1, 0, 0);
   printf("  vkCmdDraw(3, 1, 0, 0) recorded\n");

   cmd_end_rendering(cmdbuf);

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

   /* -------------------------------------------------- diagnostic readback */
   if (r == VK_SUCCESS) {
      printf("\n=== diagnostic: the texture image itself, read back before "
             "sampling ===\n");
      printf("  uploaded  %02x%02x%02x%02x\n", TEX_RGBA[0], TEX_RGBA[1],
             TEX_RGBA[2], TEX_RGBA[3]);
      printf("  image now holds %02x%02x%02x%02x - %s\n", tex_rb_mapped[0],
             tex_rb_mapped[1], tex_rb_mapped[2], tex_rb_mapped[3],
             memcmp(tex_rb_mapped, TEX_RGBA, BYTES_PER_PIXEL) == 0
                ? "the upload reached the image; the fault is in sampling"
                : "the upload itself did not reach the image");
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
         else if (memcmp(p, TEX_RGBA, BYTES_PER_PIXEL) == 0)
            tri_px++;
         else {
            if (!other_px)
               first_other = px;
            other_px++;
         }
      }

      printf("  %u clear-colour, %u texture-colour, %u other (of %u "
             "total)\n",
             clear_px, tri_px, other_px, IMG_W * IMG_H);
      printf("  every earlier probe got 190/66/0 on the same geometry - "
             "%s\n",
             (clear_px == 190 && tri_px == 66 && other_px == 0)
                ? "this matches exactly"
                : "this DIFFERS - see below");
      if (other_px) {
         const uint8_t *p = mapped + first_other * BYTES_PER_PIXEL;
         printf("  first unexpected pixel [%u] = %02x%02x%02x%02x\n",
                first_other, p[0], p[1], p[2], p[3]);
      }

      check(clear_px > 0, "some pixels are still the clear colour");
      check(tri_px > 0,
            "some pixels hold the texture colour (sampler resolved to "
            "the right texture)");
      check(other_px == 0, "no pixel is anything other than clear or "
                           "texture colour");
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
   destroy_dpool(device, dpool, NULL);
   destroy_dsl(device, dsl, NULL);
   destroy_module(device, fs_module, NULL);
   destroy_module(device, vs_module, NULL);
   destroy_sampler(device, sampler, NULL);
   destroy_view(device, tex_view, NULL);
   destroy_image(device, tex_image, NULL);
   free_mem(device, tex_memory, NULL);
   destroy_buffer(device, staging, NULL);
   free_mem(device, staging_memory, NULL);
   destroy_buffer(device, tex_readback, NULL);
   free_mem(device, tex_rb_memory, NULL);
   destroy_buffer(device, vbo, NULL);
   free_mem(device, vbo_memory, NULL);

   printf("\n=== %d failure(s) ===\n", failures);
   if (failures == 0)
      printf("\n=> Texture sampling works in a graphics pipeline on kbase:\n"
             "   upload, layout transitions, the sampler, and the combined\n"
             "   image sampler descriptor all resolved correctly.\n");
   return failures ? 1 : 0;
}
