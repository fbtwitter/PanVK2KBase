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

if "init_gpu_queue" in src:
    print("    panvk_queue.h: already patched")
else:
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

/* The tiler heap descriptor, geometry buffer and scratch FBD. Everything in
 * it is shared; only the heap-create ioctl differs, which goes through
 * panvk_per_arch(kbase_create_tiler_heap) below.
 */
VkResult panvk_per_arch(init_gpu_tiler)(struct panvk_gpu_queue *queue);
void panvk_per_arch(cleanup_gpu_tiler)(struct panvk_gpu_queue *queue);

/* Implemented by the kbase queue. Publishes an already-built init stream to
 * the subqueue's ring and blocks until the GPU has run it. The panthor
 * equivalent is GROUP_SUBMIT + drmSyncobjWait, which needs a DRM fd.
 *
 * Takes the stream's CPU address because the kbase path copies it into a
 * ring buffer rather than pointing the GPU at it in place.
 */
VkResult panvk_per_arch(kbase_submit_and_wait)(struct panvk_gpu_queue *queue,
                                               enum panvk_subqueue_id subqueue,
                                               const void *stream,
                                               uint32_t stream_size);

/* Implemented by the kbase queue. CS_TILER_HEAP_INIT, which unlike
 * panthor's TILER_HEAP_CREATE needs no VM id and hands back both addresses
 * directly. Also records the heap VA so teardown can terminate it.
 */
int panvk_per_arch(kbase_create_tiler_heap)(struct panvk_gpu_queue *queue,
                                            uint32_t chunk_size,
                                            uint32_t initial_chunks,
                                            uint32_t max_chunks,
                                            uint64_t *heap_ctx_va,
                                            uint64_t *first_chunk_va);
void panvk_per_arch(kbase_destroy_tiler_heap)(struct panvk_gpu_queue *queue);""", 1)

    open(QUEUE_H, "w").write(src)
    print("    patched csf/panvk_queue.h")

# ------------------------------------------------------------------ source
src = open(QUEUE_C).read()

if "kbase_submit_and_wait" in src:
    print("    panvk_vX_gpu_queue.c: already patched")
else:
    # --- 1. the tiler heap ------------------------------------------------
    #
    # The descriptor, geometry buffer and scratch FBD allocations are all
    # shared. Only the heap-create ioctl differs: kbase's CS_TILER_HEAP_INIT
    # needs no VM id and returns both addresses directly, where panthor's
    # returns a handle as well.
    old = """   struct drm_panthor_tiler_heap_create thc = {
      .vm_id = pan_kmod_vm_handle(dev->kmod.vm),
      .chunk_size = tiler_heap->chunk_size,
      .initial_chunk_count = phys_dev->csf.tiler.initial_chunks,
      .max_chunks = phys_dev->csf.tiler.max_chunks,
      .target_in_flight = 65535,
   };

   int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE,
                            &thc);
   if (ret) {
      result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                            "Failed to create a tiler heap context");
      goto err_free_desc;
   }

   tiler_heap->context.handle = thc.handle;
   tiler_heap->context.dev_addr = thc.tiler_heap_ctx_gpu_va;

   panvk_priv_mem_write_desc(tiler_heap->desc, 0, TILER_HEAP, cfg) {
      cfg.size = tiler_heap->chunk_size;
      cfg.base = thc.first_heap_chunk_gpu_va;
      cfg.bottom = cfg.base + 64;
      cfg.top = cfg.base + cfg.size;
   }"""
    assert old in src, "init_tiler heap-create block not found"

    new = """   uint64_t heap_ctx_va, first_chunk_va;

   if (to_panvk_physical_device(dev->vk.physical)->is_kbase) {
      /* CS_TILER_HEAP_INIT. No VM id - kbase has no VM object - and no
       * handle either: the heap is identified by its GPU address, which is
       * also what cs_heap_set() wants. */
      if (panvk_per_arch(kbase_create_tiler_heap)(
             queue, tiler_heap->chunk_size, phys_dev->csf.tiler.initial_chunks,
             phys_dev->csf.tiler.max_chunks, &heap_ctx_va, &first_chunk_va)) {
         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                               "Failed to create a tiler heap context");
         goto err_free_desc;
      }

      tiler_heap->context.handle = 0;
   } else {
      struct drm_panthor_tiler_heap_create thc = {
         .vm_id = pan_kmod_vm_handle(dev->kmod.vm),
         .chunk_size = tiler_heap->chunk_size,
         .initial_chunk_count = phys_dev->csf.tiler.initial_chunks,
         .max_chunks = phys_dev->csf.tiler.max_chunks,
         .target_in_flight = 65535,
      };

      int ret = pan_kmod_ioctl(dev->drm_fd,
                               DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &thc);
      if (ret) {
         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                               "Failed to create a tiler heap context");
         goto err_free_desc;
      }

      tiler_heap->context.handle = thc.handle;
      heap_ctx_va = thc.tiler_heap_ctx_gpu_va;
      first_chunk_va = thc.first_heap_chunk_gpu_va;
   }

   tiler_heap->context.dev_addr = heap_ctx_va;

   panvk_priv_mem_write_desc(tiler_heap->desc, 0, TILER_HEAP, cfg) {
      cfg.size = tiler_heap->chunk_size;
      cfg.base = first_chunk_va;
      cfg.bottom = cfg.base + 64;
      cfg.top = cfg.base + cfg.size;
   }"""
    src = src.replace(old, new, 1)

    # Teardown: kbase terminates the heap by address, and has no handle.
    old = """   struct drm_panthor_tiler_heap_destroy thd = {
      .handle = tiler_heap->context.handle,
   };
   ASSERTED int ret =
      pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &thd);
   assert(!ret);"""
    assert old in src, "cleanup_tiler heap-destroy block not found"

    new = """   if (to_panvk_physical_device(dev->vk.physical)->is_kbase) {
      /* Terminated through the backend, by address - see
       * panvk_per_arch(kbase_create_tiler_heap)'s implementation. */
      panvk_per_arch(kbase_destroy_tiler_heap)(queue);
   } else {
      struct drm_panthor_tiler_heap_destroy thd = {
         .handle = tiler_heap->context.handle,
      };
      ASSERTED int ret = pan_kmod_ioctl(
         dev->drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &thd);
      assert(!ret);
   }"""
    src = src.replace(old, new, 1)

    # Export init_tiler/cleanup_tiler so the kbase queue can call them.
    old = """static VkResult
