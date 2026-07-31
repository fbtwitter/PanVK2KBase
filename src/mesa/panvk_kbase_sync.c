/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 */

/* A vk_sync implementation for kbase.
 *
 * PanVK's sync code assumes DRM syncobjs: get_device_sync_types() calls
 * vk_drm_syncobj_get_type(dev->fd) unconditionally, which cannot work on
 * /dev/mali0 because it is a misc character device with no DRM fd behind
 * it. That is what makes physical device creation fail with
 * VK_ERROR_INITIALIZATION_FAILED on a kbase device even once enumeration
 * and the pan_kmod backend are working.
 *
 * kbase offers no fence object to translate, so there is nothing to shim.
 * What it does offer is CSF event memory: a GPU-visible allocation
 * (BASE_MEM_CSF_EVENT) that a command stream can write with SYNC_SET64 and
 * the CPU can then read. tests/event_slot_probe confirms the whole loop on
 * real hardware - the slot moves and poll() delivers a
 * base_csf_notification. This builds a vk_sync on that.
 *
 * One 64-bit slot per vk_sync, treated as a monotonically increasing
 * timeline value. Binary syncs are the same thing with 0 = unsignalled and
 * >= 1 = signalled, which is how vk_drm_syncobj models binary syncs too.
 *
 * SCOPE - read before relying on this:
 *
 *   Working: CPU signal, CPU reset, CPU wait, get_value, and the type
 *   registration that unblocks physical device creation.
 *
 *   Not wired: GPU-side signalling. That needs VkQueueSubmit to emit a
 *   SYNC_SET64 into the submitted command stream targeting this sync's
 *   slot, which is Phase 4 work (panvk_vX_gpu_queue.c still talks
 *   DRM_IOCTL_PANTHOR_*). Until then no GPU work ever moves a slot, so
 *   VK_SYNC_FEATURE_GPU_WAIT is deliberately not advertised.
 *
 *   Consequently the CPU wait below polls. Once submission signals slots,
 *   it should block on poll() for the kbase fd's base_csf_notification
 *   instead - the mechanism is proven, it just has no producer yet. Note
 *   that reading a notification consumes it, so that change needs a single
 *   owner of the fd's event stream, not a read() per waiter.
 */

#include <string.h>
#include <time.h>

#include "util/os_time.h"
#include "util/u_math.h"
#include "vk_log.h"
#include "vk_util.h"

#include "kmod/pan_kmod_kbase.h"
#include "panvk_kbase_sync.h"

static struct panvk_kbase_sync_type *
to_kbase_sync_type(const struct vk_sync_type *type)
{
   return container_of(type, struct panvk_kbase_sync_type, base);
}

static struct panvk_kbase_sync *
to_kbase_sync(struct vk_sync *sync)
{
   return container_of(sync, struct panvk_kbase_sync, base);
}

/* The value word. The error word (slot + 8) is reserved for firmware fault
 * reporting; it is zeroed at init and not otherwise used yet.
 */
static volatile uint64_t *
slot_value(struct panvk_kbase_sync_type *type, uint32_t slot)
{
   return (volatile uint64_t *)(type->slots +
                                (size_t)slot * PANVK_KBASE_EVENT_SLOT_SIZE);
}

static int
alloc_slot(struct panvk_kbase_sync_type *type)
{
   simple_mtx_lock(&type->lock);
   for (uint32_t w = 0; w < ARRAY_SIZE(type->used); w++) {
      if (type->used[w] == UINT64_MAX)
         continue;
      uint32_t bit = ffsll(~type->used[w]) - 1;
      type->used[w] |= 1ull << bit;
      simple_mtx_unlock(&type->lock);
      return w * 64 + bit;
   }
   simple_mtx_unlock(&type->lock);
   return -1;
}

static void
free_slot(struct panvk_kbase_sync_type *type, uint32_t slot)
{
   simple_mtx_lock(&type->lock);
   type->used[slot / 64] &= ~(1ull << (slot % 64));
   simple_mtx_unlock(&type->lock);
}

static VkResult
panvk_kbase_sync_init(struct vk_device *device, struct vk_sync *sync,
                      uint64_t initial_value)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   int slot = alloc_slot(type);
   if (slot < 0) {
      /* One page of slots. Raising this means allocating more event pages
       * and indexing across them; nothing here assumes a single page except
       * the allocator.
       */
      return vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "out of kbase event slots (max %u)",
                       PANVK_KBASE_MAX_SYNCS);
   }

   s->slot = slot;

   volatile uint64_t *v = slot_value(type, s->slot);
   v[0] = initial_value;
   v[1] = 0; /* error word */
   __sync_synchronize();

   return VK_SUCCESS;
}

