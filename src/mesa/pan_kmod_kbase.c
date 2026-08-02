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
#include <poll.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "util/macros.h"
#include "util/os_file.h" /* os_dupfd_cloexec, for the dma-buf import */
#include "util/os_misc.h"
#include "util/simple_mtx.h"
#include "util/u_memory.h"
#include "util/vma.h"

/* For struct drm_panthor_csif_info. PanVK reads CSF geometry through
 * panthor_kmod_get_csif_props(), so a kbase device has to be able to fill
 * that same struct - see kbase_query_csif_props().
 */
#include "drm-uapi/panthor_drm.h"

#include "pan_kmod.h"
#include "pan_kmod_backend.h"
#include "pan_kmod_kbase.h"

/* Vendored kbase UAPI headers. The include path is supplied by the meson
 * snippet in src/mesa/meson.build.kbase - see src/mesa/README.md.
 */
#include "mali_base_common_kernel.h" /* BASE_MEM_PROT_* */
#include "mali_base_kernel.h"        /* base_mem_alloc_flags */
#include "mali_kbase_ioctl.h"
#include "csf/mali_base_csf_kernel.h" /* base_csf_notification */
#include "csf/mali_kbase_csf_ioctl.h"

/* CS_INSERT / CS_EXTRACT / CS_ACTIVE offsets and, more importantly, which
 * of the three mmap'd user-IO pages each lives in. Copied into the Mesa
 * tree next to this file by `make mesa-backend-sync`, since it is not part
 * of the vendored uapi header set - these are firmware-interface offsets,
 * not ioctl uapi. src/utils/csf_user_regs.h in this repo is the original.
 */
#include "csf_user_regs.h"

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
/* Base of the kbase FIXED_VA zone, and how much of it this backend is
 * willing to hand out.
 *
 * Measured on a Poco X8 Pro / Mali-G720 / r49p1 by tests/fixed_va_probe: a
 * BASE_MEM_FIXED allocation is honoured exactly at 0x800200000000 and
 * above, and rejected with ENOMEM outside the zone. The zone's *size* was
 * not established - allocations were verified up to base + 0x30001000
 * (768MB). The range below is therefore deliberately larger than what has
 * been proven: running off the end shows up as MEM_ALLOC_EX failing
 * ENOMEM, which bo_alloc handles by returning the VA to the heap and
 * failing cleanly, so over-estimating costs a failed allocation rather
 * than corruption.
 *
 * Overridable so a device with a different zone can be pointed at without
 * a rebuild - the base is a kernel constant and may well differ across
 * kbase versions or vendors.
 */
#define KBASE_FIXED_VA_ZONE_START 0x800200000000ull
#define KBASE_FIXED_VA_ZONE_SIZE (8ull << 30)

/* Alternative bases tried at device-init if the default is rejected.
 * Deliberately probed with BASE_MEM_FIXED rather than BASE_MEM_FIXABLE:
 * the two are mutually exclusive per context, so a FIXABLE probe would
 * poison every subsequent FIXED allocation with EINVAL. That is not
 * hypothetical - it is exactly what happened while writing
 * tests/fixed_va_probe. See docs/kbase-notes.md.
 */
static const uint64_t kbase_fixed_va_candidates[] = {
   KBASE_FIXED_VA_ZONE_START,
   0x800000000000ull,
   0x800100000000ull,
   0x800400000000ull,
};

struct kbase_kmod_bo {
   struct pan_kmod_bo base;

   /* GPU virtual address, chosen by this backend's VA allocator and made
    * real by MEM_ALLOC_EX + BASE_MEM_FIXED. Unlike the old SAME_VA model,
    * this is NOT the CPU address: pan_kmod_bo_mmap() maps it separately
    * and lands wherever the kernel puts it.
    *
    * For an imported BO this is instead the address that mmap() resolved
    * the import's NEED_MMAP cookie to - see kbase_kmod_bo_import_fd().
    */
   uint64_t gpu_va;

   /* Imported (dma-buf) BOs only; NULL and 0 for everything else.
    *
    * MEM_IMPORT hands back an mmap cookie rather than an address, so the
    * mapping below is what turned it into one. It has to be kept: the
    * cookie is single-use (a second mmap of it fails EINVAL, measured by
    * tests/dmabuf_import_probe), so the address cannot be re-derived, and
    * the region dies with this munmap exactly like a SAME_VA allocation.
    */
   void *import_map;
   size_t import_map_size;
   int import_dmabuf_fd; /* our dup; -1 when not an import */
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

   /* CSF interface geometry, in the shape PanVK expects.
    *
    * PanVK reads this through panthor_kmod_get_csif_props() in six places
    * (panvk_vX_device.c, exception_handler, gpu_queue, cmd_buffer,
    * cmd_draw, utrace). That accessor container_of()s the pan_kmod_dev
    * into a panthor_kmod_dev, so on a kbase device it reads whatever
    * happens to follow this struct - which is how nr_registers ended up
    * garbage and cs_builder wrote off the end of its buffer. Filling a
    * real one here and dispatching to it is the fix.
    */
   struct drm_panthor_csif_info csif;

   /* GPU VA allocator over the FIXED_VA zone.
    *
    * On the device rather than on the VM because kbase genuinely has one
    * address space per context - there is nothing per-VM to allocate from,
    * and BOs can be created with exclusive_vm == NULL.
    */
   struct {
      simple_mtx_t lock;
      struct util_vma_heap heap;
      uint64_t start;
      uint64_t size;
      bool ready;
   } va;
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

/* Temporary diagnostic instrumentation for the ~307-case ceiling found
 * running dEQP-VK.api.object_management through deqp-vk (docs/kbase-notes.md
 * - "root-cause dig"): after a few hundred VkInstance/VkDevice create+destroy
 * cycles in one process, vkCreateDevice starts failing with
 * VK_ERROR_OUT_OF_DEVICE_MEMORY. Two passes of hand-written synthetic
 * reproduction (tests/device_churn_probe) came up clean at far larger scale
 * than the real failure, so this instruments the real driver instead of
 * guessing the reproduction shape further. Gated behind
 * PANVK_KBASE_DEBUG_COUNTERS so it is silent by default, matching this
 * file's PANVK_KBASE_FIXED_VA_BASE convention. Atomic because CTS's
 * multithreaded_* test groups create devices from multiple threads at once
 * (see tests/device_churn_probe's --threads mode, which confirmed
 * concurrent creation itself is not the ceiling) - a torn counter here would
 * make the log lie about live_devices at the moment of any real failure.
 */
#include <stdatomic.h>

static int
kbase_debug_counters_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0) {
      const char *v = getenv("PANVK_KBASE_DEBUG_COUNTERS");
      enabled = v && *v && *v != '0';
   }

   return enabled;
}

static atomic_uint kbase_debug_live_devices;
static atomic_uint kbase_debug_total_devices_created;
static atomic_uint kbase_debug_live_bos;
static atomic_uint kbase_debug_total_bos_created;

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
      /* EXECUTABLE maps onto BASE_MEM_PROT_GPU_EX - see kbase_kmod_bo_alloc().
       * Deliberately not WB_MMAP: this backend's mappings are write-combine,
       * which is what makes pan_kmod_queue_bo_map_sync() a no-op here.
       */
      .supported_bo_flags = PAN_KMOD_BO_FLAG_EXECUTABLE,
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

