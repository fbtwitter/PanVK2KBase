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
 *   Working: CPU signal, CPU reset, CPU wait, get_value, GPU signalling via
 *   SYNC_SET64 from the submitted stream, and the type registration that
 *   unblocks physical device creation.
 *
 *   VK_SYNC_FEATURE_GPU_WAIT is advertised, which is what lets
 *   vkCreateSemaphore succeed - get_semaphore_sync_type() in the runtime
 *   requires it and nothing else non-trivial. But read
 *   kbase_queue_submit()'s wait loop before assuming what it means here:
 *   the wait is honoured by blocking the submitting thread until the slot
 *   reaches its value, not by a SYNC_WAIT64 in the stream. That satisfies
 *   the feature's contract - a submission does not begin until its waits
 *   are satisfied - but gives up the pipelining a real GPU-side wait would
 *   buy. The reasoning for stopping there, and the specific hazard that
 *   makes SYNC_WAIT64 unsafe today, is in that comment rather than
 *   duplicated here.
 *
 *   VK_SYNC_FEATURE_WAIT_PENDING comes with it, and is not optional: with
 *   GPU_WAIT set and WAIT_PENDING clear, get_timeline_mode() in the runtime
 *   asserts. Under NDEBUG that assert vanishes and the mode selection
 *   silently proceeds on a false premise, which is the same shape of bug as
 *   the vkCmdFillBuffer crash. See wait_satisfied() for what "pending"
 *   actually means for these slots.
 *
 *   Advertising GPU_WAIT without WAIT_BEFORE_SIGNAL puts the device in
 *   VK_DEVICE_TIMELINE_MODE_ASSISTED, so the runtime holds a submit on its
 *   queue thread until the waits are pending rather than handing us a wait
 *   whose signal has not been submitted yet. That is deliberate: this sync
 *   type genuinely cannot wait before signal - a slot carries no record
 *   that a signal is coming - and claiming otherwise would turn an ordinary
 *   application pattern into a hang.
 *
 *   Still polling, not blocking. The CPU wait below spins with backoff
 *   rather than blocking on poll() for the kbase fd's base_csf_notification.
 *   That mechanism is proven (tests/event_slot_probe) and now has a real
 *   producer, so the change is finally possible; the reason it has not been
 *   made is that reading a notification consumes it, so it needs a single
 *   owner of the fd's event stream rather than a read() per waiter.
 */

#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How long export_sync_file() will wait for the payload before giving up.
 * Nothing in this driver blocks forever; see the timeout inventory in
 * panvk_vX_kbase_queue.c.
 */
#define PANVK_KBASE_EXPORT_TIMEOUT_NS (5ull * 1000 * 1000 * 1000)

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

uint64_t
panvk_kbase_sync_slot_gpu_va(const struct vk_sync *sync)
{
   const struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   const struct panvk_kbase_sync *s =
      container_of(sync, const struct panvk_kbase_sync, base);

   /* The event memory is BASE_MEM_SAME_VA, so type->gpu_va and type->slots
    * are the same address; going through gpu_va rather than casting the CPU
    * pointer keeps that an implementation detail of the allocation rather
    * than something this arithmetic depends on.
    */
   return type->gpu_va + (uint64_t)s->slot * PANVK_KBASE_EVENT_SLOT_SIZE;
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
   s->sync_fd = -1;
   s->imported = false;

   volatile uint64_t *v = slot_value(type, s->slot);
   v[0] = initial_value;
   v[1] = 0; /* error word */
   __sync_synchronize();

   return VK_SUCCESS;
}

/* Discard an imported sync_file payload, returning the sync to its slot.
 *
 * Called from every operation that replaces the payload - signal, reset and
 * move - because after any of those the slot is the truth again and a stale
 * imported fd would keep answering waits with someone else's completion.
 */
static void
drop_imported(struct panvk_kbase_sync *s)
{
   if (s->sync_fd >= 0)
      close(s->sync_fd);
   s->sync_fd = -1;
   s->imported = false;
}

static void
panvk_kbase_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   drop_imported(s);
   free_slot(type, s->slot);
}

static VkResult
panvk_kbase_sync_signal(struct vk_device *device, struct vk_sync *sync,
                        uint64_t value)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   /* A CPU signal replaces whatever the payload was, including an imported
    * fence.
    */
   drop_imported(s);

   /* vk_sync passes value == 0 for binary syncs, meaning "signalled". */
   if (!(sync->flags & VK_SYNC_IS_TIMELINE))
      value = 1;

   *slot_value(type, s->slot) = value;
   __sync_synchronize();

   return VK_SUCCESS;
}

