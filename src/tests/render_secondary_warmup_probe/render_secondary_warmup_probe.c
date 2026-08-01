// Tests whether the cold-start bug found in dEQP-VK.api.command_buffers.
// record_many_draws_secondary_2 (docs/kbase-notes.md: fails 100% of the
// time as the FIRST secondary-command-buffer draw in a process, passes if
// literally any other secondary-buffer draw ran first - fully
// deterministic, not flakiness) also covers
// many_indirect_draws_on_secondary, which combines two things: drawing
// from a secondary command buffer, and drawing indirectly
// (vkCmdDrawIndirect). deqp-vk's own case selection could not test this -
// it always executes cases in fixed alphabetical order regardless of
// --deqp-case/--deqp-caselist-file order, and nothing that actually draws
// sorts alphabetically before many_indirect_draws_on_secondary within
// dEQP-VK.api.command_buffers.* (confirmed by dumping the full sorted
// caselist). Hence this probe.
//
// Three rounds, same device, same process - the same shape that proved the
// cold-start behaviour for the plain-draw case:
//
//   round 1 (cold):   ONE indirect draw from a secondary command buffer,
//                      as the very first secondary-buffer operation in this
//                      process. Expected to reproduce the failure if this
//                      is the same bug as record_many_draws_secondary_2's.
//   round 2 (warm-up): a completely ordinary, non-indirect draw from a
//                      secondary command buffer, to a throwaway target -
//                      exactly the kind of draw that fixed
//                      record_many_draws_secondary_2 when run first.
//   round 3 (warm):    the exact same indirect-draw-from-secondary as
//                      round 1, on a fresh target, now that round 2 has
//                      run.
//
// If round 1 fails and round 3 passes: many_indirect_draws_on_secondary is
// very likely the same cold-start bug landing on the alphabetically-first
// case, not a distinct indirect-specific issue. If round 1 AND round 3
// both fail: indirect draws from a secondary buffer are broken
// independently of the cold-start mechanism, warm-up or not.
//
// Same safety convention as every render_*/alias_cs probe in this repo:
// unbuffered stdout, refuses to run without --i-know-it-hangs - though the
// actual hang risk here is lower than most of this repo's render probes,
// since deqp-vk itself has already executed this exact combination
// (secondary command buffer + indirect draw) via
// many_indirect_draws_on_secondary without hanging, many times, across
// this session's CTS runs - only ever wrong pixels, never a stall. The
// gate stays for consistency and because this probe's *specific* sequence
// (three rounds, two command pools, mid-process state reuse) has not
// itself been run before.
//
// Usage: render_secondary_warmup_probe <path-to-.so> --i-know-it-hangs
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "warmup_frag_spv.h"
#include "warmup_vert_spv.h"

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
static const uint8_t TRIANGLE_RGBA[4] = {0xff, 0x00, 0xff, 0xff};

/* The same triangle geometry every render probe this session has used -
 * 66 pixels at 16x16, an exact-match cross-check against the established
 * baseline.
 */
