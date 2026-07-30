/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
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
