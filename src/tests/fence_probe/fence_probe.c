// Probes whether KBASE_IOCTL_INTERNAL_FENCE_WAIT (an MTK-only addition,
// present in r49p1 but not r44p0, documented as gated behind
// CONFIG_MALI_MTK_FENCE_DEBUG) is actually implemented by the running
// kernel, and - past that - whether it behaves like a real completion-
// wait mechanism against a live, bound CS queue (candidate for Phase 4's
// fence-translation shim) rather than a no-op on degenerate input.
// See docs/kbase-notes.md and ROADMAP.md Phase 4.
//
// Version-adaptive by design: KBASE_IOCTL_INTERNAL_FENCE_WAIT is only
// #defined by header sets that actually declare it (r49p1; not r44p0,
// and not necessarily any future vendored version either). Everything
// specific to it is behind "#ifdef KBASE_IOCTL_INTERNAL_FENCE_WAIT" so
// this file builds against any vendored kbase-uapi-* version - the
// probe just skips what that header doesn't support instead of failing
// to compile. Follow this pattern for future ioctls that aren't
// guaranteed present across versions: gate on the ioctl/struct name for
// "does this header declare it at all", or on
// "BASE_UK_VERSION_MINOR >= N" (every version defines this) for fields
// added to an already-present struct at a specific UK minor version -
// see the changelog comments at the top of csf/mali_kbase_csf_ioctl.h
// for which minor version introduced what.
#include "csf/mali_kbase_csf_ioctl.h"
#include "initialize.h"
#include "memory.h"
#include <time.h>
#include <unistd.h>

#ifdef KBASE_IOCTL_INTERNAL_FENCE_WAIT
static double elapsed_ms(struct timespec *start, struct timespec *end) {
  return (end->tv_sec - start->tv_sec) * 1000.0 +
         (end->tv_nsec - start->tv_nsec) / 1e6;
}

// Times KBASE_IOCTL_INTERNAL_FENCE_WAIT so we can tell "returned near-
// instantly" (no real wait happened) from "blocked close to the
// requested timeout" (looks like a real wait mechanism).
static void try_fence_wait(int fd, const char *label, uint32_t pid,
                            uint64_t queue, uint32_t flags,
                            uint64_t timeout_us) {
  struct kbase_ioctl_internal_fence_wait wait = {
      .pid = pid,
      .flags = flags,
      .time_in_microseconds = timeout_us,
      .queue = queue,
  };

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  errno = 0;
  int ret = ioctl(fd, KBASE_IOCTL_INTERNAL_FENCE_WAIT, &wait);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  double ms = elapsed_ms(&t0, &t1);
  printf("[%s] ret=%d errno=%d (%s) elapsed=%.1fms (requested timeout=%.1fms)\n",
         label, ret, errno, strerror(errno), ms, timeout_us / 1000.0);
}
#endif /* KBASE_IOCTL_INTERNAL_FENCE_WAIT */

int main(void) {
  int fd = open_gpu();

#ifdef KBASE_IOCTL_INTERNAL_FENCE_WAIT
  // Reachability check with all-zero args (what the earlier version of
  // this probe did).
  try_fence_wait(fd, "zeroed args, no queue set up", 0, 0, 0, 0);
#else
  printf("KBASE_IOCTL_INTERNAL_FENCE_WAIT not declared by this header "
         "set - skipping (see ROADMAP.md Phase 4: ruled out as the "
         "fence mechanism on r49p1 anyway, so this is expected/fine on "
         "any header set, MTK-derived or not).\n");
#endif

  // ---- Set up a real queue group + bound queue, same shape as
  // ---- tests/queue_group/queue_group.c, so we have a real GPU VA to
  // ---- pass as "queue" instead of 0.
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
#ifdef KBASE_IOCTL_INTERNAL_FENCE_WAIT
  uint32_t pid = (uint32_t)getpid();

  // Before KICK: nothing submitted yet.
  try_fence_wait(fd, "bound, before kick, IDLE, pid=0", 0, queue_addr,
                 BASE_INTERNAL_FENCE_WAIT_IDLE_FLAG, 2000000);
  try_fence_wait(fd, "bound, before kick, IDLE|RESULT, real pid", pid,
                 queue_addr,
                 BASE_INTERNAL_FENCE_WAIT_IDLE_FLAG |
                     BASE_INTERNAL_FENCE_WAIT_RESULT_FLAG,
                 2000000);
#endif

  // Kick (same sentinel-word approach as queue_group.c - not a real
  // command stream, just exercising the doorbell).
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

#ifdef KBASE_IOCTL_INTERNAL_FENCE_WAIT
  // After KICK: does timing or result change?
  try_fence_wait(fd, "bound, after kick, IDLE, pid=0", 0, queue_addr,
                 BASE_INTERNAL_FENCE_WAIT_IDLE_FLAG, 2000000);
  try_fence_wait(fd, "bound, after kick, IDLE|RESULT, real pid", pid,
                 queue_addr,
                 BASE_INTERNAL_FENCE_WAIT_IDLE_FLAG |
                     BASE_INTERNAL_FENCE_WAIT_RESULT_FLAG,
                 2000000);
  try_fence_wait(fd, "bound, after kick, DUMP, real pid", pid, queue_addr,
                 BASE_INTERNAL_FENCE_WAIT_DUMP_FLAG, 2000000);
#endif

  // Teardown, same order confirmed clean in queue_group.c.
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
