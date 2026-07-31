/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 */

/* PanVK GPU queue for kbase devices.
 *
 * csf/panvk_vX_gpu_queue.c is written against panthor throughout:
 * DRM_IOCTL_PANTHOR_GROUP_CREATE / _DESTROY / _SUBMIT / _GET_STATE,
 * _TILER_HEAP_CREATE / _DESTROY, and libdrm syncobjs on dev->drm_fd. None
 * of those exist on /dev/mali0, which is a misc character device.
 *
 * This is the kbase sibling. It is a separate file rather than a set of
 * dispatch points inside the panthor one because the two diverge at
 * essentially every ioctl, and threading ~13 conditionals through a
 * 1500-line file would be worse to read and far more fragile against
 * upstream movement. panvk_vX_device.c picks between them on
 * phys_dev->is_kbase. Same shape as Turnip's tu_knl_kgsl.cc sitting
 * alongside tu_knl_drm_msm.cc.
 *
 * The kbase ioctls themselves live in pan_kmod_kbase.c, because that is
 * the only translation unit built with the vendored kbase UAPI headers
 * and -DMALI_USE_CSF=1 (see meson.build.kbase.patch). This file drives
 * them through pan_kmod_kbase.h.
 *
 * STATUS - read before assuming this does more than it does:
 *
 *   Implemented: queue creation and teardown. Tiler heap, queue group,
 *   and one bound CS ring buffer per subqueue, all through ioctl
 *   sequences this repo has run on real hardware (tests/queue_group,
 *   tests/live_kick_probe). That is what vkCreateDevice needs to get past
 *   the queue.
 *
 *   Implemented: submission of an *empty* VkQueueSubmit. A submit carrying
 *   no command buffers builds a command stream whose entire content is one
 *   SYNC_SET64 per signalled sync, publishes it to the compute subqueue's
 *   ring and kicks. Confirmed on hardware (Poco X8 Pro, 5/5 runs): a
 *   VkFence passed to vkQueueSubmit comes back signalled by the GPU, which
 *   nothing on the CPU side of this path ever writes.
 *
 *   Implemented: the compute subqueue's GPU-side context. Queue creation
 *   calls panvk_per_arch(init_gpu_queue), which is panthor's own
 *   init_subqueue() path - the setup it does is pool allocation and CS
 *   building, none of it driver-specific, so it is shared rather than
 *   duplicated. Only the submit at the end of it is ours, through
 *   kbase_submit_and_wait() below. See patch-panvk-kbase-subqueue-init.py.
 *
 *   Implemented, NOT YET RUN ON HARDWARE: compute command buffers. A submit
 *   whose work all lands on PANVK_SUBQUEUE_COMPUTE builds a ring stream that
 *   CALLs each command buffer's own stream where it lies, ahead of the
 *   SYNC_SET64s. Every line of it is reasoned from the panthor path and from
 *   upstream's standalone compute runner; none of it has executed yet. Treat
 *   a first run as capable of faulting the GPU until it has not.
 *
 *   NOT implemented: the VERTEX_TILER and FRAGMENT subqueues' contexts.
 *   Those additionally need a tiler heap descriptor, a geometry buffer, a
 *   scratch FBD and a render descriptor ringbuf, none of which this path
 *   builds - so init_gpu_queue() only loops over COMPUTE on kbase, and
 *   kbase_queue_submit() refuses a command buffer carrying work on either of
 *   them rather than running it against a zeroed context.
 *
 *   NOT implemented: GPU-side waits. vk_submit->waits are satisfied on the
 *   CPU before anything is published, so VK_SYNC_FEATURE_GPU_WAIT stays
 *   unadvertised and semaphores still cannot be created. Signalling from
 *   the GPU works; waiting on the GPU needs SYNC_WAIT64 in the stream.
 *
 *   KNOWN LIMITATION: submissions are serialised. A kick only lands on an
 *   idle CS, so every submit waits for CS_ACTIVE to clear first - see
 *   pan_kmod_kbase_queue_wait_idle() for the measurements behind that.
 */

#include "genxml/gen_macros.h"

#include <inttypes.h>
#include <string.h>

/* For struct drm_panthor_csif_info - panthor_kmod.h only forward-declares
 * it. On a kbase device the values come from pan_kmod_kbase_get_csif_props()
 * via the patched panthor_kmod_get_csif_props(), but the struct is still
 * panthor's.
 */
#include "drm-uapi/panthor_drm.h"

#include "genxml/cs_builder.h"

