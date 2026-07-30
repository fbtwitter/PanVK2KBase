#include "memory.h"
#include "globals.h"
#include "initialize.h"
#include "dump_hex.h"
#include "mali_base_common_kernel.h"
#include "mali_base_kernel.h"
#include "mali_kbase_ioctl.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

int main(void) {
  int fd = open_gpu();

  // if the device couldn't be opened
  if (fd < 0) {
    fprintf(stderr, "open failed: %s\n", strerror(errno));
    fprintf(stderr, MALI_DEVICE_PATH);
    return 1;
  }

  fprintf(stderr, "Successfully opened device\n");

  size_t size = 1024 * 1024; // 1 MiB

  size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  struct kbase_bo *buffer = kbase_bo_create(fd, size);

  struct kbase_bo *buffer2 = kbase_bo_create(fd, size);

  struct kbase_bo *buffer3 = kbase_bo_create(fd, size);

  struct kbase_bo *buffer4 = kbase_bo_create(fd, size);

  memset(buffer->cpu, 0x5a, 64);
  dump_hex(buffer->cpu, 64);

  printf("%lu, %p, %zu\n", buffer->gpu_va, buffer->cpu, buffer->size);
  return 0;
}