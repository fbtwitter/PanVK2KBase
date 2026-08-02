// What can this kbase actually do with sync fds?
//
// This is the last driver-side blocker for Android presentation. The
// contract, read out of Mesa's runtime rather than assumed:
//
//   vk_common_AcquireImageANDROID       needs ImportSemaphoreFdKHR /
//                                       ImportFenceFdKHR with SYNC_FD, and
//                                       only when the app passed a semaphore
//                                       or fence handle. fd == -1 is legal
//                                       and means "already signalled".
//
//   vk_common_QueueSignalReleaseImageANDROID
//                                       returns -1 outright when
//                                       waitSemaphoreCount == 0; otherwise it
//                                       submits and calls GetSemaphoreFdKHR
//                                       with SYNC_FD.
//
// The import half needs no kernel help at all: a sync_file is pollable, so
// "wait for this fd" is poll(). This driver already satisfies waits on the
// CPU, so that fits its existing model exactly.
//
// The export half is the open question, and there are two possible answers:
//
//   (a) return -1, which the Vulkan spec explicitly allows for SYNC_FD and
//       which means "the payload is already signalled". Legal, and honest
//       provided we actually wait first. Costs a CPU block per present.
//
//   (b) hand back a REAL fence fd that signals when the GPU finishes, so
//       the compositor can wait on it instead of us blocking. Better, but
//       only possible if kbase gives userspace a way to create and signal a
//       fence.
//
// kbase has KBASE_IOCTL_STREAM_CREATE ("also called a timeline") and
// KBASE_IOCTL_FENCE_VALIDATE. This probe establishes which of (a) and (b)
// is actually available here, so the implementation is a measured choice
// rather than a guess.
//
// Does NO GPU work - no queue group, no kick, no command stream - so it
// needs no --i-know-it-hangs gate and is safe for the regression runner.
//
// Usage: sync_fd_probe

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/types.h>

#include "globals.h"
#include "initialize.h"
#include "mali_kbase_ioctl.h"

/* sw_sync's userspace ABI. Not in the NDK sysroot (it is a debug interface),
 * and mirrored here rather than vendored because it is three definitions and
 * they have been stable for years. Only used to ask whether kbase's stream
 * fd behaves like a sw_sync timeline.
 */
struct sw_sync_create_fence_data {
   __u32 value;
   char name[32];
   __s32 fence;
};
#define SW_SYNC_IOC_MAGIC 'W'
#define SW_SYNC_IOC_CREATE_FENCE                                               \
   _IOWR(SW_SYNC_IOC_MAGIC, 0, struct sw_sync_create_fence_data)
#define SW_SYNC_IOC_INC _IOW(SW_SYNC_IOC_MAGIC, 1, __u32)

/* sync_file's info ioctl, for asking an fd whether it is a fence and whether
 * it has signalled. linux/sync_file.h is in the NDK sysroot, but defining
 * the two fields we use keeps this file self-contained across NDK versions.
 */
struct probe_sync_file_info {
   char name[32];
   __s32 status; /* 1 signalled, 0 active, <0 error */
   __u32 flags;
   __u32 num_fences;
   __u32 pad;
   __u64 sync_fence_info;
};
#define PROBE_SYNC_IOC_MAGIC '>'
#define PROBE_SYNC_IOC_FILE_INFO                                               \
   _IOWR(PROBE_SYNC_IOC_MAGIC, 4, struct probe_sync_file_info)

static int failures;