/* Move src's payload to dst, leaving src unsignalled.
 *
 * Required, not optional, and the requirement is easy to miss: vk_queue.c
 * asserts type->move exists the moment a binary semaphore's permanent
 * payload is waited on under threaded submit, and calls it unconditionally
 * from vk_queue_submit_move_binary_waits_to_temps(). Under NDEBUG the assert
 * is gone and the call goes through a NULL pointer instead, which is how
 * this was found - a SIGSEGV at pc 0 inside vkQueueSubmit.
 *
 * Swapping the slot indices, not copying the slot contents. Copying looks
 * simpler and is wrong: at the point the runtime moves a payload, a submit
 * that will signal src may already be sitting in the ring with a SYNC_SET64
 * naming src's slot address. Copying the value that is there *now* would let
 * that write land on the slot src still owns, so dst - the object that
 * inherited the payload, and the one the wait will actually watch - would
 * stay at 0 until it timed out.
 *
 * A slot is the payload. Handing over the index hands over any write already
 * in flight against it, which is the behaviour a DRM syncobj gets for free by
 * moving the underlying fence.
 *
 * Both syncs are the same type - vk_sync_move() asserts it - so the indices
 * come from the same pool and stay valid. Each slot is still freed exactly
 * once, by whichever vk_sync holds it at finish time.
 */
static VkResult
panvk_kbase_sync_move(struct vk_device *device, struct vk_sync *dst,
                      struct vk_sync *src)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(src->type);
   struct panvk_kbase_sync *d = to_kbase_sync(dst);
   struct panvk_kbase_sync *s = to_kbase_sync(src);

   uint32_t moved = s->slot;
   s->slot = d->slot;
   d->slot = moved;

   /* The imported payload moves with the slot, for the same reason: it *is*
    * the payload when set. dst inherits it; src is left with dst's old one,
    * which is then dropped below so src reads as unsignalled.
    */
   int moved_fd = s->sync_fd;
   bool moved_imported = s->imported;
   s->sync_fd = d->sync_fd;
   s->imported = d->imported;
   d->sync_fd = moved_fd;
   d->imported = moved_imported;

   drop_imported(s);

   /* src must read as unsignalled afterwards. The slot it now holds is dst's
    * old one, which is usually a freshly created temporary and already zero,
    * but the contract is that this function leaves src reset rather than that
    * the caller happened to hand over something empty.
    */
   volatile uint64_t *v = slot_value(type, s->slot);
   v[0] = 0;
   v[1] = 0;
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

   drop_imported(s);

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
   struct panvk_kbase_sync *s = to_kbase_sync(wait->sync);

   /* An imported sync_file replaces the slot as the payload. Waiting on it
    * is poll(): a sync_file becomes readable when its fence signals, which
    * is standard kernel behaviour and needs nothing from kbase.
    *
    * sync_fd < 0 here is the "imported -1" case - the compositor had
    * nothing outstanding - and is satisfied immediately.
    */
   if (s->imported) {
      if (s->sync_fd < 0)
         return true;

      struct pollfd pfd = {.fd = s->sync_fd, .events = POLLIN};
      return poll(&pfd, 1, 0) == 1;
   }

   uint64_t have = *slot_value(type, s->slot);

   /* A binary sync is signalled at any non-zero value; a timeline is
    * satisfied once it reaches the requested point. A timeline wait_value
    * of 0 is a no-op per the vk_sync contract.
    */
   if (!(wait->sync->flags & VK_SYNC_IS_TIMELINE))
      return have != 0;

   return have >= wait->wait_value;
}

/* VK_SYNC_WAIT_PENDING asks whether a signal operation has been *submitted*,
 * not whether it has completed. A slot cannot answer that: it holds one
 * 64-bit value that changes when the GPU's SYNC_SET64 retires, and carries
 * no record that a stream containing one has been published.
 *
 * So a pending wait is answered as a complete wait. That is the conservative
 * direction - it reports "not yet" for a signal that has been submitted but
 * not executed, never the reverse - and the runtime uses PENDING to decide
 * when a submit may be released, where answering late costs latency and
 * answering early would be a correctness bug.
 *
 * It is only safe because submissions here are serialised and there is one
 * queue: the runtime's queue thread waits PENDING with no timeout before
 * calling into submission, so if a signal could sit behind the waiter in the
 * same queue this would deadlock rather than stall. It cannot - the thread
 * drains submits in order, so the signalling submit has always been
 * processed by the time the waiter is looked at. Adding a second queue means
 * revisiting this, not just this function.
 */

