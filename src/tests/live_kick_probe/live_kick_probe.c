// The actual live test: build a real, correct-by-construction CS
// instruction with Mesa's cs_builder.h (see
// tests/cs_encode_probe/cs_encode_probe.c, docs/mesa-cs-builder.md),
// write it into a bound queue's ring buffer, update CS_INSERT in the
// mmap'd user input page (see utils/csf_user_regs.h for the offsets and
// their kernel-source provenance), and KICK it for real - then poll()/
// read() for a CSF notification (see tests/event_probe/event_probe.c).
//
// This is the step docs/kbase-notes.md's "poll()/read() on the kbase
// fd" and "CSF ISA docs located" sections were building up to: previous
// attempts kicked a queue filled with sentinel 0xdeadbeef words and
// never updated CS_INSERT, so firmware had no reason to notice. This
// probe fixes both.
#include "csf/mali_kbase_csf_ioctl.h"
#include "csf_user_regs.h"
#include "genxml/cs_builder.h"
#include "initialize.h"
#include "memory.h"
#include "parse_gpu_props.h"
#include <poll.h>
#include <stdbool.h>
#include <unistd.h>

static void print_notification(const struct base_csf_notification *notif) {
  printf("  type=%u ", notif->type);
  switch (notif->type) {
  case BASE_CSF_NOTIFICATION_EVENT:
    printf("(EVENT)\n");
    break;
  case BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR: {
    const struct base_gpu_queue_group_error *err =
        &notif->payload.csg_error.error;
    printf("(GPU_QUEUE_GROUP_ERROR) handle=%u error_type=%u\n",
           notif->payload.csg_error.handle, err->error_type);
    uint32_t status = 0;
    switch (err->error_type) {
    case BASE_GPU_QUEUE_GROUP_ERROR_FATAL:
      status = err->payload.fatal_group.status;
      break;
    case BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FATAL:
      status = err->payload.fatal_queue.status;
      break;
#ifdef BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FAULT
    case BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FAULT:
      status = err->payload.fault_queue.status;
      break;
#endif
    default:
      printf("    (no status field decoded for this error_type)\n");
      return;
    }
    printf("    status=0x%08x (exception_type=0x%02x)\n", status,
           status & 0xff);
    break;
  }
  default:
    printf("(type %u, not decoded here)\n", notif->type);
    break;
  }
}

static int drain_notifications(int fd, const char *label, int timeout_ms,
                                int max_events) {
  int n = 0;
  for (; n < max_events; n++) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pret = poll(&pfd, 1, timeout_ms);

    if (pret == 0) {
      printf("[%s] poll #%d: timed out after %dms, no event\n", label, n,
             timeout_ms);
      break;
    }
    if (pret < 0) {
      perror("poll");
      break;
    }

    printf("[%s] poll #%d: POLLIN ready (revents=0x%x)\n", label, n,
           pfd.revents);

    struct base_csf_notification notif = {0};
    ssize_t r = read(fd, &notif, sizeof(notif));
    if (r != (ssize_t)sizeof(notif)) {
      printf("[%s] read: got %zd bytes (errno=%d), expected %zu\n", label, r,
             errno, sizeof(notif));
      break;
    }
    print_notification(&notif);
  }
  return n;
}

static struct cs_buffer noop_alloc(void *cookie) {
  (void)cookie;
  fprintf(stderr, "alloc_buffer called - 1-instruction test overflowed?\n");
  abort();
}


struct group_config {
  const char *name;
  uint64_t tiler_mask;
  uint64_t fragment_mask;
  uint64_t compute_mask;
  uint8_t tiler_max;
  uint8_t fragment_max;
  uint8_t compute_max;
  uint8_t csi_index;
  // Use KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6 (nr 42, older/smaller
  // struct) instead of the modern CS_QUEUE_GROUP_CREATE (nr 58). The
  // vendor blob uses _1_6 exclusively - nr 58 and _1_18 don't appear in
  // it at all (see docs/kbase-notes.md's RE section), so it's worth
  // testing whether the kernel treats them differently.
  bool use_1_6;
};

