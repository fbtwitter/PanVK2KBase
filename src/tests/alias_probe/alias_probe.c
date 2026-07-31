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
      printf("\n=> The alias exists but is not CPU-mappable at that address.\n"
             "   That is not fatal for PanVK - the ringbuf is GPU-only\n"
             "   (PAN_KMOD_BO_FLAG_NO_MMAP) - but it means this probe cannot\n"
             "   verify the aliasing from the CPU, and a command stream\n"
             "   would be needed to confirm it.\n");
      return failures ? 1 : 0;
   }

   volatile uint32_t *w0 = (volatile uint32_t *)map;
   volatile uint32_t *w1 = (volatile uint32_t *)((uint8_t *)map + RING_BYTES);

   printf("  window 0 word 0 = 0x%08x (want 0x%08x, the seed)\n", w0[0],
          0xA11A5111);
   check(w0[0] == 0xA11A5111, "window 0 sees the original pages");

   printf("  window 1 word 0 = 0x%08x\n", w1[0]);
   check(w1[0] == 0xA11A5111, "window 1 sees the same pages");

   /* The decisive test: write through one window, read through the other.
    * Two separately-allocated regions would not track each other.
    */
   w0[1] = 0xDEADBEEF;
   __sync_synchronize();
   printf("  wrote 0x%08x at window 0 word 1; window 1 word 1 = 0x%08x\n",
          0xDEADBEEF, w1[1]);
   check(w1[1] == 0xDEADBEEF, "*** a write through window 0 appears in "
                              "window 1 ***");

   w1[2] = 0x5A5A5A5A;
   __sync_synchronize();
   check(w0[2] == 0x5A5A5A5A, "and the same in the other direction");

   printf("\n================================================================\n");
   if (failures == 0)
      printf("RESULT: kbase can map one allocation at two adjacent GPU VAs\n"
             "        via MEM_ALIAS. The render descriptor ringbuf's\n"
             "        wraparound trick is expressible, so the backend can\n"
             "        grow an alias operation and the render subqueues can\n"
             "        be initialised.\n");
   else
      printf("RESULT: %d check(s) FAILED - see above. Aliasing does not do\n"
             "        what the ringbuf needs; the wraparound would have to be\n"
             "        handled another way.\n", failures);
   printf("================================================================\n");

   munmap(map, RING_BYTES * 2);
   return failures ? 1 : 0;
}
