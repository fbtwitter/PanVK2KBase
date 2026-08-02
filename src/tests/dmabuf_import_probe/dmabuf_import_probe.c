// Can kbase import a dma-buf on this device, and what exactly is the
// out.gpu_va it hands back?
//
// Why this matters: Android presentation runs through
// VK_ANDROID_native_buffer, whose memory binding ends in a dma-buf import
// (AHardwareBuffer_getNativeHandle -> handle->data[0] ->
// VkImportMemoryFdInfoKHR{DMA_BUF} -> pan_kmod_bo_import). On kbase that
// path currently dies in pan_kmod_bo_import()'s drmPrimeFDToHandle(), which
// a misc device cannot serve, so kbase_kmod_bo_import() is a stub that only
// logs "unreachable". Fixing that is worth doing only if kbase can actually
// import in the first place - and docs/kbase-notes.md has had "does your
// kernel's kbase expose the ioctls you'll need for dma-buf import?" as an
// unchecked box since Phase 1. This answers it.
//
// WHAT THIS PROBE DOES NOT DO, AND WHY IT MATTERS
//
// No queue group. No CS_QUEUE_KICK. No cs_builder stream. No doorbell write.
// Nothing here ever makes the GPU dereference anything.
//
// That is not caution for its own sake. tests/alias_cs_probe pointed a
// command stream at a MEM_ALIAS out.gpu_va on the assumption it was an
// address; it was an mmap cookie, the GPU faulted, and the kbase context
// wedged past kill -9 - uninterruptible D state, physical reboot, twice.
// MEM_IMPORT returns a gpu_va through the same union and may have the same
// property. So this probe establishes what that value *is* using CPU-side
// evidence only, and stops there. Stage two can point hardware at it once
// this says it is safe to.
//
// That restraint is also why there is deliberately no --i-know-it-hangs
// gate: the probe is designed to be un-dangerous so tools/run-probes.sh can
// run it unattended, and the no-GPU-access rule above is what backs that up.
// If you add GPU work here, add the gate too, and move it out of the runner's
// raw tier.
//
// Usage:
//   dmabuf_import_probe [--source=heap|ahb] [--heap=NAME] [--size=BYTES]
//
// Exits non-zero on any failed check. Ends with a machine-greppable
// "=== DMABUF IMPORT SUMMARY ===" block whose VERDICT line is what stage two
// keys off.

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#include "globals.h"
#include "initialize.h"
#include "flags_helper.h"
#include "mali_kbase_ioctl.h"

#include "dmabuf_source.h"

/* Must match src/mesa/pan_kmod_kbase.c - if the backend's zone moves, the
 * collision test below is testing the wrong range.
 */
#define KBASE_FIXED_VA_ZONE_START 0x800200000000ull
#define KBASE_FIXED_VA_ZONE_SIZE (8ull << 30)

#define PAGE_SZ 4096ull

static int failures;

/* A successful mmap() does not mean the pages are there. The first run of
 * this probe took a SIGBUS reading the kbase mapping of an imported dma-buf
 * - the mapping existed, the memory behind it did not. That is a real and
 * useful answer, so catch the fault and report it rather than dying: a probe
 * that crashes tells you less than one that says "this mapping is not
 * accessible, here is everything else I learned".
 *
 * SIGSEGV is caught alongside SIGBUS because which one you get for an
 * unbacked shared mapping is not worth depending on.
 */
static sigjmp_buf fault_jmp;
static volatile sig_atomic_t fault_armed;

static void fault_handler(int sig) {
  (void)sig;
  if (fault_armed)
    siglongjmp(fault_jmp, 1);
  _exit(135); /* not ours - do not swallow it */
}

static bool with_fault_guard(void (*body)(void *), void *ctx) {
  struct sigaction sa, old_bus, old_segv;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = fault_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGBUS, &sa, &old_bus);
  sigaction(SIGSEGV, &sa, &old_segv);

  bool ok;
  fault_armed = 1;
  if (sigsetjmp(fault_jmp, 1) == 0) {
    body(ctx);
    ok = true;
  } else {
    ok = false;
  }
  fault_armed = 0;

  sigaction(SIGBUS, &old_bus, NULL);
  sigaction(SIGSEGV, &old_segv, NULL);
  return ok;
}

