/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 *
 * pan_kmod backend for Arm's proprietary kbase kernel driver.
 *
 * This is a third pan_kmod backend alongside `panfrost` (DRM, JM) and
 * `panthor` (DRM, CSF). Unlike both of those, kbase is NOT a DRM driver:
 * it is a misc character device (/dev/mali0) with its own private ioctl
 * surface, so it is not enumerated through the DRM subsystem at all.
 * See docs/architecture.md in the PanVK2KBase repo for the full writeup.
 *
 * Two consequences that shape this file:
 *
 * 1. Device probing cannot go through drmGetVersion(). pan_kmod_dev_create()
 *    normally calls it and dispatches on the DRM driver name; for a kbase fd
 *    that call simply fails. The dispatch therefore needs a kbase special
 *    case *before* the DRM path - see the pan_kmod.c hunk in
 *    src/mesa/pan_kmod.c.kbase.patch. This mirrors how Turnip special-cases
 *    kgsl (/dev/kgsl-3d0) alongside DRM enumeration.
 *
 * 2. pan_kmod_ops has no submission or sync entry at all - command-stream
 *    submission and fencing live in the Vulkan driver
 *    (src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c), which calls
 *    DRM_IOCTL_PANTHOR_* and libdrm drmSyncobj* directly on a DRM fd. None
 *    of that can work on a misc device. So implementing this backend gets
 *    device probe + BO/VM management working, and submission remains a
 *    separate (larger) piece of work. Nothing here pretends otherwise.
 *
 * Status: device probe and BO alloc/free/mmap are backed by ioctl sequences
 * verified on real hardware (Poco X8 Pro, Mali-G720, kbase r49p1) by the
 * standalone probes in this repo. VM and submission-adjacent ops are
 * deliberately stubbed with explicit errors rather than plausible-looking
 * fakes - see the comments on each.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "util/macros.h"
#include "util/u_memory.h"

#include "pan_kmod.h"
#include "pan_kmod_backend.h"
#include "pan_kmod_kbase.h"

/* Vendored kbase UAPI headers. The include path is supplied by the meson
 * snippet in src/mesa/meson.build.kbase - see src/mesa/README.md.
 */
#include "mali_base_common_kernel.h" /* BASE_MEM_PROT_* */
#include "mali_base_kernel.h"        /* base_mem_alloc_flags */
#include "mali_kbase_ioctl.h"
#include "csf/mali_kbase_csf_ioctl.h"

/* Forward declaration; the definition is at the bottom of this file.
 * Declared in pan_kmod_kbase.h.
 */

/* kbase allocations in this backend are BASE_MEM_SAME_VA: the kernel hands
 * back a VA that is valid for both CPU and GPU, so the CPU pointer returned
 * by mmap() *is* the GPU virtual address. This is why bo->gpu_va below is
 * the mmap result and not the `gpu_va` field the MEM_ALLOC ioctl returns -
 * that field is a reusable cookie (it comes back as the same value for
 * every allocation), not a usable address. Verified on hardware; see
 * docs/kbase-notes.md's SAME_VA section.
 */
struct kbase_kmod_bo {
   struct pan_kmod_bo base;

   /* CPU mapping, which doubles as the GPU VA under SAME_VA. */
   void *cpu;
};

struct kbase_kmod_dev {
   struct pan_kmod_dev base;

   /* CSF UK interface version reported by KBASE_IOCTL_VERSION_CHECK.
    * Zero when this device was created on an fd whose kbase context was
    * already handshaked (see already_initialized), because the version
    * cannot be re-queried on such an fd.
    */
   struct {
      uint16_t major;
      uint16_t minor;
   } uk_version;

   /* Set when this pan_kmod_dev wraps an fd that already went through
    * VERSION_CHECK/SET_FLAGS - i.e. a dup() of one that did. vkCreateDevice
    * makes exactly one of these per logical device.
    */
   bool already_initialized;
};

/* The implicit-VM shim's state. See kbase_kmod_vm_create() for what this
 * does and does not model.
 */
struct kbase_kmod_vm {
   struct pan_kmod_vm base;

   uint32_t map_count;
   uint32_t mismatch_count;
   bool warned;
};

