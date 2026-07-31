// Can a CS pick up ring appends made while it is still executing?
//
// The driver currently serialises every submission: submit_stream() waits for
// CS_ACTIVE to clear before each kick, because a kick issued while CS_ACTIVE
// is 1 was measured not to take effect (see "A kick only lands on an idle CS"
// in docs/kbase-notes.md). That costs the whole point of a ring buffer, and
// it is now load-bearing for correctness too: it is what makes GPU-side waits
// unsafe, since a stream parked in SYNC_WAIT64 would hold CS_ACTIVE and the
// next submit would kick anyway after its timeout and overwrite a ring the
// GPU is still reading.
//
// But the measurement behind that rule never separated two very different
// states, because in the trace that produced it they were never apart:
//
//   (a) CS_ACTIVE=1 and extract == insert - the stream has finished and the
//       flag is merely lingering (~30-40ms). Firmware is not reading the
//       ring, so nothing will notice new bytes on its own.
//   (b) CS_ACTIVE=1 and extract < insert - the stream is genuinely mid
//       execution. Firmware *is* walking the ring, and CS_INSERT exists
//       precisely so it can be moved while that happens.
//
// The known-bad kick was case (a): that trace shows extract=24 at a kick with
// insert=48, i.e. caught up to the previous insert. Case (b) has never been
// tested, and it is the one that decides whether pipelining is possible here.
//
// So this probe builds a deliberately slow stream (a long run of synchronous
// cache flushes - real work, self-terminating, no scoreboard parking and no
// way to hang), starts it, and then appends more work at a chosen moment:
//
//   1. baseline        - kick an idle CS, and time the slow stream.
//   2. mid-execution, no kick   - append during (b), move CS_INSERT, and
//                                 deliberately do NOT kick. If firmware
//                                 re-reads CS_INSERT, this alone is enough.
//   3. mid-execution, with kick - same, but kick as well.
//   4. lingering-active         - append during (a), the known-bad window,
//                                 and see whether repeating the kick lands it.
//
// Nothing here can park a CS indefinitely: every stream is a finite run of
// flushes that completes on its own.
//
// Usage: kick_pipeline_probe [flushes-in-the-slow-stream]
#include "csf/mali_kbase_csf_ioctl.h"
#include "csf_user_regs.h"
#include "genxml/cs_builder.h"
#include "initialize.h"
#include "memory.h"
#include "parse_gpu_props.h"
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

/* 64KB, same as PANVK_KBASE_RINGBUF_SIZE, so ring behaviour here matches the
 * driver's rather than being a special case.
 */
#define RING_SIZE 65536

/* The slow stream's default length, in flush pairs. Tuned on a Mali-G720 so
 * one run lands in the tens of milliseconds: long enough that the CPU can
 * reliably catch it mid-execution, short enough that the probe stays quick.
 * Overridable from the command line since this is the one number that is
 * device-specific.
 */
#define DEFAULT_FLUSHES 600

#define ARRAY_SIZE_LOCAL(a) (sizeof(a) / sizeof((a)[0]))

/* How many times to repeat the linger-window test. One run proves nothing
 * about a timing-dependent window; the original rule this probe is
 * re-examining came from a three-kick trace.
 */
#define LINGER_TRIALS 10

static int failures;

