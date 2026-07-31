// Can kbase allocate at a GPU VA that userspace chooses?
//
// This is the question that decides whether pan_kmod's contract can be
// honoured on kbase at all. tests/remap_probe and the vm_bind work
// established that BASE_MEM_SAME_VA lets the *kernel* pick the address,
// and that PanVK dereferences addresses from its own util_vma_heap during
// vkCreateDevice - so a backend that cannot place an allocation where the
// caller asked cannot work (it segfaults; see ROADMAP.md Phase 2).
//
// kbase's answer is KBASE_IOCTL_MEM_ALLOC_EX (nr 59, UK 1.9+): same as
// MEM_ALLOC plus an in.fixed_address field, honoured when the allocation
// carries BASE_MEM_FIXED. What the headers do NOT say is which addresses
// are legal - the UK 1.9 changelog mentions a dedicated "FIXED_VA zone",
// implying requests outside it are rejected. If that zone does not overlap
// the range PanVK allocates from, this route is dead and the backend would
// instead have to drive PanVK's VA allocator from the kernel's choices.
//
// So this probe answers, in order:
//   1. does MEM_ALLOC_EX exist here (vs ENOTTY)?
//   2. does it work as a plain allocator with fixed_address unused?
//   3. with BASE_MEM_FIXED, does out.gpu_va == the requested address?
//   4. which addresses are accepted - i.e. where is the FIXED_VA zone?
//   5. is a fixed allocation CPU-mappable, and does it read/write?
//   6. can two allocations be placed at chosen, adjacent addresses -
//      i.e. can userspace actually drive placement like a VA allocator?
#include "csf/mali_kbase_csf_ioctl.h"
#include "flags_helper.h"
#include "initialize.h"
#include "memory.h"
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#ifndef BASE_MEM_FIXED
#define BASE_MEM_FIXED ((base_mem_alloc_flags)1 << 8)
#endif
#ifndef BASE_MEM_FIXABLE
#define BASE_MEM_FIXABLE ((base_mem_alloc_flags)1 << 29)
#endif

#define RW_FLAGS                                                               \
  (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |        \
   BASE_MEM_PROT_GPU_WR)

// Returns 0 on failure, else the granted GPU VA. Never fatal - the point is
// to learn which requests the kernel accepts.
static uint64_t try_alloc_ex(int fd, uint64_t flags, uint64_t fixed_address,
                             uint64_t *out_flags, const char *label) {
  union kbase_ioctl_mem_alloc_ex alloc = {0};
  alloc.in.va_pages = 1;
  alloc.in.commit_pages = 1;
  alloc.in.extension = 0;
  alloc.in.flags = flags;
  alloc.in.fixed_address = fixed_address;

  if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC_EX, &alloc) < 0) {
    printf("  %-34s -> FAILED (%s)\n", label, strerror(errno));
    return 0;
  }

  if (out_flags)
    *out_flags = alloc.out.flags;

  printf("  %-34s -> gpu_va=0x%llx\n", label,
         (unsigned long long)alloc.out.gpu_va);
  return alloc.out.gpu_va;
}

int main(void) {
  int fd = open_gpu();

  // The vendor blob does this before its allocations, and CS_TILER_HEAP_INIT
  // needs it; more importantly EXEC_VA/FIXED_VA zone layout is established
  // at context setup, so do it before probing zones.
  struct kbase_ioctl_mem_exec_init exec = {.va_pages = 1 << 16};
  if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec) < 0)
    printf("MEM_EXEC_INIT: failed (%s) - continuing\n", strerror(errno));
  else
    printf("MEM_EXEC_INIT: OK\n");

  // --- 1 & 2: does MEM_ALLOC_EX exist, and work without FIXED? ---------
  printf("\n=== 1/2: MEM_ALLOC_EX available, no BASE_MEM_FIXED ===\n");
  uint64_t out_flags = 0;
  uint64_t plain = try_alloc_ex(fd, RW_FLAGS, 0, &out_flags, "plain alloc_ex");
  if (!plain) {
    printf("\nMEM_ALLOC_EX is not usable on this kernel. Fixed-VA allocation\n"
           "is unavailable, so the backend cannot honour caller-chosen\n"
           "addresses this way.\n");
    return 1;
  }
  printf("    granted flags = 0x%llx\n", (unsigned long long)out_flags);
  decode_mem_alloc_output_flags(out_flags);

  // --- 3 & 4: where is the FIXED_VA zone? ------------------------------
  //
  // Discover it rather than guess. A BASE_MEM_FIXABLE allocation with no
  // requested address is placed by the kernel *inside* the FIXED_VA zone,
  // so its GPU VA reveals where that zone lives. A first version of this
  // probe hardcoded six plausible addresses spread over the 48-bit space
  // and every one returned ENOMEM - they were all outside the zone, which
  // turned out to be up at 0x8002_0000_0000.
  /* The zone base, learned empirically: a BASE_MEM_FIXABLE allocation with
   * no requested address lands here, and the kernel places those inside the
   * FIXED_VA zone. Hardcoded rather than discovered-then-used, because
   * discovery order turned out to matter - see below.
   */
