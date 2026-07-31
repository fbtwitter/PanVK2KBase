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

/* ------------------------------------------------------------------ *
 * Ringing a bound queue, and observing what the GPU did with it.
 *
 * These are the panthor DRM_IOCTL_PANTHOR_GROUP_SUBMIT equivalent, split
 * the way kbase splits it: publishing new work is a store to the queue's
 * own user-IO input page plus an ioctl, and there is no fence object to
 * wait on - completion is observed either in the command stream's own
 * output (CS_EXTRACT) or in a BASE_MEM_CSF_EVENT slot the stream writes.
 *
 * Deliberately thin. Ring-buffer management - where in the ring to write,
 * when it wraps, how much room is left - is the caller's, because that is
 * where the command stream is built and where the choice of how many
 * submissions may be in flight belongs. What lives here is only the part
 * that needs the kbase UAPI headers.
 *
 * Proven on hardware by tests/live_kick_probe (kick, CS_EXTRACT) and
 * tests/event_slot_probe (event slot, notification), both of which do
 * exactly this sequence against a queue bound the same way.
 * ------------------------------------------------------------------ */

/**
 * pan_kmod_kbase_queue_kick() - Publish work on a bound queue and ring it.
 * @dev: kbase device.
 * @cs: A queue from pan_kmod_kbase_queue_create().
 * @insert: New value for CS_INSERT.
 *
 * @insert is a *byte count*, not an address and not a ring offset: it is
 * the total number of command-stream bytes ever written to this queue, and
 * both hardware and firmware take the ring position as @insert modulo the
 * ring size. It only ever increases. The caller must have written the
 * bytes into @cs->ringbuf_cpu before calling; this publishes them with a
 * barrier, matching the dmb(osh) the kernel does before ringing doorbells.
 *
 * Returning 0 means the kernel accepted the kick, which is *not* the same
 * as the GPU having run anything - the KICK handler only flags the queue
 * and wakes the scheduler kthread, which rings the real hardware doorbell
 * asynchronously afterward. An earlier version of this repo read a
 * successful KICK as proof of execution and spent a long time chasing a
 * phantom (see docs/kbase-notes.md). Use pan_kmod_kbase_queue_extract() or
 * an event slot to find out what actually happened.
 *
 * Return: 0 on success, -1 on failure.
 */
int pan_kmod_kbase_queue_kick(struct pan_kmod_dev *dev,
                              const struct pan_kmod_kbase_cs *cs,
                              uint64_t insert);

/**
 * pan_kmod_kbase_queue_extract() - How much of the stream the GPU consumed.
 * @cs: A queue from pan_kmod_kbase_queue_create().
 *
 * The counterpart to the @insert passed to pan_kmod_kbase_queue_kick(), in
 * the same units and on the same monotonic scale: once it reaches a given
 * @insert, everything published up to that point has been read off the
 * ring. Written by firmware into the queue's user-IO output page, so it
 * costs a load, no ioctl.
 *
 * "Consumed" means read out of the ring, not finished: a stream whose last
 * instruction is asynchronous is fully extracted before its effects have
 * landed. For real completion, have the stream write an event slot.
 *
 * Return: the current CS_EXTRACT.
 */
uint64_t pan_kmod_kbase_queue_extract(const struct pan_kmod_kbase_cs *cs);

/**
 * pan_kmod_kbase_queue_active() - Whether firmware has this stream running.
 * @cs: A queue from pan_kmod_kbase_queue_create().
 *
 * Distinguishes "the group never got scheduled onto a CSG slot" from "it
 * ran and is waiting on something", which is the difference between a
 * setup bug and a stream bug when CS_EXTRACT is not advancing.
 *
 * Return: true if CS_ACTIVE is set.
 */
bool pan_kmod_kbase_queue_active(const struct pan_kmod_kbase_cs *cs);

/**
 * pan_kmod_kbase_queue_wait_idle() - Wait for CS_ACTIVE to clear.
 * @cs: A queue from pan_kmod_kbase_queue_create().
 * @timeout_ms: Milliseconds to wait before giving up.
 *
 * REQUIRED BEFORE EVERY KICK, not an optimisation. A kick issued while
 * CS_ACTIVE is 1 is accepted, returns 0, and does not run: the bytes stay
 * in the ring until some later kick happens to flush them. Measured on
 * hardware - the evidence is in the comment on the implementation.
 *
 * This serialises submissions, which is a real cost and a known
 * limitation, not a design choice. It is here because the alternative is
 * a submit path that silently loses work.
 *
 * Return: true if the CS is idle, false if it was still active at timeout
 * (in which case kicking anyway will probably not take effect).
 */
bool pan_kmod_kbase_queue_wait_idle(const struct pan_kmod_kbase_cs *cs,
                                    unsigned timeout_ms);

/**
 * enum pan_kmod_kbase_event_type - What a kbase notification reported.
 * @PAN_KMOD_KBASE_EVENT_KERNEL: Ordinary kernel event; something the
 *                               context is waiting on may have moved.
 * @PAN_KMOD_KBASE_EVENT_GROUP_ERROR: A queue group hit a fatal error or
 *                                    fault. The group is dead.
 * @PAN_KMOD_KBASE_EVENT_OTHER: A type this backend does not decode (the
 *                              CPU-queue-dump notification, which only
 *                              exists in MTK debug builds).
 */
enum pan_kmod_kbase_event_type {
   PAN_KMOD_KBASE_EVENT_KERNEL,
   PAN_KMOD_KBASE_EVENT_GROUP_ERROR,
   PAN_KMOD_KBASE_EVENT_OTHER,
};

/**
 * struct pan_kmod_kbase_event - A decoded kbase notification.
 * @type: Which kind of notification this was.
 * @group_handle: Group that failed. GROUP_ERROR only.
 * @error_type: kbase's base_gpu_queue_group_error_type. GROUP_ERROR only.
 *
 * Decoded rather than handed back raw so that callers do not need the
 * kbase UAPI headers, which only this translation unit is built with.
 */
struct pan_kmod_kbase_event {
   enum pan_kmod_kbase_event_type type;
   uint8_t group_handle;
   uint8_t error_type;
};

/**
 * pan_kmod_kbase_read_event() - Block for a notification from the device.
 * @dev: kbase device.
 * @timeout_ms: Milliseconds to wait; 0 polls, negative blocks forever.
 * @out: Filled in when a notification is read. May be NULL to just drain.
 *
 * kbase reports asynchronously through the device fd rather than through
 * fences: poll() for readability, then read() one struct off it. This is
 * what lets a waiter block instead of spinning on an event slot, and it is
 * the only way a GPU-side fault is reported at all - a group that dies
 * stops advancing CS_EXTRACT and says nothing else.
 *
 * ONE OWNER ONLY. read() consumes a notification, so two threads polling
 * the same fd will steal each other's wakeups. Anything built on this
 * needs a single reader that dispatches, not a read() per waiter.
 *
 * Return: 1 if a notification was read, 0 on timeout, -1 on error.
 */
int pan_kmod_kbase_read_event(struct pan_kmod_dev *dev, int timeout_ms,
                              struct pan_kmod_kbase_event *out);