static int kbase_init_va_heap(struct kbase_kmod_dev *dev);
static void kbase_query_csif_props(struct kbase_kmod_dev *dev);
static void kbase_query_allowed_priorities(struct kbase_kmod_dev *dev);
static void kbase_init_context_zones(struct kbase_kmod_dev *dev);

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

   unsigned dbg_creation_no = 0;
   if (kbase_debug_counters_enabled()) {
      dbg_creation_no =
         atomic_fetch_add(&kbase_debug_total_devices_created, 1) + 1;
      mesa_logi("kbase-dbg: dev_create #%u entry, fd=%d, live_devices=%u",
                dbg_creation_no, fd,
                atomic_load(&kbase_debug_live_devices));
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
         if (kbase_debug_counters_enabled())
            mesa_logi("kbase-dbg: dev_create #%u FAILED at SET_FLAGS "
                      "(errno=%d %s), live_devices=%u",
                      dbg_creation_no, errno, strerror(errno),
                      atomic_load(&kbase_debug_live_devices));
         goto err_cleanup;
      }
   }

   if (kbase_dev_query_props(kbase_dev)) {
      if (kbase_debug_counters_enabled())
         mesa_logi("kbase-dbg: dev_create #%u FAILED at "
                   "kbase_dev_query_props, live_devices=%u",
                   dbg_creation_no, atomic_load(&kbase_debug_live_devices));
      goto err_cleanup;
   }

   if (kbase_init_va_heap(kbase_dev)) {
      if (kbase_debug_counters_enabled())
         mesa_logi("kbase-dbg: dev_create #%u FAILED at kbase_init_va_heap, "
                   "live_devices=%u",
                   dbg_creation_no, atomic_load(&kbase_debug_live_devices));
      goto err_cleanup;
   }

   kbase_query_csif_props(kbase_dev);
   kbase_query_allowed_priorities(kbase_dev);
   kbase_init_context_zones(kbase_dev);

   if (kbase_debug_counters_enabled()) {
      unsigned live = atomic_fetch_add(&kbase_debug_live_devices, 1) + 1;
      mesa_logi("kbase-dbg: dev_create #%u SUCCESS, live_devices=%u",
                dbg_creation_no, live);
   }

   return &kbase_dev->base;

err_cleanup:
   pan_kmod_dev_cleanup(&kbase_dev->base);
   pan_kmod_free(allocator, kbase_dev);
   return NULL;
}

/* Find the FIXED_VA zone and set up the GPU VA allocator over it.
 *
 * Probes with a real one-page BASE_MEM_FIXED allocation and frees it again.
 * Deliberately not BASE_MEM_FIXABLE, which would also reveal the zone: the
 * two flags are mutually exclusive per context, so a FIXABLE probe would
 * make every subsequent FIXED allocation fail EINVAL and break the whole
 * backend. That mistake is documented in docs/kbase-notes.md because it
 * cost a debugging cycle in tests/fixed_va_probe.
 *
 * Returns 0 on success.
 */
static int
kbase_init_va_heap(struct kbase_kmod_dev *dev)
{
   const char *override = os_get_option("PANVK_KBASE_FIXED_VA_BASE");
   uint64_t forced = 0;

   if (override)
      forced = strtoull(override, NULL, 0);

   for (unsigned i = 0; i < ARRAY_SIZE(kbase_fixed_va_candidates); i++) {
      uint64_t base = forced ? forced : kbase_fixed_va_candidates[i];

      union kbase_ioctl_mem_alloc_ex probe = { 0 };
      probe.in.va_pages = 1;
      probe.in.commit_pages = 1;
      probe.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                       BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                       BASE_MEM_FIXED;
      probe.in.fixed_address = base;

      if (ioctl(dev->base.fd, KBASE_IOCTL_MEM_ALLOC_EX, &probe) < 0) {
         if (kbase_debug_counters_enabled())
            mesa_logi("kbase-dbg: va_heap candidate %u (0x%" PRIx64
                      ") MEM_ALLOC_EX failed: %s",
                      i, base, strerror(errno));
         if (forced)
            break;
         continue;
      }

      bool exact = probe.out.gpu_va == base;

      struct kbase_ioctl_mem_free f = { .gpu_addr = probe.out.gpu_va };
      if (ioctl(dev->base.fd, KBASE_IOCTL_MEM_FREE, &f) < 0)
         mesa_logw("kbase: could not free the FIXED_VA probe allocation");

      if (!exact) {
         if (kbase_debug_counters_enabled())
            mesa_logi("kbase-dbg: va_heap candidate %u (0x%" PRIx64
                      ") got 0x%" PRIx64 " instead - not exact, rejecting",
                      i, base, (uint64_t)probe.out.gpu_va);
         if (forced)
            break;
         continue;
      }

      if (kbase_debug_counters_enabled())
         mesa_logi("kbase-dbg: va_heap candidate %u (0x%" PRIx64
                   ") accepted",
                   i, base);

      dev->va.start = base;
      dev->va.size = KBASE_FIXED_VA_ZONE_SIZE;
      simple_mtx_init(&dev->va.lock, mtx_plain);
      util_vma_heap_init(&dev->va.heap, dev->va.start, dev->va.size);
      dev->va.ready = true;
      return 0;
   }

   mesa_loge("kbase: no usable FIXED_VA zone found - GPU allocation will "
             "not work. Set PANVK_KBASE_FIXED_VA_BASE if this device puts "
             "the zone somewhere unexpected.");
   return -1;
}

/* Number of CS registers, and how many of those the kernel reserves.
 *
 * kbase's KBASE_IOCTL_CS_GET_GLB_IFACE reports CSG and CS *slot* counts
 * but not register counts - panthor gets those from the firmware
 * interface, and there is no kbase equivalent exposed to userspace. These
 * are the architectural values for CSF, and they are not guesses here:
 * tests/cs_encode_probe and tests/live_kick_probe build command streams
 * with exactly nr_registers=96 / nr_kernel_registers=4 using Mesa's own
 * cs_builder, and those streams execute on this GPU (CS_EXTRACT advances).
 * See docs/mesa-cs-builder.md and docs/kbase-notes.md.
 */
#define KBASE_CS_REG_COUNT 96
#define KBASE_UNPRESERVED_CS_REG_COUNT 4
#define KBASE_SCOREBOARD_SLOT_COUNT 8

/* Fill in the CSF interface geometry PanVK reads via
 * panthor_kmod_get_csif_props(). Best-effort: on failure the architectural
 * defaults are kept, which is still far better than the garbage a
 * container_of() onto the wrong struct produces.
 */
static void
kbase_query_csif_props(struct kbase_kmod_dev *dev)
{
   dev->csif = (struct drm_panthor_csif_info){
      .csg_slot_count = 8,
      .cs_slot_count = 8,
      .cs_reg_count = KBASE_CS_REG_COUNT,
      .scoreboard_slot_count = KBASE_SCOREBOARD_SLOT_COUNT,
      .unpreserved_cs_reg_count = KBASE_UNPRESERVED_CS_REG_COUNT,
   };

   /* Counts only: passing 0 for the max_*_num fields means the kernel
    * fills in the totals without writing any group/stream arrays.
    */
   union kbase_ioctl_cs_get_glb_iface iface = { 0 };

   if (ioctl(dev->base.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &iface) < 0) {
      mesa_logw("kbase: CS_GET_GLB_IFACE failed (%s), using default CSF "
                "interface geometry", strerror(errno));
      return;
   }

   if (iface.out.group_num) {
      dev->csif.csg_slot_count = iface.out.group_num;

      /* total_stream_num is summed across all groups. */
      if (iface.out.total_stream_num)
         dev->csif.cs_slot_count = iface.out.total_stream_num /
                                   iface.out.group_num;
   }

   mesa_logi("kbase: CSF iface v%u.%u.%u, %u CSG slots x %u streams",
             (iface.out.glb_version >> 24) & 0xff,
             (iface.out.glb_version >> 16) & 0xff,
             iface.out.glb_version & 0xffff,
             dev->csif.csg_slot_count, dev->csif.cs_slot_count);
}