#define KNOWN_FIXED_VA_ZONE 0x800200000000ull

  const uint64_t candidates[] = {
      KNOWN_FIXED_VA_ZONE,
      KNOWN_FIXED_VA_ZONE + 0x00100000ull, /* 1MB in  */
      KNOWN_FIXED_VA_ZONE + 0x10000000ull, /* 256MB in */
      0x00000000fffff000ull,               /* control: what PanVK requested */
  };

  uint64_t honoured_addr = 0;

  /* Phase A: BASE_MEM_FIXED before anything else has touched the zone.
   *
   * Ordering is load-bearing. An earlier version of this probe did the
   * FIXABLE discovery first and every subsequent FIXED request failed
   * EINVAL - including one that had failed ENOMEM in a run where no
   * FIXABLE allocation preceded it. Same address, same flags, different
   * errno purely because of what the context had already done. So try the
   * clean case first.
   */
  printf("\n=== 4a: BASE_MEM_FIXED, before any FIXABLE allocation ===\n");
  printf("(honoured only if gpu_va == the address asked for)\n");
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    char label[64];
    snprintf(label, sizeof(label), "FIXED @ 0x%llx",
             (unsigned long long)candidates[i]);

    uint64_t got =
        try_alloc_ex(fd, RW_FLAGS | BASE_MEM_FIXED, candidates[i], NULL, label);
    if (got && got == candidates[i]) {
      printf("      *** HONOURED exactly ***\n");
      if (!honoured_addr)
        honoured_addr = got;
    } else if (got) {
      printf("      granted a DIFFERENT address - request not honoured\n");
    }
  }

  /* Phase B: confirm where the zone is, and whether FIXABLE still works
   * after FIXED attempts.
   */
  printf("\n=== 4b: locate the zone via BASE_MEM_FIXABLE ===\n");
  uint64_t zone_probe =
      try_alloc_ex(fd, RW_FLAGS | BASE_MEM_FIXABLE, 0, NULL, "FIXABLE, no addr");
  if (zone_probe) {
    printf("    -> FIXED_VA zone contains 0x%llx (assumed base 0x%llx)\n",
           (unsigned long long)zone_probe,
           (unsigned long long)KNOWN_FIXED_VA_ZONE);
  }

  /* Phase C: repeat one FIXED request now that a FIXABLE allocation
   * exists, to pin down whether the two are mutually exclusive per context.
   */
  printf("\n=== 4c: BASE_MEM_FIXED again, after a FIXABLE allocation ===\n");
  uint64_t after = try_alloc_ex(fd, RW_FLAGS | BASE_MEM_FIXED,
                                KNOWN_FIXED_VA_ZONE + 0x20000000ull, NULL,
                                "FIXED @ zone + 512MB");
  if (!honoured_addr && after)
    honoured_addr = after;

  if (!honoured_addr) {
    printf("\n================================================================\n");
    printf("RESULT: no candidate address was honoured. Either BASE_MEM_FIXED\n");
    printf("        is unsupported here, or the FIXED_VA zone is somewhere\n");
    printf("        none of these probes hit. Widen the candidate list\n");
    printf("        before concluding the route is dead.\n");
    printf("================================================================\n");
    close(fd);
    return 1;
  }

  // --- 5: is it CPU-mappable and does it work? -------------------------
  printf("\n=== 5: CPU mapping of a fixed allocation @ 0x%llx ===\n",
         (unsigned long long)honoured_addr);
  void *cpu = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   (off_t)honoured_addr);
  if (cpu == MAP_FAILED) {
    printf("  mmap FAILED (%s)\n", strerror(errno));
    printf("  -> the allocation exists at the requested GPU VA but has no\n");
    printf("     CPU view via this offset; a real backend needs one for\n");
    printf("     descriptor/command-stream writes.\n");
  } else {
    const uint64_t SENTINEL = 0x1234ABCD5678EF90ull;
    *(volatile uint64_t *)cpu = SENTINEL;
    __sync_synchronize();
    uint64_t got = *(volatile uint64_t *)cpu;
    printf("  mmap OK at %p, wrote/read 0x%016llx -> %s\n", cpu,
           (unsigned long long)got, got == SENTINEL ? "works" : "MISMATCH");
    printf("  NOTE cpu=%p gpu_va=0x%llx - these differ, which is the point:\n"
           "       the GPU address is now ours to choose, independent of\n"
           "       wherever the CPU mapping happens to land.\n",
           cpu, (unsigned long long)honoured_addr);
    munmap(cpu, PAGE_SIZE);
  }

  // --- 6: can we drive placement, e.g. two adjacent allocations? -------
  printf("\n=== 6: placing two allocations at chosen adjacent addresses ===\n");
  /* Well clear of everything phases 4a-4c allocated. An earlier version
   * used honoured_addr + 1MB, which phase 4a had already taken, and got
   * ENOMEM - that is how we learned ENOMEM means "address unavailable"
   * (occupied or outside the zone) as opposed to EINVAL's "wrong mode".
   */
  uint64_t base = KNOWN_FIXED_VA_ZONE + 0x30000000ull;
  uint64_t a = try_alloc_ex(fd, RW_FLAGS | BASE_MEM_FIXED, base, NULL,
                            "first  @ base");
  uint64_t b = try_alloc_ex(fd, RW_FLAGS | BASE_MEM_FIXED, base + PAGE_SIZE,
                            NULL, "second @ base + 4K");
  bool packed = (a == base) && (b == base + PAGE_SIZE);
  printf("  -> %s\n", packed ? "both honoured; userspace can drive placement"
                             : "placement not fully under our control");

  // --- 7: multi-page fixed allocations, mapped and written in full ------
  //
  // Section 5 only proves a single page works. The Mesa backend allocates
  // multi-page BOs (a 32KB one for the tiler-OOM handlers) and crashed
  // writing near the end of one, with a fault address ~32KB above the
  // mapping - the signature of a mapping shorter than asked for. So check
  // that mmap() of a multi-page fixed region really covers the whole
  // region, by touching every page.
  printf("\n=== 7: multi-page fixed allocation, every page written ===\n");
  const uint64_t sizes[] = {2 * PAGE_SIZE, 8 * PAGE_SIZE, 32768};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    uint64_t want = KNOWN_FIXED_VA_ZONE + 0x40000000ull + i * 0x100000ull;
    uint64_t pages = sizes[i] / PAGE_SIZE;

    union kbase_ioctl_mem_alloc_ex al = {0};
    al.in.va_pages = pages;
    al.in.commit_pages = pages;
    al.in.flags = RW_FLAGS | BASE_MEM_FIXED;
    al.in.fixed_address = want;

    if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC_EX, &al) < 0) {
      printf("  %5llu bytes @ 0x%llx -> alloc FAILED (%s)\n",
             (unsigned long long)sizes[i], (unsigned long long)want,
             strerror(errno));
      continue;
    }

    void *m = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   (off_t)al.out.gpu_va);
    if (m == MAP_FAILED) {
      printf("  %5llu bytes @ 0x%llx -> mmap FAILED (%s)\n",
             (unsigned long long)sizes[i], (unsigned long long)al.out.gpu_va,
             strerror(errno));
      continue;
    }

    // Touch the first byte of every page. A short mapping faults here
    // rather than silently corrupting.
    bool ok = true;
    for (uint64_t p = 0; p < pages; p++) {
      volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)m + p * PAGE_SIZE);
      *slot = 0xA5A50000ull + p;
      if (*slot != 0xA5A50000ull + p)
        ok = false;
    }
    printf("  %5llu bytes @ 0x%llx -> mapped at %p, all %llu pages %s\n",
           (unsigned long long)sizes[i], (unsigned long long)al.out.gpu_va, m,
           (unsigned long long)pages, ok ? "readable+writable" : "MISMATCH");
    munmap(m, sizes[i]);
  }

  printf("\n================================================================\n");
  printf("RESULT: BASE_MEM_FIXED works. A backend can allocate at a\n");
  printf("        caller-chosen GPU VA via KBASE_IOCTL_MEM_ALLOC_EX, which\n");
  printf("        is what pan_kmod's vm_bind contract needs. The accepted\n");
  printf("        address range is shown above - PanVK's VA allocator must\n");
  printf("        be constrained to it (see pan_clamp_to_usable_va_range\n");
  printf("        and panvk_vX_device.c's util_vma_heap setup).\n");
  printf("================================================================\n");

  close(fd);
  return 0;
}
