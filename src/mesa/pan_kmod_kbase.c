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
#include <stdlib.h>
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

   /* CSF UK interface version reported by KBASE_IOCTL_VERSION_CHECK. */
   struct {
      uint16_t major;
      uint16_t minor;
   } uk_version;
};

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

   if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver) < 0)
      return false;

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

   /* Handshake, in the order kbase requires. */
   struct kbase_ioctl_version_check ver = { .major = 0, .minor = 0 };
   if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver) < 0) {
      mesa_loge("kbase: VERSION_CHECK failed");
      goto err_cleanup;
   }

   kbase_dev->uk_version.major = ver.major;
   kbase_dev->uk_version.minor = ver.minor;

   struct kbase_ioctl_set_flags set_flags = { .create_flags = 0 };
   if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags) < 0) {
      mesa_loge("kbase: SET_FLAGS failed");
      goto err_cleanup;
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
    * SAME_VA address only exists once mmap() has resolved the cookie. There
    * is no separate "get an offset then mmap it later" step to expose.
    */
   mesa_loge("kbase: bo_get_mmap_offset is not meaningful for SAME_VA BOs");
   return (off_t)-1;
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
    * than through a separate bind step. Faithfully modelling pan_kmod's
    * explicit-VM semantics on top of that needs a design decision (emulate
    * a single implicit VM, or push VA management down here), so it is left
    * unimplemented rather than guessed at.
    */
   mesa_loge("kbase: vm_create not implemented yet");
   return NULL;
}

static void
kbase_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   mesa_loge("kbase: vm_destroy not implemented yet");
}

static int
kbase_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                   struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   mesa_loge("kbase: vm_bind not implemented yet");
   return -1;
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
