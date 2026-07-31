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
 *   NOT implemented: the VERTEX_TILER and FRAGMENT subqueues' contexts.
 *   Those additionally need a tiler heap descriptor, a geometry buffer, a
 *   scratch FBD and a render descriptor ringbuf, none of which this path
 *   builds - so init_gpu_queue() only loops over COMPUTE on kbase.
 *   kbase_queue_submit() still refuses any submit carrying command buffers,
 *   because a render one would run against an uninitialised context.
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

/* Tiler heap geometry. Same values tests/live_kick_probe uses, which the
 * kernel accepts and which produce a real first chunk on this hardware.
 */
#define PANVK_KBASE_TILER_CHUNK_SIZE (2 * 1024 * 1024)
#define PANVK_KBASE_TILER_INITIAL_CHUNKS 1
#define PANVK_KBASE_TILER_MAX_CHUNKS 8

/* Upper bound on a submit's command stream. Each signalled sync costs a
 * MOVE64 for the address, a MOVE64 for the value and a SYNC_SET64, all
 * 8-byte instructions; the rest is slack for the stream epilogue. A submit
 * that would exceed this is rejected rather than silently truncated.
 */
#define PANVK_KBASE_MAX_SUBMIT_CS_SIZE 4096

/* Size of the buffer the per-subqueue init stream is built into. panthor
 * uses the tiler heap's 64KB geometry buffer for this; the streams are a few
 * hundred bytes at most, and this path has no tiler descriptor to borrow.
 */
#define PANVK_KBASE_INIT_CS_SIZE 4096

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

   if (queue->tiler_heap_va) {
      pan_kmod_kbase_tiler_heap_destroy(dev->kmod.dev, queue->tiler_heap_va);
      queue->tiler_heap_va = 0;
   }
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
    */
   if (pan_kmod_kbase_tiler_heap_create(
          dev->kmod.dev, PANVK_KBASE_TILER_CHUNK_SIZE,
          PANVK_KBASE_TILER_INITIAL_CHUNKS, PANVK_KBASE_TILER_MAX_CHUNKS,
          &queue->tiler_heap_va, &queue->tiler_first_chunk_va)) {
      result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                            "kbase: failed to create the tiler heap");
      goto err_finish_queue;
   }

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

   /* Scratch for the init streams init_gpu_queue() is about to build. It
    * has to exist before that call, since the shared code reads it rather
    * than allocating it - see patch-panvk-kbase-subqueue-init.py.
    */
   struct panvk_pool_alloc_info alloc_info = {
      .size = PANVK_KBASE_INIT_CS_SIZE,
      .alignment = 64,
   };

   queue->gpu.kbase_init_cs =
      panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
   if (!panvk_priv_mem_check_alloc(queue->gpu.kbase_init_cs)) {
      result = panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            "kbase: failed to allocate the init CS buffer");
      goto err_destroy_resources;
   }

   /* Sets up the GPU-side context each subqueue needs before it can run
    * anything: the shared syncobj array, a panvk_cs_subqueue_context, and
    * an init stream that loads the context register and initialises the
    * scoreboard slots. Shared with panthor - only the submit at the end of
    * it is ours, via panvk_per_arch(kbase_submit_and_wait) below.
    *
    * Compute only for now; the render subqueues need a tiler heap
    * descriptor, geometry buffer and descriptor ringbuf that this path does
    * not build yet.
    */
   result = panvk_per_arch(init_gpu_queue)(&queue->gpu);
   if (result != VK_SUCCESS)
      goto err_free_init_cs;

   *out_queue = &queue->gpu.vk;
   return VK_SUCCESS;

err_free_init_cs:
   panvk_pool_free_mem(&queue->gpu.kbase_init_cs);

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
   panvk_pool_free_mem(&queue->gpu.kbase_init_cs);

   destroy_queue_resources(dev, queue);
   vk_queue_finish(&queue->gpu.vk);
   vk_free(&dev->vk.alloc, queue);
}

