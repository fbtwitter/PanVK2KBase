// Offline verification that Mesa's own CS instruction encoder
// (third_party/MESA-KMOD/src/panfrost/genxml/cs_builder.h) produces
// correct bytes for this device's architecture, before wiring anything
// into a live on-device KICK. No kbase/device access at all - pure
// userspace encode + dump.
//
// PAN_ARCH must be supplied at compile time (see the `mesa-cs-pack` and
// `cs_encode_probe` makefile targets) and must match the target GPU's
// architecture major version - this device (Poco X8 Pro, Mali-G720)
// decodes as architecture 12.8 via utils/parse_gpu_props.h, hence v12.
// A different device needs a different PAN_ARCH and a regenerated
// v<N>_pack.h (see docs/mesa-cs-builder.md).
//
// See docs/mesa-cs-builder.md for how to build this and
// docs/kbase-notes.md's "poll()/read() on the kbase fd" section for why
// this exists: confirming the CSF notification channel needs a real
// instruction stream, not just sentinel bytes, and this is step one -
// prove the encoder produces correct, verifiable bytes offline before
// running anything on real GPU firmware.
#ifndef PAN_ARCH
#error "PAN_ARCH must be defined (e.g. -DPAN_ARCH=12) - see docs/mesa-cs-builder.md"
#endif

#include "genxml/cs_builder.h"
#include <stdio.h>
#include <stdlib.h>

static struct cs_buffer noop_alloc(void *cookie) {
  (void)cookie;
  fprintf(stderr,
          "alloc_buffer called - unexpected for a 1-instruction test\n");
  abort();
}

int main(void) {
  size_t capacity = 64;
  uint64_t *mem = calloc(capacity, sizeof(uint64_t));

  struct cs_buffer root_buffer = {
      .cpu = mem,
      .gpu = 0x1000, /* fake GPU VA, just for this offline encode test */
      .capacity = (uint32_t)capacity,
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
    fprintf(stderr, "builder reported invalid\n");
    return 1;
  }

  uint32_t size = cs_root_chunk_size(&b);
  printf("root chunk size = %u bytes (%u instructions)\n", size, size / 8);

  for (uint32_t i = 0; i < size / 8; i++) {
    uint64_t instr = mem[i];
    printf("  [%u] 0x%016llx  opcode(top byte)=0x%02x\n", i,
           (unsigned long long)instr, (unsigned)((instr >> 56) & 0xff));
  }

  free(mem);
  return 0;
}
