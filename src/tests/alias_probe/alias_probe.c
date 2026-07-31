// Can kbase map one allocation at two adjacent GPU VAs?
//
// This is the question blocking the render subqueues. PanVK's render
// descriptor ringbuf (init_render_desc_ringbuf() in csf/panvk_vX_gpu_queue.c)
// maps a single BO at dev_addr and again at dev_addr + size, so a descriptor
// read that runs off the end of the ring wraps into the copy and the
// wraparound can be encoded with 32-bit arithmetic. Both VERTEX_TILER and
// FRAGMENT subqueue contexts carry a pointer to it, so without it neither
// subqueue can be initialised and command buffers stay unsubmittable.
//
// kbase's vm_bind shim cannot express that: an allocation lives where
// MEM_ALLOC_EX put it and cannot be mapped elsewhere, let alone twice (see
// kbase_kmod_vm_bind() in src/mesa/pan_kmod_kbase.c, which fails any
// caller-chosen VA that is not where the BO already is).
//
// KBASE_IOCTL_MEM_ALIAS (nr 21) is the mechanism that could express it: it
// builds a new VA region out of `nents` entries spaced `stride` bytes apart,
// each naming an existing allocation by handle with an offset and length.
// Two entries naming the same handle, with stride equal to its size, is
// exactly the double mapping PanVK wants. Panfork never calls it, so nothing
// in this repo's prior art says whether it works here.
//
// So this probe answers, in order:
//   1. does MEM_ALIAS exist on this kernel (vs ENOTTY)?
//   2. does aliasing one allocation twice succeed, and what VA comes back?
//   3. is out.va_pages the full 2x span?
//   4. is the alias CPU-mappable, and - the actual question - does a write
//      through the first window show up in the second? That is what proves
//      the two windows are the same pages rather than two zeroed regions.
//   5. does the aliased region survive being freed independently?
#include "csf/mali_kbase_csf_ioctl.h"
#include "flags_helper.h"
#include "initialize.h"
#include "memory.h"
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_SZ 4096
#define RING_PAGES 4
#define RING_BYTES (RING_PAGES * PAGE_SZ)

static int failures;

