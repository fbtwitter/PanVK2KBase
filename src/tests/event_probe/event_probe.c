// Tests the standard kbase CSF notification channel as a candidate for
// Phase 4's completion/fence mechanism, now that
// KBASE_IOCTL_INTERNAL_FENCE_WAIT was ruled out (see
// tests/fence_probe/fence_probe.c and docs/kbase-notes.md).
//
// The mechanism under test: poll() the kbase device fd for POLLIN, then
// read() a struct base_csf_notification off it (csf/mali_base_csf_kernel.h)
// when one's ready. Since this repo's queue "kick" writes sentinel
// 0xdeadbeef words instead of a real command stream (see
// tests/queue_group/queue_group.c), kicking it should make the CSF
// frontend choke on garbage and raise a fault - if that fault surfaces
// through this channel, it's real evidence the mechanism works, not
// just a syscall that returns 0.
#include "csf/mali_kbase_csf_ioctl.h"
#include "initialize.h"
#include "memory.h"
#include <poll.h>
#include <time.h>

static const char *exception_name(uint8_t low_byte) {
  // Only the two exception types this header's own doc comments name as
  // examples - anything else just prints as hex.
  switch (low_byte) {
  case 0x49:
    return "CS_INVALID_INSTRUCTION";
  case 0x50:
    return "INSTR_INVALID_PC";
  default:
    return "unknown";
  }
}

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
    uint64_t sideband = 0;
    switch (err->error_type) {
    case BASE_GPU_QUEUE_GROUP_ERROR_FATAL:
      status = err->payload.fatal_group.status;
      sideband = err->payload.fatal_group.sideband;
      break;
    case BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FATAL:
      status = err->payload.fatal_queue.status;
      sideband = err->payload.fatal_queue.sideband;
      printf("    csi_index=%u\n", err->payload.fatal_queue.csi_index);
      break;
#ifdef BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FAULT
    // Not declared in every vendored version (added after r44p0's UK
    // 1.20) - see docs/kbase-notes.md's version-adaptive-code section.
    case BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FAULT:
      status = err->payload.fault_queue.status;
      sideband = err->payload.fault_queue.sideband;
      printf("    csi_index=%u\n", err->payload.fault_queue.csi_index);
      break;
#endif
    default:
      printf("    (timeout/tiler-heap-oom/other, no status field decoded "
             "here)\n");
      return;
    }

    uint8_t exc_type = status & 0xff;
    printf("    status=0x%08x (exception_type=0x%02x '%s') sideband=0x%llx\n",
           status, exc_type, exception_name(exc_type),
           (unsigned long long)sideband);
    break;
  }
  case BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP:
    printf("(CPU_QUEUE_DUMP)\n");
    break;
  default:
    printf("(unrecognized type)\n");
    break;
  }
}

// Polls for and drains up to max_events notifications, each with its own
// timeout. Returns the number actually read.
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

    if (r < 0) {
      perror("read notification");
      break;
    }
    if (r != (ssize_t)sizeof(notif)) {
      printf("[%s] short read: got %zd bytes, expected %zu\n", label, r,
             sizeof(notif));
      break;
    }

    print_notification(&notif);
  }
  return n;
}

int main(void) {
  int fd = open_gpu();

  // Baseline: nothing submitted yet, should time out.
  drain_notifications(fd, "before group/queue setup", 200, 1);

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

  struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = (uint64_t)(uintptr_t)queue_bo->cpu,
      .buffer_size = queue_bo->size,
      .priority = 0,
  };

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
    perror("CS_QUEUE_REGISTER");
    return 1;
  }

  union kbase_ioctl_cs_queue_bind bind = {0};
  bind.in.buffer_gpu_addr = (uint64_t)(uintptr_t)queue_bo->cpu;
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

  uint64_t queue_addr = (uint64_t)(uintptr_t)queue_bo->cpu;

  // Bound but not kicked yet - still nothing to report.
  drain_notifications(fd, "bound, before kick", 200, 1);

  uint32_t *q = queue_bo->cpu;
  for (int i = 0; i < 16; i++)
    q[i] = 0xdeadbeef;

  struct kbase_ioctl_cs_queue_kick kick = {0};
  kick.buffer_gpu_addr = queue_addr;

  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0) {
    perror("CS_QUEUE_KICK");
  } else {
    printf("CS_QUEUE_KICK OK\n");
  }

  // The interesting one: did kicking garbage produce a fault
  // notification via poll()/read()?
  drain_notifications(fd, "after kick", 2000, 4);

  if (munmap(queue_state, queue_state_size) < 0)
    perror("munmap queue_state");

  struct kbase_ioctl_cs_queue_terminate q_term = {
      .buffer_gpu_addr = queue_addr,
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