static struct kbase_kmod_vm *
to_kbase_kmod_vm(struct pan_kmod_vm *vm)
{
   return container_of(vm, struct kbase_kmod_vm, base);
}

static struct kbase_kmod_dev *
to_kbase_kmod_dev(struct pan_kmod_dev *dev)
{
   return container_of(dev, struct kbase_kmod_dev, base);
}

static struct kbase_kmod_bo *
to_kbase_kmod_bo(struct pan_kmod_bo *bo)
{
   return container_of(bo, struct kbase_kmod_bo, base);
}

/*
 * GPU property decoding.
 *
 * KBASE_IOCTL_GET_GPUPROPS returns an opaque blob: a sequence of
 * (key, value) pairs where the key packs a property ID in the upper bits
 * and a size code in the low two bits. There is no struct to cast to.
 * This walker is the same one used by the standalone probes in this repo
 * (utils/parse_gpu_props.h), reduced to a single-property lookup.
 */
static bool
kbase_gpuprops_lookup(const uint8_t *buf, size_t len, uint32_t want_id,
                      uint64_t *out)
{
   size_t off = 0;

   while (off + 4 <= len) {
      uint32_t key;
      memcpy(&key, buf + off, sizeof(key));
      off += 4;

      uint32_t id = key >> 2;
      uint32_t size_code = key & 0x3;
      size_t value_size;

      switch (size_code) {
      case KBASE_GPUPROP_VALUE_SIZE_U8:
         value_size = 1;
         break;
      case KBASE_GPUPROP_VALUE_SIZE_U16:
         value_size = 2;
         break;
      case KBASE_GPUPROP_VALUE_SIZE_U32:
         value_size = 4;
         break;
      case KBASE_GPUPROP_VALUE_SIZE_U64:
         value_size = 8;
         break;
      default:
         return false;
      }

      if (off + value_size > len)
         return false;

      if (id == want_id) {
         uint64_t v = 0;
         for (size_t i = 0; i < value_size; i++)
            v |= ((uint64_t)buf[off + i]) << (i * 8);
         *out = v;
         return true;
      }

      off += value_size;
   }

   return false;
}

static uint64_t
kbase_gpuprop(const uint8_t *buf, size_t len, uint32_t id)
{
   uint64_t v = 0;
   kbase_gpuprops_lookup(buf, len, id, &v);
   return v;
}

static int
kbase_dev_query_props(struct kbase_kmod_dev *kbase_dev)
{
   struct pan_kmod_dev_props *props = &kbase_dev->base.props;
   int fd = kbase_dev->base.fd;

   /* Two-step: probe for the blob size, then fetch it. */
   struct kbase_ioctl_get_gpuprops probe = {
      .buffer = 0,
      .size = 0,
      .flags = 0,
   };

   int size = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &probe);
   if (size <= 0) {
      mesa_loge("kbase: GET_GPUPROPS size probe failed");
      return -1;
   }

   uint8_t *buf = calloc(1, (size_t)size);
   if (!buf)
      return -1;

   struct kbase_ioctl_get_gpuprops fetch = {
      .buffer = (uint64_t)(uintptr_t)buf,
      .size = (uint32_t)size,
      .flags = 0,
   };

   if (ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &fetch) < 0) {
      mesa_loge("kbase: GET_GPUPROPS fetch failed");
      free(buf);
      return -1;
   }

   size_t len = (size_t)size;
   uint32_t thread_features =
      kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_THREAD_FEATURES);

   *props = (struct pan_kmod_dev_props){
      .gpu_id = kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_GPU_ID),
      .gpu_variant =
         kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_CORE_FEATURES) & 0xff,
      .shader_present =
         kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_SHADER_PRESENT),
      .tiler_features =
         kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_TILER_FEATURES),
      .mem_features = kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_MEM_FEATURES),
      .mmu_features = kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_MMU_FEATURES),
      .l2_features = kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_L2_FEATURES),

      /* AFBC is not optional on the Valhall v10+ parts kbase CSF targets,
       * matching what panthor_kmod.c does.
       */
      .afbc_features = 0,

      .max_threads_per_core =
         kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_THREAD_MAX_THREADS),
      .max_threads_per_wg =
         kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE),
      .max_tasks_per_core = thread_features >> 24,
      .num_registers_per_core = thread_features & 0x3fffff,

      /* kbase exposes KBASE_IOCTL_GET_CPU_GPU_TIMEINFO rather than a
       * GPU-side timestamp query, and this backend does not wire it up
       * yet, so advertise no timestamp support rather than a wrong one.
       */
      .gpu_can_query_timestamp = false,
      .timestamp_frequency = 0,

      /* Only 4K has been exercised against a real kbase kernel here. Do not
       * advertise 2M until it is actually tested - getting this wrong
       * produces wrong mappings rather than a clean failure.
       */
      .pgsize_bitmap = PAN_PGSIZE_4K,

      /* Conservative: no special BO flags are claimed until each is
       * verified against a real kbase kernel.
       */
      .supported_bo_flags = 0,
      .supported_vm_op_flags = 0,

      /* COHERENCY_* is queryable via the props blob, but this backend does
       * not yet map kbase's coherency model onto pan_kmod's is_io_coherent
       * semantics, so assume the safe (non-coherent) answer.
       */
      .is_io_coherent = false,
   };

   /* v10+ has no THREAD_TLS_ALLOC register; the maximum TLS instances per
    * core equals the maximum threads per core (same reasoning as panthor).
    */
   props->max_tls_instance_per_core = props->max_threads_per_core;

   props->texture_features[0] =
      kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0);
   props->texture_features[1] =
      kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1);
   props->texture_features[2] =
      kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2);
   props->texture_features[3] =
      kbase_gpuprop(buf, len, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3);

   free(buf);
   return 0;
}