#include "panvk_cmd_buffer.h" /* struct panvk_cs_subqueue_context */
#include "panvk_device.h"
#include "panvk_kbase_sync.h"
#include "panvk_mempool.h"
#include "panvk_physical_device.h"
#include "panvk_queue.h"

#include "kmod/pan_kmod_kbase.h"
#include "kmod/panthor_kmod.h"

#include "util/macros.h"
#include "util/os_time.h"

#include "vk_log.h"
#include "vk_sync.h"

/* Ring buffer per subqueue. Sized to match what the panthor path uses for
 * its command stream ring; large enough that a submission does not wrap on
 * the first frame, small enough to be cheap per queue.
 */
#define PANVK_KBASE_RINGBUF_SIZE (64 * 1024)

/* Tiler heap geometry is no longer chosen here. It comes from
 * phys_dev->csf.tiler via the shared init_gpu_tiler(), which is what writes
 * the TILER_HEAP descriptor - picking it locally meant the descriptor could
 * describe a heap with a different chunk size than the one kbase actually
 * allocated.
 */

/* Upper bound on a submit's command stream. A submit whose computed bound
 * exceeds this is rejected rather than silently truncated - see
 * submit_stream_bound() for the accounting.
 */
#define PANVK_KBASE_MAX_SUBMIT_CS_SIZE 4096

/* Per-item instruction budgets for the ring stream this file builds, in
 * 8-byte CS instructions. Upper bounds, not exact counts: cs_move64_to()
 * emits one MOVE48 for a value below 2^48 and two MOVE32s otherwise, so
 * every 64-bit move is budgeted at 2.
 *
 *   REQ_RESOURCE (emitted at most once per subqueue, ever)
 *   flush: MOVE32 (flush id), FLUSH_CACHE2, WAIT
 *   per call: MOVE64 (address), MOVE32 (size), CALL
 *   per signal: MOVE64 (address), MOVE64 (value), SYNC_SET64
 */
#define PANVK_KBASE_REQ_RES_INSTRS  1
#define PANVK_KBASE_FLUSH_INSTRS    3
#define PANVK_KBASE_CALL_INSTRS     4
#define PANVK_KBASE_SIGNAL_INSTRS   5
#define PANVK_KBASE_EPILOGUE_INSTRS 4

/* Embeds panvk_gpu_queue rather than vk_queue directly, because the
 * per-subqueue GPU context setup is shared with panthor - see
 * patch-panvk-kbase-subqueue-init.py for why that half is not duplicated.
 * panvk_per_arch(init_gpu_queue) and its cleanup operate on the embedded
 * struct; everything below reaches back with container_of().
 *
 * gpu must stay first: it starts with the vk_queue that the vk_queue
 * callbacks container_of() on.
 */
struct panvk_kbase_queue {
   struct panvk_gpu_queue gpu;

   uint8_t group_handle;
   bool group_created;

   uint64_t tiler_heap_va;
   uint64_t tiler_first_chunk_va;

   struct pan_kmod_kbase_cs subqueues[PANVK_SUBQUEUE_COUNT];

   /* Total bytes ever published to each subqueue's ring - the CS_INSERT the
    * next kick will carry. Monotonic; the ring position is this modulo the
    * ring size. Kept here rather than read back from the user-IO page
    * because only this file writes it, and firmware never does.
    */
   uint64_t insert[PANVK_SUBQUEUE_COUNT];
};

static struct panvk_kbase_queue *
to_kbase_queue(struct vk_queue *vk_queue)
{
   return container_of(vk_queue, struct panvk_kbase_queue, gpu.vk);
}

static void
destroy_queue_resources(struct panvk_device *dev,
                        struct panvk_kbase_queue *queue)
{
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      pan_kmod_kbase_queue_destroy(dev->kmod.dev, &queue->subqueues[i]);

   if (queue->group_created) {
      pan_kmod_kbase_group_destroy(dev->kmod.dev, queue->group_handle);
      queue->group_created = false;
   }

   /* The tiler heap is not freed here - it belongs to the shared tiler
    * setup, and cleanup_gpu_tiler() terminates it via
    * panvk_per_arch(kbase_destroy_tiler_heap) along with the descriptor and
    * scratch FBD it allocated.
    */
}

