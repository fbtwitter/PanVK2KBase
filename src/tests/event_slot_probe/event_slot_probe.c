// Gets a real completion signal out of the GPU.
//
// tests/user_io_probe established that the GPU does execute a submitted
// command stream (CS_EXTRACT advances), once CS_INSERT/CS_EXTRACT are read
// from the right pages - see utils/csf_user_regs.h. But a bare MOVE32
// signals nothing, so there is still no way for userspace to learn that
// work finished. That is the remaining piece for Phase 2's non-DRM
// vk_sync and Phase 4's fence shim.
//
// kbase has no fence object to translate. The mechanism, taken from
// Panfork (third_party/PANFORK/src/panfrost/base/pan_vX_base.c:359 and
// :1336 - see docs/kbase-notes.md "Finding 2"), is:
//
//   1. Allocate memory with BASE_MEM_CSF_EVENT. Per the uapi docs this is
//      given an uncached GPU mapping and a permanent kernel mapping, which
//      is what lets the CPU observe firmware's writes to it.
//   2. Seed the slot to a known value. Panfork uses 1, because it waits
//      with the "Higher" condition and wants waiting-before-writing to be
//      legal. The second 64-bit word is an error field, zeroed so faults
//      are not inherited.
//   3. Have the command stream itself write the slot - SYNC_SET64 with
//      system scope, so the write is visible outside the CSG.
//   4. Userspace observes either the value changing, or a
//      base_csf_notification arriving on the kbase fd.
//
// This probe does exactly that and reports which of those two channels
// actually fires. Both matter: the memory write is what a vk_sync would
// poll, the notification is what would let it block instead of spin.
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

// Panfork's PAN_EVENT_SIZE: one 64-bit value word plus one 64-bit error
// word per slot (pan_base.h:31).
#define EVENT_SIZE 16
#define EVENT_SEED 1
#define EVENT_SIGNAL 2

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

  printf("\n=== vendor-style context setup ===\n");
  vendor_context_setup(fd);

  // --- 1. event memory ---------------------------------------------------
  printf("\n=== event memory (BASE_MEM_CSF_EVENT) ===\n");
  struct kbase_bo *event_bo =
      kbase_bo_create_flags(fd, PAGE_SIZE, BASE_MEM_CSF_EVENT);
  if (!event_bo) {
    fprintf(stderr, "event memory allocation FAILED - BASE_MEM_CSF_EVENT "
                    "may not be accepted on this kernel\n");
    return 1;
  }
  // SAME_VA: the CPU pointer is the GPU VA (see docs/kbase-notes.md).
  uint64_t event_gpu_va = (uint64_t)(uintptr_t)event_bo->cpu;
  volatile uint64_t *event_slot = (volatile uint64_t *)event_bo->cpu;

  // --- 2. seed the slot --------------------------------------------------
  event_slot[0] = EVENT_SEED;
  event_slot[1] = 0; /* error word */
  __sync_synchronize();
  printf("  event slot at gpu_va=0x%llx seeded: value=%llu error=%llu\n",
         (unsigned long long)event_gpu_va,
         (unsigned long long)event_slot[0],
         (unsigned long long)event_slot[1]);

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

  // --- 4a. did the stream run, and did the slot change? ------------------
  printf("\n=== channel 1: event memory ===\n");
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
  printf("  event slot: seeded %d, now %llu (error word %llu)%s\n", EVENT_SEED,
         (unsigned long long)event_slot[0], (unsigned long long)event_slot[1],
         signalled ? "" : "  <- UNCHANGED");
  if (signalled)
    printf("  *** GPU SIGNALLED THE EVENT SLOT within %d-%dms - this is a "
           "real completion signal ***\n",
           signalled_us / 1000, signalled_us / 1000 + 1);

  // --- 4b. did a notification arrive? ------------------------------------
  printf("\n=== channel 2: base_csf_notification on the kbase fd ===\n");
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pret = poll(&pfd, 1, 1000);
  if (pret > 0) {
    struct base_csf_notification notif = {0};
    ssize_t r = read(fd, &notif, sizeof(notif));
    if (r == (ssize_t)sizeof(notif))
      printf("  *** NOTIFICATION: type=%u%s ***\n", notif.type,
             notif.type == BASE_CSF_NOTIFICATION_EVENT ? " (EVENT)" : "");
    else
      printf("  poll ready but read returned %zd (errno=%d)\n", r, errno);
  } else if (pret == 0) {
    printf("  no notification within 1000ms - poll() stayed unreadable\n");
  } else {
    perror("  poll");
  }

  // --- summary -----------------------------------------------------------
  printf("\n================================================================\n");
  if (signalled)
    printf("RESULT: kbase event memory works as a completion signal.\n"
           "        A vk_sync can be built on BASE_MEM_CSF_EVENT + SYNC_SET64\n"
           "        without any DRM syncobj. %s\n",
           pret > 0 ? "poll() also works, so it can block rather than spin."
                    : "poll() did not fire, so this would have to spin for\n"
                      "        now - worth chasing the notification path "
                      "separately.");
  else if (consumed)
    printf("RESULT: the stream ran (CS_EXTRACT advanced) but the slot was\n"
           "        never written. The SYNC_SET64 encoding, the scope, or\n"
           "        BASE_MEM_CSF_EVENT's mapping is the thing to look at -\n"
           "        not the submission path, which is proven working.\n");
  else
    printf("RESULT: the stream did not run at all, so this says nothing\n"
           "        about event memory. Re-check against user_io_probe.\n");
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