/*
 * Returns true if `fd` refers to a kbase device, and reports the kbase UK
 * interface version it speaks. Used by the dispatch special case in
 * pan_kmod.c, since drmGetVersion() cannot identify a misc device.
 * KBASE_IOCTL_VERSION_CHECK is the natural probe: it is the very first
 * thing any kbase client must call, it is side-effect free, and it fails
 * cleanly on any fd that isn't kbase.
 *
 * The version is reported through out-params rather than having the caller
 * issue the ioctl itself, specifically so that pan_kmod.c - which is
 * generic, driver-agnostic code - does not need to include kbase UAPI
 * headers or know the ioctl encoding.
 *
 * NOTE: VERSION_CHECK must be issued before KBASE_IOCTL_SET_FLAGS, and
 * SET_FLAGS may only be called once per context, so this deliberately does
 * not call SET_FLAGS - that happens in dev_create().
 */
bool
pan_kmod_fd_is_kbase(int fd, uint16_t *uk_major, uint16_t *uk_minor)
{
   struct kbase_ioctl_version_check ver = { .major = 0, .minor = 0 };

   if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver) < 0) {
      /* EPERM means the handshake already happened on this open file
       * description. kbase allows VERSION_CHECK exactly once, and dup()
       * shares the description, so this is not "not a kbase device" - it is
       * "a kbase device this process already greeted".
       *
       * That case is reached on every vkCreateDevice: panvk_vX_device.c
       * calls pan_kmod_dev_create(os_dupfd_cloexec(phys_dev->kmod.dev->fd))
       * to build a second pan_kmod_dev for the logical device. Returning
       * false here made that fall through to drmGetVersion(), which fails
       * on a misc device, so vkCreateDevice returned
       * VK_ERROR_OUT_OF_HOST_MEMORY.
       *
       * A driver that does not implement this ioctl number answers ENOTTY,
       * so EPERM is specific enough to identify kbase on its own.
       *
       * The version cannot be re-queried, so report 0.0 and let
       * kbase_kmod_dev_create() recognise that as "already set up" and skip
       * the equally once-per-context SET_FLAGS.
       *
       * See tests/double_handshake_probe/ for the probe that established
       * the once-per-fd rule.
       */
      if (errno == EPERM) {
         if (uk_major)
            *uk_major = 0;
         if (uk_minor)
            *uk_minor = 0;
         return true;
      }
      return false;
   }

   if (uk_major)
      *uk_major = ver.major;
   if (uk_minor)
      *uk_minor = ver.minor;

   return true;
}

