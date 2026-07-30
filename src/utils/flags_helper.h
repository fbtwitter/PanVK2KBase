#ifndef FLAGS_HELPER_H
#define FLAGS_HELPER_H

#include "mali_base_common_kernel.h"
#include "mali_base_kernel.h"
#include <stdint.h>
#include <stdio.h>

struct {
  uint64_t flag;
  const char *name;
} flags[] = {
    {BASE_MEM_PROT_CPU_RD, "CPU_RD"},
    {BASE_MEM_PROT_CPU_WR, "CPU_WR"},
    {BASE_MEM_PROT_GPU_RD, "GPU_RD"},
    {BASE_MEM_PROT_GPU_WR, "GPU_WR"},
    {BASE_MEM_SAME_VA, "SAME_VA"},
    {BASE_MEM_CACHED_CPU, "CACHED_CPU"},
    {BASE_MEM_COHERENT_SYSTEM, "COHERENT_SYSTEM"},
    /* add the rest from your header */
};

void decode_mem_alloc_output_flags(unsigned long long flags_to_decode) {
  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
    if (flags_to_decode & flags[i].flag)
      printf("%s\n", flags[i].name);
}

#endif
