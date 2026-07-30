// Queries the CSF firmware's global interface via
// KBASE_IOCTL_CS_GET_GLB_IFACE - read-only, no submission, no device
// state changed.
//
// Motivation: tests/live_kick_probe/live_kick_probe.c submits a real,
// correctly-encoded instruction through the correct CS_INSERT/KICK
// protocol, and the GPU never runs it - the queue group is never placed
// on a CSG slot (see docs/kbase-notes.md). Diagnosing *why* normally
// needs kernel-side visibility (dmesg/debugfs/ktrace), all of which are
// closed on a production `user` build. This ioctl is the one remaining
// unprivileged source of firmware-side facts: how many CSG slots exist,
// how many command streams they have, and what interface version the
// firmware speaks. If group_num or total_stream_num is 0, or the
// interface version is older than what group scheduling needs, that
// explains the failure with no root required.
#include "csf/mali_base_csf_kernel.h"
#include "csf/mali_kbase_csf_ioctl.h"
#include "initialize.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int fd = open_gpu();

  // First call with max_*_num = 0: just read the counts, no buffers.
  union kbase_ioctl_cs_get_glb_iface probe = {0};

  if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &probe) < 0) {
    perror("CS_GET_GLB_IFACE (count probe)");
    return 1;
  }

  uint32_t glb_version = probe.out.glb_version;

  printf("\n== CSF global interface ==\n");
  // glb_version is packed major.minor.patch - same layout kbase's own
  // kbase_csf_interface_version() builds for its >= comparisons.
  printf("glb_version      = 0x%08x (major=%u minor=%u patch=%u)\n",
         glb_version, (glb_version >> 24) & 0xff, (glb_version >> 16) & 0xff,
         glb_version & 0xffff);
  printf("features         = 0x%08x\n", probe.out.features);
  printf("group_num        = %u   <- CSG slots the firmware exposes\n",
         probe.out.group_num);
  printf("total_stream_num = %u   <- command streams across all groups\n",
         probe.out.total_stream_num);
  printf("prfcnt_size      = 0x%08x\n", probe.out.prfcnt_size);
  printf("instr_features   = 0x%08x\n", probe.out.instr_features);

  if (probe.out.group_num == 0) {
    printf("\n=> group_num is 0: firmware exposes no CSG slots to this "
           "context, which would fully explain why a group is never "
           "scheduled.\n");
    return 0;
  }

  // Second call: actually fetch the per-group and per-stream data. The
  // kernel writes an array of struct basep_cs_group_control and
  // basep_cs_stream_control (both in csf/mali_base_csf_kernel.h).
  uint32_t group_num = probe.out.group_num;
  uint32_t stream_num = probe.out.total_stream_num;

  struct basep_cs_group_control *groups =
      calloc(group_num, sizeof(*groups));
  struct basep_cs_stream_control *streams =
      calloc(stream_num, sizeof(*streams));

  if (!groups || !streams) {
    perror("calloc");
    return 1;
  }

  union kbase_ioctl_cs_get_glb_iface fetch = {0};
  fetch.in.max_group_num = group_num;
  fetch.in.max_total_stream_num = stream_num;
  fetch.in.groups_ptr = (uint64_t)(uintptr_t)groups;
  fetch.in.streams_ptr = (uint64_t)(uintptr_t)streams;

  if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &fetch) < 0) {
    perror("CS_GET_GLB_IFACE (fetch)");
    free(groups);
    free(streams);
    return 1;
  }

  printf("\n== per-group capabilities (%u CSG slots) ==\n", group_num);
  for (uint32_t i = 0; i < group_num; i++) {
    printf("  group[%u]: features=0x%08x stream_num=%u suspend_size=%u\n", i,
           groups[i].features, groups[i].stream_num, groups[i].suspend_size);
  }

  // Stream features are identical across streams in practice, so
  // summarise rather than printing 64 identical lines.
  printf("\n== per-stream capabilities (%u streams) ==\n", stream_num);
  uint32_t first = streams[0].features;
  bool uniform = true;
  for (uint32_t i = 1; i < stream_num; i++) {
    if (streams[i].features != first) {
      uniform = false;
      break;
    }
  }
  if (uniform) {
    printf("  all %u streams: features=0x%08x\n", stream_num, first);
  } else {
    for (uint32_t i = 0; i < stream_num; i++)
      printf("  stream[%u]: features=0x%08x\n", i, streams[i].features);
  }

  free(groups);
  free(streams);
  return 0;
}