static struct pan_kmod_dev *
kbase_kmod_dev_create(int fd, uint32_t flags,
                      const struct pan_kmod_driver *drv_info,
                      const struct pan_kmod_allocator *allocator)
{
   struct kbase_kmod_dev *kbase_dev =
      pan_kmod_alloc(allocator, sizeof(*kbase_dev));

   if (!kbase_dev) {
      mesa_loge("kbase: failed to allocate device object");
      return NULL;
   }

   pan_kmod_dev_init(&kbase_dev->base, fd, flags, drv_info, &kbase_kmod_ops,
                     allocator);

   /* Deliberately NOT re-issuing KBASE_IOCTL_VERSION_CHECK here.
    *
    * kbase permits it exactly once per fd: a second call returns -EPERM,
    * even though the handshake it performed is still in effect (SET_FLAGS
    * afterwards still succeeds). Since pan_kmod_dev_create() already calls
    * pan_kmod_fd_is_kbase() to identify the device, issuing it again here
    * would fail every time - which is precisely why the UK version is
    * handed to us in drv_info rather than being re-queried.
    *
    * See tests/double_handshake_probe/ for the probe that established this.
    */
   kbase_dev->uk_version.major = drv_info->version.major;
   kbase_dev->uk_version.minor = drv_info->version.minor;

   /* A 0.0 UK version means pan_kmod_fd_is_kbase() identified this fd by the
    * EPERM-from-VERSION_CHECK path, i.e. the context behind it is already
    * set up (see the comment there). SET_FLAGS is once-per-context too, so
    * re-issuing it would fail. Skip it rather than tolerating an error
    * code, so a genuine SET_FLAGS failure on a fresh fd is still fatal.
    */
   if (drv_info->version.major == 0 && drv_info->version.minor == 0) {
      kbase_dev->already_initialized = true;
   } else {
      struct kbase_ioctl_set_flags set_flags = { .create_flags = 0 };
      if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags) < 0) {
         mesa_loge("kbase: SET_FLAGS failed: %s", strerror(errno));
         goto err_cleanup;
      }
   }

   if (kbase_dev_query_props(kbase_dev))
      goto err_cleanup;

   return &kbase_dev->base;

err_cleanup:
   pan_kmod_dev_cleanup(&kbase_dev->base);
   pan_kmod_free(allocator, kbase_dev);
   return NULL;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   struct kbase_kmod_dev *kbase_dev = to_kbase_kmod_dev(dev);

   pan_kmod_dev_cleanup(dev);
   pan_kmod_free(dev->allocator, kbase_dev);
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *dev,
                    struct pan_kmod_vm *exclusive_vm, uint64_t size,
                    uint32_t flags)
{
   /* Nothing in supported_bo_flags is advertised yet, so refuse anything
    * with flags rather than silently ignoring them.
    */
   if (flags) {
      mesa_loge("kbase: BO flags 0x%x not supported yet", flags);
      return NULL;
   }

   struct kbase_kmod_bo *bo = pan_kmod_dev_alloc(dev, sizeof(*bo));
   if (!bo)
      return NULL;

   uint64_t aligned = ALIGN_POT(size, 4096);

   union kbase_ioctl_mem_alloc alloc = { 0 };
   alloc.in.va_pages = aligned / 4096;
   alloc.in.commit_pages = aligned / 4096;
   alloc.in.extension = 0;
   alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;

   if (ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC, &alloc) < 0) {
      mesa_loge("kbase: MEM_ALLOC failed");
      pan_kmod_dev_free(dev, bo);
      return NULL;
   }

   /* The offset passed to mmap() is the cookie MEM_ALLOC returned, not a
    * real address; the mapping it produces is the SAME_VA address usable by
    * both CPU and GPU.
    */
   bo->cpu = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd,
                  alloc.out.gpu_va);

   if (bo->cpu == MAP_FAILED) {
      mesa_loge("kbase: mmap of BO failed");
      pan_kmod_dev_free(dev, bo);
      return NULL;
   }

   /* kbase has no GEM-style handle namespace. pan_kmod's common layer keys
    * its handle_to_bo sparse array off bo->handle, so a unique per-device
    * value is required. Reusing the page number of the SAME_VA address
    * gives a stable, unique key without inventing a parallel allocator.
    */
   uint32_t handle = (uint32_t)(((uintptr_t)bo->cpu) >> 12);

   pan_kmod_bo_init(&bo->base, dev, exclusive_vm, aligned, flags, handle);

   return &bo->base;
}

