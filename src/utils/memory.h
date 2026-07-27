#ifndef MEMORY_H
#define MEMORY_H

#include "flags_helper.h"
#include "mali_base_common_kernel.h"
#include "mali_base_kernel.h"
#include "mali_kbase_ioctl.h"
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

/*
    Function to create a Buffer Object
*/
struct kbase_bo *kbase_bo_create(int fd, size_t size) {
  // initialize the allocation metadata object
  union kbase_ioctl_mem_alloc alloc = {0};

  // calculate the size of the buffer
  size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  // set the allocation metadata input parameters
  alloc.in.va_pages = size / PAGE_SIZE;
  alloc.in.commit_pages = size / PAGE_SIZE;
  alloc.in.extension = 0;

  // set the allocation metadata input flags
  alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                   BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;

  // allocate the memory on the GPU
  if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc) < 0) {
    perror("KBASE_IOCTL_MEM_ALLOC");
    return NULL;
  }

  // read the output flags
  //decode_mem_alloc_output_flags(alloc.out.flags);

  // verify that the allocated GPU memory is aligned with the page size
  if (alloc.out.gpu_va & 0xfff)
    fprintf(stderr, "WARNING: gpu_va is not page aligned\n");

  // initialize the output buffer object
  struct kbase_bo *bo = calloc(1, sizeof(*bo));

  // pass the allocated memory gpu address to the output object
  bo->gpu_va = alloc.out.gpu_va;
  bo->size = size;

  // map the GPU address to a CPU readable buffer
  bo->cpu =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bo->gpu_va);

  // in case of error, return null
  if (bo->cpu == MAP_FAILED) {
    perror("mmap");
    free(bo);
    return NULL;
  }

  // initialize the memory area with 0s
  memset(bo->cpu, 0, size);

  // return the constructed buffer object
  return bo;
}

#endif