init_tiler(struct panvk_gpu_queue *queue)
{"""
    assert old in src, "init_tiler definition not found"
    src = src.replace(old, """VkResult
panvk_per_arch(init_gpu_tiler)(struct panvk_gpu_queue *queue)
{""", 1)

    old = "   result = init_tiler(queue);"
    assert old in src, "init_tiler call not found"
    src = src.replace(old, "   result = panvk_per_arch(init_gpu_tiler)(queue);", 1)

    old = """static void
cleanup_tiler(struct panvk_gpu_queue *queue)
{"""
    assert old in src, "cleanup_tiler definition not found"
    src = src.replace(old, """void
panvk_per_arch(cleanup_gpu_tiler)(struct panvk_gpu_queue *queue)
{""", 1)

    assert "cleanup_tiler(queue);" in src, "cleanup_tiler calls not found"
    src = src.replace("cleanup_tiler(queue);",
                      "panvk_per_arch(cleanup_gpu_tiler)(queue);")

    # --- 2. submit and wait ------------------------------------------------
    #
    # Everything above this point in init_subqueue is driver-agnostic. This
    # is the only part that touches panthor ioctls.
    old = """   struct drm_panthor_sync_op syncop = {"""
    assert old in src, "init_subqueue syncop not found"

    new = """   if (to_panvk_physical_device(dev->vk.physical)->is_kbase) {
      /* The stream was built in the geometry buffer, same as panthor; the
       * kbase path copies it into the subqueue's ring rather than pointing
       * the GPU at it in place, so it needs the CPU address. */
      const void *stream =
         (uint8_t *)panvk_priv_mem_host_addr(queue->tiler_heap.desc) + 4096;
      uint32_t stream_size = cs_root_chunk_size(&b);

      cs_builder_fini(&b);

      pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

      return panvk_per_arch(kbase_submit_and_wait)(queue, subqueue, stream,
                                                   stream_size);
   }

   struct drm_panthor_sync_op syncop = {"""
    src = src.replace(old, new, 1)

    # --- 3. init_queue: skip the steps kbase cannot do yet -----------------
    #
    # init_utrace() asserts vk_sync_type_is_drm_syncobj(), which kbase has no
    # fd to hang one off.
    #
    # init_render_desc_ringbuf() is blocked for a different and less obvious
    # reason. Its syncobj is a panvk_cs_sync32 in device memory, not a DRM
    # syncobj, so that part is fine - but it maps one BO at *two* adjacent
    # GPU VAs so a read running off the end wraps into the copy. kbase's
    # vm_bind cannot do that: an allocation lives where MEM_ALLOC_EX put it
    # and cannot be mapped elsewhere or twice, so both MAP ops fail the
    # caller-chosen-VA check. KBASE_IOCTL_MEM_ALIAS (nr 21) is the
    # mechanism that could express it - stride plus N entries referencing
    # the same handle - but it is unproven on this device, so the render
    # subqueues wait on that.
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
