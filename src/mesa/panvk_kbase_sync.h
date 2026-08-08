/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "util/simple_mtx.h"
#include "vk_sync.h"

/* Slot layout in the event page: a 64-bit value word followed by a 64-bit
 * error word. Matches what Panfork uses (PAN_EVENT_SIZE, pan_base.h:31) and
 * what tests/event_slot_probe seeds and reads on real hardware.
 */
#define PANVK_KBASE_EVENT_SLOT_SIZE 16
#define PANVK_KBASE_EVENT_PAGE_SIZE 4096
#define PANVK_KBASE_MAX_SYNCS                                                  \
   (PANVK_KBASE_EVENT_PAGE_SIZE / PANVK_KBASE_EVENT_SLOT_SIZE)

/* The sync type doubles as the owner of the event-memory pool, so a
 * vk_sync can reach the pool through its own ->type pointer with
 * container_of(). That keeps this self-contained: no new field on
 * panvk_device, and no global state.
 */
struct panvk_kbase_sync_type {
   struct vk_sync_type base; /* must stay first, container_of depends on it */

   int fd;
   uint8_t *slots;   /* CPU mapping of the event page */
   uint64_t gpu_va;  /* same address - the mapping is SAME_VA */

   simple_mtx_t lock; /* guards `used` */
   uint64_t used[PANVK_KBASE_MAX_SYNCS / 64];
};

struct panvk_kbase_sync {
   struct vk_sync base;
   uint32_t slot;

   /* Guards @sync_fd and @imported below. Mesa's threaded submit can run
    * this driver's queue-submit thread and an application presentation
    * thread against the same panvk_kbase_sync concurrently - e.g.
    * vk_queue_submit_cleanup() destroying a temporary binary semaphore's
    * payload on the submit thread while the app thread is mid
    * export_sync_file() on it via vkQueueSignalReleaseImageANDROID. Without
    * this, drop_imported()'s close(sync_fd) can run against a stale fd
    * number after another thread already closed and cleared it, and by then
    * the OS may have handed that fd to something unrelated - which is
    * exactly what fdsan caught on a real device (Azahar/AzaharPlus,
    * VulkanPresent thread): "attempted to close file descriptor ...,
    * actually owned by FILE* ...".
    */
   simple_mtx_t lock;

   /* An imported sync_file payload, for Android acquire/release.
    *
    * When @imported is set the payload is NOT the slot: it is whatever the
    * imported fence says, and the slot is ignored until the payload is
    * replaced (signal/reset/move). @sync_fd < 0 with @imported set is the
    * legal "fd was -1, i.e. already signalled" case, which
    * vkAcquireImageANDROID passes whenever the compositor has nothing to
    * wait for.
    *
    * Owned: the runtime hands ownership to the driver on a successful
    * import, so finish() closes it.
    */
   int sync_fd;
   bool imported;
};

/**
 * panvk_kbase_sync_type_init() - Bring up the event-memory-backed sync type.
 * @type: Type to initialise, typically embedded in panvk_physical_device.
 * @fd: kbase device fd. Borrowed, not owned.
 *
 * Return: VK_SUCCESS, or VK_ERROR_INITIALIZATION_FAILED if event memory
 * could not be allocated.
 */
VkResult panvk_kbase_sync_type_init(struct panvk_kbase_sync_type *type,
                                    int fd);

/**
 * panvk_kbase_sync_type_finish() - Release the event memory.
 * @type: Type previously initialised by panvk_kbase_sync_type_init().
 */
void panvk_kbase_sync_type_finish(struct panvk_kbase_sync_type *type);

/**
 * panvk_kbase_sync_slot_gpu_va() - GPU address of a sync's value word.
 * @sync: A vk_sync belonging to a panvk_kbase_sync_type.
 *
 * The address a command stream writes with SYNC_SET64 at system scope to
 * signal this sync from the GPU, which is the same word the CPU-side
 * signal/reset/wait paths in panvk_kbase_sync.c touch. That aliasing is
 * the whole point: it is what makes a GPU-signalled fence observable by
 * vkWaitForFences without a second mechanism.
 *
 * The caller must know @sync is one of ours - compare sync->type against
 * &phys_dev->kbase_sync_type.base. There is no runtime check here because
 * a mismatch would be a driver bug, not a recoverable condition.
 *
 * Return: GPU virtual address of the 64-bit value word.
 */
uint64_t panvk_kbase_sync_slot_gpu_va(const struct vk_sync *sync);
