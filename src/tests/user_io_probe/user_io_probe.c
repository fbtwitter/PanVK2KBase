// Settles which of the three pages mmap'd from CS_QUEUE_BIND's
// mmap_handle is the CS_USER_INPUT block, which is CS_USER_OUTPUT, and
// which is the HW doorbell.
//
// Why this exists: utils/csf_user_regs.h assumes [input][output][doorbell]
// (read out of a real r49p1 init_user_io_pages(), and consistent with the
// uapi comment at csf/mali_base_csf_kernel.h:117, "A pair of input/output
// pages and a Hw doorbell page"). Panfork - a Panfrost driver that
// demonstrably ran on real kbase/CSF hardware - assumes
// [doorbell][input][output]:
//
//   pan_vX_base.c:1434
//     #define CS_RING_DOORBELL(cs)        *((uint32_t *)(cs->user_io)) = 1
//     #define CS_READ_REGISTER(cs, r)     *((uint64_t *)(cs->user_io + 4096 * 2 + r))
//     #define CS_WRITE_REGISTER(cs, r, v) *((uint64_t *)(cs->user_io + 4096 + r)) = v
//
// with cs->user_io being the raw mmap base, exactly like queue_state in
// tests/live_kick_probe. The two disagree by one page. If Panfork is
// right, live_kick_probe has been writing CS_INSERT into the doorbell
// page and polling CS_EXTRACT/CS_ACTIVE out of the input page - which the
// kernel zeroes at bind and firmware never writes. That would produce
// CS_EXTRACT=0/CS_ACTIVE=0 forever with no hang and a healthy device,
// which is exactly the result recorded in docs/kbase-notes.md, and would
// mean the "group never reached a CSG slot" conclusion rests on readings
// of dead pages.
//
// This probe does not assume either layout. It runs three independent
// tests and reports raw observations:
//
//   Test A (bind-time state)  - dump all three pages straight after BIND,
//                               before writing anything. The kernel zeroes
//                               the input and output blocks at bind; a page
//                               that comes up non-zero is not one of them.
//   Test B (who moves)        - snapshot all 12KB, write CS_INSERT at one
//                               candidate page, KICK, then diff the full
//                               12KB. Whichever page the kernel or firmware
//                               writes identifies itself. Run once per
//                               candidate input page, each with a fresh
//                               group/queue, since these ioctls are
//                               one-shot per queue.
//   Test C (RAM vs MMIO)      - last, so it cannot perturb A or B: write a
//                               pattern into each page and read it back.
//                               Normal memory reads back what was written;
//                               a HW doorbell mapping will not.
//
// Everything else (vendor-style context setup, tiler heap, endpoint masks,
// group create) is kept identical to tests/live_kick_probe so the
// conditions match the run this is re-examining.
#include "csf/mali_kbase_csf_ioctl.h"
#include "csf_user_regs.h"
#include "dump_hex.h"
#include "genxml/cs_builder.h"
#include "initialize.h"
#include "memory.h"
#include "parse_gpu_props.h"
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SZ 4096
#define NR_PAGES BASEP_QUEUE_NR_MMAP_USER_PAGES
#define MAP_SZ (NR_PAGES * PAGE_SZ)

// Offset used by Test C. Well clear of the register blocks, which live in
// the first 0x10 bytes of the input and output pages, so a stray write
// here cannot corrupt CS_INSERT/CS_EXTRACT. It is still a write into a
// possible MMIO page, which is why Test C runs last.
#define PROBE_OFF 0x800
#define PROBE_PATTERN 0xA5A5C3C35A5A3C3CULL

static struct cs_buffer noop_alloc(void *cookie) {
  (void)cookie;
  fprintf(stderr, "alloc_buffer called - 1-instruction test overflowed?\n");
  abort();
}

// Reports every 8-byte word that differs between two 12KB snapshots,
// attributed to a page and an offset within it. This is the actual
// measurement: the page that changes is the page the hardware owns.
static int diff_pages(const uint8_t *before, const uint8_t *after,
                      const char *label) {
  int total = 0;
  for (size_t p = 0; p < NR_PAGES; p++) {
    int page_changes = 0;
    for (size_t off = 0; off + 8 <= PAGE_SZ; off += 8) {
      size_t i = p * PAGE_SZ + off;
      uint64_t b, a;
      memcpy(&b, before + i, 8);
      memcpy(&a, after + i, 8);
      if (b == a)
        continue;
      if (page_changes == 0)
        printf("  [%s] page %zu CHANGED:\n", label, p);
      if (page_changes < 8)
        printf("      +0x%04zx: 0x%016llx -> 0x%016llx\n", off,
               (unsigned long long)b, (unsigned long long)a);
      page_changes++;
    }
    if (page_changes > 8)
      printf("      ... and %d more changed words on this page\n",
             page_changes - 8);
    if (page_changes == 0)
      printf("  [%s] page %zu: unchanged\n", label, p);
    total += page_changes;
  }
  return total;
}

