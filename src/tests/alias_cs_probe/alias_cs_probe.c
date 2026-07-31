// Does MEM_ALIAS really map one allocation at two GPU addresses?
//
// tests/alias_probe got as far as showing that KBASE_IOCTL_MEM_ALIAS
// accepts two entries naming the same allocation and reports va_pages for
// the full 2x span - which is what PanVK's render descriptor ringbuf needs,
// since it relies on a read running off the end of the ring wrapping into a
// second mapping of the same pages. But "the ioctl composed the region as
// asked" is not "both windows are the same memory", and this repo has been
// burned before by assuming a mapping meant what it looked like (the
// user-IO page order, see docs/kbase-notes.md).
//
// The CPU cannot close that gap. Reading the r49p1 kernel source for this
// device:
//
//   - kbase_mem_alias() strips BASE_MEM_SAME_VA and does not accept
//     BASE_MEM_PROT_CPU_WR, so an alias is not CPU-writable; asking for it
//     is what makes mmap() fail EPERM ("VM flags inconsistent with region
//     flags"), which is what alias_probe hit.
//   - kbase_context_mmap() rejects nr_pages > stride, so a CPU mapping can
//     never span more than one window anyway.
//
// So window 1 is unreachable from the CPU by design. The GPU is the only
// thing that can write it, which is what this probe uses:
//
//   1. Allocate a normal BO and seed word 0 through its CPU mapping.
//   2. MEM_ALIAS it twice, stride = its size.
//   3. Have a command stream SYNC_SET64 a sentinel to window 1
//      (alias_va + stride), reusing the machinery event_slot_probe proved.
//   4. Read word 0 back through the *original* BO's CPU mapping.
//
// If the sentinel appears there, a GPU write through window 1 landed on the
// original's pages, and the aliasing is real. If word 0 still holds the
// seed while CS_EXTRACT advanced, the stream ran but the two windows are
// not the same memory - which would mean the ringbuf cannot be built this
// way at all.
//
// !! DANGEROUS, AND CURRENTLY KNOWN-BROKEN. REQUIRES --i-know-it-hangs. !!
//
// Both times this has been run on real hardware it hung the kbase context
// hard: the process ends up in uninterruptible D state, kill -9 will not
// reap it, and the phone needs a reboot. The GPU itself survives - a fresh
// context opens fine afterwards, and live_kick_probe still passes - but the
// stuck process does not go away.
//
// The cause is not yet understood. BASE_MEM_NEED_MMAP was the obvious
// suspect (it would mean out.gpu_va is an mmap cookie rather than an
// address, so the CS would be writing to unmapped memory), and this probe
// now refuses to run if that flag is set - but it hung again with the check
// in place, which suggests the flag is clear and something else is wrong.
//
// So DO NOT run this to "see what happens". Establish first, from
// tests/alias_probe - which is CPU-only and has never hung - what
// out.gpu_va and out.flags actually are, and what makes an aliased region
// GPU-addressable. Only then point a command stream at it.
//
// The opt-in flag exists so this cannot be run by reflex, or by a script
// walking the probe list.
#include "csf/mali_kbase_csf_ioctl.h"
#include "csf_user_regs.h"
#include "genxml/cs_builder.h"
#include "initialize.h"
#include "memory.h"
#include "parse_gpu_props.h"
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define EVENT_SIZE 16
#define EVENT_SEED 1
#define EVENT_SIGNAL 2

// The allocation being aliased. Small - the point is the aliasing, not the
// size - but more than one page so a stride is meaningfully a stride.
#define RING_PAGES 4
#define RING_BYTES (RING_PAGES * PAGE_SIZE)

static struct cs_buffer noop_alloc(void *cookie) {
  (void)cookie;
  fprintf(stderr, "alloc_buffer called - short CS overflowed?\n");
  abort();
}

static void vendor_context_setup(int fd) {
  struct kbase_ioctl_mem_jit_init jit = {
      .va_pages = 1 << 14,
      .max_allocations = 255,
      .trim_level = 0,
      .group_id = 0,
      .phys_pages = 1 << 14,
  };
  if (ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &jit) < 0)
    printf("  MEM_JIT_INIT   : FAILED (%s)\n", strerror(errno));
  else
    printf("  MEM_JIT_INIT   : OK\n");

  struct kbase_ioctl_mem_exec_init exec = {.va_pages = 1 << 16};
  if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec) < 0)
    printf("  MEM_EXEC_INIT  : FAILED (%s)\n", strerror(errno));
  else
    printf("  MEM_EXEC_INIT  : OK\n");

  union kbase_ioctl_cs_tiler_heap_init heap = {0};
  heap.in.chunk_size = 2 * 1024 * 1024;
  heap.in.initial_chunks = 1;
  heap.in.max_chunks = 8;
  heap.in.target_in_flight = 1;
  if (ioctl(fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &heap) < 0)
    printf("  TILER_HEAP_INIT: FAILED (%s)\n", strerror(errno));
  else
    printf("  TILER_HEAP_INIT: OK gpu_heap_va=0x%llx\n",
           (unsigned long long)heap.out.gpu_heap_va);
}