static const float VERTICES[3][2] = {
   {-0.8f, -0.8f}, {0.8f, -0.8f}, {-0.8f, 0.8f},
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

struct target {
   VkImage image;
   VkDeviceMemory image_memory;
   VkImageView view;
   VkBuffer readback;
   VkDeviceMemory readback_memory;
   uint8_t *mapped;
};

struct ctx {
   VkDevice device;
   VkQueue queue;
   VkPhysicalDeviceMemoryProperties mem_props;
   VkPipelineLayout layout;
   VkPipeline pipeline;
   VkBuffer vbo;
   VkBuffer indirect_buf;

   PFN_vkCreateImage create_image;
   PFN_vkGetImageMemoryRequirements get_img_reqs;
   PFN_vkAllocateMemory alloc_mem;
   PFN_vkBindImageMemory bind_img_mem;
   PFN_vkCreateImageView create_view;
   PFN_vkCreateBuffer create_buffer;
   PFN_vkGetBufferMemoryRequirements get_buf_reqs;
   PFN_vkBindBufferMemory bind_buf_mem;
   PFN_vkMapMemory map_mem;
   PFN_vkCreateCommandPool create_pool;
   PFN_vkDestroyCommandPool destroy_pool;
   PFN_vkAllocateCommandBuffers alloc_cmdbufs;
   PFN_vkBeginCommandBuffer begin_cmdbuf;
   PFN_vkEndCommandBuffer end_cmdbuf;
   PFN_vkCmdPipelineBarrier cmd_barrier;
   PFN_vkCmdBeginRendering cmd_begin_rendering;
   PFN_vkCmdEndRendering cmd_end_rendering;
   PFN_vkCmdExecuteCommands cmd_execute_commands;
   PFN_vkCmdBindPipeline cmd_bind_pipeline;
   PFN_vkCmdBindVertexBuffers cmd_bind_vbos;
   PFN_vkCmdDraw cmd_draw;
   PFN_vkCmdDrawIndirect cmd_draw_indirect;
   PFN_vkCmdCopyImageToBuffer cmd_copy_img_to_buf;
   PFN_vkQueueSubmit queue_submit;
   PFN_vkCreateFence create_fence;
   PFN_vkDestroyFence destroy_fence;
   PFN_vkWaitForFences wait_fences;
};

static bool
make_target(struct ctx *c, struct target *t)
{
   VkImageCreateInfo ici = {
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
   if (c->create_image(c->device, &ici, NULL, &t->image) != VK_SUCCESS)
      return false;

   VkMemoryRequirements img_reqs;
   c->get_img_reqs(c->device, t->image, &img_reqs);
   uint32_t img_type = find_memory_type(&c->mem_props, img_reqs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (img_type == UINT32_MAX)
      return false;

   VkMemoryAllocateInfo img_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = img_reqs.size,
      .memoryTypeIndex = img_type,
   };
   if (c->alloc_mem(c->device, &img_mai, NULL, &t->image_memory) != VK_SUCCESS)
      return false;
   if (c->bind_img_mem(c->device, t->image, t->image_memory, 0) != VK_SUCCESS)
      return false;

   VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = t->image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = IMG_FORMAT,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = 1, .layerCount = 1},
   };
   if (c->create_view(c->device, &ivci, NULL, &t->view) != VK_SUCCESS)
      return false;

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = IMG_W * IMG_H * BYTES_PER_PIXEL,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (c->create_buffer(c->device, &bci, NULL, &t->readback) != VK_SUCCESS)
      return false;

   VkMemoryRequirements buf_reqs;
   c->get_buf_reqs(c->device, t->readback, &buf_reqs);
   const VkMemoryPropertyFlags host_want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t buf_type =
      find_memory_type(&c->mem_props, buf_reqs.memoryTypeBits, host_want);
   if (buf_type == UINT32_MAX)
      return false;

   VkMemoryAllocateInfo buf_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buf_reqs.size,
      .memoryTypeIndex = buf_type,
   };
   if (c->alloc_mem(c->device, &buf_mai, NULL, &t->readback_memory) !=
       VK_SUCCESS)
      return false;
   if (c->bind_buf_mem(c->device, t->readback, t->readback_memory, 0) !=
       VK_SUCCESS)
      return false;
   if (c->map_mem(c->device, t->readback_memory, 0, VK_WHOLE_SIZE, 0,
                  (void **)&t->mapped) != VK_SUCCESS)
      return false;
   memset(t->mapped, 0x11, IMG_W * IMG_H * BYTES_PER_PIXEL);

   return true;
}

/* Records a secondary command buffer that draws the triangle (indirectly or
 * directly), executes it from a primary within a dynamic-rendering render
 * pass targeting `t`, submits, and waits. Returns true iff the whole
 * sequence completed (vkQueueSubmit accepted, fence signalled) - caller
 * checks the actual pixels separately.
 */