/* Work out which queue-group priorities this context may actually use.
 *
 * PanVK refuses vkCreateDevice with VK_ERROR_NOT_PERMITTED_KHR unless the
 * requested global priority is set in props.allowed_group_priorities_mask
 * (panvk_vX_device.c:239). Leaving that mask at 0 - which is what an
 * unset field means - rejects every priority including the default MEDIUM,
 * so no device can ever be created.
 *
 * kbase answers this directly with KBASE_IOCTL_CONTEXT_PRIORITY_CHECK,
 * which clamps a requested priority to what the context is permitted and
 * returns it. The vendor blob calls it for the same reason (see the
 * libGLES_mali.so ioctl survey in docs/kbase-notes.md). A priority is
 * allowed iff it survives the round-trip unchanged.
 */
static void
kbase_query_allowed_priorities(struct kbase_kmod_dev *dev)
{
   static const struct {
      uint8_t kbase_prio;
      enum pan_kmod_group_allow_priority_flags flag;
   } prios[] = {
      { BASE_QUEUE_GROUP_PRIORITY_LOW, PAN_KMOD_GROUP_ALLOW_PRIORITY_LOW },
      { BASE_QUEUE_GROUP_PRIORITY_MEDIUM,
        PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM },
      { BASE_QUEUE_GROUP_PRIORITY_HIGH, PAN_KMOD_GROUP_ALLOW_PRIORITY_HIGH },
      { BASE_QUEUE_GROUP_PRIORITY_REALTIME,
        PAN_KMOD_GROUP_ALLOW_PRIORITY_REALTIME },
   };

   uint32_t mask = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(prios); i++) {
      struct kbase_ioctl_context_priority_check check = {
         .priority = prios[i].kbase_prio,
      };

      if (ioctl(dev->base.fd, KBASE_IOCTL_CONTEXT_PRIORITY_CHECK, &check) < 0)
         continue;

      if (check.priority == prios[i].kbase_prio)
         mask |= prios[i].flag;
   }

   if (!mask) {
      /* Either the ioctl is missing or it clamped everything. MEDIUM is
       * what every group this repo has created on hardware uses
       * (priority 0 in tests/queue_group and tests/live_kick_probe, which
       * do get scheduled and executed), so assume it rather than leave a
       * zero mask that makes vkCreateDevice impossible.
       */
      mesa_logw("kbase: CONTEXT_PRIORITY_CHECK gave nothing usable, "
                "assuming MEDIUM is allowed");
      mask = PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM;
   }

   dev->base.props.allowed_group_priorities_mask = mask;
}

/* Initialise the JIT and EXEC_VA context zones.
 *
 * CS_TILER_HEAP_INIT fails ENOMEM without this: a tiler heap's chunks are
 * backed by the JIT allocator, so the pool has to exist before a heap can
 * be created. The vendor blob calls both of these during context setup
 * (see the libGLES_mali.so ioctl survey in docs/kbase-notes.md), and
 * tests/live_kick_probe only gets a working heap because it replicates
 * that sequence.
 *
 * Both are one-shot per kbase context. vkCreateDevice builds a second
 * pan_kmod_dev on a dup() of the physical device's fd - same context - so
 * skip when this device wraps an already-initialised one, otherwise the
 * second call fails and logs noise for no reason.
 */
static void
kbase_init_context_zones(struct kbase_kmod_dev *dev)
{
   if (dev->already_initialized)
      return;

   struct kbase_ioctl_mem_jit_init jit = {
      .va_pages = 1 << 14,   /* 64MB of 4K pages */
      .max_allocations = 255,
      .trim_level = 0,
      .group_id = 0,
      .phys_pages = 1 << 14,
   };

   if (ioctl(dev->base.fd, KBASE_IOCTL_MEM_JIT_INIT, &jit) < 0)
      mesa_logw("kbase: MEM_JIT_INIT failed (%s) - tiler heaps will not work",
                strerror(errno));

   struct kbase_ioctl_mem_exec_init exec = {
      .va_pages = 1 << 16,   /* 256MB of 4K pages */
   };

   if (ioctl(dev->base.fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec) < 0)
      mesa_logw("kbase: MEM_EXEC_INIT failed (%s)", strerror(errno));
}

/* ---------------------------------------------------------------- *
 * CSF queue-group lifecycle. See pan_kmod_kbase.h for why these exist.
 * ---------------------------------------------------------------- */

int
pan_kmod_kbase_group_create(struct pan_kmod_dev *dev, uint64_t shader_present,
                            uint8_t priority, uint8_t *group_handle)
{
   if (!shader_present)
      shader_present = dev->props.shader_present;

   /* The endpoint masks go straight to firmware as
    * CSG_ALLOW_COMPUTE/FRAGMENT/TILER. An earlier probe passed ~0ULL and
    * the kernel accepted it - it only validates *_max <= hweight64(*_mask)
    * - but asking for 64 cores on an 8-core GPU is not something to hand
    * firmware. Use the real mask.
    */
   union kbase_ioctl_cs_queue_group_create_1_6 create = { 0 };
   create.in.tiler_mask = shader_present;
   create.in.fragment_mask = shader_present;
   create.in.compute_mask = shader_present;
   create.in.cs_min = 1;
   create.in.priority = priority;
   create.in.tiler_max = 1;
   create.in.fragment_max = 1;
   create.in.compute_max = 1;

   if (ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &create) < 0) {
      mesa_loge("kbase: CS_QUEUE_GROUP_CREATE_1_6 failed: %s",
                strerror(errno));
      return -1;
   }

   *group_handle = create.out.group_handle;
   return 0;
}

void
pan_kmod_kbase_group_destroy(struct pan_kmod_dev *dev, uint8_t group_handle)
{
   struct kbase_ioctl_cs_queue_group_term term = {
      .group_handle = group_handle,
   };

   if (ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term) < 0)
      mesa_loge("kbase: CS_QUEUE_GROUP_TERMINATE failed: %s", strerror(errno));
}

int
pan_kmod_kbase_tiler_heap_create(struct pan_kmod_dev *dev, uint32_t chunk_size,
                                 uint32_t initial_chunks, uint32_t max_chunks,
                                 uint64_t *gpu_heap_va,
                                 uint64_t *first_chunk_va)
{
   union kbase_ioctl_cs_tiler_heap_init heap = { 0 };
   heap.in.chunk_size = chunk_size;
   heap.in.initial_chunks = initial_chunks;
   heap.in.max_chunks = max_chunks;
   heap.in.target_in_flight = 1;
   heap.in.group_id = 0;
   heap.in.buf_desc_va = 0;

   if (ioctl(dev->fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &heap) < 0) {
      mesa_loge("kbase: CS_TILER_HEAP_INIT failed: %s", strerror(errno));
      return -1;
   }

   if (gpu_heap_va)
      *gpu_heap_va = heap.out.gpu_heap_va;
   if (first_chunk_va)
      *first_chunk_va = heap.out.first_chunk_va;

   return 0;
}

void
pan_kmod_kbase_tiler_heap_destroy(struct pan_kmod_dev *dev,
                                  uint64_t gpu_heap_va)
{
   struct kbase_ioctl_cs_tiler_heap_term term = {
      .gpu_heap_va = gpu_heap_va,
   };

   if (ioctl(dev->fd, KBASE_IOCTL_CS_TILER_HEAP_TERM, &term) < 0)
      mesa_loge("kbase: CS_TILER_HEAP_TERM failed: %s", strerror(errno));
}