static void dump_all_pages(const uint8_t *base, const char *label) {
  for (size_t p = 0; p < NR_PAGES; p++) {
    printf("  [%s] page %zu, first 64 bytes:\n", label, p);
    dump_hex(base + p * PAGE_SZ, 64);
  }
}

/* Identical to tests/live_kick_probe's vendor_context_setup(). One-shot
   per context. */
static void vendor_context_setup(int fd) {
  printf("\n=== vendor-style context setup ===\n");

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

  struct kbase_ioctl_mem_exec_init exec = {
      .va_pages = 1 << 16,
  };
  if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec) < 0)
    printf("  MEM_EXEC_INIT  : FAILED (%s)\n", strerror(errno));
  else
    printf("  MEM_EXEC_INIT  : OK\n");

  union kbase_ioctl_cs_tiler_heap_init heap = {0};
  heap.in.chunk_size = 2 * 1024 * 1024;
  heap.in.initial_chunks = 1;
  heap.in.max_chunks = 8;
  heap.in.target_in_flight = 1;
  heap.in.group_id = 0;
  heap.in.buf_desc_va = 0;
  if (ioctl(fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &heap) < 0)
    printf("  TILER_HEAP_INIT: FAILED (%s)\n", strerror(errno));
  else
    printf("  TILER_HEAP_INIT: OK gpu_heap_va=0x%llx\n",
           (unsigned long long)heap.out.gpu_heap_va);
}