struct acc { volatile uint64_t *p; uint64_t v; };
static void do_read(void *c) { struct acc *a = c; a->v = *a->p; }
static void do_write(void *c) { struct acc *a = c; *a->p = a->v; }

static bool safe_read64(void *addr, uint64_t *out) {
  struct acc a = {.p = (volatile uint64_t *)addr, .v = 0};
  bool ok = with_fault_guard(do_read, &a);
  if (ok)
    *out = a.v;
  return ok;
}

static bool safe_write64(void *addr, uint64_t val) {
  struct acc a = {.p = (volatile uint64_t *)addr, .v = val};
  return with_fault_guard(do_write, &a);
}

static void check(bool ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    failures++;
}

/* ------------------------------------------------------------- summary vars */
static const char *sum_source = "?";
static unsigned long long sum_dmabuf_size;
static const char *sum_import_result = "not_attempted";
static int sum_import_errno;
static unsigned long long sum_out_flags;
static char sum_flags_decoded[512];
static const char *sum_need_mmap = "?";
static unsigned long long sum_gpu_va_raw;
static const char *sum_gpu_va_kind = "?";
static unsigned long long sum_resolved_va;
static const char *sum_va_zone = "?";
static const char *sum_in_fixed_zone = "?";
static unsigned long long sum_va_pages;
static const char *sum_covers = "?";
static const char *sum_mmap_a = "not_attempted";
static const char *sum_mmap_b = "not_attempted";
static const char *sum_rt_d2k = "not_attempted";
static const char *sum_rt_k2d = "not_attempted";
static const char *sum_fixed_before = "not_attempted";
static const char *sum_fixed_after = "not_attempted";
static char sum_free_model[64] = "unknown";
static const char *sum_verdict = "IMPORT_UNAVAILABLE";
static const char *sum_import_flag_combo = "base";
static const char *sum_kbase_cpu_map = "not_attempted";
static int sum_imported_fd_index = -1;

/* A cookie is not a heuristic here: the header defines the range exactly.
 * BASE_MEM_COOKIE_BASE = 64<<12 = 0x40000, and cookies run up to
 * BASE_MEM_FIRST_FREE_ADDRESS. The 0x41000 this repo has seen from every
 * SAME_VA allocation sits squarely inside it.
 *
 * BASE_MEM_FIRST_FREE_ADDRESS itself is spelled in terms of BITS_PER_LONG,
 * which is kernel-internal and not visible to userspace, so derive that from
 * the target's own long rather than hardcoding 64.
 */
#define KBASE_COOKIE_LIMIT                                                     \
  ((((uint64_t)(sizeof(long) * 8)) << LOCAL_PAGE_SHIFT) +                      \
   (uint64_t)BASE_MEM_COOKIE_BASE)

static bool va_is_cookie_shaped(uint64_t va) {
  return va >= (uint64_t)BASE_MEM_COOKIE_BASE && va < KBASE_COOKIE_LIMIT;
}

static const char *classify_va(uint64_t va) {
  if (va == 0)
    return "zero";
  if (va_is_cookie_shaped(va))
    return "cookie_range";
  if (va >= KBASE_FIXED_VA_ZONE_START &&
      va < KBASE_FIXED_VA_ZONE_START + KBASE_FIXED_VA_ZONE_SIZE)
    return "fixed_va_zone";
  if (va >= 0x800000000000ull)
    return "high_zone";
  return "same_va_or_other";
}

/* A one-page BASE_MEM_FIXED allocation in the zone pan_kmod_kbase.c's
 * util_vma_heap owns. Run before and after the import: if the import
 * poisons the context the way BASE_MEM_FIXABLE does (they are mutually
 * exclusive per context), "after" fails EINVAL and stage two is dead in its
 * current form. NEVER pass FIXABLE here - that would cause the very failure
 * this is trying to detect.
 */
static bool try_fixed_alloc(int fd, uint64_t at, const char *label) {
  union kbase_ioctl_mem_alloc_ex a = {0};
  a.in.va_pages = 1;
  a.in.commit_pages = 1;
  a.in.extension = 0;
  a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
               BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_FIXED;
  a.in.fixed_address = at;

  if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC_EX, &a) < 0) {
    printf("    %s: FIXED alloc @ 0x%llx -> FAILED (%s)\n", label,
           (unsigned long long)at, strerror(errno));
    return false;
  }
  printf("    %s: FIXED alloc @ 0x%llx -> gpu_va=0x%llx\n", label,
         (unsigned long long)at, (unsigned long long)a.out.gpu_va);
  return true;
}