static void
check(bool ok, const char *what)
{
   printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

static uint64_t
now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct queue {
   int fd;
   uint8_t group_handle;
   struct kbase_bo *ring_bo;
   uint64_t ring_gpu_va;
   void *user_io;
   volatile uint8_t *input;
   volatile uint8_t *output;
   uint64_t insert; /* bytes handed to the ring so far */
};

static uint64_t
q_extract(const struct queue *q)
{
   return *(volatile uint64_t *)(q->output + CSF_USER_CS_EXTRACT_LO);
}

static uint32_t
q_active(const struct queue *q)
{
   return *(volatile uint32_t *)(q->output + CSF_USER_CS_ACTIVE);
}

/* Copy a built stream into the ring at the current insert point, wrapping.
 * Does not move CS_INSERT - publishing is separate so the scenarios below can
 * control exactly when firmware is allowed to see the new bytes.
 */
static void
q_append(struct queue *q, const void *stream, uint32_t size)
{
   uint64_t offset = q->insert % RING_SIZE;
   uint32_t first = size < RING_SIZE - offset ? size : (uint32_t)(RING_SIZE - offset);

   memcpy((uint8_t *)q->ring_bo->cpu + offset, stream, first);
   if (first < size)
      memcpy(q->ring_bo->cpu, (const uint8_t *)stream + first, size - first);

   q->insert += size;
}

static void
q_publish(struct queue *q)
{
   *(volatile uint64_t *)(q->input + CSF_USER_CS_INSERT_LO) = q->insert;
   __sync_synchronize();
}

static bool
q_kick(struct queue *q)
{
   struct kbase_ioctl_cs_queue_kick kick = { .buffer_gpu_addr = q->ring_gpu_va };

   if (ioctl(q->fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) < 0) {
      perror("  CS_QUEUE_KICK");
      return false;
   }
   return true;
}

/* Spin until extract reaches `target`, or until `timeout_ms` elapses.
 * Returns elapsed nanoseconds, or 0 on timeout.
 */
static uint64_t
q_wait_extract(const struct queue *q, uint64_t target, unsigned timeout_ms)
{
   uint64_t start = now_ns();
   uint64_t deadline = start + (uint64_t)timeout_ms * 1000000ull;

   while (now_ns() < deadline) {
      if (q_extract(q) >= target)
         return now_ns() - start;
      usleep(200);
   }
   return 0;
}

static struct cs_buffer
noop_alloc(void *cookie)
{
   (void)cookie;
   fprintf(stderr, "cs_builder overflowed its staging buffer\n");
   abort();
}

/* A stream of `n` synchronous cache flushes.
 *
 * Chosen because it is slow for a reason the GPU cannot optimise away - each
 * one is real L2/LSC maintenance - while still being an ordinary instruction
 * that always completes. A scoreboard wait would be a simpler way to hold a
 * CS busy, but it holds it busy by *parking* it, and a parked CS that never
 * gets released is exactly the hang this repo has been careful to avoid.
 */
static uint32_t
build_slow_stream(void *dst, size_t capacity_bytes, unsigned n)
{
   struct cs_buffer buf = {
      .cpu = dst,
      .gpu = 0, /* nothing in this stream refers to its own address */
      .capacity = capacity_bytes / sizeof(uint64_t),
   };
   struct cs_builder_conf conf = {
      .nr_registers = 96,
      .nr_kernel_registers = 4,
      .alloc_buffer = noop_alloc,
      .cookie = NULL,
   };
   struct cs_builder b;
   cs_builder_init(&b, &conf, buf);

   struct cs_index flush_id = cs_reg32(&b, 0);

   for (unsigned i = 0; i < n; i++) {
      /* latest_flush_id 0 means "never already flushed", so the hardware
       * cannot skip the maintenance. That is the opposite of what a real
       * driver wants and exactly what this probe wants.
       *
       * FLUSH_CACHE2 is a pure async instruction - cs_now() asserts - so it
       * signals scoreboard slot 0 and the stream immediately waits on it.
       * Waiting is the point: it makes each flush serialise against the next,
       * so the stream's duration is n flushes rather than one flush and a
       * queue of overlapping requests. Slot 0 is what PanVK itself uses for
       * immediate flushes (PANVK_SB_IMM_FLUSH), just spelled literally here
       * since this probe does not include the driver's headers.
       */
      cs_move32_to(&b, flush_id, 0);
      cs_flush_caches(&b, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                      MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                      MALI_CS_OTHER_FLUSH_MODE_INVALIDATE, flush_id,
                      cs_defer(0, 0));
      cs_wait_slot(&b, 0);
   }

   cs_end(&b);

   if (!cs_is_valid(&b)) {
      fprintf(stderr, "cs_builder reported invalid slow stream\n");
      return 0;
   }
   return cs_root_chunk_size(&b);
}

/* A short stream whose only job is to be consumed, so that CS_EXTRACT
 * reaching the end of it proves firmware went past the previous work.
 */
static uint32_t
build_marker_stream(void *dst, size_t capacity_bytes)
{
   return build_slow_stream(dst, capacity_bytes, 2);
}

static bool
queue_setup(struct queue *q, int fd, uint64_t shader_present)
{
   q->fd = fd;

   union kbase_ioctl_cs_queue_group_create_1_6 create = { 0 };
   create.in.compute_mask = shader_present;
   create.in.cs_min = 1;
   create.in.priority = 0;
   create.in.compute_max = 1;

   if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &create) < 0) {
      perror("  CS_QUEUE_GROUP_CREATE_1_6");
      return false;
   }
   q->group_handle = create.out.group_handle;

   q->ring_bo = kbase_bo_create(fd, RING_SIZE);
   if (!q->ring_bo)
      return false;

   /* SAME_VA: the CPU pointer is the GPU address. bo->gpu_va is a cookie
    * here, not an address - see the SAME_VA section in docs/kbase-notes.md.
    */
   q->ring_gpu_va = (uint64_t)(uintptr_t)q->ring_bo->cpu;

   struct kbase_ioctl_cs_queue_register reg = {
      .buffer_gpu_addr = q->ring_gpu_va,
      .buffer_size = RING_SIZE,
      .priority = 0,
   };
   if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) < 0) {
      perror("  CS_QUEUE_REGISTER");
      return false;
   }

   union kbase_ioctl_cs_queue_bind bind = { 0 };
   bind.in.buffer_gpu_addr = q->ring_gpu_va;
   bind.in.group_handle = q->group_handle;
   bind.in.csi_index = 0;
   if (ioctl(fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind) < 0) {
      perror("  CS_QUEUE_BIND");
      return false;
   }

   q->user_io = mmap(NULL, BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     (off_t)bind.out.mmap_handle);
   if (q->user_io == MAP_FAILED) {
      perror("  CS_QUEUE_BIND mmap");
      return false;
   }

   /* [doorbell][input][output], measured by tests/user_io_probe. */
   q->input = (volatile uint8_t *)q->user_io + CSF_USER_INPUT_PAGE * 4096;
   q->output = (volatile uint8_t *)q->user_io + CSF_USER_OUTPUT_PAGE * 4096;

   return true;
}