static bool
run_round(struct ctx *c, struct target *t, bool use_indirect,
         const char *label)
{
   printf("\n=== round: %s ===\n", label);

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   VkResult r = c->create_pool(c->device, &cpci, NULL, &pool);
   check(r == VK_SUCCESS, "vkCreateCommandPool");
   if (r != VK_SUCCESS)
      return false;

   VkCommandBufferAllocateInfo prim_cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer primary = VK_NULL_HANDLE;
   r = c->alloc_cmdbufs(c->device, &prim_cbai, &primary);
   check(r == VK_SUCCESS, "vkAllocateCommandBuffers (primary)");

   VkCommandBufferAllocateInfo sec_cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer secondary = VK_NULL_HANDLE;
   r = c->alloc_cmdbufs(c->device, &sec_cbai, &secondary);
   check(r == VK_SUCCESS, "vkAllocateCommandBuffers (secondary)");
   if (r != VK_SUCCESS) {
      c->destroy_pool(c->device, pool, NULL);
      return false;
   }

   /* Secondary: inherits the primary's dynamic-rendering attachment
    * formats (VkCommandBufferInheritanceRenderingInfo), not a classic
    * renderPass/framebuffer - this is the dynamic-rendering equivalent of
    * VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT with inheritance
    * info, matching what CTS's own record_many_draws_secondary_* and
    * many_indirect_draws_on_secondary do.
    */
   VkFormat color_format = IMG_FORMAT;
   VkCommandBufferInheritanceRenderingInfo inherit_rendering = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &color_format,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkCommandBufferInheritanceInfo inherit = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
      .pNext = &inherit_rendering,
   };
   VkCommandBufferBeginInfo sec_begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
              VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
      .pInheritanceInfo = &inherit,
   };
   r = c->begin_cmdbuf(secondary, &sec_begin);
   check(r == VK_SUCCESS, "vkBeginCommandBuffer (secondary)");

   c->cmd_bind_pipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        c->pipeline);
   VkDeviceSize vbo_offset = 0;
   c->cmd_bind_vbos(secondary, 0, 1, &c->vbo, &vbo_offset);

   if (use_indirect) {
      c->cmd_draw_indirect(secondary, c->indirect_buf, 0, 1,
                           sizeof(VkDrawIndirectCommand));
      printf("  recorded vkCmdDrawIndirect(drawCount=1) into the "
             "secondary\n");
   } else {
      c->cmd_draw(secondary, 3, 1, 0, 0);
      printf("  recorded vkCmdDraw(3, 1, 0, 0) into the secondary\n");
   }

   r = c->end_cmdbuf(secondary);
   check(r == VK_SUCCESS, "vkEndCommandBuffer (secondary)");

   /* Primary: begins the render pass with
    * VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT (the
    * dynamic-rendering equivalent of
    * VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS), executes the
    * secondary, ends, then copies out for readback.
    */
   VkCommandBufferBeginInfo prim_begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = c->begin_cmdbuf(primary, &prim_begin);
   check(r == VK_SUCCESS, "vkBeginCommandBuffer (primary)");

   VkImageMemoryBarrier to_color_attachment = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = t->image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = 1, .layerCount = 1},
   };
   c->cmd_barrier(primary, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
                 NULL, 1, &to_color_attachment);

   VkClearValue clear_value = {
      .color.float32 = {CLEAR_RGBA[0] / 255.0f, CLEAR_RGBA[1] / 255.0f,
                        CLEAR_RGBA[2] / 255.0f, CLEAR_RGBA[3] / 255.0f},
   };
   VkRenderingAttachmentInfo color_attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = t->view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = clear_value,
   };
   VkRenderingInfo rendering_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
      .renderArea = {.offset = {0, 0}, .extent = {IMG_W, IMG_H}},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
   };
   c->cmd_begin_rendering(primary, &rendering_info);
   c->cmd_execute_commands(primary, 1, &secondary);
   c->cmd_end_rendering(primary);

   VkImageMemoryBarrier to_transfer_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = t->image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = 1, .layerCount = 1},
   };
   c->cmd_barrier(primary, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                 &to_transfer_src);

   VkBufferImageCopy region = {
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1},
      .imageExtent = {IMG_W, IMG_H, 1},
   };
   c->cmd_copy_img_to_buf(primary, t->image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->readback, 1,
                          &region);

   r = c->end_cmdbuf(primary);
   check(r == VK_SUCCESS, "vkEndCommandBuffer (primary)");

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   r = c->create_fence(c->device, &fci, NULL, &fence);
   check(r == VK_SUCCESS, "vkCreateFence");

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &primary,
   };
   r = c->queue_submit(c->queue, 1, &si, fence);
   printf("  vkQueueSubmit -> %d\n", r);
   check(r == VK_SUCCESS, "vkQueueSubmit accepted");

   bool ok = r == VK_SUCCESS;
   if (ok) {
      r = c->wait_fences(c->device, 1, &fence, VK_TRUE, 5000000000ull);
      printf("  vkWaitForFences -> %d%s\n", r,
             r == VK_TIMEOUT ? " (TIMEOUT - the GPU did not finish)" : "");
      check(r == VK_SUCCESS, "fence signalled by the GPU");
      ok = r == VK_SUCCESS;
   }

   c->destroy_fence(c->device, fence, NULL);
   c->destroy_pool(c->device, pool, NULL); /* frees both cmdbufs too */
   return ok;
}

