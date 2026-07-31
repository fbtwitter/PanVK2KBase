#!/usr/bin/env python3
"""Let the kbase queue reuse panthor's per-subqueue init.

init_subqueue() sets up the GPU-side context a subqueue needs before it can
run anything: a panvk_cs_subqueue_context in device memory, the shared
syncobj array, the REQ_RESOURCE stream, and a small init command stream that
loads the context register and initialises the scoreboard slots. Without it a
queue is structurally valid but has never had its context set, so real work
against it produces wrong results rather than an error.

Unlike the queue lifecycle - which diverges at every ioctl, and therefore got
a whole sibling file in csf/panvk_vX_kbase_queue.c - this code *converges*
with panthor almost entirely. It is pool allocation and CS building, none of
which is driver-specific. It diverges at exactly three points:

  1. where the init stream is built (panthor uses the tiler heap's geometry
     buffer as scratch; kbase has no tiler heap descriptor yet, so it uses a
     dedicated allocation),
  2. how that stream is submitted and waited on (GROUP_SUBMIT +
     drmSyncobjWait vs. the kbase ring/kick/event-slot path), and
  3. init_queue()'s panthor-only steps - the render descriptor ringbuf and
     utrace, both of which need a DRM syncobj.

So this patches those three points rather than duplicating ~260 lines that
would immediately start drifting from upstream.

SCOPE: compute subqueue only. PANVK_SUBQUEUE_VERTEX_TILER and _FRAGMENT
additionally need the tiler heap descriptor, geometry buffer, scratch FBD and
render descriptor ringbuf; that is the next piece of work. init_queue() only
loops over PANVK_SUBQUEUE_COMPUTE on kbase for now.

Applied as a script rather than a diff because upstream moves. Idempotent.
Run after patch-panvk-kbase-queue.py.

Usage: patch-panvk-kbase-subqueue-init.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"

QUEUE_H = os.path.join(mesa, "src/panfrost/vulkan/csf/panvk_queue.h")
QUEUE_C = os.path.join(mesa, "src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c")

# ------------------------------------------------------------------ header
src = open(QUEUE_H).read()

if "kbase_init_cs" in src:
    print("    panvk_queue.h: already patched")
else:
    anchor = """   struct panvk_subqueue subqueues[PANVK_SUBQUEUE_COUNT];
};"""
    assert anchor in src, "panvk_gpu_queue subqueues member not found"

    src = src.replace(anchor, """   struct panvk_subqueue subqueues[PANVK_SUBQUEUE_COUNT];

   /* kbase only, unused on panthor. Scratch for building the per-subqueue
    * init command stream. panthor builds it in the tiler heap's geometry
    * buffer, which the kbase path does not have yet - it creates its heap
    * through CS_TILER_HEAP_INIT and has no descriptor for it.
    */
   struct panvk_priv_mem kbase_init_cs;
};""", 1)

    anchor = "VkResult panvk_per_arch(gpu_queue_check_status)(struct vk_queue *vk_queue);"
    assert anchor in src, "gpu_queue_check_status declaration not found"

    src = src.replace(anchor, anchor + """

/* Shared with the kbase queue (csf/panvk_vX_kbase_queue.c), which needs the
 * same per-subqueue GPU context set up before anything can run on it. Was
 * static; exported rather than copied, since none of what it does is
 * driver-specific.
 */
VkResult panvk_per_arch(init_gpu_queue)(struct panvk_gpu_queue *queue);

/* The matching teardown. Safe on a partially-initialised queue: every step
 * is guarded, so the kbase path can call it even though it never set up the
 * render ringbuf or utrace.
 */
void panvk_per_arch(cleanup_gpu_queue)(struct panvk_gpu_queue *queue);

/* Implemented by the kbase queue. Publishes an already-built init stream to
 * the subqueue's ring and blocks until the GPU has run it. The panthor
 * equivalent is GROUP_SUBMIT + drmSyncobjWait, which needs a DRM fd.
 */
