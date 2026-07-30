#ifndef INITIALIZE_H
#define INITIALIZE_H

#include <errno.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "globals.h"
// vendor driver headers
#include "mali_kbase_ioctl.h"

int open_gpu(void) {
  // variable to store the file descriptor
  int fd;

  // variable to store the result of ioctl calls
  int ret;

  // open the device file in read and write mode
  fd = open(MALI_DEVICE_PATH, O_RDWR);

  // if the device couldn't be opened
  if (fd < 0) {
    fprintf(stderr, "open failed: %s\n", strerror(errno));
    fprintf(stderr, MALI_DEVICE_PATH);
    return 1;
  }

  printf("open OK, fd=%d\n", fd);

  // create the struct for making an IOCTL request to get the version of the
  // interface
  struct kbase_ioctl_version_check ver = {
      .major = 0,
      .minor = 0,
  };

  // make a ioctl call to the driver
  ret = ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver);
  if (ret < 0) {
    fprintf(stderr, "VERSION_CHECK failed: %s (errno %d)\n", strerror(errno),
            errno);
    close(fd);
    return 1;
  }
  printf("VERSION_CHECK OK: major=%u minor=%u\n", ver.major, ver.minor);

  // setup the flags for making ioctl calls
  struct kbase_ioctl_set_flags flags = {
      .create_flags = 0,
  };

  // send the command through ioctl
  ret = ioctl(fd, KBASE_IOCTL_SET_FLAGS, &flags);
  if (ret < 0) {
    fprintf(stderr, "SET_FLAGS failed: %s (errno %d)\n", strerror(errno),
            errno);
    close(fd);
    return 1;
  }
  printf("SET_FLAGS OK\n");

  return fd;
}

#endif