static void
queue_teardown(struct queue *q)
{
   if (q->user_io && q->user_io != MAP_FAILED)
      munmap(q->user_io, BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096);

   struct kbase_ioctl_cs_queue_terminate qt = {
      .buffer_gpu_addr = q->ring_gpu_va,
   };
   ioctl(q->fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &qt);

   struct kbase_ioctl_cs_queue_group_term gt = {
      .group_handle = q->group_handle,
   };
   ioctl(q->fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &gt);

   if (q->ring_bo)
      kbase_bo_free(q->fd, q->ring_bo);
}

/* Run the slow stream and wait for it to finish, leaving the CS idle-ish.
 * Returns how long it took, or 0 if it never completed.
 */
static uint64_t
run_slow_to_completion(struct queue *q, const void *slow, uint32_t slow_size)
{
   q_append(q, slow, slow_size);
   q_publish(q);
   if (!q_kick(q))
      return 0;

   return q_wait_extract(q, q->insert, 5000);
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);

   unsigned n_flushes = DEFAULT_FLUSHES;
   if (argc > 1)
      n_flushes = (unsigned)strtoul(argv[1], NULL, 0);

   int fd = open_gpu();

   uint64_t shader_present = kbase_get_shader_present(fd);
   if (!shader_present) {
      fprintf(stderr, "could not read shader-core mask, aborting\n");
      return 1;
   }
   printf("shader_present = 0x%llx (%d cores)\n",
          (unsigned long long)shader_present,
          __builtin_popcountll(shader_present));

   /* One-shot per context, and the tiler heap is what makes a group
    * schedulable at all here - see tests/live_kick_probe.
    */
   struct kbase_ioctl_mem_jit_init jit = {
      .va_pages = 1 << 14,
      .max_allocations = 255,
      .trim_level = 0,
      .group_id = 0,
      .phys_pages = 1 << 14,
   };
   ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &jit);

   union kbase_ioctl_cs_tiler_heap_init heap = { 0 };
   heap.in.chunk_size = 2 * 1024 * 1024;
   heap.in.initial_chunks = 1;
   heap.in.max_chunks = 8;
   heap.in.target_in_flight = 1;
   if (ioctl(fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &heap) < 0)
      printf("note: TILER_HEAP_INIT failed (%s)\n", strerror(errno));

   struct queue q = { 0 };
   if (!queue_setup(&q, fd, shader_present)) {
      fprintf(stderr, "queue setup failed\n");
      return 1;
   }

   /* Staging buffers. The slow stream is 16 bytes per flush; cap generously
    * and bail rather than overflow.
    */
   static uint8_t slow[RING_SIZE / 2];
   static uint8_t marker[256];

   uint32_t slow_size = build_slow_stream(slow, sizeof(slow), n_flushes);
   uint32_t marker_size = build_marker_stream(marker, sizeof(marker));
   if (!slow_size || !marker_size) {
      fprintf(stderr, "failed to build streams\n");
      queue_teardown(&q);
      return 1;
   }
   printf("slow stream: %u flushes, %u bytes; marker: %u bytes\n\n", n_flushes,
          slow_size, marker_size);

   /* ------------------------------------------------------ 1. baseline */
   printf("=== 1. baseline: kick an idle CS ===\n");
   uint64_t elapsed = run_slow_to_completion(&q, slow, slow_size);
   check(elapsed != 0, "slow stream ran to completion on an idle CS");
   if (!elapsed) {
      printf("\n=> nothing else can be measured. extract=%llu insert=%llu "
             "active=%u\n",
             (unsigned long long)q_extract(&q), (unsigned long long)q.insert,
             q_active(&q));
      queue_teardown(&q);
      return 1;
   }
   printf("  slow stream took %.1f ms, CS_ACTIVE now %u\n",
          elapsed / 1000000.0, q_active(&q));

   if (elapsed < 5000000ull) {
      printf("  note: under 5ms is tight for catching mid-execution.\n"
             "        rerun with a larger flush count, e.g. %u\n",
             n_flushes * 4);
   }

   /* ------------------------------------------- 1b. extract trajectory */
   printf("\n=== 1b. is CS_EXTRACT observable mid-flight? ===\n");
   printf("  everything below assumes progress can be watched. if extract\n"
          "  only appears at the end, it reports completion, not progress.\n");
   while (q_active(&q))
      usleep(1000);
   {
      uint64_t start_insert = q.insert;
      q_append(&q, slow, slow_size);
      q_publish(&q);
      q_kick(&q);

      uint64_t samples[64];
      unsigned n_samples = 0;
      uint64_t last = start_insert;
      uint64_t deadline = now_ns() + 5000000000ull;

      while (now_ns() < deadline) {
         uint64_t e = q_extract(&q);
         if (e != last) {
            if (n_samples < ARRAY_SIZE_LOCAL(samples))
               samples[n_samples] = e;
            n_samples++;
            last = e;
         }
         if (e >= q.insert)
            break;
         usleep(200);
      }

      printf("  %u distinct extract values seen between %llu and %llu\n",
             n_samples, (unsigned long long)start_insert,
             (unsigned long long)q.insert);
      unsigned show = n_samples < 8 ? n_samples : 8;
      if (show) {
         printf("  first values:");
         for (unsigned i = 0; i < show; i++)
            printf(" %llu", (unsigned long long)samples[i]);
         printf("\n");
      }

      if (n_samples <= 1) {
         printf("\n  => CS_EXTRACT jumps straight to the end. It is a\n"
                "     completion signal on this device, not a progress one,\n"
                "     so 'mid-execution' cannot be detected by watching it.\n");
      } else {
         printf("\n  => extract advances incrementally; progress is "
                "observable.\n");
      }
      check(true, "extract trajectory sampled");
   }

   /* --------------------------------- 2. append while running, no kick */
   printf("\n=== 2. append while the CS is running, WITHOUT kicking ===\n");
   printf("  the decisive one. CS_EXTRACT cannot show progress here, so this\n"
          "  is measured by total elapsed time instead:\n");
   printf("    ~%.0f ms  => the append was picked up while the first stream\n"
          "               was still running. pipelining works.\n",
          2 * elapsed / 1000000.0);
   printf("    much more => the append had to wait for a fresh wake-up.\n");
   printf("    never     => the append was not picked up at all.\n\n");

   /* Let CS_ACTIVE settle so this starts from the state the driver's submit
    * path sees.
    */
   while (q_active(&q))
      usleep(1000);
   {
      uint64_t t0 = now_ns();

      q_append(&q, slow, slow_size);
      q_publish(&q);
      if (!q_kick(&q)) {
         queue_teardown(&q);
         return 1;
      }

      /* Immediately, well inside the first stream's run: a second stream,
       * published but deliberately not kicked.
       */
      q_append(&q, slow, slow_size);
      q_publish(&q);

      uint64_t target = q.insert;
      uint64_t took = q_wait_extract(&q, target, 10000);

      if (!took) {
         printf("  never consumed. extract=%llu wanted=%llu active=%u\n",
                (unsigned long long)q_extract(&q),
                (unsigned long long)target, q_active(&q));
         check(false, "second stream consumed without its own kick");
         printf("\n  => firmware does not re-read CS_INSERT on its own.\n");
      } else {
         double total_ms = (now_ns() - t0) / 1000000.0;
         double one_ms = elapsed / 1000000.0;
         printf("  both streams done in %.1f ms (one stream is %.1f ms)\n",
                total_ms, one_ms);

         /* Two streams back to back is ~2x one. Anything close to that means
          * no wake-up was needed in between; a scheduler tick would add
          * roughly another 10ms on top.
          */
         bool overlapped = total_ms < 2.0 * one_ms + 5.0;
         check(overlapped, "second stream ran without needing its own kick");
         if (overlapped)
            printf("\n  => firmware re-reads CS_INSERT while running. "
                   "appends to a busy CS need no kick.\n");
         else
            printf("\n  => it ran, but only after an extra delay of ~%.0f ms "
                   "- it needed waking.\n", total_ms - 2.0 * one_ms);
      }
   }

   /* ------------------------------------- 3. append to an idle CS, no kick */
   printf("\n=== 3. append to an IDLE CS, without kicking ===\n");
   printf("  the control for scenario 2: if this is also consumed, then the\n"
          "  result above says nothing about running-vs-idle.\n");
   while (q_active(&q))
      usleep(1000);
   {
      uint64_t target = q.insert + slow_size;
      q_append(&q, slow, slow_size);
      q_publish(&q);
      /* No kick. */

      uint64_t took = q_wait_extract(&q, target, 3000);
      if (took)
         printf("  consumed after %.1f ms with no kick\n", took / 1000000.0);
      else
         printf("  not consumed within 3s, as expected for an idle CS\n");

      check(took == 0, "an idle CS does NOT pick up an unkicked append");

      if (took == 0) {
         /* Leave the ring consistent for teardown. */
         q_kick(&q);
         q_wait_extract(&q, target, 5000);
      }
   }

   /* ----------------------------------- 4. the known-bad linger window */
   printf("\n=== 4. append during the lingering-active window (%d trials) ===\n",
          LINGER_TRIALS);
   printf("  stream finished (extract == insert) but CS_ACTIVE still 1.\n");
   printf("  this is the exact state the 'kick only lands on an idle CS'\n");
   printf("  rule was measured in, so it is the rule being re-tested.\n\n");
   {
      unsigned in_window = 0, landed_first_kick = 0, needed_retry = 0, lost = 0;

      for (unsigned t = 0; t < LINGER_TRIALS; t++) {
         while (q_active(&q))
            usleep(1000);

         q_append(&q, slow, slow_size);
         q_publish(&q);
         q_kick(&q);
         if (!q_wait_extract(&q, q.insert, 5000)) {
            printf("  trial %2u: setup stream never completed - stopping\n", t);
            lost++;
            break;
         }

         if (!q_active(&q)) {
            printf("  trial %2u: CS_ACTIVE already clear, not in the window\n",
                   t);
            continue;
         }
         in_window++;

         uint64_t target = q.insert + marker_size;
         q_append(&q, marker, marker_size);
         q_publish(&q);
         q_kick(&q);

         uint64_t took = q_wait_extract(&q, target, 200);
         if (took) {
            printf("  trial %2u: in window, single kick consumed after "
                   "%.1f ms\n", t, took / 1000000.0);
            landed_first_kick++;
            continue;
         }

         /* Did not land. Retry, which is what a retrying submit path would
          * rely on, and record how long recovery took.
          */
         unsigned retries = 0;
         for (; retries < 50; retries++) {
            usleep(10000);
            q_kick(&q);
            if (q_extract(&q) >= target)
               break;
         }
         if (retries < 50) {
            printf("  trial %2u: in window, DROPPED; recovered after %u "
                   "retry kick(s)\n", t, retries + 1);
            needed_retry++;
         } else {
            printf("  trial %2u: in window, DROPPED and never recovered\n", t);
            lost++;
         }
      }

      printf("\n  %u/%d trials landed in the linger window\n", in_window,
             LINGER_TRIALS);
      printf("  of those: %u consumed on the first kick, %u needed a retry, "
             "%u lost\n", landed_first_kick, needed_retry, lost);

      if (in_window == 0) {
         printf("\n  => the window was never hit, so this run says nothing "
                "about it.\n");
      } else if (lost == 0 && needed_retry == 0) {
         printf("\n  => every kick in the window landed. The 'kick only lands\n"
                "     on an idle CS' rule does not reproduce here.\n");
      } else if (lost == 0) {
         printf("\n  => kicks are sometimes dropped, but retrying always\n"
                "     recovered them. A retrying submit path is sufficient.\n");
      } else {
         printf("\n  => kicks are dropped and retrying does not always "
                "recover.\n");
      }

      check(lost == 0, "no work was permanently lost in the linger window");
   }

   queue_teardown(&q);

   printf("\n=== %d failure(s) ===\n", failures);
   return failures ? 1 : 0;
}