static void check(bool ok, const char *what) {
   printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

/* Report-only: a "no" here is information, not a failure. */
static void note(bool ok, const char *what) {
   printf("  %-58s %s\n", what, ok ? "yes" : "no");
}

static const char *sum_stream = "not_attempted";
static const char *sum_stream_is_swsync = "not_attempted";
static const char *sum_fence_from_stream = "not_attempted";
static const char *sum_validate_eventfd = "not_attempted";
static const char *sum_poll_works = "not_attempted";
static const char *sum_export_strategy = "undetermined";

static bool fence_validate(int fd, int target) {
   struct kbase_ioctl_fence_validate v = {.fd = target};
   return ioctl(fd, KBASE_IOCTL_FENCE_VALIDATE, &v) == 0;
}

int main(void) {
   setvbuf(stdout, NULL, _IONBF, 0);

   printf("\n=== sync fd probe (no GPU work performed) ===\n");

   int fd = open_gpu();
   if (fd <= 0) {
      fprintf(stderr, "could not open the kbase device\n");
      return 1;
   }

   /* ------------------------------------------------ 1. STREAM_CREATE */
   printf("\n=== KBASE_IOCTL_STREAM_CREATE (a sync timeline?) ===\n");
   struct kbase_ioctl_stream_create sc = {0};
   snprintf(sc.name, sizeof(sc.name), "panvk-kbase-probe");

   int stream_fd = ioctl(fd, KBASE_IOCTL_STREAM_CREATE, &sc);
   if (stream_fd < 0) {
      printf("  STREAM_CREATE failed: %s\n", strerror(errno));
      printf("  (the ioctl returns the fd as its return value, not in the "
             "struct)\n");
      sum_stream = "failed";
   } else {
      printf("  STREAM_CREATE -> fd %d\n", stream_fd);
      sum_stream = "ok";
   }
   note(stream_fd >= 0, "kbase handed out a synchronisation stream");

   /* --------------------------- 2. does that stream behave like sw_sync? */
   int fence_fd = -1;
   if (stream_fd >= 0) {
      printf("\n=== can userspace create a fence on that stream? ===\n");
      struct sw_sync_create_fence_data d = {.value = 1};
      snprintf(d.name, sizeof(d.name), "panvk-probe-fence");
      d.fence = -1;

      if (ioctl(stream_fd, SW_SYNC_IOC_CREATE_FENCE, &d) == 0 && d.fence >= 0) {
         printf("  SW_SYNC_IOC_CREATE_FENCE -> fd %d\n", d.fence);
         fence_fd = d.fence;
         sum_stream_is_swsync = "yes";
         sum_fence_from_stream = "ok";
      } else {
         printf("  SW_SYNC_IOC_CREATE_FENCE: %s\n", strerror(errno));
         printf("  So the stream is not a userspace-drivable sw_sync\n"
                "  timeline. In kbase this stream is consumed internally by\n"
                "  job atoms (BASE_JD_REQ_SOFT_FENCE_*), a JM-era mechanism\n"
                "  with no CSF equivalent - there is no ioctl here that says\n"
                "  'signal this fence now'.\n");
         sum_stream_is_swsync = "no";
         sum_fence_from_stream = "failed";
      }
      note(fence_fd >= 0, "userspace can create a fence on the stream");
   }

   /* ------------------------------------------ 3. FENCE_VALIDATE behaviour */
   printf("\n=== KBASE_IOCTL_FENCE_VALIDATE: what counts as a fence? ===\n");
   int efd = eventfd(0, EFD_CLOEXEC);
   bool v_event = fence_validate(fd, efd);
   printf("  an eventfd            -> %s\n", v_event ? "accepted" : "rejected");
   sum_validate_eventfd = v_event ? "accepted" : "rejected";

   if (stream_fd >= 0) {
      bool v_stream = fence_validate(fd, stream_fd);
      printf("  the stream fd itself  -> %s\n",
             v_stream ? "accepted" : "rejected");
   }
   if (fence_fd >= 0) {
      bool v_fence = fence_validate(fd, fence_fd);
      printf("  a fence from the stream -> %s\n",
             v_fence ? "accepted" : "rejected");
   }
   /* Rejecting an eventfd is the correct answer - it means FENCE_VALIDATE is
    * a real type check we could use to reject a bogus import.
    */
   check(!v_event, "FENCE_VALIDATE rejects a non-fence fd (a real check)");

   /* ---------------------- 4. the import side: is poll() enough to wait? */
   printf("\n=== the import half: waiting on a sync fd ===\n");
   if (fence_fd >= 0) {
      struct pollfd pfd = {.fd = fence_fd, .events = POLLIN};
      int pr = poll(&pfd, 1, 0);
      printf("  poll(unsignalled fence, 0ms) -> %d (expect 0: not ready)\n", pr);

      struct probe_sync_file_info info = {0};
      if (ioctl(fence_fd, PROBE_SYNC_IOC_FILE_INFO, &info) == 0)
         printf("  SYNC_IOC_FILE_INFO: name='%s' status=%d num_fences=%u\n",
                info.name, info.status, info.num_fences);

      /* Signal the timeline and re-poll: proves poll() is a usable wait. */
      __u32 inc = 1;
      if (ioctl(stream_fd, SW_SYNC_IOC_INC, &inc) == 0) {
         pr = poll(&pfd, 1, 100);
         printf("  after SW_SYNC_IOC_INC, poll(100ms) -> %d (expect 1)\n", pr);
         sum_poll_works = (pr == 1) ? "yes" : "no";
         check(pr == 1, "poll() observes a sync fd becoming signalled");
      }
      close(fence_fd);
   } else {
      printf("  SKIPPED: no fence fd to wait on. poll() on a sync_file is\n"
             "  standard kernel behaviour and needs nothing from kbase, but\n"
             "  it is not demonstrated here for want of a fence to test.\n");
      sum_poll_works = "untested";
   }

   if (efd >= 0)
      close(efd);
   if (stream_fd >= 0)
      close(stream_fd);
   close(fd);

   /* ------------------------------------------------------- the decision */
   if (!strcmp(sum_fence_from_stream, "ok"))
      sum_export_strategy = "real_fence_possible";
   else
      sum_export_strategy = "return_minus_one";

   printf("\n=== SYNC FD SUMMARY ===\n");
   printf("STREAM_CREATE=%s\n", sum_stream);
   printf("STREAM_IS_SW_SYNC=%s\n", sum_stream_is_swsync);
   printf("FENCE_FROM_STREAM=%s\n", sum_fence_from_stream);
   printf("FENCE_VALIDATE_EVENTFD=%s\n", sum_validate_eventfd);
   printf("POLL_OBSERVES_SIGNAL=%s\n", sum_poll_works);
   printf("EXPORT_STRATEGY=%s\n", sum_export_strategy);

   printf("\n=== %d failure(s) ===\n", failures);
   if (!strcmp(sum_export_strategy, "return_minus_one"))
      printf("\n=> Export must be the spec's -1 (\"already signalled\"), which\n"
             "   obliges the driver to CPU-wait before returning it. Import\n"
             "   is poll(), which needs nothing from kbase.\n");
   else
      printf("\n=> A real exportable fence looks possible - worth preferring\n"
             "   over the -1 fallback, since it lets the compositor wait\n"
             "   instead of blocking us.\n");

   return failures ? 1 : 0;
}