/* CSF event memory.
 *
 * Kept here rather than in the Vulkan layer because this is the only
 * translation unit built with the kbase UAPI headers and -DMALI_USE_CSF=1
 * (see meson.build.kbase.patch); panvk_kbase_sync.c needs the memory, not
 * the ioctl surface.
 *
 * BASE_MEM_CSF_EVENT is the load-bearing flag: the kernel gives the region
 * a permanent kernel mapping and, on this device, adds CACHED_CPU and
 * COHERENT_SYSTEM to the granted flags, which is what lets the CPU observe
 * a firmware write without explicit cache maintenance. A plain BO must not
 * be assumed to behave the same way. Confirmed on hardware by
 * tests/event_slot_probe - see docs/kbase-notes.md "Finding 2".
 */
void *
pan_kmod_kbase_alloc_event_mem(int fd, size_t size, uint64_t *gpu_va)
{
   uint64_t aligned = ALIGN_POT(size, 4096);

   union kbase_ioctl_mem_alloc alloc = { 0 };
   alloc.in.va_pages = aligned / 4096;
   alloc.in.commit_pages = aligned / 4096;
   alloc.in.extension = 0;
   alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                    BASE_MEM_CSF_EVENT;

   if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc) < 0) {
      mesa_loge("kbase: MEM_ALLOC for event memory failed");
      return NULL;
   }

   void *cpu = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    alloc.out.gpu_va);
   if (cpu == MAP_FAILED) {
      mesa_loge("kbase: mmap of event memory failed");
      return NULL;
   }

   memset(cpu, 0, aligned);

   /* SAME_VA: the mapping address is the GPU virtual address. */
   if (gpu_va)
      *gpu_va = (uint64_t)(uintptr_t)cpu;

   return cpu;
}

void
pan_kmod_kbase_free_event_mem(int fd, void *cpu, size_t size)
{
   /* SAME_VA regions are torn down by the kernel on vm_close, so munmap()
    * alone is the free; a following MEM_FREE fails EINVAL because the
    * region is already gone. See docs/kbase-notes.md.
    */
   (void)fd;
   if (cpu)
      munmap(cpu, ALIGN_POT(size, 4096));
}

static void
kbase_kmod_bo_free(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbase_bo = to_kbase_kmod_bo(bo);

   /* For SAME_VA regions munmap() *is* the free: kbase tears the region
    * down on vm_close. Issuing KBASE_IOCTL_MEM_FREE afterwards fails with
    * EINVAL because the region is already gone - confirmed on hardware, see
    * docs/kbase-notes.md.
    */
   if (munmap(kbase_bo->cpu, bo->size))
      mesa_loge("kbase: munmap of BO failed");

   pan_kmod_bo_cleanup(bo);
   pan_kmod_dev_free(bo->dev, kbase_bo);
}

static off_t
kbase_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   /* kbase BOs are mapped at allocation time (see bo_alloc), because the
    * SAME_VA address only exists once mmap() has resolved the cookie.
    * pan_kmod_bo_mmap() is a static inline that always calls this and then
    * os_mmap()s the result, so it needs an offset that works a second time.
    *
    * kbase looks a region up by mmap offset >> PAGE_SHIFT, and after the
    * cookie has been resolved the region is addressable by its resolved
    * address - so handing that back produces a second, aliasing mapping of
    * the same pages.
    *
    * Measured, not assumed (tests/remap_probe, Poco X8 Pro / r49p1):
    *   - re-using the original MEM_ALLOC cookie fails EINVAL, so kbase
    *     consumes it on first mmap
    *   - the resolved SAME_VA address succeeds, and reads back a sentinel
    *     written through the first mapping
    *
    * Caveat that matters: the alias lands at a different CPU address from
    * the original mapping, and under SAME_VA it is the *original* that is
    * also the GPU address. So the pointer pan_kmod_bo_mmap() returns is a
    * valid CPU view but is not the GPU VA. Same caller-VA-vs-real-VA
    * divergence documented in kbase_kmod_vm_create().
    */
   struct kbase_kmod_bo *kbase_bo = to_kbase_kmod_bo(bo);

   if (!kbase_bo->cpu) {
      mesa_loge("kbase: bo_get_mmap_offset called on an unmapped BO");
      return (off_t)-1;
   }

   return (off_t)(uintptr_t)kbase_bo->cpu;
}

