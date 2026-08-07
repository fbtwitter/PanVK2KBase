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

/* --strip mode draws these four as a TRIANGLE_STRIP. */
static const float VERTICES_STRIP[4][2] = {
   {-0.8f, -0.8f}, /* A */
   {0.8f, -0.8f},  /* B */
   {-0.8f, 0.8f},  /* C */
   {0.8f, 0.8f},   /* D */
};

/* Two triangles: (A,B,C) then, with the odd-triangle swap, (C,B,D). */
static const float EXPECTED_XFB_STRIP[6][4] = {
   {-0.8f, -0.8f, 0.0f, 1.0f}, /* A */
   {0.8f, -0.8f, 0.0f, 1.0f},  /* B */
   {-0.8f, 0.8f, 0.0f, 1.0f},  /* C */
   {-0.8f, 0.8f, 0.0f, 1.0f},  /* C */
   {0.8f, -0.8f, 0.0f, 1.0f},  /* B */
   {0.8f, 0.8f, 0.0f, 1.0f},   /* D */
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
#define MANYDRAWS_COUNT 40
/* Buffer capacity for the modes that deliberately test the clamp. Keep this
 * as-is: --multidraw --instanced relies on generating more than it holds.
 */
#define MAX_CAPTURE_VERTS (BASE_VERTS * MAX_INSTANCES)
/* Host-side readback array only. --manydraws repeats the draw, so it captures
 * far more than MAX_CAPTURE_VERTS; sizing the array from that overflowed it
 * and crashed the probe *after* every driver-side check had already passed.
 */
#define MAX_READBACK_VERTS (MAX_CAPTURE_VERTS * MANYDRAWS_COUNT)

static uint32_t
instance_count(void)
{
   return instanced_mode ? 2 : 1;
}

/* --manydraws mode: record more captured draws in one render pass than the
 * driver's pending-capture queue used to be able to hold (it was a fixed
 * 16-entry array). Every draw is the same triangle, so the expected capture
 * is simply that triangle repeated.
 */
static bool manydraws_mode;

static uint32_t
draw_repeat_count(void)
{
   return manydraws_mode ? MANYDRAWS_COUNT : 1;
}

static uint32_t
capture_verts(void)
{
   return BASE_VERTS * instance_count() * draw_repeat_count();
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

/* --multidraw mode: vkCmdDrawIndirect with drawCount == 2.
 *
 * Two commands each draw the same triangle, so the capture must hold it twice
 * back-to-back. That is precisely what shows the GPU-resident write position
 * advanced *between* commands - if it had not, the second capture would land
 * on top of the first and the buffer's second half would still be poison.
 */
static bool multidraw_mode;
#define MULTIDRAW_COUNT 2

/* --strip mode: 4 vertices as a TRIANGLE_STRIP -> 2 triangles -> 6 captured
 * vertices, i.e. more captured than drawn.
 */
static bool strip_mode;
#define STRIP_VERTS 4

/* --fan mode: the same four vertices as a TRIANGLE_FAN, which assembles
 * (A,B,C) then (A,C,D) - every triangle starts at vertex 0, a different
 * mapping from the strip's sliding window.
 */
static bool fan_mode;

/* --restart mode: an indexed TRIANGLE_STRIP split by a restart index into two
 * runs of three, so two triangles with the strip parity restarting in the
 * second run.
 */
/* --bytecount mode: vkCmdDrawIndirectByteCountEXT. The counter buffer holds a
 * byte count (48) which, divided by the 16-byte vertexStride, is the 3 vertices
 * of the usual triangle - a count that only ever exists in GPU memory.
 */
static bool bytecount_mode;
#define BYTECOUNT_BYTES 48
#define BYTECOUNT_STRIDE 16

/* --backward mode: the dEQP-VK.transform_feedback.simple.backward_dependency
 * shape in one render pass - an unseeded Begin -> draw -> End(counter)
 * writeback, a barrier, then a resuming Begin(counter) whose
 * vkCmdDrawIndirectByteCountEXT reads the SAME counter buffer that writeback
 * just produced, all before CmdEndRendering. Unlike --bytecount and --resume,
 * which each seed the counter buffer from the host, here nothing ever writes
 * it but the driver itself - so a stale/zero read here is a real backward-
 * dependency bug, not a probe setup error.
 */
static bool backward_mode;

static bool restart_mode;
#define RESTART_INDEX_COUNT 7
static const uint16_t INDICES_RESTART[RESTART_INDEX_COUNT] = {
   0, 1, 2, 0xFFFF, 1, 2, 3,
};

/* --firstindex mode: the same restart stream, preceded by padding the draw
 * must skip. The padding is deliberately *not* the restart sentinel and not a
 * valid continuation, so a walk starting at 0 assembles different primitives.
 */
static bool firstindex_mode;
#define FIRSTINDEX_PAD 2
static const uint16_t INDICES_RESTART_PADDED[FIRSTINDEX_PAD +
                                             RESTART_INDEX_COUNT] = {
   3, 3, 0, 1, 2, 0xFFFF, 1, 2, 3,
};

static uint32_t
first_index(void)
{
   return firstindex_mode ? FIRSTINDEX_PAD : 0;
}

static const float EXPECTED_XFB_RESTART[6][4] = {
   {-0.8f, -0.8f, 0.0f, 1.0f}, /* A */
   {0.8f, -0.8f, 0.0f, 1.0f},  /* B */
   {-0.8f, 0.8f, 0.0f, 1.0f},  /* C */
   {0.8f, -0.8f, 0.0f, 1.0f},  /* B */
   {-0.8f, 0.8f, 0.0f, 1.0f},  /* C */
   {0.8f, 0.8f, 0.0f, 1.0f},   /* D */
};

static const float EXPECTED_XFB_FAN[6][4] = {
   {-0.8f, -0.8f, 0.0f, 1.0f}, /* A */
   {0.8f, -0.8f, 0.0f, 1.0f},  /* B */
   {-0.8f, 0.8f, 0.0f, 1.0f},  /* C */
   {-0.8f, -0.8f, 0.0f, 1.0f}, /* A */
   {-0.8f, 0.8f, 0.0f, 1.0f},  /* C */
   {0.8f, 0.8f, 0.0f, 1.0f},   /* D */
};

static uint32_t
draw_vertex_count(void)
{
   return strip_mode ? STRIP_VERTS : BASE_VERTS;
}

/* Strips and fans assemble n-2 triangles from n vertices; the list topologies
 * assemble n/3.
 */
/* Indices consumed by an indexed draw. Restart feeds a longer stream than the
 * vertex count, because the sentinel entries are consumed but never drawn.
 */
static uint32_t
draw_index_count(void)
{
   return restart_mode ? RESTART_INDEX_COUNT : draw_vertex_count();
}

static uint32_t
draw_prim_count(void)
{
   return strip_mode ? (STRIP_VERTS - 2) : (BASE_VERTS / 3);
}

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
      else if (strcmp(argv[i], "--strip") == 0)
         strip_mode = true;
      else if (strcmp(argv[i], "--bytecount") == 0)
         bytecount_mode = true;
      else if (strcmp(argv[i], "--restart") == 0) {
         strip_mode = true;   /* 4 vertices, TRIANGLE_STRIP */
         indexed_mode = true;
         restart_mode = true;
      }
      else if (strcmp(argv[i], "--fan") == 0) {
         strip_mode = true;  /* shares the 4-vertex setup */
         fan_mode = true;
      }
      else if (strcmp(argv[i], "--firstindex") == 0) {
         /* Only meaningful for an indexed draw, and the padded stream is the
          * restart one, so it implies both.
          */
         strip_mode = true;
         indexed_mode = true;
         restart_mode = true;
         firstindex_mode = true;
      }
      else if (strcmp(argv[i], "--manydraws") == 0)
         manydraws_mode = true;
      else if (strcmp(argv[i], "--multidraw") == 0) {
         indirect_mode = true;
         multidraw_mode = true;
      }
      else if (strcmp(argv[i], "--backward") == 0)
         backward_mode = true;
   }
   if (strip_mode && indexed_mode && !restart_mode) {
      fprintf(stderr,
              "--strip/--fan have no plain indexed form here: the probe's "
              "index buffer is a 3-entry list, but a strip needs %d "
              "vertices. Use --restart for the indexed strip path.\n",
              STRIP_VERTS);
      return 2;
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
   if (strip_mode)
      printf("       + %s (%d verts -> 2 triangles)\n",
             fan_mode ? "fan (TRIANGLE_FAN)" : "strip (TRIANGLE_STRIP)",
             STRIP_VERTS);
   if (restart_mode)
      printf("       + restart (indexed strip split by 0xFFFF)\n");
   if (firstindex_mode)
      printf("       + firstindex (%d padding indices the draw must skip)\n",
             FIRSTINDEX_PAD);
   if (manydraws_mode)
      printf("       + manydraws (%d draws in one render pass, past the old "
             "16-entry queue)\n",
             MANYDRAWS_COUNT);
   if (bytecount_mode)
      printf("       + bytecount (vkCmdDrawIndirectByteCountEXT, %d/%d = %d "
             "vertices)\n",
             BYTECOUNT_BYTES, BYTECOUNT_STRIDE,
             BYTECOUNT_BYTES / BYTECOUNT_STRIDE);
   if (indirect_mode)
      printf("       + indirect (vkCmdDrawIndirect%s)\n",
             multidraw_mode ? ", drawCount=2" : "");
   if (backward_mode)
      printf("       + backward (draw -> End writeback -> resuming Begin -> "
             "DrawIndirectByteCount, all in one render pass)\n");

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
   PFN_vkCmdDrawIndexedIndirect cmd_draw_indexed_indirect =
      GDPA(vkCmdDrawIndexedIndirect);
   PFN_vkCmdDrawIndirectByteCountEXT cmd_draw_byte_count =
      GDPA(vkCmdDrawIndirectByteCountEXT);
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
      .size = strip_mode ? sizeof(VERTICES_STRIP) : sizeof(VERTICES),
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
   if (strip_mode) {
      memcpy(vbo_mapped, VERTICES_STRIP, sizeof(VERTICES_STRIP));
      printf("  wrote %zu bytes of vertex data (%d x vec2, strip)\n",
             sizeof(VERTICES_STRIP), STRIP_VERTS);
   } else {
      memcpy(vbo_mapped, VERTICES, sizeof(VERTICES));
      printf("  wrote %zu bytes of vertex data (3 x vec2)\n", sizeof(VERTICES));
   }

   /* -------------------------------------------------------- counter buffer */
   VkBuffer counter_buf = VK_NULL_HANDLE;
   VkDeviceMemory counter_memory = VK_NULL_HANDLE;
   uint32_t *counter_mapped = NULL;
   if (resume_mode || bytecount_mode || backward_mode) {
      printf("\n=== counter buffer: 1 x uint32, host-visible ===\n");

      VkBufferCreateInfo cbci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = sizeof(uint32_t),
         .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
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

      /* backward_mode deliberately leaves this at 0: nothing but the
       * driver's own End(counter) writeback may ever produce its value.
       */
      *counter_mapped =
         bytecount_mode ? BYTECOUNT_BYTES : resume_mode ? RESUME_START_BYTES : 0;
      if (backward_mode)
         printf("  counter buffer left unseeded (0) - only the driver's own\n"
                "  writeback may set it\n");
      else
         printf("  seeded counter buffer with %u bytes (%u vertices)\n",
                *counter_mapped, *counter_mapped / 16);
   }

   /* ------------------------------------------------------- indirect buffer */
   VkBuffer indirect_buf = VK_NULL_HANDLE;
   VkDeviceMemory indirect_memory = VK_NULL_HANDLE;
   if (indirect_mode) {
      printf("\n=== indirect buffer: VkDrawIndirectCommand, host-visible ===\n");

      VkBufferCreateInfo ibci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = (VkDeviceSize)MULTIDRAW_COUNT *
                 (indexed_mode ? sizeof(VkDrawIndexedIndirectCommand)
                               : sizeof(VkDrawIndirectCommand)),
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

      void *icmd = NULL;
      r = map_mem(device, indirect_memory, 0, VK_WHOLE_SIZE, 0, &icmd);
      check(r == VK_SUCCESS, "vkMapMemory (indirect buffer)");
      if (r != VK_SUCCESS)
         return 1;

      uint32_t ncmd = multidraw_mode ? MULTIDRAW_COUNT : 1;

      for (uint32_t k = 0; k < ncmd; k++) {
         if (indexed_mode) {
            VkDrawIndexedIndirectCommand *c =
               (VkDrawIndexedIndirectCommand *)icmd + k;
            c->indexCount = draw_index_count();
            c->instanceCount = instance_count();
            c->firstIndex = first_index();
            c->vertexOffset = 0;
            c->firstInstance = 0;
         } else {
            VkDrawIndirectCommand *c = (VkDrawIndirectCommand *)icmd + k;
            c->vertexCount = draw_vertex_count();
            c->instanceCount = instance_count();
            c->firstVertex = 0;
            c->firstInstance = 0;
         }
      }
      printf("  wrote %u command(s), %u vertices x %u instance(s) each\n",
             ncmd, draw_vertex_count(), instance_count());
   }

   /* ---------------------------------------------------------- index buffer */
   VkBuffer ibo = VK_NULL_HANDLE;
   VkDeviceMemory ibo_memory = VK_NULL_HANDLE;
   if (indexed_mode) {
      printf("\n=== index buffer: 3 x uint16 {2, 0, 1}, host-visible ===\n");

      VkBufferCreateInfo ibo_bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = firstindex_mode ? sizeof(INDICES_RESTART_PADDED)
                 : restart_mode ? sizeof(INDICES_RESTART)
                                : sizeof(INDICES),
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
      if (firstindex_mode) {
         memcpy(ibo_mapped, INDICES_RESTART_PADDED,
                sizeof(INDICES_RESTART_PADDED));
         printf("  wrote indices {3,3, 0,1,2, 0xFFFF, 1,2,3} - the leading\n"
                "  two must be skipped via firstIndex=%d\n", FIRSTINDEX_PAD);
      } else if (restart_mode) {
         memcpy(ibo_mapped, INDICES_RESTART, sizeof(INDICES_RESTART));
         printf("  wrote indices {0,1,2, 0xFFFF, 1,2,3} - two runs of three\n");
      } else {
         memcpy(ibo_mapped, INDICES, sizeof(INDICES));
         printf("  wrote indices {2, 0, 1} - a cyclic rotation, so the render\n"
                "  result must be unchanged while the capture order permutes\n");
      }
   }

   /* ------------------------------------------------------------ XFB buffer */
   printf("\n=== XFB buffer: 3 x vec4 (48 bytes), host-visible ===\n");

   const VkDeviceSize xfb_size =
      (resume_mode || multidraw_mode || strip_mode || backward_mode) &&
            !manydraws_mode
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
      .topology = fan_mode      ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                  : strip_mode ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      .primitiveRestartEnable = restart_mode ? VK_TRUE : VK_FALSE,
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

   if (backward_mode) {
      /* Pair 1: unseeded Begin -> draw -> End(counter), a plain writeback -
       * nothing has read the counter buffer yet, so this half is exactly
       * the no-counter path every other mode already exercises.
       */
      cmd_begin_xfb(cmdbuf, 0, 0, NULL, NULL);
      printf("  vkCmdBeginTransformFeedbackEXT recorded (no counter buffer) "
             "[pair 1]\n");

      cmd_draw(cmdbuf, BASE_VERTS, 1, 0, 0);
      printf("  vkCmdDraw(%d, 1, 0, 0) recorded [pair 1]\n", BASE_VERTS);

      VkDeviceSize czero = 0;
      cmd_end_xfb(cmdbuf, 0, 1, &counter_buf, &czero);
      printf("  vkCmdEndTransformFeedbackEXT recorded (counter buffer) "
             "[pair 1, writeback]\n");

      /* Same self-dependency barrier the CTS test records between the
       * writeback and the resuming Begin.
       */
      VkMemoryBarrier tfc_barrier = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
         .dstAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT,
      };
      cmd_barrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &tfc_barrier, 0,
                 NULL, 0, NULL);
      printf("  vkCmdPipelineBarrier recorded (TRANSFORM_FEEDBACK -> "
             "DRAW_INDIRECT)\n");

      /* Pair 2: resume from the SAME counter buffer pair 1 just wrote, then
       * immediately read it back via vkCmdDrawIndirectByteCountEXT - the
       * backward dependency under test. Nothing outside the driver ever
       * touches counter_buf between the writeback above and this read.
       */
      cmd_begin_xfb(cmdbuf, 0, 1, &counter_buf, &czero);
      printf("  vkCmdBeginTransformFeedbackEXT recorded (counter buffer) "
             "[pair 2, resume]\n");

      cmd_draw_byte_count(cmdbuf, 1, 0, counter_buf, 0, 0, BYTECOUNT_STRIDE);
      printf("  vkCmdDrawIndirectByteCountEXT recorded (stride %d) "
             "[pair 2]\n", BYTECOUNT_STRIDE);

      cmd_end_xfb(cmdbuf, 0, 0, NULL, NULL);
      printf("  vkCmdEndTransformFeedbackEXT recorded [pair 2]\n");

      goto record_done;
   }

   if (resume_mode) {
      VkDeviceSize czero = 0;
      cmd_begin_xfb(cmdbuf, 0, 1, &counter_buf, &czero);
      printf("  vkCmdBeginTransformFeedbackEXT recorded (counter buffer)\n");
   } else {
      cmd_begin_xfb(cmdbuf, 0, 0, NULL, NULL);
      printf("  vkCmdBeginTransformFeedbackEXT recorded (no counter buffer)\n");
   }

   if (bytecount_mode) {
      cmd_draw_byte_count(cmdbuf, instance_count(), 0, counter_buf, 0, 0,
                          BYTECOUNT_STRIDE);
      printf("  vkCmdDrawIndirectByteCountEXT recorded (stride %d)\n",
             BYTECOUNT_STRIDE);
   } else if (indirect_mode && indexed_mode) {
      cmd_bind_ibo(cmdbuf, ibo, 0, VK_INDEX_TYPE_UINT16);
      printf("  vkCmdBindIndexBuffer recorded (UINT16)\n");
      uint32_t ncmd = multidraw_mode ? MULTIDRAW_COUNT : 1;
      cmd_draw_indexed_indirect(cmdbuf, indirect_buf, 0, ncmd,
                                sizeof(VkDrawIndexedIndirectCommand));
      printf("  vkCmdDrawIndexedIndirect(%u draw(s)) recorded\n", ncmd);
   } else if (indirect_mode) {
      uint32_t ncmd = multidraw_mode ? MULTIDRAW_COUNT : 1;
      cmd_draw_indirect(cmdbuf, indirect_buf, 0, ncmd,
                        sizeof(VkDrawIndirectCommand));
      printf("  vkCmdDrawIndirect(%u draw(s)) recorded\n", ncmd);
   } else if (indexed_mode) {
      cmd_bind_ibo(cmdbuf, ibo, 0, VK_INDEX_TYPE_UINT16);
      printf("  vkCmdBindIndexBuffer recorded (UINT16)\n");
      uint32_t nidx = draw_index_count();
      for (uint32_t d = 0; d < draw_repeat_count(); d++)
         cmd_draw_indexed(cmdbuf, nidx, instance_count(), first_index(), 0, 0);
      printf("  vkCmdDrawIndexed(%u, %u, 0, 0, 0) recorded x%u\n", nidx,
             instance_count(), draw_repeat_count());
   } else {
      uint32_t nverts = draw_vertex_count();
      for (uint32_t d = 0; d < draw_repeat_count(); d++)
         cmd_draw(cmdbuf, nverts, instance_count(), 0, 0);
      printf("  vkCmdDraw(%u, %u, 0, 0) recorded x%u\n", nverts,
             instance_count(), draw_repeat_count());
   }

   if (resume_mode) {
      VkDeviceSize czero = 0;
      cmd_end_xfb(cmdbuf, 0, 1, &counter_buf, &czero);
      printf("  vkCmdEndTransformFeedbackEXT recorded (counter buffer)\n");
   } else {
      cmd_end_xfb(cmdbuf, 0, 0, NULL, NULL);
      printf("  vkCmdEndTransformFeedbackEXT recorded\n");
   }