/*
    Replicates the context-level setup the vendor blob does but this
    repo's probes never did. Discovered by mapping libGLES_mali.so's
    kbase ioctl surface - see docs/kbase-notes.md. Each step is
    reported rather than fatal, since the point is to learn which ones
    this kernel accepts and whether any is the missing precondition for
    a queue group to become schedulable.

    All of these are one-shot per context: calling them twice fails.
*/
static void vendor_context_setup(int fd) {
  printf("\n=== vendor-style context setup ===\n");

  // JIT memory pool. The vendor driver always initialises this; tiler
  // heap growth is backed by it.
  struct kbase_ioctl_mem_jit_init jit = {
      .va_pages = 1 << 14, /* 64MB worth of 4K pages */
      .max_allocations = 255,
      .trim_level = 0,
      .group_id = 0,
      .phys_pages = 1 << 14,
  };
  if (ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &jit) < 0)
    printf("  MEM_JIT_INIT   : FAILED (%s)\n", strerror(errno));
  else
    printf("  MEM_JIT_INIT   : OK (va_pages=%llu)\n",
           (unsigned long long)jit.va_pages);

  // Executable VA zone.
  struct kbase_ioctl_mem_exec_init exec = {
      .va_pages = 1 << 16, /* 256MB worth of 4K pages */
  };
  if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec) < 0)
    printf("  MEM_EXEC_INIT  : FAILED (%s)\n", strerror(errno));
  else
    printf("  MEM_EXEC_INIT  : OK (va_pages=%llu)\n",
           (unsigned long long)exec.va_pages);
}

/*
    Creates a tiler heap the way the vendor driver does. chunk_size must
    be 4KB-aligned and <= CHUNK_SIZE_MASK (0xfff000), initial_chunks >= 1
    and <= max_chunks, target_in_flight >= 1 - all enforced by
    kbase_csf_tiler_heap_init(). Returns the heap's GPU VA, or 0.
*/
static uint64_t vendor_tiler_heap_init(int fd) {
  union kbase_ioctl_cs_tiler_heap_init heap = {0};
  heap.in.chunk_size = 2 * 1024 * 1024; /* 2MB, 4KB-aligned */
  heap.in.initial_chunks = 1;
  heap.in.max_chunks = 8;
  heap.in.target_in_flight = 1;
  heap.in.group_id = 0;
  heap.in.buf_desc_va = 0;

  if (ioctl(fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &heap) < 0) {
    printf("  TILER_HEAP_INIT: FAILED (%s)\n", strerror(errno));
    return 0;
  }

  printf("  TILER_HEAP_INIT: OK gpu_heap_va=0x%llx first_chunk_va=0x%llx\n",
         (unsigned long long)heap.out.gpu_heap_va,
         (unsigned long long)heap.out.first_chunk_va);
  return heap.out.gpu_heap_va;
}