static void dmabuf_sync(int fd, uint64_t flags) {
  struct dma_buf_sync s = {.flags = flags};
  /* Best-effort: some exporters have no begin/end_cpu_access. A failure here
   * is informational, not fatal - it is reported, not counted.
   */
  if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) < 0)
    printf("    (DMA_BUF_IOCTL_SYNC flags=0x%llx: %s)\n",
           (unsigned long long)flags, strerror(errno));
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  const char *source = "heap";
  const char *heap_name = "system";
  size_t want = 4096;
  int force_combo = -1;

  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "--source=", 9))
      source = argv[i] + 9;
    else if (!strncmp(argv[i], "--heap=", 7))
      heap_name = argv[i] + 7;
    else if (!strncmp(argv[i], "--size=", 7))
      want = (size_t)strtoul(argv[i] + 7, NULL, 0);
    else if (!strncmp(argv[i], "--combo=", 8))
      force_combo = (int)strtol(argv[i] + 8, NULL, 0);
    else {
      fprintf(stderr,
              "usage: %s [--source=heap|ahb] [--heap=NAME] [--size=BYTES]\n"
              "          [--combo=N]\n"
              "\n"
              "  --combo=N forces one import flag combination instead of\n"
              "  taking the first that works: 0=base, 1=IMPORT_SHARED,\n"
              "  2=SYNC_ON_MAP_UNMAP. Use it to test whether a combination\n"
              "  changes CPU accessibility of the imported region, which\n"
              "  the default (first-that-works) run cannot tell you.\n",
              argv[0]);
      return 2;
    }
  }
  sum_source = source;

  printf("\n=== dma-buf import probe (source=%s heap=%s size=%zu) ===\n",
         source, heap_name, want);
  printf("  no GPU work is performed by this probe - see the file header\n");

  /* ------------------------------------------------------- open the device */
  printf("\n=== kbase context ===\n");
  int fd = open_gpu();     /* exactly once: VERSION_CHECK is once-per-fd */
  if (fd <= 0) {
    fprintf(stderr, "could not open the kbase device\n");
    return 1;
  }

  /* ------------------------------------- FIXED alloc BEFORE (the control) */
  printf("\n=== FIXED_VA zone health, before import ===\n");
  bool fixed_before = try_fixed_alloc(fd, KBASE_FIXED_VA_ZONE_START, "before");
  sum_fixed_before = fixed_before ? "pass" : "fail";
  check(fixed_before, "BASE_MEM_FIXED allocation works before the import");

  /* --------------------------------------------------- get a dma-buf fd */
  printf("\n=== dma-buf source ===\n");
  struct dmabuf_src src;
  if (dmabuf_source_open(&src, source, heap_name, want) != 0) {
    printf("  could not obtain a dma-buf fd: %s\n", src.err);
    printf("\n  This is NOT the same answer as 'kbase cannot import'. The fd\n"
           "  source was unavailable; kbase was never asked. Try\n"
           "  --source=%s.\n", strcmp(source, "heap") ? "heap" : "ahb");
    sum_verdict = "FD_SOURCE_UNAVAILABLE";
    failures++;
    goto summary;
  }
  printf("  got dma-buf fd=%d via %s\n", src.fd, src.how);

  off_t lseek_size = lseek(src.fd, 0, SEEK_END);
  printf("  lseek(SEEK_END) = %lld bytes\n", (long long)lseek_size);
  sum_dmabuf_size = (unsigned long long)lseek_size;

  /* Write a sentinel through the dma-buf's own mapping first, so the kbase
   * side has something to find that it demonstrably did not write itself.
   */
  const uint64_t SENT_D2K = 0xCAFEF00DD15EA5EDull;
  const uint64_t SENT_K2D = 0x5EEDBEEFBAADF00Dull;
  size_t map_len = (size_t)((want + PAGE_SZ - 1) & ~(PAGE_SZ - 1));

  void *dmap = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                    src.fd, 0);
  bool dmap_ok = (dmap != MAP_FAILED);
  if (!dmap_ok) {
    printf("  mmap(dma-buf) failed: %s (round-trips will be SKIPped)\n",
           strerror(errno));
    dmap = NULL;
  } else {
    dmabuf_sync(src.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    memset(dmap, 0, map_len);
    *(volatile uint64_t *)dmap = SENT_D2K;
    dmabuf_sync(src.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    printf("  wrote 0x%016" PRIx64 " at dma-buf offset 0\n", SENT_D2K);
  }

  /* ------------------------------------------------------- MEM_IMPORT */
  printf("\n=== KBASE_IOCTL_MEM_IMPORT (type=UMM) ===\n");

  /* phandle is a pointer TO the fd for UMM imports - the kernel does a
   * get_user() on it. Passing the fd by value is the obvious mistake here.
   */
  const uint64_t base_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                              BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
  struct {
    const char *name;
    uint64_t extra;
  } combos[] = {
      {"base", 0},
      {"base|IMPORT_SHARED", BASE_MEM_IMPORT_SHARED},
      {"base|SYNC_ON_MAP_UNMAP", BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP},
  };

  /* Try every fd the handle carried, not only the one we were handed. For
   * the heap source that is a one-element list; for AHardwareBuffer it is
   * the difference between "the AHB path fails" and "data[0] is not the
   * dma-buf on this vendor's handle layout, but data[N] is".
   */
  int try_fds[DMABUF_MAX_HANDLE_FDS];
  int n_try = 0;
  if (src.n_handle_fds > 0) {
    for (int i = 0; i < src.n_handle_fds; i++)
      try_fds[n_try++] = src.handle_fds[i];
  } else {
    try_fds[n_try++] = src.fd;
  }

  union kbase_ioctl_mem_import imp;
  bool imported = false;
  const size_t ncombo = sizeof(combos) / sizeof(combos[0]);
  for (int fi = 0; fi < n_try && !imported; fi++) {
   if (n_try > 1)
     printf("  -- trying handle fd[%d] = %d --\n", fi, try_fds[fi]);
   for (size_t i = 0; i < ncombo; i++) {
    if (force_combo >= 0 && (size_t)force_combo != i)
      continue;
    int pass_fd = try_fds[fi];
    memset(&imp, 0, sizeof(imp));
    imp.in.flags = base_flags | combos[i].extra;
    imp.in.phandle = (uint64_t)(uintptr_t)&pass_fd;
    imp.in.type = BASE_MEM_IMPORT_TYPE_UMM;

    if (ioctl(fd, KBASE_IOCTL_MEM_IMPORT, &imp) == 0) {
      printf("  MEM_IMPORT [%s] -> OK\n", combos[i].name);
      sum_import_flag_combo = combos[i].name;
      sum_imported_fd_index = fi;
      imported = true;
      break;
    }
    printf("  MEM_IMPORT [%s] -> FAILED: %s\n", combos[i].name,
           strerror(errno));
    sum_import_errno = errno;
   }
  }

  check(imported, "KBASE_IOCTL_MEM_IMPORT accepted a dma-buf fd");
  if (!imported) {
    sum_import_result = "ioctl_failed";
    sum_verdict = "IMPORT_UNAVAILABLE";
    goto cleanup;
  }
  sum_import_result = "ok";
  sum_import_errno = 0;
  sum_out_flags = (unsigned long long)imp.out.flags;
  sum_gpu_va_raw = (unsigned long long)imp.out.gpu_va;
  sum_va_pages = (unsigned long long)imp.out.va_pages;

  format_mem_flags(imp.out.flags, sum_flags_decoded, sizeof(sum_flags_decoded));
  printf("  out.flags    = 0x%llx  [%s]\n", sum_out_flags, sum_flags_decoded);
  printf("  out.gpu_va   = 0x%llx\n", sum_gpu_va_raw);
  printf("  out.va_pages = %llu (%llu bytes)\n", sum_va_pages,
         sum_va_pages * PAGE_SZ);

  /* ------------------------------ signal 1+2: the flag and the magnitude */
  printf("\n=== is out.gpu_va an address or an mmap cookie? ===\n");

  bool need_mmap = (imp.out.flags & BASE_MEM_NEED_MMAP) != 0;
  sum_need_mmap = need_mmap ? "yes" : "no";
  sum_gpu_va_kind = need_mmap ? "cookie" : "address";
  printf("  BASE_MEM_NEED_MMAP is %s -> out.gpu_va is %s\n",
         need_mmap ? "SET" : "clear", sum_gpu_va_kind);
  printf("  (NEED_MMAP is BASE_MEM_FLAGS_OUTPUT_MASK in its entirety - it is\n"
         "   the only output-only bit kbase defines, so it is the whole\n"
         "   answer, but it is cross-checked against magnitude below)\n");

  bool cookie_shaped = va_is_cookie_shaped(imp.out.gpu_va);
  printf("  magnitude: 0x%llx is %s (cookie range is [0x%llx, 0x%llx))\n",
         sum_gpu_va_raw, cookie_shaped ? "cookie-shaped" : "not cookie-shaped",
         (unsigned long long)BASE_MEM_COOKIE_BASE,
         (unsigned long long)KBASE_COOKIE_LIMIT);

  /* Disagreement is a hard stop. alias_cs_probe is the cautionary tale:
   * one signal said the value was usable, it was not, and the device needed
   * a reboot. Two signals that disagree means we do not understand the
   * value, and not understanding it is exactly when not to use it.
   */
  check(cookie_shaped == need_mmap,
        "NEED_MMAP and address magnitude agree with each other");
  if (cookie_shaped != need_mmap) {
    printf("\n  STOPPING: the flag and the magnitude disagree. Do not point\n"
           "  anything at this address until that is understood.\n");
    sum_verdict = "IMPORT_WORKS_WITH_CAVEATS";
    goto cleanup;
  }

  /* --------------------------------------------------- resolve the address */
  printf("\n=== resolving the GPU address ===\n");
  void *kmap = NULL;
  uint64_t resolved = 0;

  if (need_mmap) {
    kmap = mmap(NULL, (size_t)(imp.out.va_pages * PAGE_SZ),
                PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                (off_t)imp.out.gpu_va);
    if (kmap == MAP_FAILED) {
      printf("  mmap(mali_fd, offset=0x%llx) FAILED: %s\n", sum_gpu_va_raw,
             strerror(errno));
      sum_mmap_a = "failed";
      kmap = NULL;
      check(false, "cookie resolved to a mapping via mmap()");
      sum_verdict = "IMPORT_WORKS_WITH_CAVEATS";
      goto cleanup;
    }
    sum_mmap_a = "ok";
    resolved = (uint64_t)(uintptr_t)kmap;
    printf("  mmap(offset=0x%llx) -> %p; under SAME_VA semantics that CPU\n"
           "  address IS the GPU address (Panfork resolves it the same way)\n",
           sum_gpu_va_raw, kmap);
    check(true, "cookie resolved to a mapping via mmap()");

    /* Is the cookie single-use? Stage two needs this to decide what
     * bo_get_mmap_offset() can return for an imported BO. remap_probe
     * answered it for MEM_ALLOC; that answer must not be assumed to carry.
     */
    void *again = mmap(NULL, (size_t)(imp.out.va_pages * PAGE_SZ),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                       (off_t)imp.out.gpu_va);
    if (again == MAP_FAILED) {
      static char b[64];
      snprintf(b, sizeof(b), "failed:%s", strerror(errno));
      sum_mmap_b = b;
      printf("  re-mmap of the same cookie: FAILED (%s) - single-use\n",
             strerror(errno));
    } else {
      sum_mmap_b = "ok";
      printf("  re-mmap of the same cookie: ok -> %p (reusable)\n", again);
      munmap(again, (size_t)(imp.out.va_pages * PAGE_SZ));
    }
  } else {
    resolved = imp.out.gpu_va;
    sum_mmap_a = "not_needed";
    printf("  out.gpu_va is already a GPU address: 0x%llx\n",
           (unsigned long long)resolved);
    kmap = mmap(NULL, (size_t)(imp.out.va_pages * PAGE_SZ),
                PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                (off_t)imp.out.gpu_va);
    if (kmap == MAP_FAILED) {
      printf("  (no CPU view: mmap failed: %s)\n", strerror(errno));
      kmap = NULL;
    }
  }
  sum_resolved_va = (unsigned long long)resolved;
  sum_va_zone = classify_va(resolved);
  printf("  resolved GPU VA = 0x%llx  (zone: %s)\n", sum_resolved_va,
         sum_va_zone);

  /* --------------------- signal 3+4: coverage and the FIXED_VA collision */
  printf("\n=== does it cover the buffer, and does it collide? ===\n");

  unsigned long long need_bytes =
      (unsigned long long)((lseek_size > 0 ? (uint64_t)lseek_size : want) +
                           PAGE_SZ - 1) &
      ~(PAGE_SZ - 1);
  bool covers = (sum_va_pages * PAGE_SZ) >= need_bytes;
  sum_covers = covers ? "yes" : "no";
  printf("  region is %llu bytes, buffer needs %llu\n",
         sum_va_pages * PAGE_SZ, need_bytes);
  check(covers, "imported region covers the whole dma-buf");

  bool in_fixed = (resolved >= KBASE_FIXED_VA_ZONE_START &&
                   resolved < KBASE_FIXED_VA_ZONE_START +
                                  KBASE_FIXED_VA_ZONE_SIZE);
  sum_in_fixed_zone = in_fixed ? "yes" : "no";
  printf("  FIXED_VA zone is [0x%llx, 0x%llx)\n",
         (unsigned long long)KBASE_FIXED_VA_ZONE_START,
         (unsigned long long)(KBASE_FIXED_VA_ZONE_START +
                              KBASE_FIXED_VA_ZONE_SIZE));
  check(!in_fixed,
        "imported VA is OUTSIDE the backend's util_vma_heap range");
  if (in_fixed)
    printf("    ^ if this ever fails, stage two must reserve the imported\n"
           "      range out of the heap (util_vma_heap_alloc_addr) or the two\n"
           "      allocators will hand out the same address\n");

  /* ------------------------------------------------- the CPU round-trips */
  printf("\n=== CPU round-trip: is it really the same memory? ===\n");
  if (!kmap || !dmap_ok) {
    printf("  SKIPPED (need both a dma-buf mapping and a kbase mapping)\n");
    sum_rt_d2k = "skipped";
    sum_rt_k2d = "skipped";
  } else {
    uint64_t got = 0;
    if (!safe_read64(kmap, &got)) {
      /* The mapping exists but is not backed. Worth stating plainly: it
       * means an imported BO cannot be given a CPU view this way, which is
       * a constraint on what the Mesa-side import can offer, not a failure
       * of the import itself.
       */
      printf("  dma-buf -> kbase: FAULTED (SIGBUS) reading the kbase "
             "mapping\n");
      printf("    mmap() succeeded but the pages are not backed. For a UMM\n"
             "    import kbase attaches the dma_buf lazily; a CPU view is\n"
             "    evidently not set up by mmap() alone.\n");
      sum_rt_d2k = "sigbus";
      sum_kbase_cpu_map = "sigbus";
    } else {
      sum_kbase_cpu_map = "readable";
      bool d2k = (got == SENT_D2K);
      sum_rt_d2k = d2k ? "pass" : "fail";
      printf("  dma-buf -> kbase: wrote 0x%016" PRIx64 ", kbase reads "
             "0x%016" PRIx64 "\n", SENT_D2K, got);
      check(d2k, "a write through the dma-buf is visible through kbase");
    }

    if (!strcmp(sum_kbase_cpu_map, "readable")) {
      if (!safe_write64((char *)kmap + 2048, SENT_K2D)) {
        printf("  kbase -> dma-buf: FAULTED writing the kbase mapping\n");
        sum_rt_k2d = "sigbus";
      } else {
        __sync_synchronize();
        dmabuf_sync(src.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
        uint64_t back = *(volatile uint64_t *)((char *)dmap + 2048);
        dmabuf_sync(src.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
        bool k2d = (back == SENT_K2D);
        sum_rt_k2d = k2d ? "pass" : "fail";
        printf("  kbase -> dma-buf: wrote 0x%016" PRIx64 ", dma-buf reads "
               "0x%016" PRIx64 "\n", SENT_K2D, back);
        check(k2d, "a write through kbase is visible through the dma-buf");
      }
    } else {
      sum_rt_k2d = "skipped";
    }
  }

  /* ------------------------------------------------- how does it free? */
  printf("\n=== teardown model (stage two needs this exactly right) ===\n");
  {
    struct kbase_ioctl_mem_free mf = {.gpu_addr = resolved};
    int r = ioctl(fd, KBASE_IOCTL_MEM_FREE, &mf);
    if (r == 0) {
      snprintf(sum_free_model, sizeof(sum_free_model), "mem_free");
      printf("  MEM_FREE(0x%llx) -> ok: this frees like a FIXED region\n",
             sum_resolved_va);
    } else {
      snprintf(sum_free_model, sizeof(sum_free_model), "munmap_only:%s",
               strerror(errno));
      printf("  MEM_FREE(0x%llx) -> %s: frees like a SAME_VA region, i.e.\n"
             "  munmap() is the free and MEM_FREE must NOT be called\n",
             sum_resolved_va, strerror(errno));
    }
    if (kmap)
      munmap(kmap, (size_t)(imp.out.va_pages * PAGE_SZ));
    kmap = NULL;
  }

cleanup:
  if (dmap)
    munmap(dmap, map_len);
  if (src.fd >= 0 || src.ahb)
    dmabuf_source_close(&src);

  /* ------------------------- FIXED alloc AFTER (did the import poison us?) */
  printf("\n=== FIXED_VA zone health, after import ===\n");
  {
    bool after = try_fixed_alloc(fd, KBASE_FIXED_VA_ZONE_START + 0x100000,
                                 "after");
    sum_fixed_after = after ? "pass" : "fail";
    check(after, "BASE_MEM_FIXED still works after the import");
    if (!after)
      printf("    ^ the import poisoned the context the way BASE_MEM_FIXABLE\n"
             "      does. Stage two cannot proceed in its current form.\n");
  }

  if (!strcmp(sum_import_result, "ok")) {
    bool clean = !strcmp(sum_in_fixed_zone, "no") &&
                 !strcmp(sum_fixed_after, "pass") &&
                 (!strcmp(sum_rt_d2k, "pass") || !strcmp(sum_rt_d2k, "skipped"));
    sum_verdict = (failures == 0 && clean) ? "IMPORT_WORKS"
                                           : "IMPORT_WORKS_WITH_CAVEATS";
  }

summary:
  close(fd);

  printf("\n=== DMABUF IMPORT SUMMARY ===\n");
  printf("SOURCE=%s\n", sum_source);
  printf("DMABUF_SIZE=%llu\n", sum_dmabuf_size);
  printf("IMPORT_RESULT=%s\n", sum_import_result);
  printf("IMPORT_ERRNO=%d\n", sum_import_errno);
  printf("IMPORT_FLAG_COMBO=%s\n", sum_import_flag_combo);
  printf("IMPORTED_HANDLE_FD_INDEX=%d\n", sum_imported_fd_index);
  printf("OUT_FLAGS=0x%llx\n", sum_out_flags);
  printf("OUT_FLAGS_DECODED=%s\n", sum_flags_decoded);
  printf("NEED_MMAP=%s\n", sum_need_mmap);
  printf("GPU_VA_RAW=0x%llx\n", sum_gpu_va_raw);
  printf("GPU_VA_KIND=%s\n", sum_gpu_va_kind);
  printf("RESOLVED_GPU_VA=0x%llx\n", sum_resolved_va);
  printf("VA_ZONE=%s\n", sum_va_zone);
  printf("IMPORT_VA_IN_FIXED_ZONE=%s\n", sum_in_fixed_zone);
  printf("VA_PAGES=%llu\n", sum_va_pages);
  printf("COVERS_BUFFER=%s\n", sum_covers);
  printf("MMAP_CANDIDATE_A=%s\n", sum_mmap_a);
  printf("MMAP_CANDIDATE_B=%s\n", sum_mmap_b);
  printf("KBASE_CPU_MAP=%s\n", sum_kbase_cpu_map);
  printf("ROUNDTRIP_DMABUF_TO_KBASE=%s\n", sum_rt_d2k);
  printf("ROUNDTRIP_KBASE_TO_DMABUF=%s\n", sum_rt_k2d);
  printf("FIXED_ALLOC_BEFORE_IMPORT=%s\n", sum_fixed_before);
  printf("FIXED_ALLOC_AFTER_IMPORT=%s\n", sum_fixed_after);
  printf("FREE_MODEL=%s\n", sum_free_model);
  printf("VERDICT=%s\n", sum_verdict);

  printf("\n=== %d failure(s) ===\n", failures);
  return failures ? 1 : 0;
}
