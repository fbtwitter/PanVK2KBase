#!/usr/bin/env python3
"""Stop panthor_kmod_get_csif_props() returning garbage on kbase devices.

PanVK reads CSF interface geometry - register counts, CSG/CS slot counts -
through panthor_kmod_get_csif_props(), in six places:

  panvk_vX_device.c, csf/panvk_vX_exception_handler.c,
  csf/panvk_vX_gpu_queue.c, csf/panvk_vX_cmd_buffer.c,
  csf/panvk_vX_cmd_draw.c, csf/panvk_vX_utrace.c

The accessor does container_of(dev, struct panthor_kmod_dev, base) with no
check of which backend the device belongs to. On a kbase device that reads
whatever memory follows struct kbase_kmod_dev, so every field is garbage.
A garbage cs_reg_count goes straight into cs_builder_conf.nr_registers,
and cs_builder then writes off the end of its buffer - which is what
segfaulted vkCreateDevice inside generate_tiler_oom_handler.

Patching the single accessor to dispatch fixes all six callers without
touching any of them.

Applied as a script rather than a diff because upstream moves. Idempotent.

Usage: patch-panthor-csif-dispatch.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"
SRC = os.path.join(mesa, "src/panfrost/lib/kmod/panthor_kmod.c")

src = open(SRC).read()

if "pan_kmod_kbase_get_csif_props" in src:
    print("    panthor_kmod.c: already patched")
    sys.exit(0)

anchor = """panthor_kmod_get_csif_props(const struct pan_kmod_dev *dev)
{
   struct panthor_kmod_dev *panthor_dev =
      container_of(dev, struct panthor_kmod_dev, base);

   return &panthor_dev->props.csif;
}"""

assert anchor in src, "panthor_kmod_get_csif_props body not found - upstream moved"

replacement = """panthor_kmod_get_csif_props(const struct pan_kmod_dev *dev)
{
   /* This is called on whatever pan_kmod_dev PanVK has, including kbase
    * ones, and the container_of() below is only valid for panthor. Without
    * this check a kbase device yields garbage geometry - in particular a
    * garbage cs_reg_count, which cs_builder turns into an out-of-bounds
    * write. See pan_kmod_kbase_get_csif_props().
    */
   if (dev->ops == &kbase_kmod_ops)
      return pan_kmod_kbase_get_csif_props(dev);

   struct panthor_kmod_dev *panthor_dev =
      container_of(dev, struct panthor_kmod_dev, base);

   return &panthor_dev->props.csif;
}"""

src = src.replace(anchor, replacement, 1)

# The kbase backend's header, for both kbase_kmod_ops and the accessor.
inc_anchor = '#include "pan_kmod_backend.h"'
assert inc_anchor in src, "pan_kmod_backend.h include not found"
src = src.replace(inc_anchor, inc_anchor + '\n#include "pan_kmod_kbase.h"', 1)

open(SRC, "w").write(src)
print("    patched panthor_kmod.c (csif dispatch)")
print("panthor csif dispatch patch applied")