static bool
kbase_kmod_bo_wait(struct pan_kmod_bo *bo, int64_t timeout_ns,
                   bool for_read_only_access)
{
   /* Requires a working completion/fence mechanism, which is exactly the
    * open problem documented in docs/kbase-notes.md: kbase is not a DRM
    * device so there are no DRM syncobjs, KBASE_IOCTL_INTERNAL_FENCE_WAIT
    * was measured never to block, and poll()/read() on the kbase fd has not
    * been shown to deliver CS events. Returning false (rather than a
    * hopeful true) keeps callers from assuming the GPU is idle.
    */
   mesa_loge("kbase: bo_wait not implemented (no working fence mechanism)");
   return false;
}

static int
kbase_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   /* No deferred syncs are ever queued, because supported_bo_flags does not
    * advertise PAN_KMOD_BO_FLAG_WB_MMAP, so nothing can be pending here.
    */
   return 0;
}

static struct pan_kmod_bo *
kbase_kmod_bo_import(struct pan_kmod_dev *dev, uint32_t handle, uint64_t size)
{
   /* dma-buf import. The common pan_kmod_bo_import() reaches this through
    * drmPrimeFDToHandle() on dev->fd, which cannot work on a misc device -
    * so wiring this up needs changes above this backend too, not just here.
    * kbase's own path is KBASE_IOCTL_MEM_IMPORT with
    * BASE_MEM_IMPORT_TYPE_UMM. Phase 3 work.
    */
   mesa_loge("kbase: bo_import not implemented yet");
   return NULL;
}

static int
kbase_kmod_bo_export(struct pan_kmod_bo *bo, int dmabuf_fd)
{
   mesa_loge("kbase: bo_export not implemented yet");
   return -1;
}

static struct pan_kmod_vm *
kbase_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                     uint64_t va_start, uint64_t va_range)
{
   /* kbase has no explicit VM object: a context owns exactly one address
    * space, and an allocation is mapped into it at MEM_ALLOC time rather
    * than through a separate bind step.
    *
    * This is the "single implicit VM" model: the VM is a bookkeeping
    * object, handle 0 (which pan_kmod.h documents as the value for KMDs
    * with one VM per context), and vm_bind does not move anything because
    * BASE_MEM_SAME_VA already placed every BO at a fixed address chosen by
    * the kernel.
    *
    * READ THIS BEFORE TRUSTING GPU-VISIBLE ADDRESSES. It is not the
    * faithful implementation. pan_kmod's contract is that the caller picks
    * a VA and vm_bind maps the BO there; here the address is whatever the
    * kernel already gave us, so a caller that computes GPU addresses from
    * its own VA allocator will disagree with reality. PanVK does exactly
    * that (panvk_vX_device.c builds util_vma_heaps and hands addresses to
    * the mempools). vm_bind below therefore checks each mapping and warns
    * when the requested VA is not the BO's real one, rather than failing
    * silently - so the divergence shows up in logcat instead of as
    * corrupted descriptors later.
    *
    * That is survivable right now only because nothing submits GPU work
    * yet. Making the GPU dereference PanVK-assigned addresses correctly
    * needs the other design: drop SAME_VA and do real VA management here.
    * See ROADMAP.md Phase 2 for both options.
    */
   struct kbase_kmod_vm *vm = pan_kmod_alloc(dev->allocator, sizeof(*vm));

   if (!vm) {
      mesa_loge("kbase: failed to allocate VM object");
      return NULL;
   }

   /* handle 0: one address space per context, nothing to allocate. */
   pan_kmod_vm_init(&vm->base, dev, 0, flags);

   vm->map_count = 0;
   vm->mismatch_count = 0;
   vm->warned = false;

   return &vm->base;
}

