// Can a SAME_VA kbase allocation be mmap'd a second time?
//
// pan_kmod_bo_mmap() (pan_kmod.h:748, a static inline the backend cannot
// override) always calls bo_get_mmap_offset() and then os_mmap() on the
// device fd. The kbase backend has no obvious offset to return: MEM_ALLOC
// hands back a *cookie*, that cookie is consumed by the mmap() in
// kbase_kmod_bo_alloc() which establishes the SAME_VA address, and the BO
// is mapped from then on.
//
// So the question is what, if anything, is a valid second mmap offset:
//
//   A. the original MEM_ALLOC cookie again  - works only if kbase does not
//      consume it on first use
//   B. the resolved SAME_VA address          - works if kbase looks regions
//      up by GPU VA >> PAGE_SHIFT, which is how offsets work for non-
//      SAME_VA allocations
//   C. neither                               - the backend has to satisfy
//      pan_kmod_bo_mmap() some other way
//
// Whichever answers decides how kbase_kmod_bo_get_mmap_offset() is
// implemented, so measure instead of guessing.
#include "initialize.h"
#include "memory.h"
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  int fd = open_gpu();

  // Allocate exactly as the Mesa backend does, but keep the cookie so both
  // candidates can be tried. kbase_bo_create() discards it.
  union kbase_ioctl_mem_alloc alloc = {0};
  alloc.in.va_pages = 1;
  alloc.in.commit_pages = 1;
  alloc.in.extension = 0;
  alloc.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                   BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;

  if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc) < 0) {
    perror("MEM_ALLOC");
    return 1;
  }

  uint64_t cookie = alloc.out.gpu_va;
  printf("MEM_ALLOC ok: cookie=0x%llx flags=0x%llx\n",
         (unsigned long long)cookie, (unsigned long long)alloc.out.flags);

  void *first = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     (off_t)cookie);
  if (first == MAP_FAILED) {
    perror("first mmap");
    return 1;
  }
  uint64_t same_va = (uint64_t)(uintptr_t)first;
  printf("first mmap ok: SAME_VA address = 0x%llx\n",
         (unsigned long long)same_va);

  // Write a sentinel so an aliasing second mapping can be proven to be the
  // same memory, not merely a successful mmap of something else.
  const uint64_t SENTINEL = 0xCAFEF00DD15EA5EDull;
  *(volatile uint64_t *)first = SENTINEL;
  __sync_synchronize();

  printf("\n--- candidate A: mmap the original cookie again ---\n");
  void *a = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (off_t)cookie);
  if (a == MAP_FAILED) {
    printf("  FAILED: %s\n", strerror(errno));
  } else {
    uint64_t got = *(volatile uint64_t *)a;
    printf("  mapped at %p, reads 0x%016llx -> %s\n", a,
           (unsigned long long)got,
           got == SENTINEL ? "SAME MEMORY (usable)" : "different memory");
    munmap(a, PAGE_SIZE);
  }

  printf("\n--- candidate B: mmap the resolved SAME_VA address ---\n");
  void *b = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (off_t)same_va);
  if (b == MAP_FAILED) {
    printf("  FAILED: %s\n", strerror(errno));
  } else {
    uint64_t got = *(volatile uint64_t *)b;
    printf("  mapped at %p, reads 0x%016llx -> %s\n", b,
           (unsigned long long)got,
           got == SENTINEL ? "SAME MEMORY (usable)" : "different memory");
    munmap(b, PAGE_SIZE);
  }

  printf("\n================================================================\n");
  printf("Whichever candidate reports SAME MEMORY is what\n");
  printf("kbase_kmod_bo_get_mmap_offset() should return. If neither does,\n");
  printf("pan_kmod_bo_mmap() cannot be satisfied on kbase as written and\n");
  printf("the caller has to be given the existing mapping instead.\n");
  printf("================================================================\n");

  munmap(first, PAGE_SIZE);
  close(fd);
  return 0;
}