int
panvk_per_arch(kbase_create_tiler_heap)(struct panvk_gpu_queue *gpu_queue,
                                        uint32_t chunk_size,
                                        uint32_t initial_chunks,
                                        uint32_t max_chunks,
                                        uint64_t *heap_ctx_va,
                                        uint64_t *first_chunk_va)
{
   struct panvk_kbase_queue *queue =
      container_of(gpu_queue, struct panvk_kbase_queue, gpu);
   struct panvk_device *dev = to_panvk_device(gpu_queue->vk.base.device);

   if (pan_kmod_kbase_tiler_heap_create(dev->kmod.dev, chunk_size,
                                        initial_chunks, max_chunks,
                                        &queue->tiler_heap_va,
                                        &queue->tiler_first_chunk_va))
      return -1;

   *heap_ctx_va = queue->tiler_heap_va;
   *first_chunk_va = queue->tiler_first_chunk_va;

   return 0;
}

void
panvk_per_arch(kbase_destroy_tiler_heap)(struct panvk_gpu_queue *gpu_queue)
{
   struct panvk_kbase_queue *queue =
      container_of(gpu_queue, struct panvk_kbase_queue, gpu);
   struct panvk_device *dev = to_panvk_device(gpu_queue->vk.base.device);

   if (!queue->tiler_heap_va)
      return;

   pan_kmod_kbase_tiler_heap_destroy(dev->kmod.dev, queue->tiler_heap_va);
   queue->tiler_heap_va = 0;
}

VkResult
panvk_per_arch(create_kbase_queue)(struct panvk_device *dev,
                                   const VkDeviceQueueCreateInfo *create_info,
                                   uint32_t queue_idx,
                                   struct vk_queue **out_queue)
{
   struct panvk_kbase_queue *queue =
      vk_zalloc(&dev->vk.alloc, sizeof(*queue), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!queue)
      return panvk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      vk_queue_init(&queue->gpu.vk, &dev->vk, create_info, queue_idx);
   if (result != VK_SUCCESS)
      goto err_free_queue;

   /* Tiler heap first: the panthor path does the same, and a group with no
    * heap behind it is not useful for anything that tiles.
    *
    * Shared with panthor, which also allocates the heap descriptor, the
    * geometry buffer that follows it and the scratch FBD the tiler-OOM
    * handler writes into. Only CS_TILER_HEAP_INIT itself is ours, through
    * kbase_create_tiler_heap() below - which is also what makes the heap's
    * geometry agree with phys_dev->csf.tiler, rather than with constants
    * this file used to pick on its own.
    */
   result = panvk_per_arch(init_gpu_tiler)(&queue->gpu);
   if (result != VK_SUCCESS)
      goto err_finish_queue;

   /* Priority 0 is BASE_QUEUE_GROUP_PRIORITY_HIGH in kbase's numbering,
    * but what actually matters is that it is the value every group this
    * repo has successfully created on hardware used. Mapping Vulkan global
    * priority onto kbase priorities properly is left for when submission
    * works and the difference is observable.
    */
   if (pan_kmod_kbase_group_create(dev->kmod.dev, 0, 0,
                                   &queue->group_handle)) {
      result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                            "kbase: failed to create the queue group");
      goto err_destroy_resources;
   }
   queue->group_created = true;

   /* One CS per subqueue, bound to consecutive CS interfaces of the group.
    * The firmware reports 8 streams per group (see
    * pan_kmod_kbase_get_csif_props), so PANVK_SUBQUEUE_COUNT of them fit.
    */
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (pan_kmod_kbase_queue_create(dev->kmod.dev, queue->group_handle, i,
                                      PANVK_KBASE_RINGBUF_SIZE,
                                      &queue->subqueues[i])) {
         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                               "kbase: failed to bind CS queue %u", i);
         goto err_destroy_resources;
      }
   }

   queue->gpu.vk.driver_submit = panvk_per_arch(kbase_queue_submit);

   /* Sets up the GPU-side context each subqueue needs before it can run
    * anything: the shared syncobj array, a panvk_cs_subqueue_context, and
    * an init stream that loads the context register and initialises the
    * scoreboard slots. Shared with panthor - only the submit at the end of
    * it is ours, via panvk_per_arch(kbase_submit_and_wait) below.
    *
    * Compute only for now; the render subqueues additionally need the
    * descriptor ringbuf, which is blocked on BO aliasing - see the comment
    * on that step in patch-panvk-kbase-subqueue-init.py.
    */
   result = panvk_per_arch(init_gpu_queue)(&queue->gpu);
   if (result != VK_SUCCESS)
      goto err_destroy_resources;

   *out_queue = &queue->gpu.vk;
   return VK_SUCCESS;

err_destroy_resources:
   destroy_queue_resources(dev, queue);

