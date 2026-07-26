#include "globals.h"
#include "initialize.h"
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

#define PAGE_SIZE 4096

struct kbase_bo {
  uint64_t gpu_va;
  void *cpu;
  size_t size;
};

struct kbase_bo *kbase_bo_create(int fd, size_t size) {
  // struct kbase_ioctl_set_flags flags = {0};

  fprintf(stderr, "%zu\n", size);

  union kbase_ioctl_mem_alloc alloc = {0};

  size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  alloc.in.va_pages = size / PAGE_SIZE;
  alloc.in.commit_pages = size / PAGE_SIZE;
  alloc.in.extension = 0;

  /*alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                   BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                   BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM |
                   BASE_MEM_SAME_VA;*/

  alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                   BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR
  ;

  fprintf(stderr, "requested size = %zu bytes\n", size);
  fprintf(stderr, "va_pages       = %lu\n", alloc.in.va_pages);
  fprintf(stderr, "commit_pages   = %lu\n", alloc.in.commit_pages);
  fprintf(stderr, "in flags       = 0x%016lx\n", alloc.in.flags);

  fprintf(stderr, "SENDING IOCTL\n");

  if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc) < 0) {
    perror("KBASE_IOCTL_MEM_ALLOC");
    return NULL;
  }

  fprintf(stderr, "MEM_ALLOC succeeded\n");
  fprintf(stderr, "gpu_va  = 0x%016lx\n", alloc.out.gpu_va);
  fprintf(stderr, "flags   = 0x%016lx\n", alloc.out.flags);
  fprintf(stderr, "returned flags = 0x%lx\n", alloc.out.flags);
  fprintf(stderr, "requested size = %zu bytes\n", size);
  fprintf(stderr, "va_pages       = %lu\n", alloc.in.va_pages);
  fprintf(stderr, "commit_pages   = %lu\n", alloc.in.commit_pages);
  fprintf(stderr, "in flags       = 0x%016lx\n", alloc.in.flags);

  if (alloc.out.gpu_va & 0xfff)
    fprintf(stderr, "WARNING: gpu_va is not page aligned\n");

  struct kbase_bo *bo = calloc(1, sizeof(*bo));

  fprintf(stderr, "CALLOC\n");

  bo->gpu_va = alloc.out.gpu_va;
  bo->size = size;

  /*bo->cpu = mmap((void *)bo->gpu_va, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd, bo->gpu_va);*/
  bo->cpu = (void *)alloc.out.gpu_va;

  if (bo->cpu == MAP_FAILED) {
    perror("mmap");
    free(bo);
    return NULL;
  }

  fprintf(stderr, "BEFORE MEMSET\n");

  memset(bo->cpu, 0, size);

  fprintf(stderr, "AFTER MEMSET\n");

  printf("write succeeded\n");

  return bo;
}

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

  printf("%lu, %p, %zu", buffer->gpu_va, buffer->cpu, buffer->size);
  return 0;
}