/* cs_builder wants somewhere to go when a stream outgrows its buffer. This

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
 * into queue->gpu.kbase_init_cs, and block until the GPU has run it.
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
                                      uint64_t stream_addr,
                                      uint32_t stream_size)
{
   struct panvk_kbase_queue *queue =
      container_of(gpu_queue, struct panvk_kbase_queue, gpu);
   struct panvk_device *dev = to_panvk_device(gpu_queue->vk.base.device);
   struct pan_kmod_kbase_cs *cs = &queue->subqueues[subqueue];

   if (!stream_size)
      return VK_SUCCESS;

   /* The shared code hands back the GPU address it built at; the bytes to
    * copy are the CPU view of the same allocation. Nothing else is ever
    * built there, so the offset is zero and the two must correspond.
    */
   assert(stream_addr == panvk_priv_mem_dev_addr(gpu_queue->kbase_init_cs));

   VkResult result =
      submit_stream(dev, queue, subqueue,
                    panvk_priv_mem_host_addr(gpu_queue->kbase_init_cs),
                    stream_size);
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

VkResult
panvk_per_arch(kbase_queue_submit)(struct vk_queue *vk_queue,
                                   struct vk_queue_submit *vk_submit)
{
   struct panvk_kbase_queue *queue = to_kbase_queue(vk_queue);
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);

   /* Command buffers need the per-subqueue context that init_subqueue()
    * would have set up, and that does not exist yet - see the file comment.
    * Refusing is the honest answer; running them against an uninitialised
    * context would produce wrong results rather than an error.
    */
   if (vk_submit->command_buffer_count) {
      return panvk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                          "kbase: command buffer submission is not "
                          "implemented yet (%u in this submit)",
                          vk_submit->command_buffer_count);
   }

   /* No GPU-side wait exists yet - panvk_kbase_sync deliberately withholds
    * VK_SYNC_FEATURE_GPU_WAIT - so waits are satisfied on the CPU before
    * anything is published. Correct, just pessimistic: it serialises the
    * queue thread against the wait. In practice this loop does nothing,
    * because a semaphore cannot currently be created at all.
    */
   for (uint32_t i = 0; i < vk_submit->wait_count; i++) {
      VkResult result = vk_sync_wait(&dev->vk, vk_submit->waits[i].sync,
                                     vk_submit->waits[i].wait_value,
                                     VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!vk_submit->signal_count)
      return VK_SUCCESS;

   /* One SYNC_SET64 per signalled sync, at system scope so the write lands
    * where the CPU can see it - CSG scope would keep it inside the group.
    * This is the whole submit: with no command buffers there is nothing
    * else to run, which is exactly what makes it a useful first target.
    */
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

   /* SYNC_SET64 takes register indices, not immediates, so the address and
    * value go through registers first. Same shape as tests/event_slot_probe
    * and as Panfork's 0x48/0x4a pair.
    */
   struct cs_index addr_reg = cs_reg64(&b, 0);
   struct cs_index val_reg = cs_reg64(&b, 2);

   for (uint32_t i = 0; i < vk_submit->signal_count; i++) {
      struct vk_sync *sync = vk_submit->signals[i].sync;

      if (sync->type != &phys_dev->kbase_sync_type.base) {
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

   /* The compute subqueue, because an empty submit needs no tiler or
    * fragment resources and this one is not carrying any real work. When
    * command buffers arrive they will pick per their own subqueue.
    */
   return submit_stream(dev, queue, PANVK_SUBQUEUE_COMPUTE, stream, size);
}

VkResult
panvk_per_arch(kbase_queue_check_status)(struct vk_queue *vk_queue)
{
   /* panthor answers this with DRM_IOCTL_PANTHOR_GROUP_GET_STATE, which
    * reports whether the group was killed by a fault. kbase's equivalent
    * signal is a GPU_QUEUE_GROUP_ERROR notification read off the device fd
    * (tests/live_kick_probe decodes those). Nothing submits work yet, so
    * there is no state to report; wiring this up belongs with submission.
    */
   return VK_SUCCESS;
}