err_finish_queue:
   vk_queue_finish(&queue->gpu.vk);

err_free_queue:
   vk_free(&dev->vk.alloc, queue);
   return result;
}

void
panvk_per_arch(destroy_kbase_queue)(struct vk_queue *vk_queue)
{
   struct panvk_kbase_queue *queue = to_kbase_queue(vk_queue);
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);

   /* Frees the subqueue contexts and syncobjs init_gpu_queue() allocated.
    * Safe here even though the render half was never set up - every step in
    * it is guarded.
    */
   panvk_per_arch(cleanup_gpu_queue)(&queue->gpu);
   panvk_per_arch(cleanup_gpu_tiler)(&queue->gpu);

   destroy_queue_resources(dev, queue);
   vk_queue_finish(&queue->gpu.vk);
   vk_free(&dev->vk.alloc, queue);
}

/* cs_builder wants somewhere to go when a stream outgrows its buffer. This
 * one builds into a fixed staging buffer that is bounds-checked up front,
 * so overflow means the bound was computed wrong - a driver bug, not a
 * runtime condition to recover from.
 */
static struct cs_buffer
submit_cs_overflow(void *cookie)
{
   UNUSED struct panvk_kbase_queue *queue = cookie;

   assert(!"kbase submit command stream outgrew its staging buffer");
   return (struct cs_buffer){ 0 };
}

/* Wait for the GPU to consume enough of the ring that `size` more bytes
 * fit. CS_EXTRACT is written by firmware, so this is a load, not an ioctl.
 *
 * Polling rather than blocking on the fd: pan_kmod_kbase_read_event()
 * consumes a notification, and this queue is not the single owner of that
 * stream (see its header comment). Ring pressure also should not arise at
 * all until submissions carry real command buffers.
 */
static VkResult
wait_for_ring_space(struct panvk_device *dev,
                    struct panvk_kbase_queue *queue, unsigned subqueue,
                    uint32_t size)
{
   const struct pan_kmod_kbase_cs *cs = &queue->subqueues[subqueue];

   if (size > cs->ringbuf_size) {
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "kbase: %u-byte command stream exceeds the %" PRIu64
                          "-byte ring",
                          size, cs->ringbuf_size);
   }

   /* 2s, the same budget tests/live_kick_probe uses to decide a stream is
    * never going to run.
    */
   for (unsigned i = 0; i < 2000; i++) {
      uint64_t extract = pan_kmod_kbase_queue_extract(cs);

      assert(queue->insert[subqueue] >= extract);
      if (cs->ringbuf_size - (queue->insert[subqueue] - extract) >= size)
         return VK_SUCCESS;

      os_time_sleep(1000);
   }

   return panvk_errorf(dev, VK_ERROR_DEVICE_LOST,
                       "kbase: subqueue %u ring did not drain (insert=%" PRIu64
                       ", extract=%" PRIu64 ", active=%u)",
                       subqueue, queue->insert[subqueue],
                       pan_kmod_kbase_queue_extract(cs),
                       pan_kmod_kbase_queue_active(cs));
}

/* Copy a built stream into the ring at the current insert point and kick.
 *
 * The ring is circular in the units CS_INSERT counts, so a stream that runs
 * off the end continues at offset 0 - hence the two-part copy. The stream
 * is built in a staging buffer and copied rather than built in place
 * precisely so it can be split like this; that is only safe because a
 * stream this simple contains no references to its own address.
 */
static VkResult
submit_stream(struct panvk_device *dev, struct panvk_kbase_queue *queue,
              unsigned subqueue, const void *stream, uint32_t size)
{
   struct pan_kmod_kbase_cs *cs = &queue->subqueues[subqueue];

   if (!size)
      return VK_SUCCESS;

   VkResult result = wait_for_ring_space(dev, queue, subqueue, size);
   if (result != VK_SUCCESS)
      return result;

   uint64_t offset = queue->insert[subqueue] % cs->ringbuf_size;
   uint32_t first = MIN2(size, cs->ringbuf_size - offset);

   memcpy((uint8_t *)cs->ringbuf_cpu + offset, stream, first);
   if (first < size)
      memcpy(cs->ringbuf_cpu, (const uint8_t *)stream + first, size - first);

   /* A kick lands only on an idle CS - see pan_kmod_kbase_queue_wait_idle().
    * Waiting here rather than inside the kick keeps the backend a thin
    * mirror of the hardware and puts the policy where the submit is.
    *
    * 100ms: CS_ACTIVE was measured to clear ~30-40ms after a stream ends.
    * Timing out is not fatal - kick anyway and let the caller's own wait
    * report a stuck queue, which produces a better error than failing here
    * would.
    */
   if (!pan_kmod_kbase_queue_wait_idle(cs, 100)) {
      mesa_logw("kbase: subqueue %u still active before kick; "
                "the submit may not take effect", subqueue);
   }

   /* pan_kmod_kbase_queue_kick() barriers between these writes and the
    * kick, so no explicit ordering is needed here.
    */
   if (pan_kmod_kbase_queue_kick(dev->kmod.dev, cs,
                                 queue->insert[subqueue] + size)) {
      return panvk_errorf(dev, VK_ERROR_DEVICE_LOST,
                          "kbase: failed to kick subqueue %u", subqueue);
   }

   queue->insert[subqueue] += size;

   return VK_SUCCESS;
}

