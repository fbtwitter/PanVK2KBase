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