// One full cycle with CS_INSERT written at insert_page. Returns the number
// of bytes that changed anywhere in the 12KB mapping as a result.
static int run_trial(int fd, uint64_t shader_present, unsigned insert_page,
                     bool first_trial, bool run_test_c) {
  printf("\n================================================================\n");
  printf("=== TRIAL: CS_INSERT written to page %u (%s layout)\n", insert_page,
         insert_page == 0 ? "csf_user_regs.h / this repo" : "Panfork");
  printf("================================================================\n");

  // _1_6 (nr 42) is what the vendor blob uses exclusively - see
  // docs/kbase-notes.md's RE section. Group-create variant is irrelevant to
  // page layout, but keeping it blob-identical removes a variable.
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
    printf("  CS_QUEUE_GROUP_CREATE_1_6 failed: %s\n", strerror(errno));
    return -1;
  }
  printf("  group_handle=%u group_uid=%u\n", create.out.group_handle,
         create.out.group_uid);

  struct kbase_bo *queue_bo = kbase_bo_create(fd, PAGE_SZ);
  if (!queue_bo)
    return -1;

  // SAME_VA: the CPU pointer is the GPU VA. bo->gpu_va is a reusable
  // cookie, not an address - see docs/kbase-notes.md.
  uint64_t queue_gpu_va = (uint64_t)(uintptr_t)queue_bo->cpu;

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
  cs_move32_to(&b, cs_reg32(&b, 0), 0x1234);
  cs_end(&b);

  if (!cs_is_valid(&b)) {
    fprintf(stderr, "  cs_builder reported invalid\n");
    return -1;
  }
  uint32_t cs_size = cs_root_chunk_size(&b);
  printf("  encoded %u bytes of CS\n", cs_size);

  struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = queue_gpu_va,
      .buffer_size = queue_bo->size,
      .priority = 0,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
    perror("  CS_QUEUE_REGISTER");
    return -1;
  }

  union kbase_ioctl_cs_queue_bind bind = {0};
  bind.in.buffer_gpu_addr = queue_gpu_va;
  bind.in.group_handle = create.out.group_handle;
  bind.in.csi_index = 0;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
    perror("  CS_QUEUE_BIND");
    return -1;
  }

  uint8_t *map = mmap(NULL, MAP_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      (off_t)bind.out.mmap_handle);
  if (map == MAP_FAILED) {
    perror("  CS_QUEUE_BIND mmap");
    return -1;
  }
  printf("  mapped %d pages at %p\n", (int)NR_PAGES, (void *)map);

  // --- Test A: state straight after BIND, before we touch anything ---
  printf("\n--- Test A: page contents immediately after BIND ---\n");
  if (first_trial) {
    dump_all_pages(map, "post-bind");
  } else {
    printf("  (full dump printed in the first trial; showing summary only)\n");
  }
  for (size_t p = 0; p < NR_PAGES; p++) {
    bool all_zero = true;
    for (size_t i = 0; i < PAGE_SZ; i++) {
      if (map[p * PAGE_SZ + i] != 0) {
        all_zero = false;
        break;
      }
    }
    printf("  page %zu: %s at bind time\n", p,
           all_zero ? "all zero (consistent with a kernel-zeroed I/O block)"
                    : "NOT all zero");
  }
  if (!first_trial)
    printf("  NOTE: kbase's init_user_io_pages() zeroes the real input and\n"
           "  output blocks on every BIND. If a page still holds the previous\n"
           "  trial's data here, this BIND did not hand out freshly zeroed\n"
           "  per-queue pages - evidence the mapping is a shared dummy/scratch\n"
           "  backing rather than a live CSG slot's user-IO pages.\n");

  // --- Test B: snapshot, poke CS_INSERT, kick, diff ---
  printf("\n--- Test B: write CS_INSERT to page %u, KICK, diff all %d pages "
         "---\n",
         insert_page, (int)NR_PAGES);

  uint8_t *before = malloc(MAP_SZ);
  if (!before) {
    fprintf(stderr, "  malloc failed\n");
    return -1;
  }
  memcpy(before, map, MAP_SZ);

  // CS_INSERT is a byte offset into the ring buffer, not an address. A
  // single 64-bit store is one STR on aarch64, satisfying the kernel's
  // "access atomically" requirement.
  volatile uint64_t *insert_slot =
      (volatile uint64_t *)(map + insert_page * PAGE_SZ + CSF_USER_CS_INSERT_LO);
  *insert_slot = cs_size;
  __sync_synchronize();
  printf("  wrote CS_INSERT=%u at page %u + 0x%02x\n", cs_size, insert_page,
         CSF_USER_CS_INSERT_LO);

  struct kbase_ioctl_cs_queue_kick kick = {0};
  kick.buffer_gpu_addr = queue_gpu_va;
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0)
    perror("  CS_QUEUE_KICK");
  else
    printf("  CS_QUEUE_KICK OK\n");

  // Watch every page that is NOT the one we wrote CS_INSERT into, so this
  // reports what happened rather than what either layout predicts.
  // CS_INSERT and CS_EXTRACT are both at +0x00 of their respective blocks,
  // so polling the insert page would just read our own write back and
  // report a false positive.
  for (int i = 0; i < 40; i++) {
    bool advanced = false;
    for (size_t p = 0; p < NR_PAGES; p++) {
      if (p == insert_page)
        continue;
      uint64_t v =
          *(volatile uint64_t *)(map + p * PAGE_SZ + CSF_USER_CS_EXTRACT_LO);
      uint64_t base;
      memcpy(&base, before + p * PAGE_SZ + CSF_USER_CS_EXTRACT_LO, 8);
      // Compare against the post-BIND baseline, not against zero: a page
      // can carry a stale value from a previous run (page 0 does exactly
      // this), and that is not a write by anyone.
      if (v != base && v >= cs_size) {
        printf("  *** page %zu +0x%02x reached %llu (cs_size=%u) after ~%dms "
               "- something other than us wrote it ***\n",
               p, CSF_USER_CS_EXTRACT_LO, (unsigned long long)v, cs_size,
               i * 50);
        advanced = true;
      }
    }
    if (advanced)
      break;
    usleep(50000);
  }

  printf("  after ~2000ms, reading CS_EXTRACT/CS_ACTIVE at every page:\n");
  for (size_t p = 0; p < NR_PAGES; p++) {
    printf("    page %zu as CS_USER_OUTPUT: CS_EXTRACT=%llu CS_ACTIVE=%u%s\n",
           p,
           (unsigned long long)*(volatile uint64_t *)(map + p * PAGE_SZ +
                                                      CSF_USER_CS_EXTRACT_LO),
           *(volatile uint32_t *)(map + p * PAGE_SZ + CSF_USER_CS_ACTIVE),
           p == insert_page ? "   <- OUR OWN CS_INSERT WRITE, ignore" : "");
  }

  printf("\n  full 12KB diff vs. the post-BIND snapshot:\n");
  int changed = diff_pages(before, map, "post-kick");
  printf("  -> %d changed 8-byte words total\n", changed);
  printf("  NOTE: the word we wrote ourselves (page %u + 0x%02x) will show "
         "up here; anything else is the kernel or firmware writing.\n",
         insert_page, CSF_USER_CS_INSERT_LO);

  // Any CSF notification is itself evidence firmware engaged.
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pret = poll(&pfd, 1, 300);
  if (pret > 0) {
    struct base_csf_notification notif = {0};
    ssize_t r = read(fd, &notif, sizeof(notif));
    printf("  notification: poll ready, read %zd bytes, type=%u\n", r,
           r == (ssize_t)sizeof(notif) ? notif.type : 0);
  } else {
    printf("  notification: none within 300ms\n");
  }

  free(before);

  // --- Test C: RAM vs MMIO readback. Runs only in the final trial, since
  // its pattern writes would otherwise show up as "not zeroed at bind" in
  // the next trial's Test A and mask the real signal there. ---
  if (!run_test_c) {
    printf("\n--- Test C: skipped (runs in the final trial only, so its "
           "writes cannot pollute the next trial's Test A) ---\n");
    goto teardown;
  }
  printf("\n--- Test C: pattern readback at +0x%03x (RAM vs MMIO) ---\n",
         PROBE_OFF);
  for (size_t p = 0; p < NR_PAGES; p++) {
    volatile uint64_t *slot =
        (volatile uint64_t *)(map + p * PAGE_SZ + PROBE_OFF);
    *slot = PROBE_PATTERN;
    __sync_synchronize();
    uint64_t got = *slot;
    printf("  page %zu: wrote 0x%016llx, read 0x%016llx -> %s\n", p,
           (unsigned long long)PROBE_PATTERN, (unsigned long long)got,
           got == PROBE_PATTERN ? "reads back (normal memory)"
                                : "does NOT read back (MMIO-like)");
  }