// Runs one full create/bind/encode/insert/kick/observe/teardown cycle.
// Returns true only if the GPU actually consumed the instruction
// (CS_EXTRACT advanced) - KICK returning 0 does not mean the GPU ran
// anything, see docs/kbase-notes.md.
static bool run_config(int fd, const struct group_config *cfg) {
  printf("\n=== config: %s ===\n", cfg->name);
  printf("  tiler_mask=0x%llx fragment_mask=0x%llx compute_mask=0x%llx\n",
         (unsigned long long)cfg->tiler_mask,
         (unsigned long long)cfg->fragment_mask,
         (unsigned long long)cfg->compute_mask);
  printf("  tiler_max=%u fragment_max=%u compute_max=%u csi_index=%u\n",
         cfg->tiler_max, cfg->fragment_max, cfg->compute_max, cfg->csi_index);

  uint8_t group_handle;
  uint32_t group_uid;

  if (cfg->use_1_6) {
    union kbase_ioctl_cs_queue_group_create_1_6 create = {0};
    create.in.tiler_mask = cfg->tiler_mask;
    create.in.fragment_mask = cfg->fragment_mask;
    create.in.compute_mask = cfg->compute_mask;
    create.in.cs_min = 1;
    create.in.priority = 0;
    create.in.tiler_max = cfg->tiler_max;
    create.in.fragment_max = cfg->fragment_max;
    create.in.compute_max = cfg->compute_max;

    if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &create) < 0) {
      printf("  CS_QUEUE_GROUP_CREATE_1_6 failed: %s - skipping\n",
             strerror(errno));
      return false;
    }
    group_handle = create.out.group_handle;
    group_uid = create.out.group_uid;
    printf("  [via _1_6 / nr 42] ");
  } else {
    union kbase_ioctl_cs_queue_group_create create = {0};
    create.in.tiler_mask = cfg->tiler_mask;
    create.in.fragment_mask = cfg->fragment_mask;
    create.in.compute_mask = cfg->compute_mask;
    create.in.cs_min = 1;
    create.in.priority = 0;
    create.in.tiler_max = cfg->tiler_max;
    create.in.fragment_max = cfg->fragment_max;
    create.in.compute_max = cfg->compute_max;

    if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE, &create) < 0) {
      printf("  CS_QUEUE_GROUP_CREATE failed: %s - skipping\n",
             strerror(errno));
      return false;
    }
    group_handle = create.out.group_handle;
    group_uid = create.out.group_uid;
    printf("  [via nr 58] ");
  }

  printf("group_handle=%u group_uid=%u\n", group_handle, group_uid);

  struct kbase_bo *queue_bo = kbase_bo_create(fd, 4096);
  if (!queue_bo)
    return false;

  // SAME_VA: the CPU pointer *is* the GPU virtual address. bo->gpu_va is
  // NOT usable here - it's a reusable cookie (0x41000 for every
  // allocation), see docs/kbase-notes.md's SAME_VA section.
  uint64_t queue_gpu_va = (uint64_t)(uintptr_t)queue_bo->cpu;

  struct cs_buffer root_buffer = {
      .cpu = queue_bo->cpu,
      .gpu = queue_gpu_va,
      .capacity = (uint32_t)(queue_bo->size / sizeof(uint64_t)),
  };
  struct cs_builder_conf conf = {
      .nr_registers = 96,
      .nr_kernel_registers = 4,
      .alloc_buffer = noop_alloc,
      .cookie = NULL,
  };
  struct cs_builder b;
  cs_builder_init(&b, &conf, root_buffer);
  cs_move32_to(&b, cs_reg32(&b, 0), 0x1234);
  cs_end(&b);

  if (!cs_is_valid(&b)) {
    fprintf(stderr, "  cs_builder reported invalid\n");
    return false;
  }

  uint32_t cs_size = cs_root_chunk_size(&b);
  printf("  encoded %u bytes: 0x%016llx\n", cs_size,
         (unsigned long long)*(uint64_t *)queue_bo->cpu);

  struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = queue_gpu_va,
      .buffer_size = queue_bo->size,
      .priority = 0,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
    perror("  CS_QUEUE_REGISTER");
    return false;
  }

  union kbase_ioctl_cs_queue_bind bind = {0};
  bind.in.buffer_gpu_addr = queue_gpu_va;
  bind.in.group_handle = group_handle;
  bind.in.csi_index = cfg->csi_index;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
    perror("  CS_QUEUE_BIND");
    return false;
  }

  size_t queue_state_size = BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096;
  void *queue_state = mmap(NULL, queue_state_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, (off_t)bind.out.mmap_handle);
  if (queue_state == MAP_FAILED) {
    perror("  CS_QUEUE_BIND mmap");
    return false;
  }

  // Page order is [doorbell][input][output] - measured by
  // tests/user_io_probe, see utils/csf_user_regs.h. This was previously
  // page 0/page 1, which meant writing CS_INSERT into the doorbell page
  // and polling CS_EXTRACT out of the input page; that is why every run
  // of this probe reported CS_EXTRACT=0 / CS_ACTIVE=0.
  uint8_t *input_page = (uint8_t *)queue_state + CSF_USER_INPUT_PAGE * 4096;
  uint8_t *output_page = (uint8_t *)queue_state + CSF_USER_OUTPUT_PAGE * 4096;

  // CS_INSERT is a *byte offset* into the ring buffer, not an address -
  // confirmed against the kernel's own diagnostics (see
  // utils/csf_user_regs.h). A single 64-bit store is one STR on aarch64,
  // satisfying the kernel's "access atomically" requirement.
  *(volatile uint64_t *)(input_page + CSF_USER_CS_INSERT_LO) = cs_size;

  // Make the ring buffer contents and CS_INSERT visible before the kick;
  // the kernel does the equivalent (dmb(osh)) before ringing doorbells.
  __sync_synchronize();

  struct kbase_ioctl_cs_queue_kick kick = {0};
  kick.buffer_gpu_addr = queue_gpu_va;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0)
    perror("  CS_QUEUE_KICK");
  else
    printf("  CS_QUEUE_KICK OK\n");

  // CS_EXTRACT advancing is the only real proof the GPU ran our
  // instruction. CS_ACTIVE says whether firmware ever started the stream
  // at all - it stays 0 if the group never reached a CSG slot.
  uint64_t extract = 0;
  bool consumed = false;
  for (int i = 0; i < 40; i++) {
    extract = *(volatile uint64_t *)(output_page + CSF_USER_CS_EXTRACT_LO);
    if (extract >= cs_size) {
      printf("  *** CS_EXTRACT advanced to %llu after ~%dms - GPU CONSUMED "
             "the instruction ***\n",
             (unsigned long long)extract, i * 50);
      consumed = true;
      break;
    }
    usleep(50000);
  }

  uint32_t active = *(volatile uint32_t *)(output_page + CSF_USER_CS_ACTIVE);
  if (!consumed)
    printf("  CS_EXTRACT still %llu after ~2000ms (expected %u), CS_ACTIVE=%u"
           " - firmware never ran it\n",
           (unsigned long long)extract, cs_size, active);
  else
    printf("  CS_ACTIVE=%u\n", active);

  drain_notifications(fd, "notifications", 300, 4);

  if (munmap(queue_state, queue_state_size) < 0)
    perror("  munmap queue_state");

  struct kbase_ioctl_cs_queue_terminate q_term = {
      .buffer_gpu_addr = queue_gpu_va,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &q_term) < 0)
    perror("  CS_QUEUE_TERMINATE");

  struct kbase_ioctl_cs_queue_group_term term = {
      .group_handle = group_handle,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term) < 0)
    perror("  CS_QUEUE_GROUP_TERMINATE");

  kbase_bo_free(fd, queue_bo);

  return consumed;
}