int
pan_kmod_kbase_queue_create(struct pan_kmod_dev *dev, uint8_t group_handle,
                            uint8_t csi_index, uint64_t ringbuf_size,
                            struct pan_kmod_kbase_cs *out)
{
   struct kbase_kmod_dev *kbase_dev = to_kbase_kmod_dev(dev);
   uint64_t aligned = ALIGN_POT(ringbuf_size, 4096);

   memset(out, 0, sizeof(*out));
   out->csi_index = csi_index;

   if (!kbase_dev->va.ready) {
      mesa_loge("kbase: no VA zone, cannot create a CS queue");
      return -1;
   }

   simple_mtx_lock(&kbase_dev->va.lock);
   uint64_t va = util_vma_heap_alloc(&kbase_dev->va.heap, aligned, 4096);
   simple_mtx_unlock(&kbase_dev->va.lock);

   if (!va) {
      mesa_loge("kbase: out of GPU VA for a CS ring buffer");
      return -1;
   }

   union kbase_ioctl_mem_alloc_ex alloc = { 0 };
   alloc.in.va_pages = aligned / 4096;
   alloc.in.commit_pages = aligned / 4096;
   alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                    BASE_MEM_FIXED;
   alloc.in.fixed_address = va;

   if (ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC_EX, &alloc) < 0 ||
       alloc.out.gpu_va != va) {
      mesa_loge("kbase: ring buffer allocation at 0x%" PRIx64 " failed: %s",
                va, strerror(errno));
      goto err_free_va;
   }

   out->ringbuf_gpu_va = va;
   out->ringbuf_size = aligned;

   out->ringbuf_cpu = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_SHARED,
                           dev->fd, (off_t)va);
   if (out->ringbuf_cpu == MAP_FAILED) {
      mesa_loge("kbase: ring buffer mmap failed: %s", strerror(errno));
      out->ringbuf_cpu = NULL;
      goto err_free_mem;
   }

   memset(out->ringbuf_cpu, 0, aligned);

   struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = va,
      .buffer_size = aligned,
      .priority = 0,
   };

   if (ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
      mesa_loge("kbase: CS_QUEUE_REGISTER failed: %s", strerror(errno));
      goto err_unmap_ring;
   }

   union kbase_ioctl_cs_queue_bind bind = { 0 };
   bind.in.buffer_gpu_addr = va;
   bind.in.group_handle = group_handle;
   bind.in.csi_index = csi_index;

   if (ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
      mesa_loge("kbase: CS_QUEUE_BIND failed: %s", strerror(errno));
      goto err_terminate_queue;
   }

   out->user_io = mmap(NULL, BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096,
                       PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd,
                       (off_t)bind.out.mmap_handle);
   if (out->user_io == MAP_FAILED) {
      mesa_loge("kbase: user-IO mmap failed: %s", strerror(errno));
      out->user_io = NULL;
      goto err_terminate_queue;
   }

   return 0;

err_terminate_queue: {
   struct kbase_ioctl_cs_queue_terminate term = { .buffer_gpu_addr = va };
   ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &term);
}
err_unmap_ring:
   munmap(out->ringbuf_cpu, aligned);
   out->ringbuf_cpu = NULL;
err_free_mem: {
   struct kbase_ioctl_mem_free f = { .gpu_addr = va };
   ioctl(dev->fd, KBASE_IOCTL_MEM_FREE, &f);
}
err_free_va:
   simple_mtx_lock(&kbase_dev->va.lock);
   util_vma_heap_free(&kbase_dev->va.heap, va, aligned);
   simple_mtx_unlock(&kbase_dev->va.lock);
   memset(out, 0, sizeof(*out));
   return -1;
}

void
pan_kmod_kbase_queue_destroy(struct pan_kmod_dev *dev,
                             struct pan_kmod_kbase_cs *cs)
{
   struct kbase_kmod_dev *kbase_dev = to_kbase_kmod_dev(dev);

   if (!cs->ringbuf_gpu_va)
      return;

   if (cs->user_io)
      munmap(cs->user_io, BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096);

   struct kbase_ioctl_cs_queue_terminate term = {
      .buffer_gpu_addr = cs->ringbuf_gpu_va,
   };
   if (ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &term) < 0)
      mesa_loge("kbase: CS_QUEUE_TERMINATE failed: %s", strerror(errno));

   if (cs->ringbuf_cpu)
      munmap(cs->ringbuf_cpu, cs->ringbuf_size);

   struct kbase_ioctl_mem_free f = { .gpu_addr = cs->ringbuf_gpu_va };
   if (ioctl(dev->fd, KBASE_IOCTL_MEM_FREE, &f) < 0)
      mesa_loge("kbase: ring buffer MEM_FREE failed: %s", strerror(errno));

   simple_mtx_lock(&kbase_dev->va.lock);
   util_vma_heap_free(&kbase_dev->va.heap, cs->ringbuf_gpu_va,
                      cs->ringbuf_size);
   simple_mtx_unlock(&kbase_dev->va.lock);

   memset(cs, 0, sizeof(*cs));
}

/* The user-IO pages are [doorbell][input][output], measured on-device by
 * tests/user_io_probe rather than read off the uapi comment, which describes
 * the contents and not the order. Getting this wrong does not fail loudly:
 * CS_INSERT lands in the doorbell page, CS_EXTRACT is read out of a page
 * firmware never writes, and everything looks like the GPU is ignoring you.
 * That cost this repo several commits - see csf_user_regs.h.
 */
static volatile uint8_t *
kbase_cs_page(const struct pan_kmod_kbase_cs *cs, unsigned page)
{
   return (volatile uint8_t *)cs->user_io + page * 4096;
}

void
pan_kmod_kbase_queue_publish_insert(const struct pan_kmod_kbase_cs *cs,
                                    uint64_t insert)
{
   if (!cs->user_io) {
      mesa_loge("kbase: publish on a queue that is not bound");
      return;
   }

   /* One 64-bit store, which is a single STR on aarch64 and so satisfies
    * the kernel's requirement that CS_INSERT be accessed atomically.
    */
   volatile uint8_t *input = kbase_cs_page(cs, CSF_USER_INPUT_PAGE);
   *(volatile uint64_t *)(input + CSF_USER_CS_INSERT_LO) = insert;

   /* Order the ring-buffer writes and CS_INSERT ahead of anything that
    * follows. The kernel does the equivalent (dmb(osh)) before ringing a
    * doorbell, and a still-running CS reads this value with no kernel
    * involvement at all, so the barrier is what makes the lock-free handoff
    * in panvk_vX_kbase_queue.c's submit_stream() sound.
    */
   __sync_synchronize();
}

int
pan_kmod_kbase_queue_kick(struct pan_kmod_dev *dev,
                          const struct pan_kmod_kbase_cs *cs)
{
   if (!cs->user_io) {
      mesa_loge("kbase: kick on a queue that is not bound");
      return -1;
   }

   /* Callers are expected to have waited for CS_ACTIVE to clear - see
    * pan_kmod_kbase_queue_wait_idle() below for what happens otherwise, and
    * submit_stream() for why the kick is now reached far less often.
    */
   struct kbase_ioctl_cs_queue_kick kick = {
      .buffer_gpu_addr = cs->ringbuf_gpu_va,
   };

   if (ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0) {
      mesa_loge("kbase: CS_QUEUE_KICK failed: %s", strerror(errno));
      return -1;
   }

   return 0;
}

