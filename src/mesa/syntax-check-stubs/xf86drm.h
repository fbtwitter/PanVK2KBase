/*
 * Minimal libdrm stub - ONLY for syntax-checking pan_kmod_kbase.c against
 * the real pan_kmod.h. This is NOT libdrm and must never be on the include
 * path of a real build.
 *
 * Mesa's pan_kmod.h includes <xf86drm.h> unconditionally (every other
 * pan_kmod backend is a DRM driver). libdrm is a meson wrap that a shallow
 * Mesa clone doesn't fetch, so `make mesa-backend-check` supplies these two
 * declarations - the only libdrm symbols pan_kmod.h actually references -
 * to let the compiler get through the header and check our source.
 *
 * A real Mesa build links the real libdrm and never sees this file.
 */
#pragma once

#include <stdint.h>

static inline int
drmIoctl(int fd, unsigned long request, void *arg)
{
   (void)fd;
   (void)request;
   (void)arg;
   return -1;
}

static inline int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd;
   (void)handle;
   (void)flags;
   (void)prime_fd;
   return -1;
}