record_done:
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
      if (strip_mode)
         check(tri_px > 0 && clear_px > 0,
               "strip rendered (two triangles, so not the 190/66 split)");
      else
         check(clear_px == 190 && tri_px == 66,
               "render is unaffected by the XFB-capture shader variant");

      printf("\n=== readback: XFB buffer ===\n");
      float captured[MAX_READBACK_VERTS][4];
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

      if (backward_mode) {
         /* Pair 1's 3 vertices, then pair 2's - captured via
          * vkCmdDrawIndirectByteCountEXT reading the counter buffer pair 1's
          * End just wrote, entirely within one render pass and before
          * CmdEndRendering ever runs. If the driver's synchronization for
          * that backward dependency is broken, pair 2 reads a stale/zero
          * counter and either draws nothing (region 2 stays poison) or the
          * wrong count.
          */
         bool region1_ok = memcmp(raw, EXPECTED_XFB, sizeof(EXPECTED_XFB)) == 0;
         bool region2_ok =
            memcmp(raw + sizeof(EXPECTED_XFB), EXPECTED_XFB,
                   sizeof(EXPECTED_XFB)) == 0;
         bool region2_poison = true;
         for (size_t i = sizeof(EXPECTED_XFB); i < 2 * sizeof(EXPECTED_XFB); i++)
            region2_poison = region2_poison && raw[i] == 0x11;

         check(region1_ok, "pair 1 captured the expected triangle");
         check(region2_ok, "pair 2 (byte-count draw) captured the expected "
                           "triangle");
         if (!region2_ok && region2_poison)
            printf("  NOTE: pair 2's region is untouched poison - the "
                   "byte-count draw drew nothing, meaning it read the "
                   "counter buffer as 0 or its vertex count as 0.\n");

         uint32_t final_counter = *counter_mapped;
         uint32_t want_counter = (uint32_t)sizeof(EXPECTED_XFB);
         printf("  counter buffer (final) = %u (expected %u)\n",
                final_counter, want_counter);
         check(final_counter == want_counter,
               "counter buffer holds pair 1's writeback after full "
               "completion");
         if (final_counter == want_counter && !region2_ok)
            printf("  NOTE: the writeback landed correctly by the time the "
                   "host reads it, but pair 2 still failed - this is a "
                   "genuine mid-batch visibility gap, not a broken "
                   "writeback.\n");
         else if (final_counter != want_counter)
            printf("  NOTE: the writeback itself is wrong even after full "
                   "completion - not a visibility gap, the write position "
                   "advance or the writeback copy is broken.\n");

         goto xfb_done;
      }

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

      if (restart_mode) {
         bool ok = memcmp(raw, EXPECTED_XFB_RESTART,
                          sizeof(EXPECTED_XFB_RESTART)) == 0;
         float(*got)[4] = (float(*)[4])raw;

         for (int v = 0; v < 6; v++)
            printf("  captured[%d] = (%.3f, %.3f) expected (%.3f, %.3f)\n", v,
                   got[v][0], got[v][1], EXPECTED_XFB_RESTART[v][0],
                   EXPECTED_XFB_RESTART[v][1]);

         check(ok, "restart split the strip into two runs correctly");
         goto xfb_done;
      }

      if (strip_mode) {
         /* Six captured vertices from four drawn: the strip's shared vertices
          * are emitted once per triangle that uses them, and triangle 1 swaps
          * its first two inputs to keep winding.
          */
         const float(*want)[4] =
            fan_mode ? EXPECTED_XFB_FAN : EXPECTED_XFB_STRIP;
         bool ok = memcmp(raw, want, sizeof(EXPECTED_XFB_STRIP)) == 0;
         float(*got)[4] = (float(*)[4])raw;

         for (int v = 0; v < 6; v++)
            printf("  captured[%d] = (%.3f, %.3f) expected (%.3f, %.3f)\n", v,
                   got[v][0], got[v][1], want[v][0], want[v][1]);

         check(ok, fan_mode
                      ? "fan captured 6 vertices in assembled-primitive order"
                      : "strip captured 6 vertices in assembled-primitive "
                        "order");

         if (!ok && memcmp(raw, VERTICES_STRIP, sizeof(VERTICES_STRIP)) == 0)
            printf("  NOTE: only the 4 input vertices were captured - the\n"
                   "  capture was sized by input vertices, not primitives.\n");

         goto xfb_done;
      }

      if (multidraw_mode) {
         /* Each command captures the whole triangle, so command k must land at
          * offset k * sizeof(EXPECTED_XFB). Anything else means the write
          * position did not advance between commands.
          */
         if (overflow_mode) {
            /* Neither command can fit a whole primitive in the bound 32
             * bytes, so the correct result is that nothing was captured.
             * Transform feedback drops whole primitives rather than
             * truncating them, so this must run *instead of* the
             * per-command comparison below, not after it.
             */
            bool nothing_captured = true;
            for (size_t i = 0; i < (size_t)xfb_size; i++)
               nothing_captured = nothing_captured && raw[i] == 0x11;

            check(nothing_captured,
                  "multi-draw overflow: every primitive dropped whole");
            goto xfb_done;
         }

         const float(*want)[4] =
            indexed_mode ? EXPECTED_XFB_INDEXED : EXPECTED_XFB;
         bool all_ok = true;

         for (uint32_t k = 0; k < MULTIDRAW_COUNT; k++) {
            bool ok = memcmp(raw + k * sizeof(EXPECTED_XFB), want,
                             sizeof(EXPECTED_XFB)) == 0;
            printf("  command[%u] capture at offset %zu %s\n", k,
                   k * sizeof(EXPECTED_XFB), ok ? "ok" : "MISMATCH");
            all_ok = all_ok && ok;
         }
         check(all_ok, "each indirect command captured to its own region");

         bool second_untouched = true;
         for (size_t i = sizeof(EXPECTED_XFB); i < 2 * sizeof(EXPECTED_XFB); i++)
            second_untouched = second_untouched && raw[i] == 0x11;
         if (second_untouched)
            printf("  NOTE: the second half is untouched poison - the write\n"
                   "  position did not advance between indirect commands.\n");

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
         /* One triangle per instance, per indirect command. Multi-draw makes
          * the query span both commands, since it is scoped to the render
          * pass rather than to a single draw.
          */
         const uint64_t draws = (multidraw_mode ? MULTIDRAW_COUNT : 1) *
                                draw_repeat_count();
         const uint64_t want_generated =
            (uint64_t)draw_prim_count() * instance_count() * draws;

         /* "written" is what actually fit. Derive it from the bound buffer
          * rather than assuming everything did: --multidraw --instanced
          * deliberately generates more than the buffer holds, so the first
          * command fills it and the second is clamped away entirely. That
          * divergence is the whole point of the query.
          */
         const uint64_t capacity_prims =
            ((uint64_t)xfb_size / sizeof(EXPECTED_XFB[0])) / BASE_VERTS;
         const uint64_t want_written =
            overflow_mode ? 0
                          : (want_generated < capacity_prims ? want_generated
                                                             : capacity_prims);

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
   if (resume_mode || bytecount_mode || backward_mode) {
      destroy_buffer(device, counter_buf, NULL);
      free_mem(device, counter_memory, NULL);
   }
   if (indirect_mode) {
      destroy_buffer(device, indirect_buf, NULL);
      free_mem(device, indirect_memory, NULL);
   }

   printf("\n=== %d failure(s) ===\n", failures);
   if (failures == 0 && backward_mode)
      printf("\n=> VK_EXT_transform_feedback backward dependency works:\n"
             "   a vkCmdDrawIndirectByteCountEXT read the counter buffer an\n"
             "   earlier End in the same render pass wrote, entirely before\n"
             "   CmdEndRendering.\n");
   else if (failures == 0 && bytecount_mode)
      printf("\n=> VK_EXT_transform_feedback draw-by-byte-count works:\n"
             "   the vertex count came from a counter buffer and never\n"
             "   existed on the host, and its own capture round-tripped.\n");
   else if (failures == 0 && restart_mode)
      printf("\n=> VK_EXT_transform_feedback primitive restart works:\n"
             "   the index stream split into two runs, each assembled\n"
             "   independently with its own strip parity.\n");
   else if (failures == 0 && strip_mode)
      printf("\n=> VK_EXT_transform_feedback %s capture works:\n"
             "   4 input vertices assembled into 2 triangles and captured as\n"
             "   6 vertices in assembled-primitive order.\n",
             fan_mode ? "fan" : "strip");
   else if (failures == 0 && multidraw_mode)
      printf("\n=> VK_EXT_transform_feedback multi-draw indirect works:\n"
             "   each command captured to its own region, so the GPU-resident\n"
             "   write position advanced between them.\n");
   else if (failures == 0 && indirect_mode)
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