/* Why this exists, measured on a Mali-G720 / kbase r49p1:
 *
 * Kicking a queue three times in a row, logging state at each kick:
 *
 *   kick 1: insert=24 extract=0  CS_ACTIVE=0  -> ran, extract reached 24
 *   kick 2: insert=48 extract=24 CS_ACTIVE=1  -> DID NOT RUN
 *   kick 3: insert=72 extract=48 CS_ACTIVE=0  -> ran, extract reached 72
 *
 * The failing kick is exactly the one issued while CS_ACTIVE was 1, which
 * lingers ~30-40ms after a stream finishes. Its bytes were not lost - they
 * sat in the ring until kick 3 flushed them, which is why the symptom is a
 * submit that never completes rather than one that errors. Adding a 200ms
 * delay before every kick made 4/4 submits run within 10ms; removing it
 * made every submit after the first time out.
 *
 * Ruled out - do not retry: ringing the user-IO doorbell page (page 0,
 * offset 0, value 1) the way Panfork's kbase_cs_submit() would if its
 * doorbell branch were not hardcoded off. Tried unconditionally, and the
 * failing kick still did not run. That page also reads back whatever was
 * last written to it (wrote 1, read 1 at the next kick), so on this device
 * it behaves as ordinary memory, not as an MMIO doorbell register. Panfork
 * disabling that branch looks deliberate.
 *
 * What this costs: serialisation. A caller that waits for idle before
 * every kick cannot keep two streams in flight, which defeats much of the
 * point of a ring buffer. It is correct but slow, and it is the honest
 * option while the real wake mechanism for an onslot idle CS is unknown -
 * the alternative is a submit path that silently drops work.
 *
 * SUPERSEDED - no longer called from the submit path.
 *
 * tests/kick_pipeline_probe drives a CS into the exact state above (slow
 * stream finished, extract == insert, CS_ACTIVE still 1) and kicks. The
 * behaviour does not reproduce: 10/10 kicks in that window were consumed,
 * most within a millisecond, none lost. panvk_vX_kbase_queue.c's
 * submit_stream() no longer waits.
 *
 * The most likely explanation is that the original trace predates the tiler
 * heap being set up through the shared init_gpu_tiler() path, so the group
 * was never properly schedulable and the kick had nothing to wake. That is a
 * hypothesis about the cause. The measurement that the wait is unnecessary
 * is not, and it is repeatable.
 *
 * Kept rather than deleted, because it is the record of what was measured
 * and it is what a bisect would want if this turns out to be device- or
 * firmware-specific rather than a fixed bug.
 */
bool
pan_kmod_kbase_queue_wait_idle(const struct pan_kmod_kbase_cs *cs,
                               unsigned timeout_ms)
{
   for (unsigned i = 0; i < timeout_ms; i++) {
      if (!pan_kmod_kbase_queue_active(cs))
         return true;

      os_time_sleep(1000);
   }

   return !pan_kmod_kbase_queue_active(cs);
}

uint64_t
pan_kmod_kbase_queue_extract(const struct pan_kmod_kbase_cs *cs)
{
   if (!cs->user_io)
      return 0;

   volatile uint8_t *output = kbase_cs_page(cs, CSF_USER_OUTPUT_PAGE);
   return *(volatile uint64_t *)(output + CSF_USER_CS_EXTRACT_LO);
}

bool
pan_kmod_kbase_queue_active(const struct pan_kmod_kbase_cs *cs)
{
   if (!cs->user_io)
      return false;

   volatile uint8_t *output = kbase_cs_page(cs, CSF_USER_OUTPUT_PAGE);
   return *(volatile uint32_t *)(output + CSF_USER_CS_ACTIVE) != 0;
}

int
pan_kmod_kbase_read_event(struct pan_kmod_dev *dev, int timeout_ms,
                          struct pan_kmod_kbase_event *out)
{
   struct pollfd pfd = {
      .fd = dev->fd,
      .events = POLLIN,
   };

   int pret = poll(&pfd, 1, timeout_ms);
   if (pret == 0)
      return 0;
   if (pret < 0) {
      if (errno == EINTR)
         return 0;
      mesa_loge("kbase: poll on the device fd failed: %s", strerror(errno));
      return -1;
   }

   /* A short read would leave the stream misaligned for every later
    * reader, so treat anything but a whole notification as an error
    * rather than trying to piece it together.
    */
   struct base_csf_notification notif = { 0 };
   ssize_t r = read(dev->fd, &notif, sizeof(notif));
   if (r != (ssize_t)sizeof(notif)) {
      mesa_loge("kbase: notification read returned %zd of %zu bytes: %s", r,
                sizeof(notif), strerror(errno));
      return -1;
   }

   if (!out)
      return 1;

   memset(out, 0, sizeof(*out));

   switch (notif.type) {
   case BASE_CSF_NOTIFICATION_EVENT:
      out->type = PAN_KMOD_KBASE_EVENT_KERNEL;
      break;
   case BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR:
      out->type = PAN_KMOD_KBASE_EVENT_GROUP_ERROR;
      out->group_handle = notif.payload.csg_error.handle;
      out->error_type = notif.payload.csg_error.error.error_type;
      break;
   default:
      out->type = PAN_KMOD_KBASE_EVENT_OTHER;
      break;
   }

   return 1;
}