/* Publish an init stream that panvk_per_arch(init_gpu_queue) already built
 * into the tiler heap's geometry buffer, and block until the GPU has run it.
 *
 * Blocking is the point: the stream sets up context registers that
 * everything submitted afterwards depends on, so returning before it has
 * executed would let real work run against an uninitialised context -
 * exactly the state this exists to remove.
 *
 * Completion is CS_EXTRACT reaching the end of the stream, not an event
 * slot. The stream is built by shared code that knows nothing about kbase
 * event memory, and "the GPU has read every byte" is a sufficient signal
 * for a stream whose instructions are all synchronous register writes.
 */
VkResult
panvk_per_arch(kbase_submit_and_wait)(struct panvk_gpu_queue *gpu_queue,
                                      enum panvk_subqueue_id subqueue,
                                      const void *stream, uint32_t stream_size)
{
   struct panvk_kbase_queue *queue =
      container_of(gpu_queue, struct panvk_kbase_queue, gpu);
   struct panvk_device *dev = to_panvk_device(gpu_queue->vk.base.device);
   struct pan_kmod_kbase_cs *cs = &queue->subqueues[subqueue];

   if (!stream_size)
      return VK_SUCCESS;

   VkResult result =
      submit_stream(dev, queue, subqueue, stream, stream_size);
   if (result != VK_SUCCESS)
      return result;

   uint64_t target = queue->insert[subqueue];

   /* 2s, the budget the standalone probes use before calling a stream
    * never-going-to-run.
    */
   for (unsigned i = 0; i < 2000; i++) {
      if (pan_kmod_kbase_queue_extract(cs) >= target)
         return VK_SUCCESS;

      os_time_sleep(1000);
   }

   return panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                       "kbase: subqueue %u init stream never ran "
                       "(insert=%" PRIu64 ", extract=%" PRIu64 ", active=%u)",
                       subqueue, target, pan_kmod_kbase_queue_extract(cs),
                       pan_kmod_kbase_queue_active(cs));
}

/* Collect the command-buffer streams this submit should CALL, and reject
 * anything this driver cannot honestly run.
 *
 * Only PANVK_SUBQUEUE_COMPUTE has a GPU-side context - init_gpu_queue()
 * loops over that one alone on kbase, because the render subqueues also
 * need the descriptor ringbuf that BO aliasing cannot express here. Work
 * recorded against a render subqueue would execute against a zeroed
 * context, so it is refused rather than run.
 *
 * The streams are not copied into the ring. Each is CALLed at the address
 * the command buffer built it at, which is what panthor's kernel does with
 * stream_addr/stream_size, and it is not merely an optimisation: a
 * cs_builder stream that outgrew its first chunk contains absolute
 * addresses linking chunk to chunk, so a stream relocated by a ring copy
 * would jump back to the original chunk. Only streams built to be executed
 * where they lie survive that, and these are not.
 */
struct kbase_submit_calls {
   struct {
      uint64_t addr;
      uint32_t size;
   } entries[PANVK_KBASE_MAX_SUBMIT_CS_SIZE / (8 * PANVK_KBASE_CALL_INSTRS)];
   uint32_t count;

   /* OR of the resource masks the collected streams ask for. */
   uint32_t req_resource_mask;
};