static bool
check_pixels(struct target *t, const char *label)
{
   uint32_t clear_px = 0, tri_px = 0, other_px = 0, first_other = 0;
   for (uint32_t px = 0; px < IMG_W * IMG_H; px++) {
      const uint8_t *p = t->mapped + px * BYTES_PER_PIXEL;
      if (memcmp(p, CLEAR_RGBA, BYTES_PER_PIXEL) == 0)
         clear_px++;
      else if (memcmp(p, TRIANGLE_RGBA, BYTES_PER_PIXEL) == 0)
         tri_px++;
      else {
         if (!other_px)
            first_other = px;
         other_px++;
      }
   }
   printf("  [%s] %u clear, %u triangle, %u other (of %u total)\n", label,
          clear_px, tri_px, other_px, IMG_W * IMG_H);
   if (other_px) {
      const uint8_t *p = t->mapped + first_other * BYTES_PER_PIXEL;
      printf("  [%s] first unexpected pixel [%u] = %02x%02x%02x%02x\n", label,
             first_other, p[0], p[1], p[2], p[3]);
   }
   bool ok = tri_px == 66 && other_px == 0;
   check(ok, label);
   return ok;
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 3 || strcmp(argv[2], "--i-know-it-hangs") != 0) {
      fprintf(stderr,
              "Tests whether the cold-start bug found in\n"
              "record_many_draws_secondary_2 (docs/kbase-notes.md - fails\n"
              "100%% of the time as the FIRST secondary-command-buffer\n"
              "draw in a process, passes after any other secondary draw)\n"
              "also covers many_indirect_draws_on_secondary. Draws\n"
              "indirectly from a secondary command buffer cold, then again\n"
              "after a warm-up secondary draw, in the same process.\n"
              "\n"
              "deqp-vk itself has already run this exact combination\n"
              "(secondary buffer + indirect draw) many times this session\n"
              "without hanging - only ever wrong pixels - but this probe's\n"
              "specific three-round sequence has not itself run before.\n"
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
      .pApplicationName = "panvk-kbase-render-secondary-warmup-probe",
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

   struct ctx c = {.device = device};
#define GDPA(name) (PFN_##name) gdpa(device, #name)
   PFN_vkGetDeviceQueue get_queue = GDPA(vkGetDeviceQueue);
   c.create_image = GDPA(vkCreateImage);
   c.get_img_reqs = GDPA(vkGetImageMemoryRequirements);
   c.alloc_mem = GDPA(vkAllocateMemory);
   c.bind_img_mem = GDPA(vkBindImageMemory);
   c.create_view = GDPA(vkCreateImageView);
   c.create_buffer = GDPA(vkCreateBuffer);
   c.get_buf_reqs = GDPA(vkGetBufferMemoryRequirements);
   c.bind_buf_mem = GDPA(vkBindBufferMemory);
   c.map_mem = GDPA(vkMapMemory);
   c.create_pool = GDPA(vkCreateCommandPool);
   c.destroy_pool = GDPA(vkDestroyCommandPool);
   c.alloc_cmdbufs = GDPA(vkAllocateCommandBuffers);
   c.begin_cmdbuf = GDPA(vkBeginCommandBuffer);
   c.end_cmdbuf = GDPA(vkEndCommandBuffer);
   c.cmd_barrier = GDPA(vkCmdPipelineBarrier);
   c.cmd_begin_rendering = GDPA(vkCmdBeginRendering);
   c.cmd_end_rendering = GDPA(vkCmdEndRendering);
   c.cmd_execute_commands = GDPA(vkCmdExecuteCommands);
   c.cmd_bind_pipeline = GDPA(vkCmdBindPipeline);
   c.cmd_bind_vbos = GDPA(vkCmdBindVertexBuffers);
   c.cmd_draw = GDPA(vkCmdDraw);
   c.cmd_draw_indirect = GDPA(vkCmdDrawIndirect);
   c.cmd_copy_img_to_buf = GDPA(vkCmdCopyImageToBuffer);
   c.queue_submit = GDPA(vkQueueSubmit);
   c.create_fence = GDPA(vkCreateFence);
   c.destroy_fence = GDPA(vkDestroyFence);
   c.wait_fences = GDPA(vkWaitForFences);
   PFN_vkCreateShaderModule create_module = GDPA(vkCreateShaderModule);
   PFN_vkCreatePipelineLayout create_pl = GDPA(vkCreatePipelineLayout);
   PFN_vkCreateGraphicsPipelines create_gfx_pipelines =
      GDPA(vkCreateGraphicsPipelines);

   get_queue(device, 0, 0, &c.queue);
   get_mem_props(pd, &c.mem_props);
   const VkMemoryPropertyFlags host_want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

   /* -------------------------------------------------------- vertex buffer */
   printf("\n=== vertex buffer + indirect-draw buffer ===\n");

   VkBufferCreateInfo vbo_bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(VERTICES),
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   r = c.create_buffer(device, &vbo_bci, NULL, &c.vbo);
   check(r == VK_SUCCESS, "vkCreateBuffer (vertex buffer)");

   VkMemoryRequirements vbo_reqs;
   c.get_buf_reqs(device, c.vbo, &vbo_reqs);
   uint32_t vbo_type =
      find_memory_type(&c.mem_props, vbo_reqs.memoryTypeBits, host_want);
   VkMemoryAllocateInfo vbo_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vbo_reqs.size,
      .memoryTypeIndex = vbo_type,
   };
   VkDeviceMemory vbo_memory = VK_NULL_HANDLE;
   r = c.alloc_mem(device, &vbo_mai, NULL, &vbo_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (VBO)");
   r = c.bind_buf_mem(device, c.vbo, vbo_memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory (VBO)");
   void *vbo_mapped = NULL;
   r = c.map_mem(device, vbo_memory, 0, VK_WHOLE_SIZE, 0, &vbo_mapped);
   check(r == VK_SUCCESS, "vkMapMemory (VBO)");
   if (r != VK_SUCCESS)
      return 1;
   memcpy(vbo_mapped, VERTICES, sizeof(VERTICES));

   VkBufferCreateInfo ind_bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(VkDrawIndirectCommand),
      .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   r = c.create_buffer(device, &ind_bci, NULL, &c.indirect_buf);
   check(r == VK_SUCCESS, "vkCreateBuffer (indirect)");

   VkMemoryRequirements ind_reqs;
   c.get_buf_reqs(device, c.indirect_buf, &ind_reqs);
   uint32_t ind_type =
      find_memory_type(&c.mem_props, ind_reqs.memoryTypeBits, host_want);
   VkMemoryAllocateInfo ind_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = ind_reqs.size,
      .memoryTypeIndex = ind_type,
   };
   VkDeviceMemory ind_memory = VK_NULL_HANDLE;
   r = c.alloc_mem(device, &ind_mai, NULL, &ind_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (indirect)");
   r = c.bind_buf_mem(device, c.indirect_buf, ind_memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory (indirect)");
   void *ind_mapped = NULL;
   r = c.map_mem(device, ind_memory, 0, VK_WHOLE_SIZE, 0, &ind_mapped);
   check(r == VK_SUCCESS, "vkMapMemory (indirect)");
   if (r != VK_SUCCESS)
      return 1;
   VkDrawIndirectCommand indirect_cmd = {
      .vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0,
   };
   memcpy(ind_mapped, &indirect_cmd, sizeof(indirect_cmd));

   /* --------------------------------------------------------- pipeline */
   printf("\n=== pipeline (same shape as render_triangle_probe) ===\n");

   VkShaderModuleCreateInfo vs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(warmup_probe_vert),
      .pCode = warmup_probe_vert,
   };
   VkShaderModule vs_module = VK_NULL_HANDLE;
   r = create_module(device, &vs_smci, NULL, &vs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (vertex)");
   if (r != VK_SUCCESS)
      return 1;

   VkShaderModuleCreateInfo fs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(warmup_probe_frag),
      .pCode = warmup_probe_frag,
   };
   VkShaderModule fs_module = VK_NULL_HANDLE;
   r = create_module(device, &fs_smci, NULL, &fs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (fragment)");
   if (r != VK_SUCCESS)
      return 1;

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   r = create_pl(device, &plci, NULL, &c.layout);
   check(r == VK_SUCCESS, "vkCreatePipelineLayout");

   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_module, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_module, .pName = "main"},
   };
   VkVertexInputBindingDescription vbo_binding = {
      .binding = 0, .stride = sizeof(float) * 2,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   VkVertexInputAttributeDescription vbo_attr = {
      .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0,
   };
   VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbo_binding,
      .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vbo_attr,
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
      .viewportCount = 1, .pViewports = &viewport,
      .scissorCount = 1, .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &blend_attachment,
   };
   VkFormat color_format = IMG_FORMAT;
   VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format,
   };
   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_create_info,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .layout = c.layout,
   };
   r = create_gfx_pipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &c.pipeline);
   check(r == VK_SUCCESS, "vkCreateGraphicsPipelines");
   if (r != VK_SUCCESS)
      return 1;

   /* ----------------------------------------------------- render targets */
   struct target cold_target = {0}, warmup_target = {0}, warm_target = {0};
   check(make_target(&c, &cold_target), "render target: cold round");
   check(make_target(&c, &warmup_target), "render target: warm-up round");
   check(make_target(&c, &warm_target), "render target: warm round");
   if (failures)
      return 1;

   /* --------------------------------------------------------------- rounds */
   printf("\n########################################################\n");
   printf("# round 1: COLD - indirect draw, first secondary op ever\n");
   printf("########################################################\n");
   bool cold_submitted = run_round(&c, &cold_target, true, "cold indirect");
   bool cold_ok = cold_submitted && check_pixels(&cold_target, "cold indirect result");

   printf("\n########################################################\n");
   printf("# round 2: WARM-UP - ordinary draw, throwaway target\n");
   printf("########################################################\n");
   bool warmup_submitted =
      run_round(&c, &warmup_target, false, "warm-up plain draw");
   check_pixels(&warmup_target, "warm-up result (informational)");
   (void)warmup_submitted;

   printf("\n########################################################\n");
   printf("# round 3: WARM - indirect draw, after warm-up\n");
   printf("########################################################\n");
   bool warm_submitted = run_round(&c, &warm_target, true, "warm indirect");
   bool warm_ok = warm_submitted && check_pixels(&warm_target, "warm indirect result");

   printf("\n=== %d failure(s) (excluding the two summary lines below) "
          "===\n",
          failures);
   printf("\n=== VERDICT ===\n");
   printf("  cold indirect (round 1): %s\n", cold_ok ? "PASS" : "FAIL");
   printf("  warm indirect (round 3): %s\n", warm_ok ? "PASS" : "FAIL");
   if (!cold_ok && warm_ok) {
      printf("\n=> Same cold-start bug as record_many_draws_secondary_2:\n"
             "   the indirect draw only fails as the FIRST secondary op in\n"
             "   the process. many_indirect_draws_on_secondary is very\n"
             "   likely this same bug, not a distinct indirect-specific\n"
             "   issue.\n");
   } else if (!cold_ok && !warm_ok) {
      printf("\n=> NOT the same bug: indirect draws from a secondary\n"
             "   command buffer are broken independently of warm-up.\n"
             "   many_indirect_draws_on_secondary needs its own\n"
             "   investigation, separate from record_many_draws_"
             "secondary_2.\n");
   } else if (cold_ok) {
      printf("\n=> Unexpected: the cold round passed here, where CTS's\n"
             "   own many_indirect_draws_on_secondary fails. A real\n"
             "   difference between this probe's sequence and CTS's exact\n"
             "   test (draw count, target format/size, or something else)\n"
             "   matters - not a clean reproduction.\n");
   }

   /* Exit code reflects whether anything unexpected happened at all
    * (including the informational warm-up round) - the VERDICT block
    * above is what actually answers the question this probe exists to
    * ask, not this return code.
    */
   return failures ? 1 : 0;
}
