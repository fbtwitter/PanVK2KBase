/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pan_kmod.h"

/* The kbase pan_kmod backend. Mirrors panthor_kmod.h's role for the panthor
 * backend.
 */
extern const struct pan_kmod_ops kbase_kmod_ops;

/**
 * pan_kmod_fd_is_kbase() - Test whether an fd refers to an Arm kbase device.
 * @fd: File descriptor to probe.
 * @uk_major: Where to store the kbase UK interface major version, or NULL.
 * @uk_minor: Where to store the kbase UK interface minor version, or NULL.
 *
 * kbase is not a DRM driver - it is a misc character device (/dev/mali0)
 * with its own private ioctl surface - so drmGetVersion() cannot identify
 * it and pan_kmod_dev_create() has to probe for it separately, before
 * falling through to DRM enumeration.
 *
 * The UK interface version is reported through out-params rather than
 * having the caller issue the ioctl itself, so that pan_kmod.c - generic,
 * driver-agnostic code - needs no kbase UAPI headers.
 *
 * Return: true if @fd is a kbase device.
 */
bool pan_kmod_fd_is_kbase(int fd, uint16_t *uk_major, uint16_t *uk_minor);

/**
 * pan_kmod_kbase_alloc_event_mem() - Allocate CSF event memory.
 * @fd: kbase device fd.
 * @size: Bytes to allocate; rounded up to a page.
 * @gpu_va: Where to store the GPU virtual address, or NULL.
 *
 * Allocates memory with BASE_MEM_CSF_EVENT, which is what a command stream
 * can signal with SYNC_SET64 and the CPU can then observe. This is the
 * substrate for panvk_kbase_sync - kbase exposes no fence object to
 * translate, so vk_sync is built on this instead of on DRM syncobjs.
 *
 * The mapping is BASE_MEM_SAME_VA, so the returned CPU pointer and @gpu_va
 * are the same address. Zeroed on return.
 *
 * Return: CPU pointer, or NULL on failure.
 */
void *pan_kmod_kbase_alloc_event_mem(int fd, size_t size, uint64_t *gpu_va);

/**
 * pan_kmod_kbase_free_event_mem() - Release CSF event memory.
 * @fd: kbase device fd.
 * @cpu: Pointer returned by pan_kmod_kbase_alloc_event_mem().
 * @size: Size originally requested.
 */
void pan_kmod_kbase_free_event_mem(int fd, void *cpu, size_t size);

struct drm_panthor_csif_info;

/**
 * pan_kmod_kbase_get_csif_props() - CSF interface geometry for a kbase dev.
 * @dev: A kbase pan_kmod device.
 *
 * PanVK reads CSF geometry through panthor_kmod_get_csif_props(), which
 * container_of()s a pan_kmod_dev into a panthor_kmod_dev. On a kbase
 * device that reads past the end of the real struct and returns garbage -
 * a garbage cs_reg_count makes cs_builder write out of bounds, which
 * segfaulted vkCreateDevice inside generate_tiler_oom_handler.
 *
 * panthor_kmod_get_csif_props() is patched to dispatch here for kbase
 * devices, so all six of its callers get correct values unchanged.
 *
 * Return: geometry filled from KBASE_IOCTL_CS_GET_GLB_IFACE where kbase
 * exposes it, and architectural CSF defaults where it does not.
 */
const struct drm_panthor_csif_info *
pan_kmod_kbase_get_csif_props(const struct pan_kmod_dev *dev);

/* ------------------------------------------------------------------ *
 * CSF queue-group lifecycle.
 *
 * PanVK's GPU queue (csf/panvk_vX_gpu_queue.c) is written against
 * panthor: DRM_IOCTL_PANTHOR_GROUP_CREATE / _DESTROY,
 * _TILER_HEAP_CREATE / _DESTROY, _GROUP_SUBMIT, plus libdrm syncobjs on
 * dev->drm_fd. None of that exists on a misc device.
 *
 * These are the kbase equivalents, in the order PanVK needs them. Each
 * wraps an ioctl sequence already exercised on real hardware by this
 * repo's standalone probes - tests/queue_group and tests/live_kick_probe
 * create a group, register and bind a queue, kick it and watch the GPU
 * consume the instruction. See docs/kbase-notes.md.
 * ------------------------------------------------------------------ */

