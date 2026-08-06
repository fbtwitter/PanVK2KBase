// One variable changed from tests/render_vbo_probe, which proved vertex
// attribute fetch from a real bound VkBuffer works: this probe adds
// VK_EXT_transform_feedback capture on top of the exact same triangle -
// same vertex buffer, same pipeline shape, same draw. The one new thing
// under test is whether the driver's XFB-capture compute dispatch
// (panvk_per_arch(cmd_dispatch_xfb_capture), csf/panvk_vX_cmd_xfb.c)
// actually runs and writes correct data, not just whether the normal
// render still completes.
//
// The vertex shader (xfb.vert) writes its clip-space position to both
// gl_Position (rasterized normally, checked exactly like render_vbo_probe
// already does) and an XFB-captured output (captured to a bound buffer).
// A passing result requires BOTH: the triangle still renders correctly
// (proving the two-variant shader compile didn't break the render VS),
// AND the XFB buffer holds the exact same 3 positions the shader computed
// (proving the compute-dispatch capture actually ran and wrote the right
// data, not zeros or garbage).
//
// Phase 1 scope only: one non-indexed vkCmdDraw, one XFB buffer, no
// counter buffer at Begin. See docs/kbase-notes.md and ROADMAP.md.
//
// Same safety convention as every render_*/alias_cs probe in this repo:
// unbuffered stdout, refuses to run without --i-know-it-hangs. This
// exercises a brand new compute-dispatch code path
// (PANVK_SUBQUEUE_COMPUTE launched from inside a graphics command buffer,
// reusing the render VS's resource table) that has never run on this
// device before.
//
// Usage: render_xfb_probe <path-to-libvulkan_panfrost.so> --i-know-it-hangs
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "xfb_frag_spv.h"
#include "xfb_vert_spv.h"

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

/* Identical geometry to render_vbo_probe/render_triangle_probe - a passing
 * render result should reproduce their exact 190/66/0 pixel split.
 */
static const float VERTICES[3][2] = {
   {-0.8f, -0.8f},
   {0.8f, -0.8f},
   {-0.8f, 0.8f},
};

/* Expected XFB capture: each vertex's clip-space position (vec2, 0.0, 1.0),
 * matching what xfb.vert writes to xfbPosition - byte-identical to
 * gl_Position since the shader computes both from the same value.
 */
static const float EXPECTED_XFB[3][4] = {
   {-0.8f, -0.8f, 0.0f, 1.0f},
   {0.8f, -0.8f, 0.0f, 1.0f},
   {-0.8f, 0.8f, 0.0f, 1.0f},
};

/* --indexed mode (phase 2): the same three vertices drawn through an index
 * buffer that permutes them.
 *
 * {2, 0, 1} is chosen deliberately. It is a cyclic rotation, so the triangle's
 * winding is unchanged and the render result must still be the exact same
 * 190/66/0 pixel split - meaning any render difference is a real regression
 * rather than an artefact of the reordering. The *capture* order, though, does
 * change: it must come out permuted. That is what distinguishes a correct
 * index-buffer fetch from a shader that just used its sequential invocation
 * number, which would produce the unpermuted VERTICES order and pass a weaker
 * test.
 */
static const uint16_t INDICES[3] = {2, 0, 1};

static const float EXPECTED_XFB_INDEXED[3][4] = {
   {-0.8f, 0.8f, 0.0f, 1.0f},  /* VERTICES[2] */
   {-0.8f, -0.8f, 0.0f, 1.0f}, /* VERTICES[0] */
   {0.8f, -0.8f, 0.0f, 1.0f},  /* VERTICES[1] */
};

static bool indexed_mode;

/* --overflow mode: bind less XFB space than the draw needs.
 *
 * The buffer is still *allocated* at the full 48 bytes and poisoned, but only
 * OVERFLOW_BOUND_SIZE of it is bound via pSizes. Transform feedback discards
 * whole primitives rather than truncating them, and the draw is a single
 * triangle needing all 48 bytes, so the correct result is that *nothing* is
 * captured and the entire buffer stays poison.
 *
 * That makes this both an out-of-bounds check (before the bounds clamp
 * existed, the capture wrote past the bound range for real) and a
 * partial-primitive check.
 */
static bool overflow_mode;
#define OVERFLOW_BOUND_SIZE 32