const struct drm_panthor_csif_info *
pan_kmod_kbase_get_csif_props(const struct pan_kmod_dev *dev)
{
   const struct kbase_kmod_dev *kbase_dev =
      container_of(dev, const struct kbase_kmod_dev, base);

   return &kbase_dev->csif;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   struct kbase_kmod_dev *kbase_dev = to_kbase_kmod_dev(dev);

   if (kbase_debug_counters_enabled()) {
      unsigned live = atomic_fetch_sub(&kbase_debug_live_devices, 1) - 1;
      mesa_logi("kbase-dbg: dev_destroy, fd=%d, owns_fd=%d, live_devices=%u",
                dev->fd, !!(dev->flags & PAN_KMOD_DEV_FLAG_OWNS_FD), live);
   }

   if (kbase_dev->va.ready) {
      util_vma_heap_finish(&kbase_dev->va.heap);
      simple_mtx_destroy(&kbase_dev->va.lock);
   }

   pan_kmod_dev_cleanup(dev);
   pan_kmod_free(dev->allocator, kbase_dev);
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *dev,
                    struct pan_kmod_vm *exclusive_vm, uint64_t size,
                    uint32_t flags)
{
   /* Only what supported_bo_flags advertises. Refused rather than silently
    * ignored: a BO that quietly comes back without the property the caller
    * asked for fails later and further away, which is exactly how the
    * EXECUTABLE case first showed up - as a null deref two frames into a
    * dispatch, because the shader upload had failed and the assert that
    * would have caught it is compiled out under NDEBUG.
    */
   if (flags & ~(uint32_t)PAN_KMOD_BO_FLAG_EXECUTABLE) {
      mesa_loge("kbase: BO flags 0x%x not supported yet", flags);
      return NULL;
   }

   struct kbase_kmod_dev *kbase_dev = to_kbase_kmod_dev(dev);

   if (!kbase_dev->va.ready) {
      mesa_loge("kbase: no usable FIXED_VA zone, cannot allocate");
      return NULL;
   }

   struct kbase_kmod_bo *bo = pan_kmod_dev_alloc(dev, sizeof(*bo));
   if (!bo)
      return NULL;

   uint64_t aligned = ALIGN_POT(size, 4096);

   /* Shader code goes in the EXEC_VA zone, and nowhere else.
    *
    * kbase keeps executable memory in its own zone, reserved by the
    * KBASE_IOCTL_MEM_EXEC_INIT in dev_create(). BASE_MEM_FIXED places an
    * allocation in the FIXED_VA zone instead, so the two are mutually
    * exclusive: asking for both returns ENOMEM, which is what this backend
    * did on its first attempt at executable memory.
    *
    * So the address cannot come from this backend's own allocator here. The
    * kernel picks it, and because BASE_MEM_SAME_VA is absent the returned
    * gpu_va is a real GPU address rather than an mmap cookie - the same
    * address is then used as the mmap offset by bo_get_mmap_offset(), which
    * is how the CPU gets to write the code in.
    *
    * No GPU_WR: shader code is not written by the GPU, and asking for write
    * and execute on the same region is worth not doing by default.
    */
   if (flags & PAN_KMOD_BO_FLAG_EXECUTABLE) {
      union kbase_ioctl_mem_alloc alloc = { 0 };
      alloc.in.va_pages = aligned / 4096;
      alloc.in.commit_pages = aligned / 4096;
      alloc.in.extension = 0;
      alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                       BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX;

      if (ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC, &alloc) < 0) {
         mesa_loge("kbase: MEM_ALLOC for %" PRIu64
                   " bytes of executable memory failed: %s",
                   aligned, strerror(errno));
         if (kbase_debug_counters_enabled())
            mesa_logi("kbase-dbg: bo_alloc EXEC FAILED, size=%" PRIu64
                      " errno=%d %s, live_bos=%u",
                      aligned, errno, strerror(errno),
                      atomic_load(&kbase_debug_live_bos));
         pan_kmod_dev_free(dev, bo);
         return NULL;
      }

      bo->gpu_va = alloc.out.gpu_va;

      pan_kmod_bo_init(&bo->base, dev, exclusive_vm, aligned, flags,
                       (uint32_t)(bo->gpu_va >> 12));

      if (kbase_debug_counters_enabled()) {
         unsigned total =
            atomic_fetch_add(&kbase_debug_total_bos_created, 1) + 1;
         unsigned live = atomic_fetch_add(&kbase_debug_live_bos, 1) + 1;
         mesa_logi("kbase-dbg: bo_alloc EXEC #%u SUCCESS, gpu_va=0x%" PRIx64
                   ", size=%" PRIu64 ", live_bos=%u",
                   total, bo->gpu_va, aligned, live);
      }

      return &bo->base;
   }

   /* Pick the GPU address ourselves, from the FIXED_VA zone.
    *
    * This is the core of the non-SAME_VA model: previously the kernel
    * chose the address and it doubled as the CPU pointer, which meant
    * vm_bind could never honour a caller-chosen VA. Now the address comes
    * from this allocator and MEM_ALLOC_EX is told to use it, so vm_bind
    * has a real answer to give. See docs/kbase-notes.md.
    */
   simple_mtx_lock(&kbase_dev->va.lock);
   uint64_t va = util_vma_heap_alloc(&kbase_dev->va.heap, aligned, 4096);
   simple_mtx_unlock(&kbase_dev->va.lock);

   if (!va) {
      mesa_loge("kbase: FIXED_VA zone exhausted (%" PRIu64 " bytes requested)",
                aligned);
      pan_kmod_dev_free(dev, bo);
      return NULL;
   }

   union kbase_ioctl_mem_alloc_ex alloc = { 0 };
   alloc.in.va_pages = aligned / 4096;
   alloc.in.commit_pages = aligned / 4096;
   alloc.in.extension = 0;
   alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                    BASE_MEM_FIXED;

   /* Shader code. PanVK's dev->mempools.exec asks for this, and every shader
    * binary - including the precompiled ones behind vkCmdFillBuffer and
    * friends - is uploaded through it, so without this nothing that runs a
    * shader can work.
    *
    * kbase wants the executable region declared up front too, which is what
    * the KBASE_IOCTL_MEM_EXEC_INIT in dev_create() is for; the vendor blob
    * calls it and this backend copied that before it had a use for it.
    */
   if (flags & PAN_KMOD_BO_FLAG_EXECUTABLE)
      alloc.in.flags |= BASE_MEM_PROT_GPU_EX;

   alloc.in.fixed_address = va;

   if (ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC_EX, &alloc) < 0) {
      /* ENOMEM here means the address is unavailable - past the real end of
       * the zone, or already occupied. Either way the VA is ours again.
       */
      mesa_loge("kbase: MEM_ALLOC_EX at 0x%" PRIx64 " failed: %s", va,
                strerror(errno));
      simple_mtx_lock(&kbase_dev->va.lock);
      util_vma_heap_free(&kbase_dev->va.heap, va, aligned);
      simple_mtx_unlock(&kbase_dev->va.lock);
      pan_kmod_dev_free(dev, bo);
      return NULL;
   }

   if (alloc.out.gpu_va != va) {
      /* BASE_MEM_FIXED is documented as exact, and measured exact. If the
       * kernel ever silently relocates, every GPU address PanVK derives
       * from this would be wrong, so fail loudly rather than continue.
       */
      mesa_loge("kbase: MEM_ALLOC_EX ignored fixed_address (asked 0x%" PRIx64
                ", got 0x%" PRIx64 ")", va, (uint64_t)alloc.out.gpu_va);
      simple_mtx_lock(&kbase_dev->va.lock);
      util_vma_heap_free(&kbase_dev->va.heap, va, aligned);
      simple_mtx_unlock(&kbase_dev->va.lock);
      pan_kmod_dev_free(dev, bo);
      return NULL;
   }

   bo->gpu_va = va;
   bo->import_map = NULL;
   bo->import_map_size = 0;
   bo->import_dmabuf_fd = -1;

   /* No mmap() here. Under SAME_VA the CPU mapping had to happen at alloc
    * time because it was what resolved the cookie into an address; with a
    * fixed address there is nothing to resolve, so the CPU mapping is left
    * to pan_kmod_bo_mmap() via bo_get_mmap_offset(). BOs that are never
    * CPU-accessed now cost no address space in this process.
    */

   /* kbase has no GEM-style handle namespace, and pan_kmod's common layer
    * keys its handle_to_bo sparse array off bo->handle. The GPU VA's page
    * number is unique per device and stable for the BO's lifetime.
    */
   uint32_t handle = (uint32_t)(va >> 12);

   pan_kmod_bo_init(&bo->base, dev, exclusive_vm, aligned, flags, handle);

   if (kbase_debug_counters_enabled()) {
      unsigned total = atomic_fetch_add(&kbase_debug_total_bos_created, 1) + 1;
      unsigned live = atomic_fetch_add(&kbase_debug_live_bos, 1) + 1;
      mesa_logi("kbase-dbg: bo_alloc #%u SUCCESS, gpu_va=0x%" PRIx64
                ", size=%" PRIu64 ", live_bos=%u",
                total, va, aligned, live);
   }

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
bool
pan_kmod_kbase_fence_validate(int fd, int fence_fd)
{
   struct kbase_ioctl_fence_validate v = { .fd = fence_fd };

   return ioctl(fd, KBASE_IOCTL_FENCE_VALIDATE, &v) == 0;
}

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

   struct kbase_kmod_dev *kbase_dev = to_kbase_kmod_dev(bo->dev);

   /* An imported region frees the other way round: munmap() *is* the free,
    * and MEM_FREE on it returns EINVAL - measured by
    * tests/dmabuf_import_probe (FREE_MODEL=munmap_only). Doing it backwards
    * would leak a kernel region per imported buffer, which for a swapchain
    * means per image per resize.
    */
   if (bo->flags & PAN_KMOD_BO_FLAG_IMPORTED) {
      if (kbase_bo->import_map)
         munmap(kbase_bo->import_map, kbase_bo->import_map_size);
      if (kbase_bo->import_dmabuf_fd >= 0)
         close(kbase_bo->import_dmabuf_fd);

      if (kbase_debug_counters_enabled()) {
         unsigned live = atomic_fetch_sub(&kbase_debug_live_bos, 1) - 1;
         mesa_logi("kbase-dbg: bo_free (imported) gpu_va=0x%" PRIx64
                   ", live_bos=%u", kbase_bo->gpu_va, live);
      }

      /* Its address was never ours to hand out, so it does not go back to
       * the heap - same reasoning as the executable case below.
       */
      pan_kmod_bo_cleanup(bo);
      pan_kmod_dev_free(bo->dev, kbase_bo);
      return;
   }

   /* Unlike the old SAME_VA path - where munmap() *was* the free, because
    * kbase tore the region down on vm_close and a following MEM_FREE
    * returned EINVAL - a fixed-address region is an ordinary named
    * allocation, so MEM_FREE is the right call. Any CPU mapping
    * pan_kmod_bo_mmap() made is unmapped by the common layer, not here.
    */
   struct kbase_ioctl_mem_free free_req = { .gpu_addr = kbase_bo->gpu_va };

   int free_ret = ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FREE, &free_req);
   int free_errno = errno;
   if (free_ret < 0)
      mesa_loge("kbase: MEM_FREE of 0x%" PRIx64 " failed: %s",
                kbase_bo->gpu_va, strerror(free_errno));

   if (kbase_debug_counters_enabled()) {
      unsigned live = atomic_fetch_sub(&kbase_debug_live_bos, 1) - 1;
      mesa_logi("kbase-dbg: bo_free gpu_va=0x%" PRIx64
                ", executable=%d, MEM_FREE_ret=%d%s, live_bos=%u",
                kbase_bo->gpu_va,
                !!(bo->flags & PAN_KMOD_BO_FLAG_EXECUTABLE), free_ret,
                free_ret < 0 ? strerror(free_errno) : "", live);
   }

   /* Only addresses this backend handed out go back to it. An executable BO
    * lives in kbase's EXEC_VA zone and its address was chosen by the kernel,
    * so returning it here would insert a range the heap never owned and
    * corrupt every later allocation.
    */
   if (!(bo->flags & PAN_KMOD_BO_FLAG_EXECUTABLE)) {
      simple_mtx_lock(&kbase_dev->va.lock);
      util_vma_heap_free(&kbase_dev->va.heap, kbase_bo->gpu_va, bo->size);
      simple_mtx_unlock(&kbase_dev->va.lock);
   }

   pan_kmod_bo_cleanup(bo);
   pan_kmod_dev_free(bo->dev, kbase_bo);
}