static VkResult
collect_cmdbuf_calls(struct panvk_device *dev,
                     const struct vk_queue_submit *vk_submit,
                     struct kbase_submit_calls *calls)
{
   for (uint32_t i = 0; i < vk_submit->command_buffer_count; i++) {
      struct panvk_cmd_buffer *cmdbuf = container_of(
         vk_submit->command_buffers[i], struct panvk_cmd_buffer, vk);

      for (uint32_t j = 0; j < ARRAY_SIZE(cmdbuf->state.cs); j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);

         /* A builder that went invalid recorded a failure that
          * vkEndCommandBuffer already reported. Running its partial stream
          * would be worse than refusing it.
          */
         if (!cs_is_valid(b)) {
            return panvk_errorf(dev, VK_ERROR_DEVICE_LOST,
                                "kbase: command buffer %u has an invalid "
                                "stream on subqueue %u",
                                i, j);
         }

         if (cs_is_empty(b))
            continue;

         if (j != PANVK_SUBQUEUE_COMPUTE) {
            /* Logged as well as returned: panvk_errorf()'s message goes to a
             * debug messenger that these probes do not install, so the
             * VkResult would otherwise arrive with no explanation.
             */
            mesa_logw("kbase: command buffer %u has a %u-byte stream on "
                      "subqueue %u, which has no GPU-side context yet",
                      i, cs_root_chunk_size(b), j);
            return panvk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                                "kbase: command buffer %u carries work on "
                                "subqueue %u, which has no GPU-side context "
                                "yet (compute only)",
                                i, j);
         }

         if (calls->count >= ARRAY_SIZE(calls->entries)) {
            return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                "kbase: more than %zu command streams in one "
                                "submit",
                                ARRAY_SIZE(calls->entries));
         }

         calls->entries[calls->count].addr = cs_root_chunk_gpu_addr(b);
         calls->entries[calls->count].size = cs_root_chunk_size(b);
         calls->count++;
         calls->req_resource_mask |= b->req_resource_mask;
      }
   }

   return VK_SUCCESS;
}

VkResult
panvk_per_arch(kbase_queue_submit)(struct vk_queue *vk_queue,
                                   struct vk_queue_submit *vk_submit)
{
   struct panvk_kbase_queue *queue = to_kbase_queue(vk_queue);
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct panvk_subqueue *subq =
      &queue->gpu.subqueues[PANVK_SUBQUEUE_COMPUTE];

   struct kbase_submit_calls calls = { 0 };
   VkResult result = collect_cmdbuf_calls(dev, vk_submit, &calls);
   if (result != VK_SUCCESS)
      return result;

   /* No GPU-side wait exists yet - panvk_kbase_sync deliberately withholds
    * VK_SYNC_FEATURE_GPU_WAIT - so waits are satisfied on the CPU before
    * anything is published. Correct, just pessimistic: it serialises the
    * queue thread against the wait. In practice this loop does nothing,
    * because a semaphore cannot currently be created at all.
    */
   for (uint32_t i = 0; i < vk_submit->wait_count; i++) {
      result = vk_sync_wait(&dev->vk, vk_submit->waits[i].sync,
                            vk_submit->waits[i].wait_value,
                            VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!calls.count && !vk_submit->signal_count)
      return VK_SUCCESS;

   /* Resources the subqueue has not asked the firmware for yet. Tracked on
    * the shared panvk_subqueue exactly as the panthor path tracks it, so
    * REQ_RESOURCE is emitted once rather than on every submit.
    */
   const uint32_t new_resources =
      calls.req_resource_mask & ~subq->req_resource.mask;

   const uint32_t bound_instrs =
      (new_resources ? PANVK_KBASE_REQ_RES_INSTRS : 0) +
      (calls.count ? PANVK_KBASE_FLUSH_INSTRS : 0) +
      calls.count * PANVK_KBASE_CALL_INSTRS +
      vk_submit->signal_count * PANVK_KBASE_SIGNAL_INSTRS +
      PANVK_KBASE_EPILOGUE_INSTRS;

   if (bound_instrs * 8 > PANVK_KBASE_MAX_SUBMIT_CS_SIZE) {
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "kbase: submit needs up to %u bytes of command "
                          "stream, over the %u-byte staging buffer",
                          bound_instrs * 8, PANVK_KBASE_MAX_SUBMIT_CS_SIZE);
   }

   uint8_t stream[PANVK_KBASE_MAX_SUBMIT_CS_SIZE];
   const struct drm_panthor_csif_info *csif_info =
      panthor_kmod_get_csif_props(dev->kmod.dev);