VkResult panvk_per_arch(kbase_submit_and_wait)(struct panvk_gpu_queue *queue,
                                               enum panvk_subqueue_id subqueue,
                                               uint64_t stream_addr,
                                               uint32_t stream_size);""", 1)

    open(QUEUE_H, "w").write(src)
    print("    patched csf/panvk_queue.h")

# ------------------------------------------------------------------ source
src = open(QUEUE_C).read()

if "kbase_submit_and_wait" in src:
    print("    panvk_vX_gpu_queue.c: already patched")
else:
    # --- 1. the init stream's scratch buffer -------------------------------
    #
    # panthor puts it in the tiler heap descriptor's geometry buffer, at
    # +4096. On kbase that allocation does not exist, so point the builder at
    # the dedicated one instead. Same shape, different memory.
    old = """   /* We use the geometry buffer for our temporary CS buffer. */
   root_cs = (struct cs_buffer){
      .cpu = panvk_priv_mem_host_addr(queue->tiler_heap.desc) + 4096,
      .gpu = panvk_priv_mem_dev_addr(queue->tiler_heap.desc) + 4096,
      .capacity = 64 * 1024 / sizeof(uint64_t),
   };"""
    assert old in src, "init_subqueue root_cs setup not found"

    new = """   /* We use the geometry buffer for our temporary CS buffer - except on
    * kbase, which has no tiler heap descriptor to carve it out of and uses
    * a dedicated allocation instead. */
   const bool is_kbase =
      to_panvk_physical_device(dev->vk.physical)->is_kbase;
   struct panvk_priv_mem *init_cs_mem =
      is_kbase ? &queue->kbase_init_cs : &queue->tiler_heap.desc;
   const uint32_t init_cs_offset = is_kbase ? 0 : 4096;

   root_cs = (struct cs_buffer){
      .cpu = panvk_priv_mem_host_addr(*init_cs_mem) + init_cs_offset,
      .gpu = panvk_priv_mem_dev_addr(*init_cs_mem) + init_cs_offset,
      .capacity = 64 * 1024 / sizeof(uint64_t),
   };"""
    src = src.replace(old, new, 1)

    # The matching flush has to follow the same buffer.
    old = "   panvk_priv_mem_flush(queue->tiler_heap.desc, 4096, cs_root_chunk_size(&b));"
    assert old in src, "init_subqueue tiler_heap.desc flush not found"
    src = src.replace(
        old,
        "   panvk_priv_mem_flush(*init_cs_mem, init_cs_offset,\n"
        "                        cs_root_chunk_size(&b));", 1)

    # --- 2. submit and wait ------------------------------------------------
    #
    # Everything above this point in init_subqueue is driver-agnostic. This
    # is the only part that touches panthor ioctls.
    old = """   struct drm_panthor_sync_op syncop = {"""
    assert old in src, "init_subqueue syncop not found"

    new = """   if (is_kbase) {
      cs_builder_fini(&b);

      pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

      return panvk_per_arch(kbase_submit_and_wait)(
         queue, subqueue, cs_root_chunk_gpu_addr(&b), cs_root_chunk_size(&b));
   }

   struct drm_panthor_sync_op syncop = {"""
    src = src.replace(old, new, 1)

    # --- 3. init_queue: skip the panthor-only steps ------------------------
    #
    # init_render_desc_ringbuf() allocates a syncobj-backed ringbuf and
    # init_utrace() asserts on vk_sync_type_is_drm_syncobj(); neither can
    # work without a DRM fd. Both are only needed by the render subqueues,
    # which this pass does not initialise.
    old = """   result = init_render_desc_ringbuf(queue);
   if (result != VK_SUCCESS)
      goto err_cleanup_queue;

   result = init_utrace(queue);
   if (result != VK_SUCCESS)
      goto err_cleanup_queue;

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      result = init_subqueue(queue, i);
      if (result != VK_SUCCESS)
         goto err_cleanup_queue;
   }"""
    assert old in src, "init_queue body not found"

    new = """   const bool is_kbase =
      to_panvk_physical_device(dev->vk.physical)->is_kbase;

   /* Both of these need a DRM syncobj, which kbase has no fd to hang one
    * off, and both are only needed by the render subqueues - which are not
    * initialised on kbase yet. */
   if (!is_kbase) {
      result = init_render_desc_ringbuf(queue);
      if (result != VK_SUCCESS)
         goto err_cleanup_queue;

      result = init_utrace(queue);
      if (result != VK_SUCCESS)
         goto err_cleanup_queue;
   }

   /* kbase: compute only for now. VERTEX_TILER and FRAGMENT additionally
    * need the tiler heap descriptor, geometry buffer, scratch FBD and
    * render descriptor ringbuf. */
   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (is_kbase && i != PANVK_SUBQUEUE_COMPUTE)
         continue;

      result = init_subqueue(queue, i);
      if (result != VK_SUCCESS)
         goto err_cleanup_queue;
   }"""
    src = src.replace(old, new, 1)

    # --- 4. export init_queue ---------------------------------------------
    old = """static VkResult
init_queue(struct panvk_gpu_queue *queue)
{"""
    assert old in src, "init_queue definition not found"
    src = src.replace(old, """VkResult
panvk_per_arch(init_gpu_queue)(struct panvk_gpu_queue *queue)
{""", 1)

    # Its one existing caller, in create_gpu_queue().
    old = "   result = init_queue(queue);"
    assert old in src, "init_queue call not found"
    src = src.replace(old, "   result = panvk_per_arch(init_gpu_queue)(queue);", 1)

    # --- 5. export cleanup_queue ------------------------------------------
    old = """static void
cleanup_queue(struct panvk_gpu_queue *queue)
{"""
    assert old in src, "cleanup_queue definition not found"
    src = src.replace(old, """void
panvk_per_arch(cleanup_gpu_queue)(struct panvk_gpu_queue *queue)
{""", 1)

    # Its callers, in init_queue()'s error path and destroy_gpu_queue().
    assert "cleanup_queue(queue);" in src, "cleanup_queue calls not found"
    src = src.replace("cleanup_queue(queue);",
                      "panvk_per_arch(cleanup_gpu_queue)(queue);")

    open(QUEUE_C, "w").write(src)
    print("    patched csf/panvk_vX_gpu_queue.c")

print("panvk kbase subqueue-init patch applied")