static off_t
kbase_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   /* An imported BO cannot serve one. Its cookie was consumed by the mmap
    * that resolved it (a second mmap of the same cookie fails EINVAL,
    * measured), and the resolved address is not itself a valid offset. Fail
    * loudly rather than return something plausible: pan_kmod_bo_mmap() is a
    * static inline this backend cannot override, so a wrong offset here
    * would silently map the wrong memory.
    *
    * Costs nothing functional - imported memory has no CPU view on this
    * kernel anyway (touching one takes SIGBUS; see docs/kbase-notes.md).
    */
   if (bo->flags & PAN_KMOD_BO_FLAG_IMPORTED) {
      mesa_loge("kbase: imported BOs have no mmap offset (the import cookie "
                "is single-use and the region has no CPU view)");
      return (off_t)-1;
   }

   /* kbase looks a region up by mmap offset >> PAGE_SHIFT, so a region's
    * own GPU address is its mmap offset. Verified for fixed-address
    * allocations by tests/fixed_va_probe, which mmap()s a BASE_MEM_FIXED
    * region at its gpu_va and round-trips a sentinel through it.
    *
    * The resulting CPU mapping lands at an unrelated address - that
    * separation is the entire point of moving off SAME_VA, where the two
    * were forced to be the same number and vm_bind could therefore never
    * honour a caller-chosen VA.
    */
   return (off_t)to_kbase_kmod_bo(bo)->gpu_va;
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

/*
 * The handle-taking import hook. Still unreachable on kbase, and now for a
 * benign reason: pan_kmod_bo_import() only gets here after
 * drmPrimeFDToHandle() succeeds, which a misc device cannot do. The live
 * entry point is kbase_kmod_bo_import_fd() below, which the common layer
 * calls *instead* when a backend provides it (see
 * src/mesa/patch-pan-kmod-import-fd.py).
 *
 * Kept wired rather than left NULL so that a caller reaching it despite all
 * that fails loudly instead of hitting a NULL function pointer.
 */
static struct pan_kmod_bo *
kbase_kmod_bo_import(struct pan_kmod_dev *dev, uint32_t handle, uint64_t size)
{
   mesa_loge("kbase: handle-taking bo_import is not the kbase path "
             "(bo_import_fd is); reaching this means the pan_kmod import "
             "dispatch patch is not applied");
   return NULL;
}

/*
 * dma-buf import, the kbase way: take the fd directly, before any DRM call.
 *
 * Every constant here was measured by tests/dmabuf_import_probe rather than
 * assumed, because three of them are counter-intuitive:
 *
 *  - in.phandle is a POINTER TO the fd, not the fd. The kernel does a
 *    get_user() on it for UMM imports.
 *  - out.gpu_va is an mmap COOKIE, not an address: BASE_MEM_NEED_MMAP comes
 *    back set. mmap()ing the cookie yields the real address. This is the
 *    same trap that wedged the device twice via tests/alias_cs_probe, so it
 *    is checked rather than hoped for.
 *  - the region frees like a SAME_VA allocation: munmap() is the free and
 *    MEM_FREE returns EINVAL. See kbase_kmod_bo_free().
 */
#ifdef PAN_KMOD_HAS_BO_IMPORT_FD
static struct pan_kmod_bo *
kbase_kmod_bo_import_fd(struct pan_kmod_dev *dev, int fd, uint64_t size)
{
   struct kbase_kmod_dev *kbase_dev = to_kbase_kmod_dev(dev);

   /* Our own reference. The caller closes the fd it passed once the import
    * succeeds (panvk_device_memory.c), so borrowing it would leave this BO
    * holding a stale descriptor - and closing the caller's would corrupt an
    * fd number it still owns.
    */
   int dup_fd = os_dupfd_cloexec(fd);
   if (dup_fd < 0) {
      mesa_loge("kbase: dup of dma-buf fd %d failed: %s", fd, strerror(errno));
      return NULL;
   }

   union kbase_ioctl_mem_import import = { 0 };
   import.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
   import.in.phandle = (uint64_t)(uintptr_t)&dup_fd;
   import.in.type = BASE_MEM_IMPORT_TYPE_UMM;

   if (ioctl(dev->fd, KBASE_IOCTL_MEM_IMPORT, &import) < 0) {
      mesa_loge("kbase: MEM_IMPORT of dma-buf fd %d failed: %s", fd,
                strerror(errno));
      close(dup_fd);
      return NULL;
   }

   /* out.va_pages is authoritative, not the caller's size: lseek() on a
    * gralloc buffer can report the whole allocation rather than the
    * addressable extent, and on some handles fails outright.
    */
   uint64_t region_size = (uint64_t)import.out.va_pages * 4096;
   if (region_size < ALIGN_POT(size, 4096)) {
      mesa_logw("kbase: imported region is %" PRIu64 " bytes but the caller "
                "expected %" PRIu64, region_size, ALIGN_POT(size, 4096));
   }

   void *map = NULL;
   uint64_t gpu_va;

   if (import.out.flags & BASE_MEM_NEED_MMAP) {
      map = mmap(NULL, (size_t)region_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 dev->fd, (off_t)import.out.gpu_va);
      if (map == MAP_FAILED) {
         mesa_loge("kbase: could not resolve import cookie 0x%" PRIx64
                   " via mmap: %s", (uint64_t)import.out.gpu_va,
                   strerror(errno));
         close(dup_fd);
         return NULL;
      }
      gpu_va = (uint64_t)(uintptr_t)map;
   } else {
      /* Not observed on this device, but Panfork handles both, so do too. */
      gpu_va = import.out.gpu_va;
   }

   /* The single most important check in this function.
    *
    * The imported address is chosen by the kernel, not by our util_vma_heap.
    * If it ever landed inside the range the heap hands out, the two
    * allocators would eventually return the same address and the corruption
    * would surface arbitrarily far away. Measured to land outside on this
    * device (tests/dmabuf_import_probe reports IMPORT_VA_IN_FIXED_ZONE), but
    * "measured once" is not "guaranteed", so refuse rather than trust it.
    */
   if (gpu_va >= kbase_dev->va.start &&
       gpu_va < kbase_dev->va.start + kbase_dev->va.size) {
      mesa_loge("kbase: imported buffer landed at 0x%" PRIx64 ", inside the "
                "VA heap range [0x%" PRIx64 ", 0x%" PRIx64 ") - refusing, as "
                "the heap could hand out the same address",
                gpu_va, kbase_dev->va.start,
                kbase_dev->va.start + kbase_dev->va.size);
      if (map)
         munmap(map, (size_t)region_size);
      close(dup_fd);
      return NULL;
   }

   struct kbase_kmod_bo *bo = pan_kmod_dev_alloc(dev, sizeof(*bo));
   if (!bo) {
      if (map)
         munmap(map, (size_t)region_size);
      close(dup_fd);
      return NULL;
   }

   bo->gpu_va = gpu_va;
   bo->import_map = map;
   bo->import_map_size = (size_t)region_size;
   bo->import_dmabuf_fd = dup_fd;

   /* exclusive_vm = NULL: an imported BO is shareable by definition. Matches
    * what panthor_kmod and panfrost_kmod do for their own imports.
    */
   pan_kmod_bo_init(&bo->base, dev, NULL, region_size,
                    PAN_KMOD_BO_FLAG_IMPORTED, (uint32_t)(gpu_va >> 12));

   if (kbase_debug_counters_enabled()) {
      unsigned live = atomic_fetch_add(&kbase_debug_live_bos, 1) + 1;
      mesa_logi("kbase-dbg: bo_import_fd gpu_va=0x%" PRIx64 ", size=%" PRIu64
                ", cookie=0x%" PRIx64 ", live_bos=%u",
                gpu_va, region_size, (uint64_t)import.out.gpu_va, live);
   }

   return &bo->base;
}
#endif /* PAN_KMOD_HAS_BO_IMPORT_FD */