static void
panvk_kbase_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   free_slot(type, s->slot);
}

static VkResult
panvk_kbase_sync_signal(struct vk_device *device, struct vk_sync *sync,
                        uint64_t value)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   /* vk_sync passes value == 0 for binary syncs, meaning "signalled". */
   if (!(sync->flags & VK_SYNC_IS_TIMELINE))
      value = 1;

   *slot_value(type, s->slot) = value;
   __sync_synchronize();

   return VK_SUCCESS;
}

static VkResult
panvk_kbase_sync_get_value(struct vk_device *device, struct vk_sync *sync,
                           uint64_t *value)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   *value = *slot_value(type, s->slot);

   return VK_SUCCESS;
}

static VkResult
panvk_kbase_sync_reset(struct vk_device *device, struct vk_sync *sync)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   volatile uint64_t *v = slot_value(type, s->slot);
   v[0] = 0;
   v[1] = 0;
   __sync_synchronize();

   return VK_SUCCESS;
}

static bool
wait_satisfied(struct panvk_kbase_sync_type *type,
               const struct vk_sync_wait *wait)
{
   uint64_t have = *slot_value(type, to_kbase_sync(wait->sync)->slot);

   /* A binary sync is signalled at any non-zero value; a timeline is
    * satisfied once it reaches the requested point. A timeline wait_value
    * of 0 is a no-op per the vk_sync contract.
    */
   if (!(wait->sync->flags & VK_SYNC_IS_TIMELINE))
      return have != 0;

   return have >= wait->wait_value;
}

static VkResult
panvk_kbase_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                           const struct vk_sync_wait *waits,
                           enum vk_sync_wait_flags wait_flags,
                           uint64_t abs_timeout_ns)
{
   if (wait_count == 0)
      return VK_SUCCESS;

   struct panvk_kbase_sync_type *type = to_kbase_sync_type(waits[0].sync->type);

   /* Polling, not blocking. See the SCOPE note at the top of this file: the
    * notification path exists and works, but nothing signals a slot from the
    * GPU yet, so there is no event to block on. Back off to 1ms so a long
    * wait does not burn a core; start tight so the common
    * already-signalled case stays cheap.
    */
   uint64_t sleep_ns = 1000;

   for (;;) {
      uint32_t satisfied = 0;

      for (uint32_t i = 0; i < wait_count; i++) {
         if (wait_satisfied(type, &waits[i])) {
            if (wait_flags & VK_SYNC_WAIT_ANY)
               return VK_SUCCESS;
            satisfied++;
         }
      }

      if (satisfied == wait_count)
         return VK_SUCCESS;

      if (os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;

      os_time_sleep(sleep_ns / 1000);
      sleep_ns = MIN2(sleep_ns * 2, 1000000);
   }
}

VkResult
panvk_kbase_sync_type_init(struct panvk_kbase_sync_type *type, int fd)
{
   memset(type, 0, sizeof(*type));

   type->fd = fd;
   type->slots = pan_kmod_kbase_alloc_event_mem(
      fd, PANVK_KBASE_EVENT_PAGE_SIZE, &type->gpu_va);

   if (!type->slots)
      return VK_ERROR_INITIALIZATION_FAILED;

   simple_mtx_init(&type->lock, mtx_plain);

   type->base = (struct vk_sync_type){
      .size = sizeof(struct panvk_kbase_sync),
      /* No GPU_WAIT: submission does not signal these yet. Advertising it
       * would let the runtime hand a sync to a queue operation that can
       * never complete.
       */
      .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_TIMELINE |
                  VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_RESET |
                  VK_SYNC_FEATURE_CPU_SIGNAL | VK_SYNC_FEATURE_WAIT_ANY,
      .init = panvk_kbase_sync_init,
      .finish = panvk_kbase_sync_finish,
      .signal = panvk_kbase_sync_signal,
      .get_value = panvk_kbase_sync_get_value,
      .reset = panvk_kbase_sync_reset,
      .wait_many = panvk_kbase_sync_wait_many,
   };

   return VK_SUCCESS;
}

void
panvk_kbase_sync_type_finish(struct panvk_kbase_sync_type *type)
{
   if (!type->slots)
      return;

   simple_mtx_destroy(&type->lock);
   pan_kmod_kbase_free_event_mem(type->fd, type->slots,
                                 PANVK_KBASE_EVENT_PAGE_SIZE);
   type->slots = NULL;
}
