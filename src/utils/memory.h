#ifndef MEMORY_H
#define MEMORY_H

#include "flags_helper.h"
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
  alloc.in.flags =
      BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |
      BASE_MEM_PROT_GPU_WR /*| BASE_MEM_COHERENT_SYSTEM | BASE_MEM_SAME_VA*/;

  int ret = ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc);

  // allocate the memory on the GPU
  if (ret < 0) {
    printf("ret=%d\n", ret);
    perror("KBASE_IOCTL_MEM_ALLOC");
    return NULL;
  }

  printf("ret=%d errno=%d\n", ret, errno);
  printf("gpu_va=0x%llx\n",
        (unsigned long long)alloc.out.gpu_va);
  printf("flags=0x%llx\n",
        (unsigned long long)alloc.out.flags);

  // read the output flags
  decode_mem_alloc_output_flags(alloc.out.flags);

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

  /*bo->cpu =
    mmap((void *)(uintptr_t)bo->gpu_va,
         size,
         PROT_READ | PROT_WRITE,
         MAP_SHARED | MAP_FIXED,
         fd,
         (off_t)bo->gpu_va);*/

  // in case of error, return null
  if (bo->cpu == MAP_FAILED) {
    perror("mmap");
    free(bo);
    return NULL;
  }

  // initialize the memory area with 0s
  memset(bo->cpu, 0, size);

  printf("buffer gpu_va = 0x%016lx\n", bo->gpu_va);
  printf("buffer cpu = 0x%016lx\n", bo->cpu);
  printf("buffer size   = %zu\n", bo->size);

  /*
      Why this is an "if" and not just an assignment.

      kbase can report an allocation's GPU address in two different ways,
      and you can only tell which one you got by looking at out.flags.

      Case 1 - BASE_MEM_SAME_VA (what we get today).
      MEM_ALLOC does not return an address at all. It returns a small
      placeholder number, a "cookie", and the memory has no GPU mapping
      yet. Calling mmap() with that cookie as the offset is what actually
      creates the mapping, and the CPU address mmap() gives back is then
      *also* the GPU address - the same number works for both sides. That
      is why we overwrite gpu_va with the CPU pointer here.

      Case 2 - no SAME_VA (GPU-executable, or BASE_MEM_FIXED/FIXABLE).
      kbase maps the memory immediately and out.gpu_va is already the real
      GPU address. The CPU mapping ends up somewhere completely unrelated.
      Overwriting gpu_va here would throw away a valid address and replace
      it with a meaningless one.

      Measured on a Mali-G720, three allocations from this same function:

        plain     SAME_VA=YES   out.gpu_va=0x41000         cpu=0x799ae21000
        GPU_EX    SAME_VA=NO    out.gpu_va=0x800000001000  cpu=0x799ae1d000
        FIXABLE   SAME_VA=NO    out.gpu_va=0x800200000000  cpu=0x799ae19000

      Only the first should be overwritten. The other two already hold the
      address the GPU wants.

      Note the flags are read from out.flags, not the in.flags we sent:
      kbase adds SAME_VA on our behalf, so we ask for 0xf and get 0x200f
      back. The request cannot tell you which case you are in - only the
      reply can.

      Right now every allocation here lands in case 1, so both paths agree.
      The check is what keeps this correct once the flags become a
      parameter. tests/same_va_probe demonstrates all three rows above.
  */
  if (alloc.out.flags & BASE_MEM_SAME_VA)
    bo->gpu_va = (uint64_t)(uintptr_t)bo->cpu;

  // return the constructed buffer object
  return bo;
}

#endif