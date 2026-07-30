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
#include <poll.h>
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

int main(void) {
  int fd = open_gpu();

  union kbase_ioctl_cs_queue_group_create create = {0};
  create.in.tiler_mask = ~0ULL;
  create.in.fragment_mask = ~0ULL;
  create.in.compute_mask = ~0ULL;
  create.in.cs_min = 1;
  create.in.priority = 0;
  create.in.tiler_max = 1;
  create.in.fragment_max = 1;
  create.in.compute_max = 1;

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE, &create) < 0) {
    perror("CS_QUEUE_GROUP_CREATE");
    return 1;
  }

  struct kbase_bo *queue_bo = kbase_bo_create(fd, 4096);

  // These allocations come back as SAME_VA, so the CPU pointer *is* the
  // GPU virtual address - which is why every ioctl in this repo passes
  // the CPU pointer as buffer_gpu_addr. Note bo->gpu_va is NOT usable
  // here: it's a reusable cookie (0x41000 for every allocation), not a
  // real address - see docs/kbase-notes.md's SAME_VA section.
  uint64_t queue_gpu_va = (uint64_t)(uintptr_t)queue_bo->cpu;

  // ---- Build a real instruction into the ring buffer via Mesa's
  // ---- encoder instead of writing sentinel bytes. ----
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
    fprintf(stderr, "cs_builder reported invalid\n");
    return 1;
  }

  uint32_t cs_size = cs_root_chunk_size(&b);
  printf("encoded %u bytes into the ring buffer: 0x%016llx\n", cs_size,
         (unsigned long long)*(uint64_t *)queue_bo->cpu);

  struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = queue_gpu_va,
      .buffer_size = queue_bo->size,
      .priority = 0,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
    perror("CS_QUEUE_REGISTER");
    return 1;
  }

  union kbase_ioctl_cs_queue_bind bind = {0};
  bind.in.buffer_gpu_addr = queue_gpu_va;
  bind.in.group_handle = create.out.group_handle;
  bind.in.csi_index = 1;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
    perror("CS_QUEUE_BIND");
    return 1;
  }

  size_t queue_state_size = BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096;
  void *queue_state = mmap(NULL, queue_state_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, (off_t)bind.out.mmap_handle);
  if (queue_state == MAP_FAILED) {
    perror("CS_QUEUE_BIND mmap");
    return 1;
  }

  uint8_t *input_page = (uint8_t *)queue_state;
  uint8_t *output_page = (uint8_t *)queue_state + 4096;

  // ---- The fix: tell firmware there's real data now, before KICK. ----
  // CS_INSERT is a *byte offset* into the ring buffer, not an address -
  // confirmed against the kernel's own diagnostics, which print the ring
  // buffer base, insert, and extract as three separate values with
  // insert/extract starting at 0 (see utils/csf_user_regs.h). A single
  // 64-bit store is one STR on aarch64, satisfying the kernel's "should
  // be accessed atomically" requirement for this register.
  *(volatile uint64_t *)(input_page + CSF_USER_CS_INSERT_LO) = cs_size;

  // Make the ring buffer contents and the CS_INSERT update visible before
  // the kick. The kernel does the equivalent (dmb(osh)) before ringing
  // doorbells; the ioctl below is a syscall boundary, but don't rely on
  // that implicitly for GPU-visible writes.
  __sync_synchronize();

  printf("CS_INSERT set to %u, CS_EXTRACT_INIT=%llu (pre-kick)\n", cs_size,
         (unsigned long long)*(volatile uint64_t *)(input_page +
                                                      CSF_USER_CS_EXTRACT_INIT_LO));

  struct kbase_ioctl_cs_queue_kick kick = {0};
  kick.buffer_gpu_addr = queue_gpu_va;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0) {
    perror("CS_QUEUE_KICK");
  } else {
    printf("CS_QUEUE_KICK OK\n");
  }

  // The most direct evidence of real GPU execution: CS_EXTRACT is
  // firmware's "I have consumed up to here" counter. If it advances from
  // 0 to cs_size, the GPU actually ran our instruction - a stronger and
  // more specific signal than any notification, which is why it's polled
  // separately rather than only checked once at the end.
  uint64_t extract = 0;
  for (int i = 0; i < 40; i++) {
    extract = *(volatile uint64_t *)(output_page + CSF_USER_CS_EXTRACT_LO);
    if (extract >= cs_size) {
      printf("CS_EXTRACT advanced to %llu after ~%dms - GPU consumed the "
             "instruction\n",
             (unsigned long long)extract, i * 50);
      break;
    }
    usleep(50000);
  }

  if (extract < cs_size)
    printf("CS_EXTRACT still %llu after ~2000ms (expected %u) - firmware "
           "did not consume the instruction\n",
           (unsigned long long)extract, cs_size);

  uint32_t active = *(volatile uint32_t *)(output_page + CSF_USER_CS_ACTIVE);
  printf("post-kick output page: CS_EXTRACT=%llu CS_ACTIVE=%u\n",
         (unsigned long long)extract, active);

  drain_notifications(fd, "after real kick", 500, 4);

  if (munmap(queue_state, queue_state_size) < 0)
    perror("munmap queue_state");

  struct kbase_ioctl_cs_queue_terminate q_term = {
      .buffer_gpu_addr = queue_gpu_va,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &q_term) < 0)
    perror("CS_QUEUE_TERMINATE");

  struct kbase_ioctl_cs_queue_group_term term = {
      .group_handle = create.out.group_handle,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term) < 0)
    perror("CS_QUEUE_GROUP_TERMINATE");

  kbase_bo_free(fd, queue_bo);

  return 0;
}
