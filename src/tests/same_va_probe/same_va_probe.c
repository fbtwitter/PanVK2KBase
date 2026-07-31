// What this probe is for
// ----------------------
// utils/memory.h ends kbase_bo_create() with:
//
//     bo->gpu_va = (uint64_t)(uintptr_t)bo->cpu;
//
// That says "the CPU pointer is also the GPU address". The question is
// whether that is true for every allocation, or only for some of them.
//
// It turns out kbase has two ways of telling you where an allocation
// lives, and you only find out which one you got by reading out.flags:
//
//   With BASE_MEM_SAME_VA, MEM_ALLOC hands back a placeholder number
//   ("a cookie"), not an address, and the memory is not mapped on the GPU
//   yet. You mmap() the cookie, and the CPU address you get back is then
//   also the GPU address. Here the line above is correct.
//
//   Without SAME_VA, kbase maps the memory straight away and out.gpu_va
//   is already the real GPU address. The CPU mapping is somewhere else
//   entirely. Here the line above destroys a good address.
//
// So the rule this probe tests is:
//
//     SAME_VA granted     ->  out.gpu_va is a cookie; the CPU pointer is
//                             the real GPU address
//     SAME_VA not granted ->  out.gpu_va IS the real GPU address; the CPU
//                             pointer is unrelated to it
//
// How it decides, without trusting either answer
// ----------------------------------------------
// MEM_ALIAS needs a real GPU address as its input handle and rejects
// anything else. That makes it a usable oracle: feed it both numbers and
// see which one kbase accepts. The probe asserts that the accepted one
// flips over exactly when SAME_VA does.
//
// It runs three allocations that land in three different address regions
// (plain, GPU-executable, and FIXED/FIXABLE), so the result reflects a
// general rule rather than one special case.
//
// This file deliberately does NOT include the repo's memory.h. It repeats
// the same MEM_ALLOC + mmap that kbase_bo_create() does, so what it
// measures is the kernel's behaviour and not the repo's opinion of it.
//
// For reference, this is the kernel code the rule comes from -
// kbase_mem_alloc() in mali_kbase_mem_linux.c, which branches on that one
// flag and nothing else:
//
//     if (*flags & BASE_MEM_SAME_VA) {
//         cookie = cookie_nr + PFN_DOWN(BASE_MEM_COOKIE_BASE);
//         cookie <<= PAGE_SHIFT;
//         *gpu_va = (u64) cookie;                    /* a COOKIE */
//     } else /* we control the VA */ {
//         kbase_gpu_mmap(kctx, reg, *gpu_va, va_pages, 1, mmu_sync_info);
//         *gpu_va = reg->start_pfn << PAGE_SHIFT;    /* a REAL ADDRESS */
//     }

/* The system headers come first deliberately: utils/initialize.h uses
 * open()/O_RDWR and strerror() without including <fcntl.h> or <string.h>
 * itself, so it only compiles when the includer has pulled those in.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "initialize.h"
#include "mali_base_common_kernel.h"
#include "mali_base_kernel.h"
#include "mali_kbase_ioctl.h"

/* Not present in every vendored uapi version. Values from
 * mali_base_common_kernel.h where they are defined.
 */
#ifndef BASE_MEM_FIXABLE
#define BASE_MEM_FIXABLE ((base_mem_alloc_flags)1 << 29)
#endif
#ifndef BASE_MEM_FIXED
#define BASE_MEM_FIXED ((base_mem_alloc_flags)1 << 8)
#endif

#define PAGE_SZ 4096
#define PAGES 4
#define BYTES (PAGES * PAGE_SZ)

#define RW_FLAGS                                                               \
   (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |       \
    BASE_MEM_PROT_GPU_WR)

static int failures;