   struct cs_buffer root_cs = {
      .cpu = (void *)stream,
      /* Where it will land in the ring. Nothing in this stream refers to
       * its own address, so this only has to be plausible, but pointing it
       * at the real destination keeps any future self-reference honest.
       */
      .gpu = queue->subqueues[PANVK_SUBQUEUE_COMPUTE].ringbuf_gpu_va +
             queue->insert[PANVK_SUBQUEUE_COMPUTE] %
                queue->subqueues[PANVK_SUBQUEUE_COMPUTE].ringbuf_size,
      .capacity = sizeof(stream) / sizeof(uint64_t),
   };
   struct cs_builder_conf conf = {
      .nr_registers = csif_info->cs_reg_count,
      .nr_kernel_registers = MAX2(csif_info->unpreserved_cs_reg_count, 4),
      .alloc_buffer = submit_cs_overflow,
      .cookie = queue,
   };

   struct cs_builder b;
   cs_builder_init(&b, &conf, root_cs);

   /* Scratch registers throughout, never cs_reg*() directly. The register
    * file is shared with the command-buffer streams CALLed below: registers
    * up to PANVK_CS_REG_SCRATCH_END are theirs to clobber, but the progress
    * seqnos and the subqueue context pointer live above it and must survive
    * between submits - init_gpu_queue() is what loaded the context pointer,
    * and every command buffer dereferences it.
    *
    * The CALLed streams clobber the scratch registers too (finish_cs() uses
    * scratch 0-2 for its error check), so anything needed after a CALL is
    * reloaded rather than assumed to have survived it.
    */
   struct cs_index scratch32 = cs_scratch_reg32(&b, 0);
   struct cs_index addr_reg = cs_scratch_reg64(&b, 2);
   struct cs_index val_reg = cs_scratch_reg64(&b, 4);

   if (new_resources)
      cs_req_res(&b, new_resources | subq->req_resource.mask);

   if (calls.count) {
      /* Invalidate before reading anything the CPU just wrote.
       *
       * On panthor the kernel emits this ahead of the CALL and uses the
       * submit's latest_flush to let the hardware skip it when the caches
       * are known clean. Nothing does that here: kbase has no kernel in the
       * submit path at all, since userspace writes the ring and rings the
       * doorbell itself. So the flush has to be in the stream, and the
       * flush id is 0 - "never already flushed" - which always flushes.
       *
       * It has to invalidate, not just clean. Command-buffer and descriptor
       * memory is recycled through the command pool, so the GPU's caches
       * can hold valid lines for an address the CPU has since rewritten
       * with a different allocation's contents. Cleaning writes back dirty
       * lines but leaves them valid, which is the half PanVK already emits
       * at the end of every command buffer for the opposite hazard.
       *
       * Modes match src/panfrost/compiler/kraid/hw_runner, the standalone
       * compute runner upstream, which is the closest thing to this stream
       * that already runs on hardware.
       */
      cs_move32_to(&b, scratch32, 0);
      cs_flush_caches(&b, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                      MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                      MALI_CS_OTHER_FLUSH_MODE_INVALIDATE, scratch32,
                      cs_defer(SB_IMM_MASK, SB_ID(IMM_FLUSH)));
      cs_wait_slot(&b, SB_ID(IMM_FLUSH));
   }

   /* CALL, not JUMP: the stream has to come back here so the signals below
    * still run. Safe to signal straight after, because finish_cs() opens
    * with cs_wait_slots(all_mask) - a command buffer's stream waits for its
    * own outstanding work before it ends, so the CALL returning means the
    * work is done, not merely issued.
    */
   for (uint32_t i = 0; i < calls.count; i++) {
      cs_move64_to(&b, addr_reg, calls.entries[i].addr);
      cs_move32_to(&b, scratch32, calls.entries[i].size);
      cs_call(&b, addr_reg, scratch32);
   }

   /* One SYNC_SET64 per signalled sync, at system scope so the write lands
    * where the CPU can see it - CSG scope would keep it inside the group.
    * SYNC_SET64 takes register indices, not immediates, so the address and
    * value go through registers first. Same shape as tests/event_slot_probe
    * and as Panfork's 0x48/0x4a pair.
    */
   for (uint32_t i = 0; i < vk_submit->signal_count; i++) {
      struct vk_sync *sync = vk_submit->signals[i].sync;

      if (sync->type != &phys_dev->kbase_sync_type.base) {
         cs_builder_fini(&b);
         return panvk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                             "kbase: cannot signal a sync of a foreign type");
      }

      /* Binary syncs come through with value 0 meaning "signalled", the
       * same convention panvk_kbase_sync_signal() applies on the CPU side.
       */
      uint64_t value = vk_submit->signals[i].signal_value;
      if (!(sync->flags & VK_SYNC_IS_TIMELINE))
         value = 1;