static VkResult
panvk_kbase_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                           const struct vk_sync_wait *waits,
                           enum vk_sync_wait_flags wait_flags,
                           uint64_t abs_timeout_ns)
{
   if (wait_count == 0)
      return VK_SUCCESS;

   struct panvk_kbase_sync_type *type = to_kbase_sync_type(waits[0].sync->type);

   /* VK_SYNC_WAIT_PENDING is deliberately not distinguished from a complete
    * wait - see the note above wait_satisfied() for why a slot cannot answer
    * "pending" and why collapsing the two is the safe direction. Only
    * VK_SYNC_WAIT_ANY changes behaviour below.
    */

   /* Polling, not blocking. See the SCOPE note at the top of this file: the
    * kbase fd's notification path would let this block, but consuming a
    * notification is destructive, so it needs a single owner of the event
    * stream first. Back off to 1ms so a long wait does not burn a core;
    * start tight so the common already-signalled case stays cheap.
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

/* Android acquire: take ownership of a sync_file and make it the payload.
 *
 * The runtime hands this in from vkAcquireImageANDROID, which owns the fd it
 * got from the compositor and transfers that ownership to us on success.
 *
 * fd < 0 is legal and common - it means the compositor had nothing
 * outstanding, i.e. "already signalled" - so it is recorded as an imported
 * payload that is immediately satisfied rather than rejected.
 */
static VkResult
panvk_kbase_sync_import_sync_file(struct vk_device *device,
                                  struct vk_sync *sync, int sync_file)
{
   struct panvk_kbase_sync_type *type = to_kbase_sync_type(sync->type);
   struct panvk_kbase_sync *s = to_kbase_sync(sync);

   if (sync_file >= 0) {
      /* KBASE_IOCTL_FENCE_VALIDATE is a real type check, not a rubber stamp
       * - tests/sync_fd_probe confirmed it rejects an eventfd and a kbase
       * sync-stream fd alike. Checking here refuses a bogus import at the
       * point the mistake was made, rather than letting a poll() on
       * something that is not a fence quietly never complete.
       */
      if (!pan_kmod_kbase_fence_validate(type->fd, sync_file)) {
         return vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                          "fd %d is not a fence (FENCE_VALIDATE rejected it)",
                          sync_file);
      }
   }

   /* Only now that the import cannot fail: the Vulkan spec requires the fd
    * to be left alone on failure, so nothing above may have consumed it.
    */
   drop_imported(s);

   s->sync_fd = sync_file;
   s->imported = true;

   return VK_SUCCESS;
}

/* Android release: hand back a fence the compositor can wait on.
 *
 * kbase cannot produce one. KBASE_IOCTL_STREAM_CREATE yields what its header
 * calls a timeline, but it is not a userspace-drivable sw_sync timeline -
 * SW_SYNC_IOC_CREATE_FENCE gives ENOTTY, because in kbase that stream is
 * driven by JM-era job atoms with no CSF equivalent. There is no ioctl that
 * says "signal this fence now", so userspace cannot manufacture a fence tied
 * to GPU completion. Measured by tests/sync_fd_probe.
 *
 * The Vulkan spec provides the way out: exporting SYNC_FD may return -1,
 * meaning the payload is already signalled. That is legal and honest
 * *provided we actually wait first* - which is what this does. It costs a
 * CPU block per present instead of letting the compositor wait, and that is
 * the real price of this backend, not a shortcut.
 */
static VkResult
panvk_kbase_sync_export_sync_file(struct vk_device *device,
                                  struct vk_sync *sync, int *sync_file)
{
   /* If the payload is itself an imported fence, hand back a dup of it
    * rather than blocking - it is already exactly the fence being asked
    * for, and passing it along is both cheaper and more useful than
    * collapsing it to -1.
    */
   struct panvk_kbase_sync *s = to_kbase_sync(sync);
   if (s->imported) {
      *sync_file = s->sync_fd >= 0 ? dup(s->sync_fd) : -1;
      return VK_SUCCESS;
   }

   const struct vk_sync_wait wait = {
      .sync = sync,
      .wait_value = (sync->flags & VK_SYNC_IS_TIMELINE) ? 1 : 0,
   };

   VkResult result = panvk_kbase_sync_wait_many(
      device, 1, &wait, 0,
      os_time_get_absolute_timeout(PANVK_KBASE_EXPORT_TIMEOUT_NS));

   if (result == VK_TIMEOUT) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "timed out after %ums waiting to export a sync file",
                       (unsigned)(PANVK_KBASE_EXPORT_TIMEOUT_NS / 1000000));
   }
   if (result != VK_SUCCESS)
      return result;

   /* -1: "already signalled", which it now is. */
   *sync_file = -1;

   return VK_SUCCESS;
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
      /* GPU_WAIT and WAIT_PENDING travel together, and neither is a free
       * claim - see the SCOPE note for what each one commits this type to.
       *
       * Deliberately absent: WAIT_BEFORE_SIGNAL, which this cannot do and
       * which would move the device out of the ASSISTED timeline mode that
       * compensates for that; and GPU_MULTI_WAIT, so the runtime keeps
       * resetting binary payloads after a wait rather than assuming one
       * signal can release several waiters.
       */
      .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_TIMELINE |
                  VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_RESET |
                  VK_SYNC_FEATURE_CPU_SIGNAL | VK_SYNC_FEATURE_WAIT_ANY |
                  VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_WAIT_PENDING,
      .init = panvk_kbase_sync_init,
      .finish = panvk_kbase_sync_finish,
      .signal = panvk_kbase_sync_signal,
      .move = panvk_kbase_sync_move,
      .get_value = panvk_kbase_sync_get_value,
      .reset = panvk_kbase_sync_reset,
      /* Android acquire/release. The runtime keys off these pointers being
       * non-NULL; there is no feature bit for sync files.
       */
      .import_sync_file = panvk_kbase_sync_import_sync_file,
      .export_sync_file = panvk_kbase_sync_export_sync_file,
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