/*
 * BELONGS-UPSTREAM(kernel): there is nothing to implement here until kbase
 * itself grows a dma-buf export path. This tag is a marker that the gap is
 * known and deliberate, not an oversight - it is the one entry on the list
 * that no amount of userspace work can close.
 *
 * dma-buf export. Not "not yet" - not possible.
 *
 * kbase has no export mechanism at all: there is no PRIME ioctl, no
 * dmabuf-out, nothing anywhere in the r49p1 UAPI that turns an allocation
 * into an fd. Import is a one-way door on this driver.
 *
 * Even if there were, this hook could not carry it. pan_kmod_bo_export() is
 * a static inline in pan_kmod.h that calls drmPrimeHandleToFD() itself and
 * only then invokes ops->bo_export as an optional *post*-export step, with
 * the fd already made. There is nothing here to override.
 *
 * Kept as a hard failure rather than removed, so that a caller which gets
 * here despite the capability gating fails loudly instead of receiving a
 * plausible-looking fd.
 */
static int
kbase_kmod_bo_export(struct pan_kmod_bo *bo, int dmabuf_fd)
{
   mesa_loge("kbase: bo_export impossible (kbase has no dma-buf export path)");
   return -1;
}

static struct pan_kmod_vm *
kbase_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                     uint64_t va_start, uint64_t va_range)
{
   /* kbase has no explicit VM object: a context owns exactly one address
    * space, and MEM_ALLOC_EX places an allocation into it directly. So the
    * VM here is a bookkeeping object with handle 0, which pan_kmod.h
    * documents as the value for KMDs with one VM per context.
    *
    * PAN_KMOD_VM_FLAG_AUTO_VA is forced on, and that is the load-bearing
    * decision in this file.
    *
    * pan_kmod's normal contract is that the caller picks a VA and vm_bind
    * maps the BO there. kbase cannot honour arbitrary addresses: fixed
    * placement only works inside the FIXED_VA zone (0x800200000000 here),
    * and PanVK's util_vma_heap allocates from a completely different range
    * - it asked for 0xfffff000, which kbase rejects with ENOMEM. Rather
    * than fight PanVK's allocator or rewrite its VA setup, use the
    * inversion pan_kmod already supports: with AUTO_VA set,
    * panvk_priv_bo.c leaves op.va.start as PAN_KMOD_VM_MAP_AUTO_VA and
    * takes whatever address vm_bind writes back. So the backend chooses,
    * from the zone the kernel will actually accept, and PanVK adopts it.
    *
    * This is the same path panfrost (arch < 10) already uses, so it is a
    * supported configuration rather than a special case invented here.
    */
   struct kbase_kmod_vm *vm = pan_kmod_alloc(dev->allocator, sizeof(*vm));

   if (!vm) {
      mesa_loge("kbase: failed to allocate VM object");
      return NULL;
   }

   pan_kmod_vm_init(&vm->base, dev, 0, flags | PAN_KMOD_VM_FLAG_AUTO_VA);

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
         /* The BO already lives at the address bo_alloc chose, inside the
          * FIXED_VA zone, and MEM_ALLOC_EX put it there. There is no
          * separate map step in kbase, so binding is reporting where the
          * memory is.
          */
         uint64_t real_va = to_kbase_kmod_bo(op->map.bo)->gpu_va +
                            (uint64_t)op->map.bo_offset;

         kbase_vm->map_count++;

         if (op->va.start == PAN_KMOD_VM_MAP_AUTO_VA) {
            /* The expected path: vm_create forces AUTO_VA precisely so the
             * caller asks us instead of telling us. panvk_priv_bo.c then
             * uses this as the BO's device address.
             */
            op->va.start = real_va;
            break;
         }

         if (op->va.start != real_va) {
            /* A caller-chosen VA that is not where the BO is. Fail rather
             * than pretend: PanVK dereferences addresses returned from
             * here during vkCreateDevice's mempool setup, so succeeding
             * would turn a clean error into a segfault - measured, see
             * ROADMAP.md Phase 2.
             *
             * Honouring this properly would mean allocating the BO at the
             * caller's address in the first place, which only works if the
             * caller allocates inside the FIXED_VA zone. AUTO_VA avoids
             * needing that.
             */
            kbase_vm->mismatch_count++;
            if (!kbase_vm->warned) {
               kbase_vm->warned = true;
               mesa_loge("kbase: vm_bind asked to map at 0x%" PRIx64
                         " but the BO is at 0x%" PRIx64
                         ". kbase cannot relocate an existing allocation; "
                         "the VM should be using AUTO_VA. See "
                         "kbase_kmod_vm_create()'s comment.",
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
   /* The live import path. Only exists once patch-pan-kmod-import-fd.py has
    * added the hook to struct pan_kmod_ops; guarded so this file still
    * compiles against an unpatched tree.
    */
#ifdef PAN_KMOD_HAS_BO_IMPORT_FD
   .bo_import_fd = kbase_kmod_bo_import_fd,
#endif
   .bo_export = kbase_kmod_bo_export,
   .bo_get_mmap_offset = kbase_kmod_bo_get_mmap_offset,
   .bo_wait = kbase_kmod_bo_wait,
   .flush_bo_map_syncs = kbase_kmod_flush_bo_map_syncs,
   .vm_create = kbase_kmod_vm_create,
   .vm_destroy = kbase_kmod_vm_destroy,
   .vm_bind = kbase_kmod_vm_bind,
};