/* --query mode: VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT. */
static bool query_mode;

/* --instanced mode: draw the triangle twice via instancing.
 *
 * This is what actually exercises the flat 1D capture grid. The dispatch is
 * one invocation per captured vertex across all instances, so the shader
 * recovers instance = slot / num_vertices and vertex = slot - instance *
 * num_vertices. With instanceCount == 1 that decomposition is degenerate
 * (instance is always 0) and a broken one would still pass.
 *
 * With two instances, slot 3 must resolve to instance 1 / vertex 0. If the
 * decomposition were wrong - instance stuck at 0 - vertex would come out as 3,
 * which is past the end of the 3-vertex vertex buffer, so the captured data
 * would not be a clean repeat of the base triangle.
 */
static bool instanced_mode;
#define BASE_VERTS      3
#define MAX_INSTANCES   2
#define MAX_CAPTURE_VERTS (BASE_VERTS * MAX_INSTANCES)

static uint32_t
instance_count(void)
{
   return instanced_mode ? 2 : 1;
}

static uint32_t
capture_verts(void)
{
   return BASE_VERTS * instance_count();
}

/* --resume mode: start the capture part-way into the buffer, from a counter
 * buffer, and write the final position back.
 *
 * The XFB buffer is sized for 6 vertices and the counter buffer is seeded with
 * RESUME_START_BYTES, so a 3-vertex draw must land in the *second* half and
 * leave the first half untouched. That is what distinguishes a real resume
 * from an implementation that ignored the counter buffer and started at 0.
 *
 * At End the counter buffer must read back RESUME_START_BYTES + 48 - proving
 * the write position was tracked on the GPU across the capture and copied back
 * out afterwards.
 */
static bool resume_mode;
#define RESUME_START_BYTES 48

/* --indirect mode: drive the same draw through vkCmdDrawIndirect.
 *
 * vertexCount/instanceCount/firstVertex then live in GPU memory rather than
 * being known when the command buffer is recorded, so the capture's clamp,
 * its grid size and the num_vertices the shader uses to split its linear slot
 * all have to be derived on the GPU. Producing the same three vertices as the
 * direct path is what shows that happened correctly.
 */
