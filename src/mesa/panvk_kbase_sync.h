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
