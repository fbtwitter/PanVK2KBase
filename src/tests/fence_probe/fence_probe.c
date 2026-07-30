// Probes whether KBASE_IOCTL_INTERNAL_FENCE_WAIT (r49p1-only, ioctl 80,
// documented as gated behind CONFIG_MALI_MTK_FENCE_DEBUG) is actually
// implemented by the running kernel, not just declared in the vendored
// header. Zeroed/dummy args - this only checks reachability (does the
// kernel recognize the ioctl number at all), not correct fence semantics.
// See docs/kbase-notes.md and ROADMAP.md Phase 4.
#include "initialize.h"
#include "mali_kbase_ioctl.h"

int main(void) {
  int fd = open_gpu();

  struct kbase_ioctl_internal_fence_wait wait = {
      .pid = 0,
      .flags = 0,
      .time_in_microseconds = 1000,
      .queue = 0,
  };

  errno = 0;
  int ret = ioctl(fd, KBASE_IOCTL_INTERNAL_FENCE_WAIT, &wait);

  printf("KBASE_IOCTL_INTERNAL_FENCE_WAIT: ret=%d errno=%d (%s)\n", ret,
         errno, strerror(errno));

  if (ret < 0 && errno == ENOTTY)
    printf("=> kernel does NOT recognize this ioctl number "
           "(CONFIG_MALI_MTK_FENCE_DEBUG likely not built in)\n");
  else if (ret < 0)
    printf("=> kernel DOES recognize this ioctl (rejected the dummy args, "
           "not the ioctl number itself)\n");
  else
    printf("=> ioctl succeeded with dummy args (unexpected, investigate)\n");

  return 0;
}