teardown:
  if (munmap(map, MAP_SZ) < 0)
    perror("  munmap");

  struct kbase_ioctl_cs_queue_terminate q_term = {
      .buffer_gpu_addr = queue_gpu_va,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &q_term) < 0)
    perror("  CS_QUEUE_TERMINATE");

  struct kbase_ioctl_cs_queue_group_term term = {
      .group_handle = create.out.group_handle,
  };
  if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term) < 0)
    perror("  CS_QUEUE_GROUP_TERMINATE");

  kbase_bo_free(fd, queue_bo);
  return changed;
}

int main(void) {
  int fd = open_gpu();

  uint64_t shader_present = kbase_get_shader_present(fd);
  printf("RAW_SHADER_PRESENT = 0x%llx (%d cores)\n",
         (unsigned long long)shader_present,
         __builtin_popcountll(shader_present));
  if (!shader_present) {
    fprintf(stderr, "could not read shader-core mask, aborting\n");
    return 1;
  }

  vendor_context_setup(fd);

  int changed_p0 = run_trial(fd, shader_present, 0, true, false);
  int changed_p1 = run_trial(fd, shader_present, 1, false, true);

  printf("\n================================================================\n");
  printf("=== HOW TO READ THIS\n");
  printf("================================================================\n");
  printf("Test A: the HW doorbell page is the one that is NOT a freshly\n");
  printf("        zeroed block at bind time (if any stands out).\n");
  printf("Test B: the page that changes without us writing it is the one\n");
  printf("        the kernel/firmware owns - that is CS_USER_OUTPUT.\n");
  printf("        CS_INSERT on page 0 changed %d words; on page 1, %d.\n",
         changed_p0, changed_p1);
  printf("        If the page-1 trial moves something the page-0 trial did\n");
  printf("        not, Panfork's layout is right and csf_user_regs.h plus\n");
  printf("        tests/live_kick_probe are off by one page.\n");
  printf("Test C: a page that does not read back what was written to it is\n");
  printf("        an MMIO mapping, i.e. the doorbell.\n");
  printf("\nIf all three tests come out inconclusive (nothing moves in\n");
  printf("either trial, all pages read back, all zero at bind), then the\n");
  printf("page layout is NOT the blocker and the CSG-slot hypothesis in\n");
  printf("docs/kbase-notes.md stands. Record whichever way it goes.\n");

  close(fd);
  return 0;
}