int main(int argc, char **argv) {
  /* Unbuffered. This probe hangs the kbase context in ways that end in a
   * SIGKILL, and with the default block buffering on a pipe that discards
   * everything printed so far - which is exactly what happened on both runs
   * so far, leaving no evidence of how far it got. Do not remove.
   */
  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc < 2 || strcmp(argv[1], "--i-know-it-hangs") != 0) {
    fprintf(stderr,
            "This probe has hung the kbase context every time it has been\n"
            "run on hardware: the process ends in uninterruptible D state,\n"
            "kill -9 will not reap it, and the device needs a reboot.\n"
            "The cause is not understood yet - see the file header.\n"
            "\n"
            "Use tests/alias_probe first; it is CPU-only and safe.\n"
            "\n"
            "If you really mean it: %s --i-know-it-hangs\n",
            argv[0]);
    return 2;
  }

  int fd = open_gpu();

  uint64_t shader_present = kbase_get_shader_present(fd);
  printf("RAW_SHADER_PRESENT = 0x%llx (%d cores)\n",
         (unsigned long long)shader_present,
         __builtin_popcountll(shader_present));
  if (!shader_present) {
    fprintf(stderr, "could not read shader-core mask, aborting\n");
    return 1;
  }

  printf("\n=== vendor-style context setup ===\n");
  vendor_context_setup(fd);

  // --- 1. the allocation, and an alias of it -----------------------------
  printf("\n=== the aliased allocation ===\n");
  struct kbase_bo *event_bo = kbase_bo_create(fd, RING_BYTES);
  if (!event_bo) {
    fprintf(stderr, "allocation FAILED\n");
    return 1;
  }
  // SAME_VA: the CPU pointer is the GPU VA (see docs/kbase-notes.md), and
  // that - not the reported gpu_va, which is an mmap cookie - is what
  // MEM_ALIAS wants as a handle. tests/alias_probe establishes this.
  uint64_t orig_gpu_va = (uint64_t)(uintptr_t)event_bo->cpu;
  volatile uint64_t *event_slot = (volatile uint64_t *)event_bo->cpu;

  event_slot[0] = EVENT_SEED;
  __sync_synchronize();
  printf("  allocation at gpu_va=0x%llx, word 0 seeded = %llu\n",
         (unsigned long long)orig_gpu_va,
         (unsigned long long)event_slot[0]);

  struct base_mem_aliasing_info ai[2] = {
      {.handle = {.basep = {.handle = orig_gpu_va}},
       .offset = 0,
       .length = RING_PAGES},
      {.handle = {.basep = {.handle = orig_gpu_va}},
       .offset = 0,
       .length = RING_PAGES},
  };

  union kbase_ioctl_mem_alias alias = {0};
  // No SAME_VA and no CPU_WR: kbase_mem_alias() strips both from the
  // accepted mask, and asking for CPU_WR is what makes a later mmap fail
  // EPERM ("VM flags inconsistent with region flags"). Not that this probe
  // mmaps the alias - it cannot see window 1 that way, since
  // kbase_context_mmap() rejects nr_pages > stride. That is exactly why
  // this probe exists: the GPU is the only thing that can reach window 1.
  alias.in.flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
  alias.in.stride = RING_PAGES; // pages, like offset and length
  alias.in.nents = 2;
  alias.in.aliasing_info = (uint64_t)(uintptr_t)ai;

  if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &alias) < 0) {
    perror("  KBASE_IOCTL_MEM_ALIAS");
    return 1;
  }

  printf("  alias: gpu_va=0x%llx va_pages=%llu out.flags=0x%llx\n",
         (unsigned long long)alias.out.gpu_va,
         (unsigned long long)alias.out.va_pages,
         (unsigned long long)alias.out.flags);

  // CHECK THIS BEFORE POINTING THE GPU AT IT. BASE_MEM_NEED_MMAP means
  // out.gpu_va is an mmap cookie, not an address - the region is not mapped
  // for the GPU until userspace mmap()s it. An earlier version of this probe
  // skipped this check, assumed the returned value was an address because
  // SAME_VA had been stripped, and had the command stream write to
  // cookie + stride. That faulted the GPU and wedged the kbase context hard
  // enough that kill -9 would not reap it and the phone needed a reboot.
  //
  // Cost of the check: one branch. Cost of skipping it: a reboot.
  if (alias.out.flags & BASE_MEM_NEED_MMAP) {
    printf("\n  out.flags has BASE_MEM_NEED_MMAP: gpu_va is a COOKIE, not an\n"
           "  address. Realising it needs an mmap() first - and per\n"
           "  kbase_context_mmap() that can cover at most `stride` pages, so\n"
           "  it cannot produce a single address range spanning both\n"
           "  windows. Refusing to hand the GPU an unmapped address.\n");
    printf("\n=> The alias is not directly usable as a GPU address this way.\n"
           "   Next thing to try: allocate the source with BASE_MEM_FIXABLE\n"
           "   and the alias without SAME_VA, so the kernel places the alias\n"
           "   in the custom-VA zone and returns a real address rather than\n"
           "   a cookie. See the FIXED/FIXABLE note in docs/kbase-notes.md -\n"
           "   the two are mutually exclusive per context, so this interacts\n"
           "   with the backend's existing choice of FIXED.\n");
    kbase_bo_free(fd, event_bo);
    return 1;
  }

  // Window 1: one stride past the start of the alias. If aliasing works,
  // this is the same physical page as offset 0 of the original.
  uint64_t event_gpu_va = alias.out.gpu_va + (uint64_t)RING_BYTES;
  printf("  the CS will write %d to window 1 = 0x%llx\n", EVENT_SIGNAL,
         (unsigned long long)event_gpu_va);

  // --- group + queue -----------------------------------------------------
  printf("\n=== queue group ===\n");
  union kbase_ioctl_cs_queue_group_create_1_6 create = {0};
  create.in.tiler_mask = shader_present;
  create.in.fragment_mask = shader_present;
  create.in.compute_mask = shader_present;
  create.in.cs_min = 1;
  create.in.priority = 0;
  create.in.tiler_max = 1;
  create.in.fragment_max = 1;
  create.in.compute_max = 1;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &create) < 0) {
    perror("  CS_QUEUE_GROUP_CREATE_1_6");
    return 1;
  }
  printf("  group_handle=%u group_uid=%u\n", create.out.group_handle,
         create.out.group_uid);

  struct kbase_bo *queue_bo = kbase_bo_create(fd, PAGE_SIZE);
  if (!queue_bo)
    return 1;
  uint64_t queue_gpu_va = (uint64_t)(uintptr_t)queue_bo->cpu;

  // --- 3. a CS that signals the slot -------------------------------------
  printf("\n=== encoding the CS ===\n");
  struct cs_buffer root_buffer = {
      .cpu = queue_bo->cpu,
      .gpu = queue_gpu_va,
      .capacity = (uint32_t)(queue_bo->size / sizeof(uint64_t)),
  };
  struct cs_builder_conf conf = {
      .nr_registers = 96,
      .nr_kernel_registers = 4,
      .alloc_buffer = noop_alloc,
      .cookie = NULL,
  };
  struct cs_builder b;
  cs_builder_init(&b, &conf, root_buffer);

  // Address and value have to go through CS registers - SYNC_SET64 takes
  // register indices, not immediates. Panfork does the same thing with its
  // 0x48/0x4a pair (pan_cmdstream.c:3094).
  struct cs_index addr_reg = cs_reg64(&b, 0);
  struct cs_index val_reg = cs_reg64(&b, 2);
  cs_move64_to(&b, addr_reg, event_gpu_va);
  cs_move64_to(&b, val_reg, EVENT_SIGNAL);

  // System scope: the write must be visible outside the CSG, i.e. to the
  // CPU. CSG scope would keep it inside the group.
  cs_sync64_set(&b, false, MALI_CS_SYNC_SCOPE_SYSTEM, val_reg, addr_reg,
                cs_now());
  cs_end(&b);

  if (!cs_is_valid(&b)) {
    fprintf(stderr, "  cs_builder reported invalid\n");
    return 1;
  }
  uint32_t cs_size = cs_root_chunk_size(&b);
  printf("  encoded %u bytes (MOVE64 addr, MOVE64 val, SYNC_SET64 system)\n",
         cs_size);
  for (uint32_t i = 0; i < cs_size / 8; i++)
    printf("    [%u] 0x%016llx\n", i,
           (unsigned long long)((uint64_t *)queue_bo->cpu)[i]);

  // --- register / bind ---------------------------------------------------
  struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = queue_gpu_va,
      .buffer_size = queue_bo->size,
      .priority = 0,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
    perror("  CS_QUEUE_REGISTER");
    return 1;
  }

  union kbase_ioctl_cs_queue_bind bind = {0};
  bind.in.buffer_gpu_addr = queue_gpu_va;
  bind.in.group_handle = create.out.group_handle;
  bind.in.csi_index = 0;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
    perror("  CS_QUEUE_BIND");
    return 1;
  }

  size_t map_sz = BASEP_QUEUE_NR_MMAP_USER_PAGES * PAGE_SIZE;
  uint8_t *map = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      (off_t)bind.out.mmap_handle);
  if (map == MAP_FAILED) {
    perror("  CS_QUEUE_BIND mmap");
    return 1;
  }
  uint8_t *input_page = map + CSF_USER_INPUT_PAGE * PAGE_SIZE;
  uint8_t *output_page = map + CSF_USER_OUTPUT_PAGE * PAGE_SIZE;

  // --- submit ------------------------------------------------------------
  printf("\n=== submit ===\n");
  *(volatile uint64_t *)(input_page + CSF_USER_CS_INSERT_LO) = cs_size;
  __sync_synchronize();

  struct kbase_ioctl_cs_queue_kick kick = {.buffer_gpu_addr = queue_gpu_va};
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0)
    perror("  CS_QUEUE_KICK");
  else
    printf("  CS_QUEUE_KICK OK\n");

  // --- 4a. did the stream run, and did the ORIGINAL page change? ---------
  // event_slot points at the original allocation's CPU mapping. The CS
  // wrote to window 1 of the alias, which is a different GPU address
  // entirely - so a change here can only mean the two are the same pages.
  printf("\n=== did a write through window 1 reach the original? ===\n");
  // 1ms granularity over a 2s budget. Coarser polling here would report
  // its own sleep interval as if it were GPU latency.
  bool consumed = false, signalled = false;
  int consumed_us = -1, signalled_us = -1;
  for (int i = 0; i < 2000; i++) {
    uint64_t extract =
        *(volatile uint64_t *)(output_page + CSF_USER_CS_EXTRACT_LO);
    if (!consumed && extract >= cs_size) {
      consumed = true;
      consumed_us = i * 1000;
    }
    if (!signalled && event_slot[0] != EVENT_SEED) {
      signalled = true;
      signalled_us = i * 1000;
    }
    if (consumed && signalled)
      break;
    usleep(1000);
  }

  printf("  CS_EXTRACT: %s%s", consumed ? "advanced" : "never advanced",
         consumed ? "" : " - the stream did not run at all\n");
  if (consumed)
    printf(" within %d-%dms (=%llu, cs_size=%u)\n", consumed_us / 1000,
           consumed_us / 1000 + 1,
           (unsigned long long)*(volatile uint64_t *)(output_page +
                                                     CSF_USER_CS_EXTRACT_LO),
           cs_size);
  printf("  original word 0: seeded %d, now %llu%s\n", EVENT_SEED,
         (unsigned long long)event_slot[0],
         signalled ? "" : "  <- UNCHANGED");
  if (signalled)
    printf("  *** THE ALIAS IS REAL: a GPU write through window 1 landed on "
           "the original's pages, within %d-%dms - this is a "
           "real completion signal ***\n",
           signalled_us / 1000, signalled_us / 1000 + 1);

  // --- summary -----------------------------------------------------------
  printf("\n================================================================\n");
  if (signalled)
    printf("RESULT: MEM_ALIAS genuinely maps one allocation at two GPU\n"
           "        addresses. A GPU write to window 1 was read back through\n"
           "        the original allocation's own CPU mapping, so the two\n"
           "        windows are the same physical pages - which is what\n"
           "        PanVK's render descriptor ringbuf relies on to wrap.\n"
           "        The backend can grow an alias operation on this.\n");
  else if (consumed)
    printf("RESULT: the stream ran (CS_EXTRACT advanced) but the original\n"
           "        page never changed. The two windows are NOT the same\n"
           "        memory, so the ringbuf cannot be built this way - the\n"
           "        wraparound would have to be handled another way, e.g.\n"
           "        bounds-checking in the command stream. Check the alias\n"
           "        flags and stride before concluding that, though.\n");
  else
    printf("RESULT: the stream did not run at all, so this says nothing\n"
           "        about aliasing. Re-check against event_slot_probe -\n"
           "        a kick only lands on an idle CS, see kbase-notes.md.\n");
  printf("================================================================\n");

  munmap(map, map_sz);
  struct kbase_ioctl_cs_queue_terminate q_term = {.buffer_gpu_addr =
                                                      queue_gpu_va};
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &q_term) < 0)
    perror("  CS_QUEUE_TERMINATE");
  struct kbase_ioctl_cs_queue_group_term term = {.group_handle =
                                                     create.out.group_handle};
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term) < 0)
    perror("  CS_QUEUE_GROUP_TERMINATE");
  kbase_bo_free(fd, queue_bo);
  kbase_bo_free(fd, event_bo);
  close(fd);
  return signalled ? 0 : 1;
}