static void check(bool ok, const char *what)
{
   printf("    %-58s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

/* Does kbase accept `handle` as a MEM_ALIAS source handle? MEM_ALIAS
 * needs a real GPU address, so this is an independent test of whether a
 * given number is one.
 */
static bool alias_accepts(int fd, uint64_t handle)
{
   struct base_mem_aliasing_info ai[2] = {
      { .handle = { .basep = { .handle = handle } },
        .offset = 0,
        .length = PAGES },
      { .handle = { .basep = { .handle = handle } },
        .offset = 0,
        .length = PAGES },
   };

   union kbase_ioctl_mem_alias alias = { 0 };
   alias.in.flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
   alias.in.stride = PAGES;
   alias.in.nents = 2;
   alias.in.aliasing_info = (uint64_t)(uintptr_t)ai;

   return ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &alias) >= 0;
}

/* Exactly what kbase_bo_create()/kbase_bo_create_flags() does today:
 * plain KBASE_IOCTL_MEM_ALLOC, then mmap() at the reported gpu_va.
 */
static bool probe_flags(int fd, uint64_t in_flags, const char *label)
{
   union kbase_ioctl_mem_alloc alloc = { 0 };

   alloc.in.va_pages = PAGES;
   alloc.in.commit_pages = PAGES;
   alloc.in.extension = 0;
   alloc.in.flags = in_flags;

   printf("\n=== %s ===\n", label);
   printf("  in.flags  = 0x%llx\n", (unsigned long long)alloc.in.flags);

   if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc) < 0) {
      printf("  MEM_ALLOC FAILED (%s)\n", strerror(errno));
      return false;
   }

   bool same_va = (alloc.out.flags & BASE_MEM_SAME_VA) != 0;
   uint64_t out_gpu_va = alloc.out.gpu_va;

   printf("  out.flags = 0x%llx   SAME_VA granted = %s\n",
          (unsigned long long)alloc.out.flags, same_va ? "YES" : "NO");
   printf("  out.gpu_va= 0x%016llx\n", (unsigned long long)out_gpu_va);

   /* PROT_READ only: kbase refuses PROT_WRITE for regions without CPU_WR,
    * and the GPU_EX cases below deliberately drop it.
    */
   int prot = PROT_READ | ((in_flags & BASE_MEM_PROT_CPU_WR) ? PROT_WRITE : 0);
   void *cpu = mmap(NULL, BYTES, prot, MAP_SHARED, fd, (off_t)out_gpu_va);
   if (cpu == MAP_FAILED) {
      printf("  mmap(offset=out.gpu_va) FAILED (%s)\n", strerror(errno));
      return false;
   }
   uint64_t cpu_va = (uint64_t)(uintptr_t)cpu;
   printf("  cpu       = 0x%016llx\n", (unsigned long long)cpu_va);

   printf("  unconditional 'gpu_va = cpu' stores 0x%016llx\n",
          (unsigned long long)cpu_va);
   printf("  checking SAME_VA first stores     0x%016llx  <- correct\n",
          (unsigned long long)(same_va ? cpu_va : out_gpu_va));

   /* The invariant, checked both ways round via MEM_ALIAS. */
   bool alias_takes_out = alias_accepts(fd, out_gpu_va);
   bool alias_takes_cpu = alias_accepts(fd, cpu_va);

   printf("  MEM_ALIAS accepts out.gpu_va? %-3s   accepts cpu? %s\n",
          alias_takes_out ? "YES" : "NO", alias_takes_cpu ? "YES" : "NO");

   if (same_va) {
      check(out_gpu_va != cpu_va, "SAME_VA: out.gpu_va is a cookie, != cpu");
      check(alias_takes_cpu, "SAME_VA: the CPU pointer IS the real address");
      check(!alias_takes_out, "SAME_VA: out.gpu_va is NOT a real address");
   } else {
      check(out_gpu_va != cpu_va, "!SAME_VA: out.gpu_va != cpu");
      check(alias_takes_out, "!SAME_VA: out.gpu_va IS the real address");
      check(!alias_takes_cpu,
            "!SAME_VA: the CPU pointer is NOT the real address");
   }

   return true;
}

int main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   int fd = open_gpu();
   if (fd < 0)
      return 1;

   /* Creates the EXEC_VA zone. kbase_has_exec_va_zone() gates whether a
    * BASE_MEM_PROT_GPU_EX allocation is withheld SAME_VA, so this must
    * happen before the GPU_EX case below to exercise that path.
    */
   struct kbase_ioctl_mem_exec_init exec = { .va_pages = 1 << 16 };
   if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec) < 0)
      printf("MEM_EXEC_INIT: failed (%s) - the GPU_EX case below will not\n"
             "               reach the EXEC_VA zone\n",
             strerror(errno));
   else
      printf("MEM_EXEC_INIT: OK (EXEC_VA zone created)\n");

   /* Expected SAME_VA: the default, and what every kbase_bo_create()
    * call in this repo produces today.
    */
   probe_flags(fd, RW_FLAGS, "1. plain allocation  -> expect SAME_VA, cookie");

   /* Expected no SAME_VA: EXEC_VA zone (zone 2). Which flag combination
    * kbase will accept alongside GPU_EX is not obvious - a writable
    * executable region is refused - so try progressively narrower sets
    * rather than asserting one.
    */
   static const struct {
      uint64_t flags;
      const char *label;
   } exec_attempts[] = {
      { RW_FLAGS | BASE_MEM_PROT_GPU_EX,
        "2a. GPU_EX + full RW -> expect EXEC_VA, real address" },
      { BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
           BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX,
        "2b. GPU_EX, no GPU_WR -> expect EXEC_VA, real address" },
      { BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX,
        "2c. GPU_EX, read-only -> expect EXEC_VA, real address" },
      { BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_EX,
        "2d. GPU_EX, GPU-only  -> expect EXEC_VA, real address" },
   };

   bool exec_ok = false;
   for (unsigned i = 0; i < sizeof(exec_attempts) / sizeof(exec_attempts[0]);
        i++) {
      if (probe_flags(fd, exec_attempts[i].flags, exec_attempts[i].label)) {
         exec_ok = true;
         break;
      }
   }
   if (!exec_ok)
      printf("\n  (no GPU_EX combination was accepted via plain MEM_ALLOC on\n"
             "   this kernel - the EXEC_VA zone may need MEM_ALLOC_EX)\n");

   /* Expected no SAME_VA: FIXED_VA zone (zone 5).
    * Kept last: FIXED and FIXABLE are mutually exclusive per context, so
    * one FIXABLE allocation makes later FIXED requests fail EINVAL.
    */
   probe_flags(fd, RW_FLAGS | BASE_MEM_FIXABLE,
               "3. BASE_MEM_FIXABLE -> expect FIXED_VA, real address");

   printf("\n=== verdict ===\n");
   if (failures == 0)
      printf("  All invariant checks passed.\n"
             "  out.gpu_va is the real GPU address exactly when SAME_VA is\n"
             "  absent, so 'gpu_va = cpu' cannot be applied unconditionally.\n");
   else
      printf("  %d check(s) FAILED - the invariant does not hold as stated.\n",
             failures);

   close(fd);
   return failures ? 1 : 0;
}