static void
kbase_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   struct kbase_kmod_vm *kbase_vm = to_kbase_kmod_vm(vm);

   if (kbase_vm->mismatch_count) {
      mesa_logw("kbase: VM torn down after %u of %u mappings landed at a "
                "different address than requested (SAME_VA shim)",
                kbase_vm->mismatch_count, kbase_vm->map_count);
   }

   pan_kmod_vm_cleanup(vm);
   pan_kmod_free(vm->dev->allocator, kbase_vm);
}

static int
kbase_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                   struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   struct kbase_kmod_vm *kbase_vm = to_kbase_kmod_vm(vm);

   for (uint32_t i = 0; i < op_count; i++) {
      struct pan_kmod_vm_op *op = &ops[i];

      switch (op->type) {
      case PAN_KMOD_VM_OP_TYPE_MAP: {
         /* SAME_VA: the BO is already mapped, at this address. */
         uint64_t real_va =
            (uint64_t)(uintptr_t)to_kbase_kmod_bo(op->map.bo)->cpu +
            (uint64_t)op->map.bo_offset;

         kbase_vm->map_count++;

         if (op->va.start == PAN_KMOD_VM_MAP_AUTO_VA) {
            /* Caller let us choose - report where it actually is. This is
             * the one case the shim models correctly.
             */
            op->va.start = real_va;
            break;
         }

         if (op->va.start != real_va) {
            kbase_vm->mismatch_count++;

            /* FAIL, do not pretend. An earlier version of this shim logged
             * a warning and returned success, on the theory that a wrong
             * VA is harmless until something submits GPU work. That is
             * false: PanVK dereferences addresses from its own VA
             * allocator during vkCreateDevice's mempool setup, so
             * succeeding here turns a clean error into a segfault inside
             * device creation. Measured - see ROADMAP.md Phase 2.
             *
             * Returning an error makes vkCreateDevice fail cleanly instead,
             * which is the honest state until BASE_MEM_FIXED-based
             * allocation lands and this can actually honour the request.
             */
            if (!kbase_vm->warned) {
               kbase_vm->warned = true;
               mesa_loge("kbase: vm_bind cannot honour a caller-chosen VA - "
                         "requested 0x%" PRIx64 ", BO is at 0x%" PRIx64
                         " (SAME_VA). Failing rather than returning a "
                         "mapping the caller would dereference at the wrong "
                         "address. See kbase_kmod_vm_create()'s comment.",
                         op->va.start, real_va);
            }
            return -1;
         }
         break;
      }

      case PAN_KMOD_VM_OP_TYPE_UNMAP:
         /* Nothing to do: the mapping goes away with the BO, when
          * kbase_kmod_bo_free() munmaps it. See docs/kbase-notes.md on
          * SAME_VA free semantics.
          */
         break;

      case PAN_KMOD_VM_OP_TYPE_SYNC_ONLY:
         /* No VM queue to order against - binds take effect immediately. */
         break;

      default:
         mesa_loge("kbase: unknown vm_op type %d", op->type);
         return -1;
      }
   }

   return 0;
}

const struct pan_kmod_ops kbase_kmod_ops = {
   .dev_create = kbase_kmod_dev_create,
   .dev_destroy = kbase_kmod_dev_destroy,
   .bo_alloc = kbase_kmod_bo_alloc,
   .bo_free = kbase_kmod_bo_free,
   .bo_import = kbase_kmod_bo_import,
   .bo_export = kbase_kmod_bo_export,
   .bo_get_mmap_offset = kbase_kmod_bo_get_mmap_offset,
   .bo_wait = kbase_kmod_bo_wait,
   .flush_bo_map_syncs = kbase_kmod_flush_bo_map_syncs,
   .vm_create = kbase_kmod_vm_create,
   .vm_destroy = kbase_kmod_vm_destroy,
   .vm_bind = kbase_kmod_vm_bind,
};