/**
 * pan_kmod_kbase_group_create() - Create a CSF queue group.
 * @dev: kbase device.
 * @shader_present: Shader-core mask to expose to the group; the endpoint
 *                  masks are all set to this. Pass 0 to use the device's
 *                  full mask.
 * @priority: kbase queue-group priority (BASE_QUEUE_GROUP_PRIORITY_*).
 * @group_handle: Where to store the created group's handle.
 *
 * Uses KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6 (nr 42), which is what the
 * vendor blob uses exclusively - the modern nr 58 does not appear in
 * libGLES_mali.so at all (see the ioctl survey in docs/kbase-notes.md).
 *
 * Return: 0 on success, -1 on failure.
 */
int pan_kmod_kbase_group_create(struct pan_kmod_dev *dev,
                                uint64_t shader_present, uint8_t priority,
                                uint8_t *group_handle);

/**
 * pan_kmod_kbase_group_destroy() - Terminate a CSF queue group.
 * @dev: kbase device.
 * @group_handle: Handle from pan_kmod_kbase_group_create().
 */
void pan_kmod_kbase_group_destroy(struct pan_kmod_dev *dev,
                                  uint8_t group_handle);

/**
 * pan_kmod_kbase_tiler_heap_create() - Create a tiler heap.
 * @dev: kbase device.
 * @chunk_size: Bytes per chunk; must be 4KB-aligned.
 * @initial_chunks: Chunks to commit up front, >= 1.
 * @max_chunks: Growth limit, >= @initial_chunks.
 * @gpu_heap_va: Where to store the heap's GPU address.
 * @first_chunk_va: Where to store the first chunk's GPU address.
 *
 * Return: 0 on success, -1 on failure.
 */
int pan_kmod_kbase_tiler_heap_create(struct pan_kmod_dev *dev,
                                     uint32_t chunk_size,
                                     uint32_t initial_chunks,
                                     uint32_t max_chunks,
                                     uint64_t *gpu_heap_va,
                                     uint64_t *first_chunk_va);

/**
 * pan_kmod_kbase_tiler_heap_destroy() - Tear down a tiler heap.
 * @dev: kbase device.
 * @gpu_heap_va: Address from pan_kmod_kbase_tiler_heap_create().
 */
void pan_kmod_kbase_tiler_heap_destroy(struct pan_kmod_dev *dev,
                                       uint64_t gpu_heap_va);

/**
 * struct pan_kmod_kbase_cs - A bound CSF command stream.
 * @ringbuf_gpu_va: GPU address of the ring buffer the CS reads from.
 * @ringbuf_cpu: CPU mapping of the same.
 * @ringbuf_size: Size in bytes.
 * @user_io: The 3 pages CS_QUEUE_BIND hands back, ordered
 *           [doorbell][input][output]. Measured, not assumed - see
 *           src/utils/csf_user_regs.h and tests/user_io_probe.
 * @csi_index: CS interface index within the group.
 */
struct pan_kmod_kbase_cs {
   uint64_t ringbuf_gpu_va;
   void *ringbuf_cpu;
   uint64_t ringbuf_size;
   void *user_io;
   uint8_t csi_index;
};

/**
 * pan_kmod_kbase_queue_create() - Allocate, register and bind a CS queue.
 * @dev: kbase device.
 * @group_handle: Group from pan_kmod_kbase_group_create().
 * @csi_index: CS interface index within that group.
 * @ringbuf_size: Ring buffer size in bytes; rounded up to a page.
 * @out: Filled in on success.
 *
 * Does MEM_ALLOC_EX (ring buffer) -> CS_QUEUE_REGISTER -> CS_QUEUE_BIND ->
 * mmap of the user-IO pages. The ring buffer is allocated here rather than
 * by the caller so the whole sequence stays in the one translation unit
 * that has the kbase UAPI headers.
 *
 * Return: 0 on success, -1 on failure.
 */
int pan_kmod_kbase_queue_create(struct pan_kmod_dev *dev,
                                uint8_t group_handle, uint8_t csi_index,
                                uint64_t ringbuf_size,
                                struct pan_kmod_kbase_cs *out);

/**
 * pan_kmod_kbase_queue_destroy() - Unbind and free a CS queue.
 * @dev: kbase device.
 * @cs: Queue from pan_kmod_kbase_queue_create(). Zeroed on return.
 */
void pan_kmod_kbase_queue_destroy(struct pan_kmod_dev *dev,
                                  struct pan_kmod_kbase_cs *cs);
