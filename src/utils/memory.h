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
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096

/*
    A kbase allocation, in the two forms its address takes.

    Under BASE_MEM_SAME_VA - which this device's kbase grants for our flag
    combination whether or not we ask for it - MEM_ALLOC does NOT return a
    GPU address. It returns a reusable mmap *cookie*, and the region has no
    GPU mapping at all until userspace mmap()s that cookie; the CPU address
    mmap() hands back is then the real GPU VA. See the SAME_VA/cookie
    section in docs/kbase-notes.md for how this was established.

    Both values are kept because under SAME_VA they are different numbers
    and picking the wrong one is not a clean failure:
      - cookie: exactly what MEM_ALLOC reported, and the correct mmap()
        offset. Under SAME_VA it is single-use and reused across
        allocations (every BO tends to report the same value).
      - gpu_va: the real GPU address. This is what goes into command
        streams, MEM_ALIAS handles, and any other ioctl wanting an address.

    For a non-SAME_VA allocation (BASE_MEM_FIXABLE/BASE_MEM_FIXED) the two
    are the same number and the CPU pointer is the unrelated one instead.

    Handing a cookie to the GPU faults it and wedges the kbase context past
    kill -9, needing a device reboot - see tests/alias_probe.
*/
struct kbase_bo {
  uint64_t gpu_va; // real GPU address (== cpu under SAME_VA)
  uint64_t cookie; // MEM_ALLOC's out.gpu_va; an mmap offset, not an address
  void *cpu;
  size_t size;
};

/*
    Function to create a Buffer Object, with extra allocation flags OR'd
    into the usual CPU/GPU read-write set.

    The only caller that needs this today is tests/event_slot_probe, which
    passes BASE_MEM_CSF_EVENT to get memory the CSF firmware can signal
    completion through - see docs/kbase-notes.md's "Finding 2". Pass 0 for
    the historical behaviour.
*/
struct kbase_bo *kbase_bo_create_flags(int fd, size_t size,
                                       base_mem_alloc_flags extra_flags) {
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
      BASE_MEM_PROT_GPU_WR /*| BASE_MEM_COHERENT_SYSTEM | BASE_MEM_SAME_VA*/
      | extra_flags;

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

  // whatever MEM_ALLOC returned, it is the right mmap() offset; whether it
  // is also a usable GPU address depends on SAME_VA, resolved below
  bo->cookie = alloc.out.gpu_va;
  bo->size = size;

  // resolve the cookie into a mapping; the address this returns is both the
  // CPU pointer and, under SAME_VA, the real GPU VA
  bo->cpu =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bo->cookie);

  // in case of error, return null
  if (bo->cpu == MAP_FAILED) {
    perror("mmap");
    free(bo);
    return NULL;
  }

  /*
      Why this is an if/else and not just an assignment.

      kbase can report an allocation's GPU address in two different ways,
      and you can only tell which one you got by looking at out.flags.

      Case 1 - BASE_MEM_SAME_VA (extra_flags == 0, the common path).
      MEM_ALLOC does not return an address at all. It returns a small
      placeholder number, a "cookie", and the memory has no GPU mapping
      yet. Calling mmap() with that cookie as the offset is what actually
      creates the mapping, and the CPU address mmap() gives back is then
      *also* the GPU address - the same number works for both sides. So
      here the real address is bo->cpu.

      Case 2 - no SAME_VA (BASE_MEM_PROT_GPU_EX, or BASE_MEM_FIXED /
      BASE_MEM_FIXABLE passed as extra_flags). kbase maps the memory
      immediately and out.gpu_va is already the real GPU address. The CPU
      mapping ends up somewhere completely unrelated, so using bo->cpu
      here would throw away a valid address for a meaningless one.

      Measured on the Poco X8 Pro (Mali-G720, UK 1.30) by
      tests/same_va_probe, three allocations from this same function:

        plain     SAME_VA=YES   out.gpu_va=0x41000         cpu=0x799ae21000
        GPU_EX    SAME_VA=NO    out.gpu_va=0x800000001000  cpu=0x799ae1d000
        FIXABLE   SAME_VA=NO    out.gpu_va=0x800200000000  cpu=0x799ae19000

      Note the two non-SAME_VA rows land in two different regions - the
      EXEC_VA zone and the FIXED_VA zone - so this is about whether SAME_VA
      was granted, not about any one flag.

      Note also the flags are read from out.flags, not the in.flags we
      sent: kbase adds SAME_VA on our behalf, so we ask for 0xf and get
      0x200f back. The request cannot tell you which case you are in -
      only the reply can.

      Getting this wrong is not a clean failure. tests/alias_probe passes
      bo->gpu_va to MEM_ALIAS for its FIXABLE source and gets ENOMEM, which
      reads like a resource limit rather than a bad address. Handing a
      cookie to a command stream is worse - it faults the GPU and wedges
      the kbase context past kill -9. See "Caller-chosen GPU VAs" in
      docs/kbase-notes.md.
  */
  if (alloc.out.flags & BASE_MEM_SAME_VA)
    bo->gpu_va = (uint64_t)(uintptr_t)bo->cpu;
  else
    bo->gpu_va = alloc.out.gpu_va;

  // initialize the memory area with 0s
  memset(bo->cpu, 0, size);

  printf("buffer gpu_va = 0x%016llx\n", (unsigned long long)bo->gpu_va);
  printf("buffer cookie = 0x%016llx\n", (unsigned long long)bo->cookie);
  printf("buffer cpu    = %p\n", bo->cpu);
  printf("buffer size   = %zu\n", bo->size);

  // return the constructed buffer object
  return bo;
}

/*
    Function to create a Buffer Object
*/
struct kbase_bo *kbase_bo_create(int fd, size_t size) {
  return kbase_bo_create_flags(fd, size, 0);
}

/*
    Function to free a Buffer Object created by kbase_bo_create()

    kbase_bo_create() always ends up with BASE_MEM_SAME_VA granted (see
    decoded output flags) even though the input flags don't request it
    explicitly - this device's kbase treats it as the default for this
    flag combination. For SAME_VA regions the GPU-side allocation is tied
    1:1 to the CPU mmap: munmap() alone releases it on vm_close, and a
    follow-up KBASE_IOCTL_MEM_FREE fails with EINVAL because the region
    is already gone by the time it runs (confirmed on-device). Only
    munmap here; don't also call MEM_FREE.
*/
void kbase_bo_free(int fd, struct kbase_bo *bo) {
  (void)fd;

  if (!bo)
    return;

  if (munmap(bo->cpu, bo->size) < 0)
    perror("munmap");

  free(bo);
}

#endif