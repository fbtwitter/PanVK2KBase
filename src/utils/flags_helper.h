#ifndef FLAGS_HELPER_H
#define FLAGS_HELPER_H

#include "mali_base_common_kernel.h"
#include "mali_base_kernel.h"
#include <stdint.h>
#include <stdio.h>

/* Every BASE_MEM_* bit in the vendored header set, not a subset. An earlier
 * version of this table carried 7 of them and a "add the rest from your
 * header" note; that is fine when you are eyeballing an allocation you
 * already understand, and not fine when the whole question is "what did the
 * kernel just hand back?" - an undecoded bit reads as absent.
 *
 * static because this lives in a header: two translation units including it
 * would otherwise collide at link time.
 */
static const struct {
  uint64_t flag;
  const char *name;
} flags[] = {
    {BASE_MEM_PROT_CPU_RD, "CPU_RD"},
    {BASE_MEM_PROT_CPU_WR, "CPU_WR"},
    {BASE_MEM_PROT_GPU_RD, "GPU_RD"},
    {BASE_MEM_PROT_GPU_WR, "GPU_WR"},
    {BASE_MEM_PROT_GPU_EX, "GPU_EX"},
    {BASE_MEM_GPU_VA_SAME_4GB_PAGE, "SAME_4GB_PAGE"},
    {BASE_MEM_GROW_ON_GPF, "GROW_ON_GPF"},
    {BASE_MEM_COHERENT_SYSTEM, "COHERENT_SYSTEM"},
    {BASE_MEM_COHERENT_LOCAL, "COHERENT_LOCAL"},
    {BASE_MEM_CACHED_CPU, "CACHED_CPU"},
    {BASE_MEM_SAME_VA, "SAME_VA"},
    {BASE_MEM_NEED_MMAP, "NEED_MMAP"},
    {BASE_MEM_COHERENT_SYSTEM_REQUIRED, "COHERENT_SYSTEM_REQUIRED"},
    {BASE_MEM_PROTECTED, "PROTECTED"},
    {BASE_MEM_DONT_NEED, "DONT_NEED"},
    {BASE_MEM_IMPORT_SHARED, "IMPORT_SHARED"},
    {BASE_MEM_UNCACHED_GPU, "UNCACHED_GPU"},
    {BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP, "IMPORT_SYNC_ON_MAP_UNMAP"},
    {BASE_MEM_KERNEL_SYNC, "KERNEL_SYNC"},
#ifdef BASE_MEM_FIXED
    {BASE_MEM_FIXED, "FIXED"},
#endif
#ifdef BASE_MEM_FIXABLE
    {BASE_MEM_FIXABLE, "FIXABLE"},
#endif
};

static void decode_mem_alloc_output_flags(unsigned long long flags_to_decode) {
  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
    if (flags_to_decode & flags[i].flag)
      printf("%s\n", flags[i].name);
}

/* Same table, formatted onto one line, plus the group_id nibble and any bit
 * this table does not know about. The unknown-bit report is the point: a
 * kernel that sets something we have no name for should say so loudly rather
 * than silently vanish from the decode.
 */
static void format_mem_flags(uint64_t f, char *buf, size_t buflen) {
  size_t n = 0;
  uint64_t known = 0;

  if (!buflen)
    return;
  buf[0] = '\0';

  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
    known |= flags[i].flag;
    if (f & flags[i].flag) {
      int w = snprintf(buf + n, buflen - n, "%s%s", n ? " " : "", flags[i].name);
      if (w < 0 || (size_t)w >= buflen - n)
        return;
      n += (size_t)w;
    }
  }

  unsigned gid =
      (unsigned)((f & BASE_MEM_GROUP_ID_MASK) >> BASEP_MEM_GROUP_ID_SHIFT);
  if (gid) {
    int w = snprintf(buf + n, buflen - n, "%sgroup_id=%u", n ? " " : "", gid);
    if (w < 0 || (size_t)w >= buflen - n)
      return;
    n += (size_t)w;
  }

  uint64_t unknown = f & ~(known | BASE_MEM_GROUP_ID_MASK);
  if (unknown)
    snprintf(buf + n, buflen - n, "%sUNKNOWN(0x%llx)", n ? " " : "",
             (unsigned long long)unknown);
}

#endif
