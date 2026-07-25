// Library imports
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// vendor driver headers
#include "mali_kbase_ioctl.h"

// helpers
#include "globals.h"
#include "parse_gpu_props.h"

int main(void) {
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

  // setup the struct to store the result of the gpu properties probe (this is done to check that something is returned)
  struct kbase_ioctl_get_gpuprops probe = {
      .buffer = 0,
      .size = 0,
      .flags = 0,
  };
  ret = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &probe);
  if (ret < 0) {
    fprintf(stderr, "GET_GPUPROPS (size probe) failed: %s\n", strerror(errno));
    close(fd);
    return 1;
  }

  // extract the size of the properties from the return value
  size_t props_size = (size_t)ret;

  if (props_size == 0) {
    fprintf(stderr, "error obtaining the gpu properties\n");
    close(fd);
    return 1;
  }

  printf("GET_GPUPROPS reports size=%zu bytes\n", props_size);

  // create a buffer to store the data
  unsigned char *props_buf = calloc(1, props_size);
  if (!props_buf) {
    perror("calloc");
    close(fd);
    return 1;
  }

  // create a new struct to actually get the information
  struct kbase_ioctl_get_gpuprops fetch = {
      .buffer = (__u64)(uintptr_t)props_buf,
      .size = (__u32)props_size,
      .flags = 0,
  };
  // make the ioctl request
  ret = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &fetch);
  if (ret < 0) {
    fprintf(stderr, "GET_GPUPROPS (fetch) failed: %s\n", strerror(errno));
    free(props_buf);
    close(fd);
    return 1;
  }

  printf("GET_GPUPROPS fetch OK\n");
  
  // parse the GPU properties from the obtained buffer
  parse_gpuprops(props_buf, props_size);

  // free the allocated memory
  free(props_buf);

  // close the file
  close(fd);

  printf("\n== probe complete, device is talking ==\n");
  return 0;
}