int main(void) {
  int fd = open_gpu();

  // The first version of this probe passed ~0ULL for all three endpoint
  // masks. The kernel only validates *_max <= hweight64(*_mask), so that
  // passes - but it then hands the mask straight to firmware as
  // CSG_ALLOW_COMPUTE/FRAGMENT/TILER. Asking for 64 shader cores on a GPU
  // that physically has 8 is a plausible reason firmware would refuse to
  // put the group on a slot, so query and use the real mask here.
  uint64_t shader_present = kbase_get_shader_present(fd);
  printf("RAW_SHADER_PRESENT = 0x%llx (%d cores)\n",
         (unsigned long long)shader_present,
         __builtin_popcountll(shader_present));

  if (!shader_present) {
    fprintf(stderr, "could not read shader-core mask, aborting\n");
    return 1;
  }

  // Do the context setup the vendor blob does and this repo never did,
  // before creating any group. These are one-shot per context, so they
  // happen once here rather than per-config.
  vendor_context_setup(fd);
  vendor_tiler_heap_init(fd);

  const struct group_config configs[] = {
      {
          .name = "vendor-style setup + nr 58 group create, all endpoints",
          .tiler_mask = shader_present,
          .fragment_mask = shader_present,
          .compute_mask = shader_present,
          .tiler_max = 1,
          .fragment_max = 1,
          .compute_max = 1,
          .csi_index = 0,
          .use_1_6 = false,
      },
      {
          .name = "vendor-style setup + _1_6 (nr 42) group create, as the "
                  "blob does",
          .tiler_mask = shader_present,
          .fragment_mask = shader_present,
          .compute_mask = shader_present,
          .tiler_max = 1,
          .fragment_max = 1,
          .compute_max = 1,
          .csi_index = 0,
          .use_1_6 = true,
      },
      {
          .name = "vendor-style setup + _1_6, compute-only",
          .tiler_mask = 0,
          .fragment_mask = 0,
          .compute_mask = shader_present,
          .tiler_max = 0,
          .fragment_max = 0,
          .compute_max = 1,
          .csi_index = 0,
          .use_1_6 = true,
      },
  };

  int executed = 0;
  size_t n = sizeof(configs) / sizeof(configs[0]);
  for (size_t i = 0; i < n; i++) {
    if (run_config(fd, &configs[i]))
      executed++;
  }

  printf("\n==== %d of %zu configs actually executed on the GPU ====\n",
         executed, n);

  return 0;
}