static bool indirect_mode;

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
              "This draws the same triangle tests/render_vbo_probe already\n"
              "proved renders correctly, but adds VK_EXT_transform_feedback\n"
              "capture on top - a brand new compute-dispatch code path\n"
              "(PANVK_SUBQUEUE_COMPUTE launched from inside a graphics\n"
              "command buffer) that has never run on this device before.\n"
              "tests/alias_cs_probe hung the kbase context twice on adjacent\n"
              "GPU-fault territory - uninterruptible D state, needs a device\n"
              "reboot.\n"
              "\n"
              "Run tests/render_vbo_probe first if you have not already.\n"
              "\n"
              "If you really mean it: %s <path-to-.so> --i-know-it-hangs\n"
              "Add --indexed to drive the capture from an index buffer\n"
              "(phase 2) instead of a non-indexed vkCmdDraw.\n",
              argv[0]);
      return 2;
   }

   for (int i = 3; i < argc; i++) {
      if (strcmp(argv[i], "--indexed") == 0)
         indexed_mode = true;
      else if (strcmp(argv[i], "--overflow") == 0)
         overflow_mode = true;
      else if (strcmp(argv[i], "--query") == 0)
         query_mode = true;
      else if (strcmp(argv[i], "--instanced") == 0)
         instanced_mode = true;
      else if (strcmp(argv[i], "--resume") == 0)
         resume_mode = true;
      else if (strcmp(argv[i], "--indirect") == 0)
         indirect_mode = true;
   }
   printf("mode: %s%s%s%s\n",
          indexed_mode ? "indexed (vkCmdDrawIndexed)"
                       : "non-indexed (vkCmdDraw)",
          overflow_mode ? " + overflow (XFB buffer bound too small)" : "",
          query_mode ? " + xfb query" : "",
          instanced_mode ? " + instanced (instanceCount=2)" : "");
   if (resume_mode)
      printf("       + resume (counter buffer seeded to %d bytes)\n",
             RESUME_START_BYTES);
   if (indirect_mode)
      printf("       + indirect (vkCmdDrawIndirect)\n");

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
      .pApplicationName = "panvk-kbase-render-xfb-probe",
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
   VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
      .pNext = &features13,
   };
   VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &xfb_features,
   };
   get_features2(pd, &features2);
   check(features13.dynamicRendering, "dynamicRendering feature supported");
   check(xfb_features.transformFeedback, "transformFeedback feature supported");
   printf("  (transformFeedback is expected scaffolding-stage output here -\n"
          "   see docs/kbase-notes.md for what's landed vs not)\n");
   if (!features13.dynamicRendering)
      return 1;

   VkPhysicalDeviceTransformFeedbackFeaturesEXT enable_xfb = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
      .transformFeedback = VK_TRUE,
   };
   VkPhysicalDeviceVulkan13Features enable13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &enable_xfb,
      .dynamicRendering = VK_TRUE,
   };
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const char *dev_exts[] = {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &enable13,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = dev_exts,
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
   PFN_vkCreateQueryPool create_query_pool = GDPA(vkCreateQueryPool);
   PFN_vkDestroyQueryPool destroy_query_pool = GDPA(vkDestroyQueryPool);
   PFN_vkCmdResetQueryPool cmd_reset_query_pool = GDPA(vkCmdResetQueryPool);
   PFN_vkGetQueryPoolResults get_query_results = GDPA(vkGetQueryPoolResults);
   PFN_vkCmdBeginQueryIndexedEXT cmd_begin_query_indexed =
      GDPA(vkCmdBeginQueryIndexedEXT);
   PFN_vkCmdEndQueryIndexedEXT cmd_end_query_indexed =
      GDPA(vkCmdEndQueryIndexedEXT);

   PFN_vkCmdBindVertexBuffers cmd_bind_vbos = GDPA(vkCmdBindVertexBuffers);
   PFN_vkCmdBindIndexBuffer cmd_bind_ibo = GDPA(vkCmdBindIndexBuffer);
   PFN_vkCmdDraw cmd_draw = GDPA(vkCmdDraw);
   PFN_vkCmdDrawIndexed cmd_draw_indexed = GDPA(vkCmdDrawIndexed);
   PFN_vkCmdDrawIndirect cmd_draw_indirect = GDPA(vkCmdDrawIndirect);
   PFN_vkCmdCopyImageToBuffer cmd_copy_img_to_buf =
      GDPA(vkCmdCopyImageToBuffer);
   PFN_vkQueueSubmit queue_submit = GDPA(vkQueueSubmit);
   PFN_vkCreateFence create_fence = GDPA(vkCreateFence);
   PFN_vkDestroyFence destroy_fence = GDPA(vkDestroyFence);
   PFN_vkWaitForFences wait_fences = GDPA(vkWaitForFences);

   PFN_vkCmdBindTransformFeedbackBuffersEXT cmd_bind_xfb_bufs =
      GDPA(vkCmdBindTransformFeedbackBuffersEXT);
   PFN_vkCmdBeginTransformFeedbackEXT cmd_begin_xfb =
      GDPA(vkCmdBeginTransformFeedbackEXT);
   PFN_vkCmdEndTransformFeedbackEXT cmd_end_xfb =
      GDPA(vkCmdEndTransformFeedbackEXT);
   check(cmd_bind_xfb_bufs && cmd_begin_xfb && cmd_end_xfb,
         "VK_EXT_transform_feedback entry points resolved");
   if (!cmd_bind_xfb_bufs || !cmd_begin_xfb || !cmd_end_xfb)
      return 1;

   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);

   VkPhysicalDeviceMemoryProperties mem_props;
   get_mem_props(pd, &mem_props);

   const VkMemoryPropertyFlags host_want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

   /* -------------------------------------------------------- vertex buffer */
   printf("\n=== vertex buffer: 3 vertices, host-visible ===\n");

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
   printf("  wrote %zu bytes of vertex data (3 x vec2)\n", sizeof(VERTICES));

   /* -------------------------------------------------------- counter buffer */
   VkBuffer counter_buf = VK_NULL_HANDLE;
   VkDeviceMemory counter_memory = VK_NULL_HANDLE;
   uint32_t *counter_mapped = NULL;
   if (resume_mode) {
      printf("\n=== counter buffer: 1 x uint32, host-visible ===\n");

      VkBufferCreateInfo cbci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = sizeof(uint32_t),
         .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      r = create_buffer(device, &cbci, NULL, &counter_buf);
      check(r == VK_SUCCESS, "vkCreateBuffer (counter buffer)");

      VkMemoryRequirements creqs;
      get_buf_reqs(device, counter_buf, &creqs);

      uint32_t ctype =
         find_memory_type(&mem_props, creqs.memoryTypeBits, host_want);
      check(ctype != UINT32_MAX, "host-visible memory type for counter buffer");
      if (ctype == UINT32_MAX)
         return 1;

      VkMemoryAllocateInfo cmai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = creqs.size,
         .memoryTypeIndex = ctype,
      };
      r = alloc_mem(device, &cmai, NULL, &counter_memory);
      check(r == VK_SUCCESS, "vkAllocateMemory (counter buffer)");
      r = bind_buf_mem(device, counter_buf, counter_memory, 0);
      check(r == VK_SUCCESS, "vkBindBufferMemory (counter buffer)");
      r = map_mem(device, counter_memory, 0, VK_WHOLE_SIZE, 0,
                  (void **)&counter_mapped);
      check(r == VK_SUCCESS, "vkMapMemory (counter buffer)");
      if (r != VK_SUCCESS)
         return 1;

      *counter_mapped = RESUME_START_BYTES;
      printf("  seeded counter buffer with %d bytes (%d vertices)\n",
             RESUME_START_BYTES, RESUME_START_BYTES / 16);
   }

   /* ------------------------------------------------------- indirect buffer */
   VkBuffer indirect_buf = VK_NULL_HANDLE;
   VkDeviceMemory indirect_memory = VK_NULL_HANDLE;
   if (indirect_mode) {
      printf("\n=== indirect buffer: VkDrawIndirectCommand, host-visible ===\n");

      VkBufferCreateInfo ibci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = sizeof(VkDrawIndirectCommand),
         .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      r = create_buffer(device, &ibci, NULL, &indirect_buf);
      check(r == VK_SUCCESS, "vkCreateBuffer (indirect buffer)");

      VkMemoryRequirements ireqs;
      get_buf_reqs(device, indirect_buf, &ireqs);

      uint32_t itype =
         find_memory_type(&mem_props, ireqs.memoryTypeBits, host_want);
      check(itype != UINT32_MAX, "host-visible memory type for indirect buffer");
      if (itype == UINT32_MAX)
         return 1;

      VkMemoryAllocateInfo imai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = ireqs.size,
         .memoryTypeIndex = itype,
      };
      r = alloc_mem(device, &imai, NULL, &indirect_memory);
      check(r == VK_SUCCESS, "vkAllocateMemory (indirect buffer)");
      r = bind_buf_mem(device, indirect_buf, indirect_memory, 0);
      check(r == VK_SUCCESS, "vkBindBufferMemory (indirect buffer)");

      VkDrawIndirectCommand *icmd = NULL;
      r = map_mem(device, indirect_memory, 0, VK_WHOLE_SIZE, 0, (void **)&icmd);
      check(r == VK_SUCCESS, "vkMapMemory (indirect buffer)");
      if (r != VK_SUCCESS)
         return 1;

      icmd->vertexCount = BASE_VERTS;
      icmd->instanceCount = instance_count();
      icmd->firstVertex = 0;
      icmd->firstInstance = 0;
      printf("  wrote {vertexCount=%u, instanceCount=%u, firstVertex=0}\n",
             icmd->vertexCount, icmd->instanceCount);
   }

   /* ---------------------------------------------------------- index buffer */
   VkBuffer ibo = VK_NULL_HANDLE;
   VkDeviceMemory ibo_memory = VK_NULL_HANDLE;
   if (indexed_mode) {
      printf("\n=== index buffer: 3 x uint16 {2, 0, 1}, host-visible ===\n");

      VkBufferCreateInfo ibo_bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = sizeof(INDICES),
         .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      r = create_buffer(device, &ibo_bci, NULL, &ibo);
      check(r == VK_SUCCESS, "vkCreateBuffer (index buffer)");

      VkMemoryRequirements ibo_reqs;
      get_buf_reqs(device, ibo, &ibo_reqs);

      uint32_t ibo_type =
         find_memory_type(&mem_props, ibo_reqs.memoryTypeBits, host_want);
      check(ibo_type != UINT32_MAX, "host-visible memory type found for IBO");
      if (ibo_type == UINT32_MAX)
         return 1;

      VkMemoryAllocateInfo ibo_mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = ibo_reqs.size,
         .memoryTypeIndex = ibo_type,
      };
      r = alloc_mem(device, &ibo_mai, NULL, &ibo_memory);
      check(r == VK_SUCCESS, "vkAllocateMemory (IBO)");

      r = bind_buf_mem(device, ibo, ibo_memory, 0);
      check(r == VK_SUCCESS, "vkBindBufferMemory (IBO)");

      void *ibo_mapped = NULL;
      r = map_mem(device, ibo_memory, 0, VK_WHOLE_SIZE, 0, &ibo_mapped);
      check(r == VK_SUCCESS, "vkMapMemory (IBO)");
      if (r != VK_SUCCESS)
         return 1;
      memcpy(ibo_mapped, INDICES, sizeof(INDICES));
      printf("  wrote indices {2, 0, 1} - a cyclic rotation, so the render\n"
             "  result must be unchanged while the capture order permutes\n");
   }

   /* ------------------------------------------------------------ XFB buffer */
   printf("\n=== XFB buffer: 3 x vec4 (48 bytes), host-visible ===\n");

   const VkDeviceSize xfb_size =
      resume_mode
         ? (VkDeviceSize)MAX_CAPTURE_VERTS * sizeof(EXPECTED_XFB[0])
         : (VkDeviceSize)capture_verts() * sizeof(EXPECTED_XFB[0]);
   VkBufferCreateInfo xfb_bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = xfb_size,
      .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer xfb_buf = VK_NULL_HANDLE;
   r = create_buffer(device, &xfb_bci, NULL, &xfb_buf);
   check(r == VK_SUCCESS, "vkCreateBuffer (XFB buffer)");

   VkMemoryRequirements xfb_reqs;
   get_buf_reqs(device, xfb_buf, &xfb_reqs);

   uint32_t xfb_type =
      find_memory_type(&mem_props, xfb_reqs.memoryTypeBits, host_want);
   check(xfb_type != UINT32_MAX, "host-visible memory type found for XFB buffer");
   if (xfb_type == UINT32_MAX)
      return 1;

   VkMemoryAllocateInfo xfb_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = xfb_reqs.size,
      .memoryTypeIndex = xfb_type,
   };
   VkDeviceMemory xfb_memory = VK_NULL_HANDLE;
   r = alloc_mem(device, &xfb_mai, NULL, &xfb_memory);
   check(r == VK_SUCCESS, "vkAllocateMemory (XFB buffer)");

   r = bind_buf_mem(device, xfb_buf, xfb_memory, 0);
   check(r == VK_SUCCESS, "vkBindBufferMemory (XFB buffer)");

   uint8_t *xfb_mapped = NULL;
   r = map_mem(device, xfb_memory, 0, VK_WHOLE_SIZE, 0, (void **)&xfb_mapped);
   check(r == VK_SUCCESS, "vkMapMemory (XFB buffer)");
   if (r != VK_SUCCESS)
      return 1;
   /* Poison before the draw, same discipline as the image readback below -
    * a pass has to mean "the driver wrote this", not "it happened to
    * already be zero/right".
    */
   memset(xfb_mapped, 0x11, xfb_size);

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

   /* --------------------------------------------------------- pipeline */
   printf("\n=== pipeline: vertex fetch + XFB capture ===\n");

   VkShaderModuleCreateInfo vs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_xfb_probe_vert),
      .pCode = render_xfb_probe_vert,
   };
   VkShaderModule vs_module = VK_NULL_HANDLE;
   r = create_module(device, &vs_smci, NULL, &vs_module);
   check(r == VK_SUCCESS, "vkCreateShaderModule (vertex)");
   if (r != VK_SUCCESS)
      return 1;

   VkShaderModuleCreateInfo fs_smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(render_xfb_probe_frag),
      .pCode = render_xfb_probe_frag,
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
   printf("\n=== record: draw with XFB capture active ===\n");

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

   /* The XFB query brackets the render pass: reset outside it (resets are not
    * allowed inside), begin before the draw, end after EndRendering so the
    * deferred capture has been flushed and counted.
    */
   VkQueryPool query_pool = VK_NULL_HANDLE;
   if (query_mode) {
      VkQueryPoolCreateInfo qpci = {
         .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
         .queryType = VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT,
         .queryCount = 1,
      };
      r = create_query_pool(device, &qpci, NULL, &query_pool);
      check(r == VK_SUCCESS, "vkCreateQueryPool (XFB stream)");
      if (r != VK_SUCCESS)
         return 1;

      cmd_reset_query_pool(cmdbuf, query_pool, 0, 1);
      printf("  vkCmdResetQueryPool recorded\n");
   }

   cmd_begin_rendering(cmdbuf, &rendering_info);
   cmd_bind_pipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   VkDeviceSize vbo_offset = 0;
   cmd_bind_vbos(cmdbuf, 0, 1, &vbo, &vbo_offset);
   printf("  vkCmdBindVertexBuffers recorded\n");

   VkDeviceSize xfb_offset = 0;
   VkDeviceSize xfb_bound_size =
      overflow_mode ? (VkDeviceSize)OVERFLOW_BOUND_SIZE : xfb_size;
   cmd_bind_xfb_bufs(cmdbuf, 0, 1, &xfb_buf, &xfb_offset, &xfb_bound_size);
   printf("  vkCmdBindTransformFeedbackBuffersEXT recorded (%llu bytes bound "
          "of %llu allocated)\n",
          (unsigned long long)xfb_bound_size, (unsigned long long)xfb_size);

   if (query_mode) {
      cmd_begin_query_indexed(cmdbuf, query_pool, 0, 0, 0);
      printf("  vkCmdBeginQueryIndexedEXT recorded (stream 0)\n");
   }

   if (resume_mode) {
      VkDeviceSize czero = 0;
      cmd_begin_xfb(cmdbuf, 0, 1, &counter_buf, &czero);
      printf("  vkCmdBeginTransformFeedbackEXT recorded (counter buffer)\n");
   } else {
      cmd_begin_xfb(cmdbuf, 0, 0, NULL, NULL);
      printf("  vkCmdBeginTransformFeedbackEXT recorded (no counter buffer)\n");
   }

   if (indirect_mode) {
      cmd_draw_indirect(cmdbuf, indirect_buf, 0, 1, 0);
      printf("  vkCmdDrawIndirect(1 draw) recorded\n");
   } else if (indexed_mode) {
      cmd_bind_ibo(cmdbuf, ibo, 0, VK_INDEX_TYPE_UINT16);
      printf("  vkCmdBindIndexBuffer recorded (UINT16)\n");
      cmd_draw_indexed(cmdbuf, BASE_VERTS, instance_count(), 0, 0, 0);
      printf("  vkCmdDrawIndexed(%u, %u, 0, 0, 0) recorded\n", BASE_VERTS,
             instance_count());
   } else {
      cmd_draw(cmdbuf, BASE_VERTS, instance_count(), 0, 0);
      printf("  vkCmdDraw(%u, %u, 0, 0) recorded\n", BASE_VERTS,
             instance_count());
   }

   if (resume_mode) {
      VkDeviceSize czero = 0;
      cmd_end_xfb(cmdbuf, 0, 1, &counter_buf, &czero);
      printf("  vkCmdEndTransformFeedbackEXT recorded (counter buffer)\n");
   } else {
      cmd_end_xfb(cmdbuf, 0, 0, NULL, NULL);
      printf("  vkCmdEndTransformFeedbackEXT recorded\n");
   }

   cmd_end_rendering(cmdbuf);

   if (query_mode) {
      /* Deliberately after EndRendering: the capture dispatch (and therefore
       * the counter accumulation) is deferred to there. Ending the query
       * before it exercises the driver's deferred-availability path instead,
       * which is covered by --query-inside.
       */
      cmd_end_query_indexed(cmdbuf, query_pool, 0, 0);
      printf("  vkCmdEndQueryIndexedEXT recorded\n");
   }

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

   /* --------------------------------------------------------- readback */
   if (r == VK_SUCCESS) {
      printf("\n=== readback: render target ===\n");
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
      printf("  render_vbo_probe got 190/66/0 on the same geometry - %s\n",
             (clear_px == 190 && tri_px == 66 && other_px == 0)
                ? "this matches exactly"
                : "this DIFFERS - see below");
      if (other_px) {
         const uint8_t *p = mapped + first_other * BYTES_PER_PIXEL;
         printf("  first unexpected pixel [%u] = %02x%02x%02x%02x\n",
                first_other, p[0], p[1], p[2], p[3]);
      }

      check(clear_px > 0, "some pixels are still the clear colour");
      check(tri_px > 0, "some pixels are the triangle colour");
      check(other_px == 0, "no pixel is anything other than clear or "
                           "triangle colour");
      check(clear_px == 190 && tri_px == 66,
            "render is unaffected by the XFB-capture shader variant");

      printf("\n=== readback: XFB buffer ===\n");
      float captured[MAX_CAPTURE_VERTS][4];
      memcpy(captured, xfb_mapped, (size_t)xfb_size);

      /* 0.000 at 3 decimals is ambiguous: it prints the same for a real
       * zero write and for the untouched 0x11111111 poison pattern
       * (~2.36e-27, too small to show at this precision) - dump raw hex
       * too so "wrote zero" and "never wrote at all" are distinguishable.
       */
      const uint8_t *raw = xfb_mapped;
      printf("  raw bytes: ");
      for (size_t i = 0; i < (size_t)xfb_size; i++)
         printf("%02x", raw[i]);
      printf("\n");

      if (resume_mode) {
         /* The capture must start at the seeded offset, so the first half of
          * the buffer stays poison and the triangle lands in the second.
          */
         const float(*want)[4] =
            indexed_mode ? EXPECTED_XFB_INDEXED : EXPECTED_XFB;

         bool head_untouched = true;
         for (size_t i = 0; i < RESUME_START_BYTES; i++)
            head_untouched = head_untouched && raw[i] == 0x11;
         check(head_untouched,
               "capture started at the counter-buffer offset, not 0");

         bool tail_ok =
            memcmp(raw + RESUME_START_BYTES, want, sizeof(EXPECTED_XFB)) == 0;
         check(tail_ok, "resumed capture wrote the expected vertices");

         uint32_t final_counter = *counter_mapped;
         uint32_t want_counter = RESUME_START_BYTES + (uint32_t)sizeof(EXPECTED_XFB);
         printf("  counter buffer = %u (expected %u)\n", final_counter,
                want_counter);
         check(final_counter == want_counter,
               "counter buffer holds the final byte offset");

         goto xfb_done;
      }

      const float(*expected)[4] =
         indexed_mode ? EXPECTED_XFB_INDEXED : EXPECTED_XFB;

      if (overflow_mode) {
         /* One triangle needs all 48 bytes but only 32 are bound. Transform
          * feedback drops whole primitives rather than truncating them, so
          * nothing at all may be captured - and in particular nothing past
          * the bound range, which is memory the driver was never given
          * permission to write.
          */
         bool tail_untouched = true;
         for (size_t i = OVERFLOW_BOUND_SIZE; i < (size_t)xfb_size; i++)
            tail_untouched = tail_untouched && raw[i] == 0x11;

         check(tail_untouched,
               "no write past the bound XFB range (poison tail intact)");
         if (!tail_untouched)
            printf("  OUT-OF-BOUNDS WRITE: bytes past the bound %d-byte range "
                   "were modified\n",
                   OVERFLOW_BOUND_SIZE);

         bool nothing_captured = true;
         for (size_t i = 0; i < (size_t)xfb_size; i++)
            nothing_captured = nothing_captured && raw[i] == 0x11;

         check(nothing_captured,
               "primitive that did not fit was dropped whole, not truncated");

         goto xfb_done;
      }

      bool xfb_ok = true;
      for (uint32_t v = 0; v < capture_verts(); v++) {
         /* Each instance re-captures the same triangle: the shader's
          * position does not depend on gl_InstanceIndex, so instance i
          * must reproduce the base triangle exactly. A broken
          * slot->(instance, vertex) decomposition would fetch past the
          * 3-vertex vertex buffer instead.
          */
         const float *want = expected[v % BASE_VERTS];
         bool vertex_ok =
            memcmp(captured[v], want, sizeof(captured[v])) == 0;
         printf("  vertex[%u] captured = (%.3f, %.3f, %.3f, %.3f) expected "
                "= (%.3f, %.3f, %.3f, %.3f) %s\n",
                v, captured[v][0], captured[v][1], captured[v][2],
                captured[v][3], want[0], want[1], want[2], want[3],
                vertex_ok ? "ok" : "MISMATCH");
         xfb_ok = xfb_ok && vertex_ok;
      }
      check(xfb_ok, "all captured vertices' XFB positions match exactly");

      /* In indexed mode, capturing the *unpermuted* order is the specific
       * failure mode of a shader that ignored the index buffer and used its
       * sequential invocation number - call that out rather than leaving it
       * as a generic mismatch.
       */
      if (indexed_mode && !xfb_ok &&
          memcmp(captured, EXPECTED_XFB, sizeof(captured)) == 0) {
         printf("  NOTE: captured data is the UNPERMUTED vertex order - the\n"
                "  capture shader ignored the index buffer and used its\n"
                "  sequential invocation number as the attribute index.\n");
      }

   xfb_done:
      if (query_mode) {
         printf("\n=== readback: XFB stream query ===\n");

         uint64_t results[2] = {UINT64_MAX, UINT64_MAX};
         VkResult qr = get_query_results(device, query_pool, 0, 1,
                                         sizeof(results), results,
                                         sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT |
                                            VK_QUERY_RESULT_WAIT_BIT);
         check(qr == VK_SUCCESS, "vkGetQueryPoolResults");

         /* One triangle drawn. It is captured unless the bound XFB range is
          * too small for the whole primitive, which is exactly the --overflow
          * case - so that is where written and generated must disagree.
          */
         const uint64_t want_generated = instance_count();
         const uint64_t want_written = overflow_mode ? 0 : instance_count();

         printf("  primitives written   = %llu (expected %llu)\n",
                (unsigned long long)results[0],
                (unsigned long long)want_written);
         printf("  primitives generated = %llu (expected %llu)\n",
                (unsigned long long)results[1],
                (unsigned long long)want_generated);

         check(results[0] == want_written, "XFB query: primitives written");
         check(results[1] == want_generated,
               "XFB query: primitives generated");

         if (overflow_mode && results[0] == results[1])
            printf("  NOTE: written == generated in the overflow case - the\n"
                   "  driver is not accounting for the dropped primitive.\n");
      }
   }

   if (query_pool != VK_NULL_HANDLE)
      destroy_query_pool(device, query_pool, NULL);
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
   destroy_buffer(device, xfb_buf, NULL);
   free_mem(device, xfb_memory, NULL);
   destroy_buffer(device, vbo, NULL);
   free_mem(device, vbo_memory, NULL);
   if (indexed_mode) {
      destroy_buffer(device, ibo, NULL);
      free_mem(device, ibo_memory, NULL);
   }
   if (resume_mode) {
      destroy_buffer(device, counter_buf, NULL);
      free_mem(device, counter_memory, NULL);
   }
   if (indirect_mode) {
      destroy_buffer(device, indirect_buf, NULL);
      free_mem(device, indirect_memory, NULL);
   }

   printf("\n=== %d failure(s) ===\n", failures);
   if (failures == 0 && indirect_mode)
      printf("\n=> VK_EXT_transform_feedback indirect capture works:\n"
             "   the draw counts came from GPU memory, and the capture's grid,\n"
             "   clamp and num_vertices were all derived from them.\n");
   else if (failures == 0 && resume_mode)
      printf("\n=> VK_EXT_transform_feedback counter-buffer resume works:\n"
             "   the capture began at the offset the counter buffer held and\n"
             "   the final position was written back to it.\n");
   else if (failures == 0 && overflow_mode)
      printf("\n=> VK_EXT_transform_feedback bounds clamping works:\n"
             "   the capture filled the bound range and wrote nothing past\n"
             "   it, so an undersized XFB buffer no longer causes an\n"
             "   out-of-bounds GPU write.\n");
   else if (failures == 0 && !indexed_mode)
      printf("\n=> VK_EXT_transform_feedback phase-1 capture works on kbase:\n"
             "   the render VS variant is unaffected, and the XFB-capture\n"
             "   compute dispatch wrote the exact vertex data expected.\n");
   if (failures == 0 && indexed_mode && !overflow_mode)
      printf("\n=> VK_EXT_transform_feedback phase-2 indexed capture works:\n"
             "   the capture shader fetched attributes through the index\n"
             "   buffer (permuted output) while keeping the XFB store slot\n"
             "   sequential, and the render result was unchanged.\n");
   return failures ? 1 : 0;
}
