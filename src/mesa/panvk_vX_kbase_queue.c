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
 *   NOT implemented: submission. panvk_per_arch(kbase_queue_submit)
 *   returns an error. Mapping VkQueueSubmit onto kbase means writing the
 *   command stream into the bound ring buffer, updating CS_INSERT in the
 *   user-IO input page, ringing via CS_QUEUE_KICK, and waiting on a
 *   BASE_MEM_CSF_EVENT slot. Each of those is now a backend call
 *   (pan_kmod_kbase_queue_kick/_extract/_active, _read_event), proven on
 *   hardware by tests/live_kick_probe and tests/event_slot_probe - but
 *   nothing here calls them yet.
 *
 *   Also not done: the per-subqueue init command stream panthor runs at
 *   creation time (init_subqueue() in the panthor file) to set up subqueue
 *   context registers. That needs submission, so it waits on the above.
 *   Until it exists, a created queue is structurally valid but has not had
 *   its GPU-side context initialised.
 */

#include "genxml/gen_macros.h"

#include "panvk_device.h"
#include "panvk_physical_device.h"
#include "panvk_queue.h"

#include "kmod/pan_kmod_kbase.h"

#include "vk_log.h"

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

struct panvk_kbase_queue {
   struct vk_queue vk;

   uint8_t group_handle;
   bool group_created;

   uint64_t tiler_heap_va;
   uint64_t tiler_first_chunk_va;

   struct pan_kmod_kbase_cs subqueues[PANVK_SUBQUEUE_COUNT];
};

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
      vk_queue_init(&queue->vk, &dev->vk, create_info, queue_idx);
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

   queue->vk.driver_submit = panvk_per_arch(kbase_queue_submit);

   *out_queue = &queue->vk;
   return VK_SUCCESS;

err_destroy_resources:
   destroy_queue_resources(dev, queue);

err_finish_queue:
   vk_queue_finish(&queue->vk);

err_free_queue:
   vk_free(&dev->vk.alloc, queue);
   return result;
}

void
panvk_per_arch(destroy_kbase_queue)(struct vk_queue *vk_queue)
{
   struct panvk_kbase_queue *queue =
      container_of(vk_queue, struct panvk_kbase_queue, vk);
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);

   destroy_queue_resources(dev, queue);
   vk_queue_finish(&queue->vk);
   vk_free(&dev->vk.alloc, queue);
}

VkResult
panvk_per_arch(kbase_queue_submit)(struct vk_queue *vk_queue,
                                   struct vk_queue_submit *vk_submit)
{
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);

   /* Deliberately an error rather than a silent success. Pretending a
    * submit succeeded would corrupt every fence and semaphore above it.
    *
    * What remains is assembly, not discovery. The backend now exposes each
    * step: build the command stream into subqueues[i].ringbuf_cpu, publish
    * it with pan_kmod_kbase_queue_kick() (which handles CS_INSERT and the
    * KICK ioctl), and observe completion either with
    * pan_kmod_kbase_queue_extract() or - for real completion rather than
    * "read off the ring" - a BASE_MEM_CSF_EVENT slot the stream signals
    * with SYNC_SET64, which is what panvk_kbase_sync already waits on.
    * pan_kmod_kbase_read_event() is how a GPU-side fault gets reported.
    *
    * Still owned by this file, because it is where the command stream is
    * built: where in the ring to write, when it wraps, and how many
    * submissions may be in flight at once.
    */
   return panvk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                       "kbase: VkQueueSubmit is not implemented yet "
                       "(see src/mesa/panvk_vX_kbase_queue.c)");
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