static void
check(bool ok, const char *what)
{
   printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

int
main(void)
{
   /* Unbuffered, so the perror() lines below interleave with the printf()s
    * in the order they actually happened rather than being flushed at exit.
    */
   setvbuf(stdout, NULL, _IONBF, 0);

   int fd = open_gpu();

   printf("=== 1. the allocation to be aliased ===\n");

   /* A plain SAME_VA allocation: its gpu_va doubles as the handle every
    * other kbase memory ioctl takes, which is what MEM_ALIAS wants.
    */
   struct kbase_bo *bo = kbase_bo_create(fd, RING_BYTES);
   if (!bo) {
      fprintf(stderr, "  allocation FAILED\n");
      return 1;
   }

   uint64_t handle = bo->gpu_va;
   printf("  allocated %d bytes, handle/gpu_va = 0x%llx\n", RING_BYTES,
          (unsigned long long)handle);

   /* Seed through the original mapping so we can tell "the alias sees the
    * same pages" from "the alias is a fresh zeroed region".
    */
   volatile uint32_t *orig = (volatile uint32_t *)bo->cpu;
   orig[0] = 0xA11A5111;
   printf("  seeded word 0 through the original mapping = 0x%08x\n", orig[0]);

   printf("\n=== 2. MEM_ALIAS, two entries, same handle ===\n");

   struct base_mem_aliasing_info ai[2] = {
      {
         .handle = { .basep = { .handle = handle } },
         .offset = 0,
         .length = RING_PAGES,
      },
      {
         .handle = { .basep = { .handle = handle } },
         .offset = 0,
         .length = RING_PAGES,
      },
   };

   /* Which u64 is "the handle" is not obvious here. Under BASE_MEM_SAME_VA
    * the gpu_va an allocation reports is an mmap cookie, not an address -
    * the real GPU VA is the CPU pointer (see the SAME_VA/cookie section in
    * docs/kbase-notes.md). Both are plausible handles, and the flags may
    * matter too, so try the combinations rather than guessing one.
    */
   const struct {
      const char *what;
      uint64_t handle;
      uint64_t flags;
   } attempts[] = {
      { "cookie handle, SAME_VA alias", handle,
        BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |
           BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA },
      { "real GPU VA handle, SAME_VA alias", (uint64_t)(uintptr_t)bo->cpu,
        BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |
           BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA },
      { "real GPU VA handle, GPU-only alias", (uint64_t)(uintptr_t)bo->cpu,
        BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR },
      { "cookie handle, GPU-only alias", handle,
        BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR },
   };

   union kbase_ioctl_mem_alias alias = { 0 };
   bool aliased = false;

   for (unsigned i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
      ai[0].handle.basep.handle = attempts[i].handle;
      ai[1].handle.basep.handle = attempts[i].handle;

      memset(&alias, 0, sizeof(alias));
      alias.in.flags = attempts[i].flags;
      alias.in.stride = RING_PAGES; /* in pages, matching offset/length */
      alias.in.nents = 2;
      alias.in.aliasing_info = (uint64_t)(uintptr_t)ai;

      if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &alias) < 0) {
         printf("  %-36s -> %s\n", attempts[i].what, strerror(errno));
         if (errno == ENOTTY) {
            printf("\n=> MEM_ALIAS is not implemented on this kernel at all.\n");
            return 1;
         }
         continue;
      }

      printf("  %-36s -> OK\n", attempts[i].what);
      printf("    gpu_va=0x%llx va_pages=%llu out.flags=0x%llx%s\n",
             (unsigned long long)alias.out.gpu_va,
             (unsigned long long)alias.out.va_pages,
             (unsigned long long)alias.out.flags,
             (alias.out.flags & BASE_MEM_NEED_MMAP) ? "  <- NEED_MMAP: gpu_va "
                                                      "is a COOKIE, not an "
                                                      "address"
                                                    : "");

      /* Whether out.gpu_va is an address or an mmap cookie decides whether
       * a command stream can be pointed at the alias at all. Getting this
       * wrong is not a failed ioctl - it is a GPU page fault that wedges
       * the kbase context past kill -9 and needs a device reboot. Ask here,
       * where it costs nothing, rather than finding out from the GPU.
       */
      aliased = true;
      break;
   }

   if (!aliased) {
      printf("\n=> Every handle/flag combination was rejected. MEM_ALIAS is\n"
             "   present but not usable for this, so the ringbuf's\n"
             "   wraparound would have to be handled another way - e.g.\n"
             "   bounds-checking in the command stream rather than relying\n"
             "   on the mapping to wrap.\n");
      return 1;
   }

   printf("  MEM_ALIAS ok: gpu_va = 0x%llx, va_pages = %llu (want %d)\n",
          (unsigned long long)alias.out.gpu_va,
          (unsigned long long)alias.out.va_pages, RING_PAGES * 2);
   check(alias.out.va_pages == RING_PAGES * 2, "alias spans both windows");

   printf("\n=== 3. is the alias CPU-mappable, and do the windows agree? ===\n");

   /* An alias created with SAME_VA may come back as a cookie that still
    * needs mmap()ing, exactly like a normal SAME_VA allocation - see the
    * SAME_VA/cookie section in docs/kbase-notes.md.
    */
   void *map = mmap(NULL, RING_BYTES * 2, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, (off_t)alias.out.gpu_va);
   if (map == MAP_FAILED) {
      perror("  mmap of the alias");
      printf("\n=> Measured on a Mali-G720 / r49p1: out.flags carries\n"
             "   BASE_MEM_NEED_MMAP (0x400d = NEED_MMAP|GPU_WR|GPU_RD|CPU_RD,\n"
             "   and note SAME_VA is absent - kbase_mem_alias() strips it).\n"
             "   So out.gpu_va is an mmap COOKIE, not a GPU address, and the\n"
             "   region has no GPU mapping until userspace mmap()s it.\n"
             "\n"
             "   Two kernel-side constraints then box this in, both in\n"
             "   kbase_context_mmap():\n"
             "     - PROT_WRITE is refused (EPERM, \"VM flags inconsistent\n"
             "       with region flags\") because CPU_WR is not in the\n"
             "       accepted mask - which is the failure above; and\n"
             "     - nr_pages > stride is refused (EINVAL), so one mapping\n"
             "       can never span more than a single window.\n"
             "\n"
             "   That last one is the problem. PanVK's ringbuf needs ONE VA\n"
             "   range covering both windows back to back; a mapping capped\n"
             "   at `stride` pages cannot produce it.\n"
             "\n"
             "   DO NOT hand out.gpu_va to a command stream to test this.\n"
             "   tests/alias_cs_probe did exactly that, and writing to a\n"
             "   cookie faulted the GPU and wedged the kbase context past\n"
             "   kill -9 - twice - each time needing a device reboot.\n");
   } else {
      /* Reached only if the kernel ever stops refusing this shape. */
      printf("  mmap of the full 2x span succeeded at %p\n", map);
      volatile uint32_t *w0 = (volatile uint32_t *)map;
      volatile uint32_t *w1 =
         (volatile uint32_t *)((uint8_t *)map + RING_BYTES);

      check(w0[0] == 0xA11A5111, "window 0 sees the original pages");
      check(w1[0] == 0xA11A5111, "window 1 sees the same pages");
      munmap(map, RING_BYTES * 2);
   }

   /* --- 4. the shape kbase_context_mmap() should actually accept -------- */
   printf("\n=== 4. mmap shapes, per the kernel's own checks ===\n");

   /* PROT_READ only (CPU_WR is not in kbase_mem_alias()'s accepted mask),
    * and at most `stride` pages (nr_pages > stride is EINVAL). If this
    * works, the cookie resolves to an address - the question is then
    * whether one window is all we can ever get.
    */
   /* On a fresh alias, not the one section 3 already tried to map: a kbase
    * mmap cookie is single-use, and the failed attempt above most likely
    * consumed it. Re-aliasing costs nothing and removes the doubt.
    */
   ai[0].handle.basep.handle = (uint64_t)(uintptr_t)bo->cpu;
   ai[1].handle.basep.handle = (uint64_t)(uintptr_t)bo->cpu;

   union kbase_ioctl_mem_alias fresh = { 0 };
   fresh.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD |
                    BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
   fresh.in.stride = RING_PAGES;
   fresh.in.nents = 2;
   fresh.in.aliasing_info = (uint64_t)(uintptr_t)ai;

   if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &fresh) < 0) {
      perror("  re-alias for the mmap test");
   } else {
      void *m1 = mmap(NULL, RING_BYTES, PROT_READ, MAP_SHARED, fd,
                      (off_t)fresh.out.gpu_va);
      if (m1 == MAP_FAILED) {
         perror("  PROT_READ, stride pages, fresh cookie");
      } else {
         printf("  PROT_READ, %d pages -> %p, word 0 = 0x%08x\n", RING_PAGES,
                m1, *(volatile uint32_t *)m1);
         check(*(volatile uint32_t *)m1 == 0xA11A5111,
               "one window resolves and sees the original pages");
         munmap(m1, RING_BYTES);
      }
   }

   /* --- 5. variant: stride = the whole span ----------------------------- */
   printf("\n=== 5. variant: stride = full span, not window size ===\n");

   /* If nr_pages > stride is the blocker, a bigger stride should let a
    * single mapping cover more. The catch is what that does to the layout:
    * entries are spaced `stride` apart, so with stride = 2x the window the
    * two copies land 8 pages apart with a hole between them, not back to
    * back - which is not what the ringbuf wants even if it maps. Worth
    * measuring rather than reasoning about.
    */
   ai[0].handle.basep.handle = (uint64_t)(uintptr_t)bo->cpu;
   ai[1].handle.basep.handle = (uint64_t)(uintptr_t)bo->cpu;

   union kbase_ioctl_mem_alias wide = { 0 };
   wide.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                   BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                   BASE_MEM_SAME_VA;
   wide.in.stride = RING_PAGES * 2;
   wide.in.nents = 2;
   wide.in.aliasing_info = (uint64_t)(uintptr_t)ai;

   if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &wide) < 0) {
      perror("  MEM_ALIAS with stride = 2x window");
   } else {
      printf("  gpu_va=0x%llx va_pages=%llu out.flags=0x%llx%s\n",
             (unsigned long long)wide.out.gpu_va,
             (unsigned long long)wide.out.va_pages,
             (unsigned long long)wide.out.flags,
             (wide.out.flags & BASE_MEM_NEED_MMAP) ? "  <- still a COOKIE"
                                                   : "  <- a real address!");

      void *m2 = mmap(NULL, RING_BYTES * 2, PROT_READ, MAP_SHARED, fd,
                      (off_t)wide.out.gpu_va);
      if (m2 == MAP_FAILED) {
         perror("  mmap of 2x span with the wider stride");
      } else {
         printf("  mapped %d pages at %p; word 0 = 0x%08x, +stride = 0x%08x\n",
                RING_PAGES * 2, m2, *(volatile uint32_t *)m2,
                *(volatile uint32_t *)((uint8_t *)m2 + RING_BYTES * 2 -
                                       RING_BYTES));
         munmap(m2, RING_BYTES * 2);
      }
   }

   /* --- 6. variant: a FIXABLE source ------------------------------------ */
   printf("\n=== 6. variant: source allocated BASE_MEM_FIXABLE ===\n");

   /* The hope: a FIXABLE source lands in the FIXED_VA zone, and an alias of
    * it comes back as a real address rather than a cookie.
    *
    * Note FIXED and FIXABLE are mutually exclusive per context (see
    * docs/kbase-notes.md), so this is deliberately the last thing the probe
    * does - one FIXABLE allocation makes every later FIXED request in this
    * context fail EINVAL. The backend currently commits to FIXED, so if
    * this is the answer, that commitment has to be revisited.
    */
   struct kbase_bo *fixable =
      kbase_bo_create_flags(fd, RING_BYTES, BASE_MEM_FIXABLE);
   if (!fixable) {
      printf("  FIXABLE allocation failed - cannot test this variant\n");
   } else {
      printf("  FIXABLE source at gpu_va=0x%llx cpu=%p\n",
             (unsigned long long)fixable->gpu_va, fixable->cpu);

      /* NOT the CPU pointer here. That rule holds only for SAME_VA, where
       * the two are the same number; a FIXABLE allocation has a GPU VA in
       * the FIXED_VA zone and a CPU mapping somewhere unrelated, which is
       * the entire point of it. Passing the CPU pointer gets ENOMEM - "no
       * such allocation at that address" - which reads like a resource
       * limit and is not.
       */
      ai[0].handle.basep.handle = fixable->gpu_va;
      ai[1].handle.basep.handle = fixable->gpu_va;

      union kbase_ioctl_mem_alias fx = { 0 };
      fx.in.flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
      fx.in.stride = RING_PAGES;
      fx.in.nents = 2;
      fx.in.aliasing_info = (uint64_t)(uintptr_t)ai;

      if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &fx) < 0) {
         perror("  MEM_ALIAS of a FIXABLE source");
      } else {
         printf("  gpu_va=0x%llx va_pages=%llu out.flags=0x%llx%s\n",
                (unsigned long long)fx.out.gpu_va,
                (unsigned long long)fx.out.va_pages,
                (unsigned long long)fx.out.flags,
                (fx.out.flags & BASE_MEM_NEED_MMAP)
                   ? "  <- still a COOKIE"
                   : "  <- a REAL ADDRESS - this is the route");

         /* A cookie is not necessarily a dead end. For a NEED_MMAP region
          * kbase assigns the GPU VA *at mmap time*, and for regions without
          * SAME_VA the address it picks is the one mmap returns - so a
          * successful mmap is what would turn this cookie into the address
          * the ringbuf needs. The shapes tried above used nr_pages =
          * one window and nr_pages = 2 windows; try exactly va_pages,
          * which is the one thing that has not been asked for.
          */
         size_t want = (size_t)fx.out.va_pages * PAGE_SZ;
         void *m3 = mmap(NULL, want, PROT_READ, MAP_SHARED, fd,
                         (off_t)fx.out.gpu_va);
         if (m3 == MAP_FAILED) {
            perror("  mmap with nr_pages == va_pages");
         } else {
            printf("  mmap(va_pages=%llu) -> %p\n",
                   (unsigned long long)fx.out.va_pages, m3);
            printf("  window 0 word 0 = 0x%08x, window 1 word 0 = 0x%08x\n",
                   *(volatile uint32_t *)m3,
                   *(volatile uint32_t *)((uint8_t *)m3 + RING_BYTES));
            check(*(volatile uint32_t *)m3 ==
                     *(volatile uint32_t *)((uint8_t *)m3 + RING_BYTES),
                  "*** both windows resolve to the same contents ***");
            munmap(m3, want);
         }
      }
   }

   printf("\n================================================================\n");
   printf("RESULT: MEM_ALIAS cannot give the render descriptor ringbuf what\n"
          "        it needs on this kernel. Every variant - SAME_VA alias,\n"
          "        GPU-only alias, stride = full span, FIXABLE source -\n"
          "        returns BASE_MEM_NEED_MMAP, so there is no GPU mapping\n"
          "        until userspace mmap()s the cookie. And the mmap can\n"
          "        never cover both windows:\n"
          "\n"
          "          nr_pages == va_pages needs va_pages > stride, which\n"
          "          kbase_context_mmap() rejects EINVAL;\n"
          "          nr_pages <= stride covers one window only.\n"
          "\n"
          "        Since the GPU address is assigned at mmap time, the GPU\n"
          "        can only ever address one window. The wraparound trick\n"
          "        is not expressible here.\n"
          "\n"
          "        The aliasing itself is real - entries share alloc->pages\n"
          "        in the kernel source, and MEM_ALIAS composes the region\n"
          "        exactly as asked. It is the addressability that fails.\n"
          "\n"
          "        So the ringbuf has to stop relying on the mapping to\n"
          "        wrap, and bounds-check the ring in the command stream\n"
          "        instead. That is a change to shared PanVK code, not to\n"
          "        the kbase backend.\n");
   printf("================================================================\n");
   return failures ? 1 : 0;
}