      cs_move64_to(&b, addr_reg, panvk_kbase_sync_slot_gpu_va(sync));
      cs_move64_to(&b, val_reg, value);
      cs_sync64_set(&b, false, MALI_CS_SYNC_SCOPE_SYSTEM, val_reg, addr_reg,
                    cs_now());
   }

   cs_end(&b);

   if (!cs_is_valid(&b)) {
      cs_builder_fini(&b);
      return panvk_errorf(dev, VK_ERROR_DEVICE_LOST,
                          "kbase: built an invalid command stream");
   }

   uint32_t size = cs_root_chunk_size(&b);
   cs_builder_fini(&b);

   assert(size <= bound_instrs * 8);

   /* The compute subqueue: the only one with a context, which
    * collect_cmdbuf_calls() has already enforced for the command buffers.
    */
   result = submit_stream(dev, queue, PANVK_SUBQUEUE_COMPUTE, stream, size);
   if (result != VK_SUCCESS)
      return result;

   /* Only once the stream carrying it has actually been published. Recording
    * the request before the submit could fail would leave the subqueue
    * believing it holds resources it never asked for.
    */
   subq->req_resource.mask |= new_resources;

   return VK_SUCCESS;
}

VkResult
panvk_per_arch(kbase_queue_check_status)(struct vk_queue *vk_queue)
{
   struct panvk_kbase_queue *queue = to_kbase_queue(vk_queue);
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);

   /* The CS-side fault check is driver-agnostic: last_error is written into
    * the subqueue context by the command stream itself, so it reads the
    * same on kbase as on panthor.
    *
    * Only the subqueues that were actually initialised, though. An
    * uninitialised context is a zeroed pool allocation, so it would report
    * last_error == 0 - a clean status it has no business reporting, since
    * nothing ever ran there. Checking it would turn "never initialised"
    * into "definitely fine", which is exactly the kind of false clean this
    * driver should not manufacture.
    */
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (i != PANVK_SUBQUEUE_COMPUTE)
         continue;

      if (!panvk_priv_mem_check_alloc(queue->gpu.subqueues[i].context))
         continue;

      panvk_priv_mem_readback(queue->gpu.subqueues[i].context, 0,
                              struct panvk_cs_subqueue_context, subq_ctx) {
         if (subq_ctx->last_error != 0) {
            return vk_queue_set_lost(&queue->gpu.vk,
                                     "kbase: CS fault on subqueue %u "
                                     "(last_error=0x%x)",
                                     i, subq_ctx->last_error);
         }
      }
   }

   /* kbase has no GROUP_GET_STATE. A group killed by a fault reports
    * through a GPU_QUEUE_GROUP_ERROR notification on the device fd
    * instead, so drain what is pending. Timeout 0: this is a status poll,
    * not a wait, and it is called on paths that must not block.
    *
    * THIS MAKES check_status THE SINGLE OWNER of the notification stream -
    * read() consumes a notification, so whoever reads it first is the only
    * one who sees it (see pan_kmod_kbase_read_event's header). That works
    * today because nothing else reads the fd: panvk_kbase_sync waits by
    * polling event-slot memory. If that ever changes to blocking on the
    * fd, the two have to be reconciled rather than both reading.
    *
    * The same caveat applies across queues on one device: a notification
    * for another group is consumed here and that queue never sees it.
    * There is one queue per device today, but this is the thing that
    * breaks first if that changes.
    */
   struct pan_kmod_kbase_event ev;
   int ret;

   while ((ret = pan_kmod_kbase_read_event(dev->kmod.dev, 0, &ev)) == 1) {
      if (ev.type != PAN_KMOD_KBASE_EVENT_GROUP_ERROR)
         continue;

      if (ev.group_handle != queue->group_handle) {
         mesa_logw("kbase: consumed a group-error notification for group %u "
                   "while checking group %u",
                   ev.group_handle, queue->group_handle);
         continue;
      }

      return vk_queue_set_lost(&queue->gpu.vk,
                               "kbase: GPU_QUEUE_GROUP_ERROR on group %u "
                               "(error_type=%u)",
                               ev.group_handle, ev.error_type);
   }

   /* A failed read is not itself a lost queue - it says the status could
    * not be determined, which is different from a fault. Log and report
    * clean rather than killing a device that may be fine.
    */
   if (ret < 0)
      mesa_logw("kbase: could not read device notifications for status");

   return VK_SUCCESS;
}
