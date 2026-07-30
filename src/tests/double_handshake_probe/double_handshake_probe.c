// Checks whether kbase tolerates KBASE_IOCTL_VERSION_CHECK being issued
// twice on the same fd before SET_FLAGS.
//
// Why: the pan_kmod kbase dispatch path does exactly that. pan_kmod.c calls
// pan_kmod_fd_is_kbase() to identify the device (one VERSION_CHECK), then
// hands the fd to kbase_kmod_dev_create(), which performs the full
// handshake (a second VERSION_CHECK, then SET_FLAGS). Every standalone
// probe in this repo only ever calls it once, so this ordering is untested
// - and the driver fails immediately after logging that it found
// /dev/mali0, with no further diagnostics.
#include "initialize.h"
#include "mali_kbase_ioctl.h"

static int version_check(int fd, const char *label) {
   struct kbase_ioctl_version_check ver = {.major = 0, .minor = 0};

   errno = 0;
   int ret = ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver);
   printf("  %-22s ret=%d errno=%d (%s) -> major=%u minor=%u\n", label, ret,
          errno, ret < 0 ? strerror(errno) : "ok", ver.major, ver.minor);
   return ret;
}

int main(void) {
   int fd = open(MALI_DEVICE_PATH, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      fprintf(stderr, "open(%s) failed: %s\n", MALI_DEVICE_PATH,
              strerror(errno));
      return 1;
   }

   printf("fd=%d\n", fd);

   version_check(fd, "VERSION_CHECK #1");
   int second = version_check(fd, "VERSION_CHECK #2");

   struct kbase_ioctl_set_flags flags = {.create_flags = 0};
   errno = 0;
   int sf = ioctl(fd, KBASE_IOCTL_SET_FLAGS, &flags);
   printf("  %-22s ret=%d errno=%d (%s)\n", "SET_FLAGS", sf, errno,
          sf < 0 ? strerror(errno) : "ok");

   printf("\n");
   if (second < 0)
      printf("=> a second VERSION_CHECK FAILS. That is what breaks the\n"
             "   pan_kmod dispatch path, which probes then re-handshakes.\n");
   else if (sf < 0)
      printf("=> the second VERSION_CHECK is fine, but SET_FLAGS after it\n"
             "   fails.\n");
   else
      printf("=> double VERSION_CHECK + SET_FLAGS is fine; the driver's\n"
             "   failure is elsewhere.\n");

   close(fd);
   return 0;
}
