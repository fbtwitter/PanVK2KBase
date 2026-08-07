#!/usr/bin/env python3
"""VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess.

Adds command-buffer state, the three Cmd*TransformFeedbackEXT entry points
(csf/panvk_vX_cmd_xfb.c, copied in separately - see mesa-backend-sync), the
two-variant vertex-shader compile (a render variant unchanged, plus a
second monolithic XFB-capture variant compiled with no_idvs=true and
nir_lower_xfb_to_stores applied), and the compute dispatch that launches
the XFB variant on PANVK_SUBQUEUE_COMPUTE, reusing the render VS's
vs_desc_state->res_table for hardware attribute fetch - mirroring
Panfrost GL's csf_launch_xfb in gallium/drivers/panfrost/pan_csf.c as
closely as PanVK's multi-subqueue CSF architecture allows.

Status (2026-08-06, see docs/kbase-notes.md and ROADMAP.md for the full
investigation): compiles clean across v6/v7/v10/v12/v13/v14. Verified on
the real Poco X8 Pro (Mali-G720) through two confirmed, fixed bugs:

  1. bifrost_postprocess_nir() unconditionally requires a non-NULL
     pan_compile_inputs::varying_layout for any MESA_SHADER_VERTEX
     compile - its assert() compiles out in release builds and it then
     memcpy()s from NULL. Fixed by building a (otherwise-unused) trivial
     varying_layout for the XFB variant too.
  2. The compute dispatch populated the "slot 1" CSF registers
     (PANVK_PRECOMP_SRT/FAU/SPD/TSD = MALI_COMPUTE_SR_*_1) but launched
     with cs_shader_res_sel(0, 0, 0, 0) (copied from GL's plain-offset
     convention) instead of PANVK_PRECOMP_RES_SEL (slot 1) - the hardware
     was reading uninitialized slot-0 registers. Fixed.

Both fixes are confirmed via tombstone/behavior on real hardware and are
included below.

A third issue was then found and fixed: flush_tiling() (csf/panvk_vX_cmd_draw.c)
- the only thing that signals PANVK_SUBQUEUE_VERTEX_TILER's syncobj and
gives PANVK_SUBQUEUE_COMPUTE something valid to wait on - runs once per
render pass from CmdEndRendering, never per draw. The dispatch originally
fired immediately inside CmdDraw, before flush_tiling() had ever run for
that render pass, so any wait it inserted would be against a stale or
nonexistent sync point. Fixed with a queue-and-flush restructure: CmdDraw
now records pending draws (xfb.pending_draws[], panvk_cmd_draw.h) instead
of dispatching immediately; panvk_per_arch(cmd_flush_pending_xfb_captures)(),
called from CmdEndRendering right after flush_tiling(), replays them, each
inserting an explicit cross-subqueue wait against the just-incremented
relative_sync_point (mirroring emit_barrier_insert_waits()'s primitives:
cs_subqueue_ctx_reg, panvk_cs_subqueue_context::syncobjs,
cs_progress_seqno_reg, panvk_instr_sync64_wait).

This is a real, correct fix in its own right (the previous code was
provably waiting on the wrong thing, or nothing), and further bisection
(SRT forced to 0 - still faulted; skipping cs_trace_run_compute() entirely
- fence succeeded, localizing the fault to execution itself; skipping
nir_lower_xfb_to_stores - still faulted, ruling out the XFB write
mechanism) found the actual root cause: the XFB variant's SPD, built by
panvk_shader_upload(), declares MALI_SHADER_STAGE_VERTEX (it branches
purely on shader->info.stage, still MESA_SHADER_VERTEX for this variant,
needed for bifrost_postprocess_nir()'s VS-specific lowering to run at
all), while being launched via cs_run_compute, a COMPUTE-shaped job.
FIXED by building a plain SHADER_PROGRAM descriptor here that explicitly
declares MALI_SHADER_STAGE_COMPUTE instead - same compiled binary, no
shader-side change. Confirmed on the real Poco X8 Pro:
vkWaitForFences -> 0, render unaffected. The crash is solved.

A fourth issue: nir_load_raw_vertex_id/load_vertex_id/load_instance_id
all compile to hardware-preloaded registers that firmware only populates
correctly for real VERTEX/IDVS jobs; a COMPUTE-declared job gets
something else preloaded into that slot, corrupting both attribute fetch
and the XFB store address. Fixed with a new NIR pass,
panvk_lower_xfb_compute_dispatch_ids, replacing all three intrinsics with
nir_load_workgroup_id()-derived values (the capture dispatch fixes
WG_SIZE=1x1x1, so workgroup_id.x/.y directly equal the vertex/instance
index).

A fifth and final issue - THE reason the capture wrote nothing at all,
found 2026-08-06 after the fixes above still left the XFB buffer reading
back as untouched poison: panvk_shader_upload() is the only thing that
uploads a variant's compiled binary into dev->mempools.exec and records
it in variant->code_mem, and it is driven by
panvk_shader_foreach_variant(), which walks shader->variants[].
xfb_variant is deliberately NOT in that array (it's a separate calloc'd
allocation), so its binary was never uploaded, code_mem stayed {0}, and
panvk_shader_variant_get_dev_addr(xfb_variant) returned 0 - meaning
dispatch_one_xfb_capture()'s SHADER_PROGRAM descriptor pointed
cfg.binary at device address 0. The GPU was faithfully dispatching a
shader with no code: no writes, and no fault either. FIXED by uploading
the XFB variant's binary alongside the others (step 3c below).

How it was found (the earlier fixes each looked insufficient because the
shader had never run at all): BIFROST_MESA_DEBUG=shaders showed the
compiled XFB variant was correct - a full STORE.i128 of the vec4, with
its base address read from FAU word u0 and num_vertices from u1.
Host-side instrumentation then showed FAU[0] held exactly the XFB
buffer's device address and FAU[1] held 3, with a valid SPD/TSD/
res_table - i.e. everything the driver programmed was right. CS-level
marker stores (cs_store32, not shader stores) bracketing RUN_COMPUTE both
landed, proving the COMPUTE command stream itself executed. That left
only the shader binary, and printing cfg.binary showed 0.

VERIFIED WORKING end-to-end on the real Poco X8 Pro (Mali-G720, kbase
r49p1): tests/render_xfb_probe captures all 3 vertices exactly
(-0.8,-0.8,0,1 / 0.8,-0.8,0,1 / -0.8,0.8,0,1), 0 failures, render
unaffected (190/66/0 pixel split, matching render_vbo_probe on the same
geometry), device healthy afterwards per driver_compute_probe --fill.

.EXT_transform_feedback is nonetheless still left false here: phase 1
only implements non-indexed, non-indirect vkCmdDraw and asserts on
everything else (indexed/indirect draws, counter-buffer resume, queries),
so advertising the extension to real apps would turn "unsupported" into
"assert/abort". Flipping it on is a deliberate follow-up decision once
those paths exist, not a consequence of this probe passing.

Not kbase-specific - like patch-panvk-null-device-destroy.py, this is
genuine upstream PanVK/Mesa capability work, a candidate for upstreaming.
Applied as a script rather than a diff because upstream moves. Idempotent.

Note (2026-08-06): third_party/MESA-KMOD is currently pinned to a commit
newer than the WSL /opt/mesa-src working tree this was developed/verified
against, and upstream has since refactored panvk_vX_shader.c enough that
panvk_lower_load_vs_input no longer exists there in the same form - this
script's clean-apply/idempotency has NOT been re-verified against
MESA-KMOD's current pin. Not a bug in this script; a separate pin-drift
issue to resolve before that verification can be redone.

Usage: patch-panvk-xfb-phase1.py <mesa-src-dir>
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"
VULKAN_DIR = os.path.join(mesa, "src/panfrost/vulkan")
LIBPAN_DIR = os.path.join(mesa, "src/panfrost/libpan")

# Whether the patched driver advertises VK_EXT_transform_feedback.
#
# On by default now. Every entry point the extension defines is implemented and
# hardware-verified (43 render_xfb_probe mode combinations on a Mali-G720), and
# the capture queue is growable rather than a fixed array, so a render pass may
# record any number of captured draws.
#
# Two gaps remain, both asserted rather than silently wrong: primitive restart
# combined with an indirect draw (there is no host index count to size the slot
# table with), and adjacency/patch-list topologies - which need geometryShader
# or tessellationShader, both of which this driver reports as false, so a
# conformant application cannot reach them at all.
#
# Set PANVK_XFB_HIDE=1 to emit `false` instead and keep the extension out of
# the reported list without reverting the implementation.
XFB_EXT_ENABLED = ("false" if os.environ.get("PANVK_XFB_HIDE")
                   else "PAN_ARCH >= 10")


def patch_file(relpath, edits, done_marker, base=None):
    path = os.path.join(base or VULKAN_DIR, relpath)
    src = open(path).read()

    if done_marker in src:
        print(f"    {relpath}: already patched")
        return

    for anchor, replacement in edits:
        assert anchor in src, f"{relpath}: anchor not found - upstream moved:\n{anchor[:80]}"
        src = src.replace(anchor, replacement, 1)

    open(path, "w").write(src)
    print(f"    patched {relpath}")


# 1. panvk_shader.h: MAX_XFB_BUFFERS, xfb sysvals, and the xfb_variant /
#    xfb_strides / xfb_buffers_written fields on struct panvk_shader.
patch_file(
    "panvk_shader.h",
    [
        (
            "#define MAX_RTS 8\n",
            "#define MAX_RTS 8\n"
            "\n"
            "/* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess. */\n"
            "#define MAX_XFB_BUFFERS 4\n"
            "\n"
            "/* Compact primitive topology for the XFB capture path. Ordered so that\n"
            " * verts_per_prim falls out of the value: <=0 is 1, <=2 is 2, else 3.\n"
            " */\n"
            "enum panvk_xfb_topology {\n"
            "   PANVK_XFB_TOPO_POINT_LIST = 0,\n"
            "   PANVK_XFB_TOPO_LINE_LIST = 1,\n"
            "   PANVK_XFB_TOPO_LINE_STRIP = 2,\n"
            "   PANVK_XFB_TOPO_TRIANGLE_LIST = 3,\n"
            "   PANVK_XFB_TOPO_TRIANGLE_STRIP = 4,\n"
            "   PANVK_XFB_TOPO_TRIANGLE_FAN = 5,\n"
            "};\n",
        ),
        (
            "   struct {\n"
            "#if PAN_ARCH < 9\n"
            "      int32_t raw_vertex_offset;\n"
            "#endif\n"
            "      int32_t first_vertex;\n"
            "      int32_t base_instance;\n"
            "      uint32_t noperspective_varyings;\n"
            "   } vs;\n",
            "   struct {\n"
            "#if PAN_ARCH < 9\n"
            "      int32_t raw_vertex_offset;\n"
            "#endif\n"
            "      int32_t first_vertex;\n"
            "      int32_t base_instance;\n"
            "      uint32_t noperspective_varyings;\n"
            "   } vs;\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess.\n"
            "    * Only used by the XFB-capture shader variant, not the render VS.\n"
            "    */\n"
            "   struct {\n"
            "      aligned_u64 buffer_addrs[MAX_XFB_BUFFERS];\n"
            "\n"
            "      /* Phase 2 (indexed draws): address of the first index this draw\n"
            "       * consumes, i.e. already biased by firstIndex. 0 means the draw is\n"
            "       * non-indexed and the capture shader uses its sequential invocation\n"
            "       * number as the attribute-fetch index instead. index_size is the\n"
            "       * element size in bytes (1, 2 or 4) and is only meaningful when\n"
            "       * index_buffer != 0.\n"
            "       */\n"
            "      aligned_u64 index_buffer;\n"
            "\n"
            "      /* Captured vertices per instance - prims_per_instance *\n"
            "       * verts_per_prim, which is NOT the input vertex count once strips\n"
            "       * or fans are in play.\n"
            "       */\n"
            "      uint32_t num_vertices;\n"
            "      uint32_t index_size;\n"
            "\n"
            "      /* enum panvk_xfb_topology. Dynamic state, so the capture shader\n"
            "       * has to branch on it rather than being specialised.\n"
            "       */\n"
            "      uint32_t topology;\n"
            "      uint32_t _pad;\n"
            "\n"
            "      /* Primitive restart: slot -> input vertex, resolved by\n"
            "       * panlib_xfb_setup() because restart makes the mapping data-dependent.\n"
            "       * 0 when restart is off, in which case the shader computes it.\n"
            "       */\n"
            "      aligned_u64 slot_table;\n"
            "   } xfb;\n",
        ),
        (
            "struct panvk_shader {\n"
            "   struct vk_shader vk;\n"
            "\n"
            "   struct panvk_shader_desc_info desc_info;\n"
            "\n"
            "   struct panvk_shader_variant variants[];\n"
            "};",
            "struct panvk_shader {\n"
            "   struct vk_shader vk;\n"
            "\n"
            "   struct panvk_shader_desc_info desc_info;\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess.\n"
            "    * Only set on a MESA_SHADER_VERTEX panvk_shader when the SPIR-V module\n"
            "    * has XFB-decorated outputs (nir->xfb_info != NULL at compile time).\n"
            "    * A separate, heap-allocated variant rather than growing\n"
            "    * PANVK_VS_VARIANTS unconditionally, since every other vertex shader\n"
            "    * (the overwhelming majority) has no use for it - see\n"
            "    * docs/kbase-notes.md for why this is a second compiled binary rather\n"
            "    * than a flag on the render variant: it must run with no_idvs=true\n"
            "    * (monolithic, always-shaded) since XFB has to capture every vertex's\n"
            "    * output even for triangles the normal IDVS optimization would cull\n"
            "    * before ever running the varying pass.\n"
            "    */\n"
            "   struct panvk_shader_variant *xfb_variant;\n"
            "\n"
            "   /* Per-buffer stride (bytes), copied from nir_xfb_info at compile\n"
            "    * time - needed at draw/counter-writeback time to compute how many\n"
            "    * bytes a capture wrote, and nir_xfb_info isn't kept around after\n"
            "    * compilation. Only buffers_written entries are meaningful.\n"
            "    */\n"
            "   uint16_t xfb_strides[MAX_XFB_BUFFERS];\n"
            "   uint8_t xfb_buffers_written;\n"
            "\n"
            "   struct panvk_shader_variant variants[];\n"
            "};",
        ),
    ],
    done_marker="MAX_XFB_BUFFERS",
)

# 2. panvk_cmd_draw.h: xfb command-buffer state block (including the
#    pending-draw queue - see the module comment in panvk_vX_cmd_xfb.c for
#    why the capture dispatch can't fire immediately in CmdDraw) + the
#    flush entry point declaration (defined in csf/panvk_vX_cmd_xfb.c).
patch_file(
    "panvk_cmd_draw.h",
    [
        (
            "   struct {\n"
            "      struct panvk_attrib_buf bufs[MAX_VBS];\n"
            "      unsigned count;\n"
            "   } vb;\n",
            "   struct {\n"
            "      struct panvk_attrib_buf bufs[MAX_VBS];\n"
            "      unsigned count;\n"
            "   } vb;\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess.\n"
            "    * Non-indexed, non-indirect vkCmdDraw only - see docs/kbase-notes.md\n"
            "    * for the full phase-1 scope.\n"
            "    */\n"
            "   struct {\n"
            "      struct panvk_attrib_buf bufs[MAX_XFB_BUFFERS];\n"
            "      unsigned bound_count;\n"
            "      bool active;\n"
            "\n"
            "      struct {\n"
            "         uint64_t dev_addr;\n"
            "         bool present;\n"
            "      } counter_buffers[MAX_XFB_BUFFERS];\n"
            "\n"
            "      /* Capture write position, one per bound XFB buffer, in capture\n"
            "       * *slots* rather than bytes so advancing it is a plain add. Lives\n"
            "       * in GPU memory because panlib_xfb_setup() clamps against it: the\n"
            "       * command stream cannot divide on PAN_ARCH 10 (cs_udiv32 and\n"
            "       * friends are #if PAN_ARCH >= 13). Allocated by Begin, released by\n"
            "       * cmd_flush_pending_xfb_captures() - NOT by End, which runs before\n"
            "       * the captures that still need it are dispatched.\n"
            "       */\n"
            "      uint64_t offsets_gpu;\n"
            "\n"
            "      /* The capture compute dispatch cannot run immediately in CmdDraw:\n"
            "       * flush_tiling() - the only thing that signals PANVK_SUBQUEUE_VERTEX_TILER's\n"
            "       * syncobj and gives PANVK_SUBQUEUE_COMPUTE something valid to wait on -\n"
            "       * runs once per render pass, from CmdEndRendering, not per draw. Draws\n"
            "       * recorded while XFB is active are queued here and the actual dispatches\n"
            "       * fire from CmdEndRendering, after flush_tiling(). See docs/kbase-notes.md.\n"
            "       *\n"
            "       * Growable: a render pass may record any number of captured draws, and\n"
            "       * there is no hardware limit to size a fixed array against. Cleared by\n"
            "       * cmd_flush_pending_xfb_captures(), freed when the command buffer is\n"
            "       * reset or destroyed.\n"
            "       */\n"
            "      struct util_dynarray pending_draws;\n"
            "   } xfb;\n",
        ),
        (
            "   enum mesa_prim prim;\n"
            "\n"
            "#if PAN_ARCH < 9\n"
            "   uint32_t layer_id;\n"
            "#endif\n"
            "};",
            "   enum mesa_prim prim;\n"
            "\n"
            "#if PAN_ARCH < 9\n"
            "   uint32_t layer_id;\n"
            "#endif\n"
            "};\n"
            "\n"
            "/* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess. Defined\n"
            " * in csf/panvk_vX_cmd_xfb.c; called from panvk_per_arch(CmdEndRendering)\n"
            " * in csf/panvk_vX_cmd_draw.c, after flush_tiling() has signalled this\n"
            " * render pass's VERTEX_TILER syncobj - the capture dispatch waits on\n"
            " * that signal, so it cannot run any earlier (see the xfb.pending_draws\n"
            " * comment above).\n"
            " */\n"
            "void panvk_per_arch(cmd_flush_pending_xfb_captures)(\n"
            "   struct panvk_cmd_buffer *cmdbuf);\n",
        ),
        (
            "struct panvk_cmd_graphics_state {\n",
            "struct panvk_xfb_pending_draw {\n"
            "   uint32_t vertex_count, instance_count;\n"
            "\n"
            "   /* Non-indexed: firstVertex. Indexed: vertexOffset. Either way it\n"
            "    * is what GLOBAL_ATTRIBUTE_OFFSET gets programmed with, which is\n"
            "    * exactly the bias hardware attribute fetch applies.\n"
            "    */\n"
            "   int32_t vertex_base;\n"
            "\n"
            "   /* Phase 2: 0 for a non-indexed draw, otherwise the address of the\n"
            "    * draw's first index (already biased by firstIndex).\n"
            "    */\n"
            "   uint64_t index_buffer;\n"
            "   uint32_t index_size;\n"
            "\n"
            "   /* VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: snapshotted per\n"
            "    * draw, because the query may be ended before CmdEndRendering\n"
            "    * flushes these captures and clears the live state. 0 = no query.\n"
            "    */\n"
            "   uint64_t query_ptr;\n"
            "\n"
            "   /* enum panvk_xfb_topology. The kernel derives both\n"
            "    * verts_per_prim and the primitive count from it, since strips\n"
            "    * and fans share vertices between adjacent primitives.\n"
            "    */\n"
            "   uint32_t xfb_topology;\n"
            "\n"
            "   /* Primitive restart: the sentinel index value, 0 when restart is\n"
            "    * off. panlib_xfb_setup() needs it to split the index stream into\n"
            "    * runs.\n"
            "    */\n"
            "   uint32_t restart_index;\n"
            "\n"
            "   /* Upper bound on the indices this draw can consume, used only to size\n"
            "    * the restart slot table. For a direct draw it is the real index\n"
            "    * count; for an indirect one the host cannot know that, so it is the\n"
            "    * capacity of the bound index buffer - a draw cannot read more\n"
            "    * indices than the buffer holds.\n"
            "    */\n"
            "   uint32_t index_count_bound;\n"
            "\n"
            "   /* vkCmdDrawIndirect: address of the VkDrawIndirectCommand. 0 for a\n"
            "    * direct draw, where vertex_count/instance_count above are already\n"
            "    * the real counts. When set, panlib_xfb_setup() reads the counts\n"
            "    * from here instead.\n"
            "    */\n"
            "   uint64_t indirect_buffer;\n"
            "};\n"
            "\n"
            "struct panvk_cmd_graphics_state {\n",
        ),
        (
            '#include "panvk_blend.h"\n',
            '#include "util/u_dynarray.h"\n'
            '#include "panvk_blend.h"\n',
        ),
    ],
    done_marker="panvk_xfb_pending_draw",
)

# 3. panvk_vX_shader.c: nir_xfb_info.h include, sysval intrinsic lowering,
#    the two-variant compile (XFB variant compiled before the render-variant
#    loop mutates info->nir in place), and freeing xfb_variant on destroy.
patch_file(
    "panvk_vX_shader.c",
    [
        (
            '#include "nir_deref.h"\n',
            '#include "nir_deref.h"\n'
            '#include "nir_xfb_info.h"\n',
        ),
        (
            "   case nir_intrinsic_load_base_instance:\n"
            "      val = load_sysval(b, graphics, bit_size, vs.base_instance);\n"
            "      break;\n",
            "   case nir_intrinsic_load_base_instance:\n"
            "      val = load_sysval(b, graphics, bit_size, vs.base_instance);\n"
            "      break;\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: only used by the XFB-capture\n"
            "    * shader variant. num_vertices is populated per-draw at record time\n"
            "    * (phase 1 is non-indexed, non-indirect vkCmdDraw only, so this is a\n"
            "    * host-known constant); xfb_address's BASE index selects which bound\n"
            "    * XFB buffer this store targets.\n"
            "    */\n"
            "   case nir_intrinsic_load_num_vertices:\n"
            "      val = load_sysval(b, graphics, bit_size, xfb.num_vertices);\n"
            "      break;\n"
            "   case nir_intrinsic_load_xfb_address: {\n"
            "      uint32_t buf_idx = nir_intrinsic_base(intr);\n"
            "      assert(buf_idx < MAX_XFB_BUFFERS);\n"
            "      val = load_sysval(b, graphics, bit_size, xfb.buffer_addrs[buf_idx]);\n"
            "      break;\n"
            "   }\n",
        ),
        (
            "static bool\n"
            "panvk_lower_load_vs_input(nir_builder *b, nir_intrinsic_instr *intrin,\n"
            "                           UNUSED void *data)\n"
            "{\n"
            "   if (intrin->intrinsic != nir_intrinsic_load_input)\n"
            "      return false;\n"
            "\n"
            "   b->cursor = nir_before_instr(&intrin->instr);\n"
            "   nir_def *ld_attr = nir_load_attribute_pan(\n"
            "      b, intrin->def.num_components, intrin->def.bit_size,\n"
            "      PAN_ARCH < 9 ?\n"
            "         nir_load_raw_vertex_id(b) :\n"
            "         nir_load_vertex_id(b),\n"
            "      nir_load_instance_id(b),\n"
            "      nir_get_io_offset_src(intrin)->ssa,\n"
            "      .base = nir_intrinsic_base(intrin),\n"
            "      .component = nir_intrinsic_component(intrin),\n"
            "      .dest_type = nir_intrinsic_dest_type(intrin));\n"
            "   nir_def_replace(&intrin->def, ld_attr);\n"
            "\n"
            "   return true;\n"
            "}\n",
            "static bool\n"
            "panvk_lower_load_vs_input(nir_builder *b, nir_intrinsic_instr *intrin,\n"
            "                           UNUSED void *data)\n"
            "{\n"
            "   if (intrin->intrinsic != nir_intrinsic_load_input)\n"
            "      return false;\n"
            "\n"
            "   b->cursor = nir_before_instr(&intrin->instr);\n"
            "   nir_def *ld_attr = nir_load_attribute_pan(\n"
            "      b, intrin->def.num_components, intrin->def.bit_size,\n"
            "      PAN_ARCH < 9 ?\n"
            "         nir_load_raw_vertex_id(b) :\n"
            "         nir_load_vertex_id(b),\n"
            "      nir_load_instance_id(b),\n"
            "      nir_get_io_offset_src(intrin)->ssa,\n"
            "      .base = nir_intrinsic_base(intrin),\n"
            "      .component = nir_intrinsic_component(intrin),\n"
            "      .dest_type = nir_intrinsic_dest_type(intrin));\n"
            "   nir_def_replace(&intrin->def, ld_attr);\n"
            "\n"
            "   return true;\n"
            "}\n"
            "\n"
            "/* VK_EXT_transform_feedback: per-invocation IDs for the capture dispatch.\n"
            " *\n"
            " * The capture runs as a flat 1D grid of vertex_count * instance_count\n"
            " * invocations (dispatch_one_xfb_capture(), csf/panvk_vX_cmd_xfb.c), so\n"
            " * workgroup_id.x is the linear capture slot and the vertex/instance indices\n"
            " * are recovered from it:\n"
            " *\n"
            " *   instance = slot / num_vertices\n"
            " *   vertex   = slot - instance * num_vertices\n"
            " *\n"
            " * That reconstructs the XFB store slot exactly, because\n"
            " * nir_lower_xfb_to_stores computes instance_id * num_vertices +\n"
            " * raw_vertex_id - which is `slot` again. num_vertices is the *unclamped*\n"
            " * per-instance vertex count, so the decomposition still holds when the\n"
            " * capture is clamped to fit the bound buffers.\n"
            " *\n"
            " * All three ID intrinsics have to be replaced, because they otherwise compile\n"
            " * to hardware-preloaded registers (BI_PRELOAD_VERTEX_ID / INSTANCE_ID) that\n"
            " * firmware only populates for real VERTEX/IDVS jobs; this variant is launched\n"
            " * as a MALI_SHADER_STAGE_COMPUTE job and gets something unrelated in the same\n"
            " * physical slot. Their roles differ:\n"
            " *\n"
            " *   load_raw_vertex_id - XFB store slot, from nir_lower_xfb_to_stores\n"
            " *   load_instance_id   - used by the store slot AND by attribute fetch\n"
            " *   load_vertex_id     - attribute fetch index (PAN_ARCH >= 9), which for an\n"
            " *                        indexed draw must come from the index buffer\n"
            " *\n"
            " * NOTE (PAN_ARCH < 9): attribute fetch uses raw_vertex_id there, so it would\n"
            " * collide with the store slot. Irrelevant today - XFB is gated on\n"
            " * PAN_ARCH >= 10.\n"
            " */\n"
            "struct panvk_xfb_ids {\n"
            "   nir_def *vertex;   /* index within the instance */\n"
            "   nir_def *instance;\n"
            "   nir_def *attrib;   /* attribute-fetch index, index-buffer aware */\n"
            "};\n"
            "\n"
            "/* Map a capture slot within an instance back to the input vertex it reads.\n"
            " *\n"
            " * Transform feedback captures assembled primitives, so a strip or fan emits\n"
            " * each shared vertex once per primitive that uses it. With\n"
            " *\n"
            " *     prim = s / vpp,  v = s % vpp\n"
            " *\n"
            " * the input vertex is:\n"
            " *\n"
            " *   point/line/triangle LIST   prim * vpp + v      (i.e. s - unchanged)\n"
            " *   LINE_STRIP                 prim + v\n"
            " *   TRIANGLE_STRIP             prim + v, but odd-numbered triangles swap\n"
            " *                              their first two vertices to keep winding\n"
            " *   TRIANGLE_FAN               v == 0 ? 0 : prim + v\n"
            " */\n"
            "static nir_def *\n"
            "build_xfb_input_vertex(nir_builder *b, nir_def *s)\n"
            "{\n"
            "   nir_def *topo = load_sysval(b, graphics, 32, xfb.topology);\n"
            "\n"
            "   /* verts_per_prim falls out of the enum ordering. */\n"
            "   nir_def *vpp = nir_bcsel(\n"
            "      b, nir_ieq_imm(b, topo, PANVK_XFB_TOPO_POINT_LIST), nir_imm_int(b, 1),\n"
            "      nir_bcsel(b, nir_ule_imm(b, topo, PANVK_XFB_TOPO_LINE_STRIP),\n"
            "                nir_imm_int(b, 2), nir_imm_int(b, 3)));\n"
            "\n"
            "   nir_def *prim = nir_udiv(b, s, vpp);\n"
            "   nir_def *v = nir_isub(b, s, nir_imul(b, prim, vpp));\n"
            "   nir_def *seq = nir_iadd(b, prim, v);\n"
            "\n"
            "   /* TRIANGLE_STRIP: odd triangles swap vertices 0 and 1. */\n"
            "   nir_def *odd = nir_ine_imm(b, nir_iand_imm(b, prim, 1), 0);\n"
            "   nir_def *strip = nir_bcsel(\n"
            "      b, nir_iand(b, odd, nir_ieq_imm(b, v, 0)), nir_iadd_imm(b, prim, 1),\n"
            "      nir_bcsel(b, nir_iand(b, odd, nir_ieq_imm(b, v, 1)), prim, seq));\n"
            "\n"
            "   /* TRIANGLE_FAN: every triangle starts at vertex 0. */\n"
            "   nir_def *fan =\n"
            "      nir_bcsel(b, nir_ieq_imm(b, v, 0), nir_imm_int(b, 0), seq);\n"
            "\n"
            "   return nir_bcsel(\n"
            "      b, nir_ieq_imm(b, topo, PANVK_XFB_TOPO_TRIANGLE_STRIP), strip,\n"
            "      nir_bcsel(b, nir_ieq_imm(b, topo, PANVK_XFB_TOPO_TRIANGLE_FAN), fan,\n"
            "                nir_bcsel(b, nir_ieq_imm(b, topo, PANVK_XFB_TOPO_LINE_STRIP),\n"
            "                          seq, s)));\n"
            "}\n"
            "\n"
            "static void\n"
            "build_xfb_dispatch_ids(nir_builder *b, struct panvk_xfb_ids *ids)\n"
            "{\n"
            "   nir_def *slot = nir_channel(b, nir_load_workgroup_id(b), 0);\n"
            "   nir_def *num_vertices = load_sysval(b, graphics, 32, xfb.num_vertices);\n"
            "\n"
            "   ids->instance = nir_udiv(b, slot, num_vertices);\n"
            "\n"
            "   /* Slot within the instance. This is the XFB *store* slot and stays\n"
            "    * sequential for every topology - nir_lower_xfb_to_stores turns\n"
            "    * instance_id * num_vertices + raw_vertex_id back into the linear capture\n"
            "    * slot, so perturbing it here would make primitives overwrite each other.\n"
            "    */\n"
            "   ids->vertex = nir_isub(b, slot, nir_imul(b, ids->instance, num_vertices));\n"
            "\n"
            "   /* The input vertex this slot reads is a different thing entirely: strips\n"
            "    * and fans emit shared vertices more than once, so several slots map back\n"
            "    * to the same input.\n"
            "    */\n"
            "   /* With primitive restart the mapping is data-dependent, so the setup kernel\n"
            "    * resolves it into a table and the shader just reads it.\n"
            "    */\n"
            "   nir_def *table = load_sysval(b, graphics, 64, xfb.slot_table);\n"
            "   nir_def *from_table, *computed;\n"
            "\n"
            "   nir_push_if(b, nir_ine_imm(b, table, 0));\n"
            "   {\n"
            "      from_table = nir_load_global(\n"
            "         b, 1, 32,\n"
            "         nir_iadd(b, table, nir_u2u64(b, nir_imul_imm(b, ids->vertex, 4))),\n"
            "         .align_mul = 4);\n"
            "   }\n"
            "   nir_push_else(b, NULL);\n"
            "   {\n"
            "      computed = build_xfb_input_vertex(b, ids->vertex);\n"
            "   }\n"
            "   nir_pop_if(b, NULL);\n"
            "\n"
            "   nir_def *input = nir_if_phi(b, from_table, computed);\n"
            "\n"
            "   /* Attribute fetch. For a non-indexed draw the vertex index is the\n"
            "    * attribute index; for an indexed draw it selects an index to read.\n"
            "    * xfb.index_buffer already points at the draw's first index, and\n"
            "    * vertexOffset is applied by GLOBAL_ATTRIBUTE_OFFSET in hardware, so\n"
            "    * neither needs handling here.\n"
            "    *\n"
            "    * One compiled variant serves both draw kinds, so this is a real runtime\n"
            "    * branch - a bcsel would evaluate the load even with no index buffer to\n"
            "    * load from. Each arm needs its own nir_def *, since nir_if_phi() takes\n"
            "    * the value produced by each side.\n"
            "    */\n"
            "   nir_def *from_index, *v32, *v16, *v8, *narrow;\n"
            "\n"
            "   nir_push_if(b, nir_iand(b,\n"
            "                           nir_ine_imm(b, load_sysval(b, graphics, 64,\n"
            "                                                      xfb.index_buffer), 0),\n"
            "                           nir_ieq_imm(b, table, 0)));\n"
            "   {\n"
            "      nir_def *index_buf = load_sysval(b, graphics, 64, xfb.index_buffer);\n"
            "      nir_def *index_size = load_sysval(b, graphics, 32, xfb.index_size);\n"
            "      nir_def *addr = nir_iadd(\n"
            "         b, index_buf, nir_u2u64(b, nir_imul(b, input, index_size)));\n"
            "\n"
            "      /* VK_INDEX_TYPE_UINT32 / UINT16 / UINT8. Branch on the width rather\n"
            "       * than over-reading with a wider load: the last index of a tightly\n"
            "       * sized buffer can sit right at the end of a mapping.\n"
            "       */\n"
            "      nir_push_if(b, nir_ieq_imm(b, index_size, 4));\n"
            "      {\n"
            "         v32 = nir_load_global(b, 1, 32, addr, .align_mul = 4);\n"
            "      }\n"
            "      nir_push_else(b, NULL);\n"
            "      {\n"
            "         nir_push_if(b, nir_ieq_imm(b, index_size, 2));\n"
            "         {\n"
            "            v16 = nir_u2u32(b, nir_load_global(b, 1, 16, addr, .align_mul = 2));\n"
            "         }\n"
            "         nir_push_else(b, NULL);\n"
            "         {\n"
            "            v8 = nir_u2u32(b, nir_load_global(b, 1, 8, addr, .align_mul = 1));\n"
            "         }\n"
            "         nir_pop_if(b, NULL);\n"
            "         narrow = nir_if_phi(b, v16, v8);\n"
            "      }\n"
            "      nir_pop_if(b, NULL);\n"
            "      from_index = nir_if_phi(b, v32, narrow);\n"
            "   }\n"
            "   nir_push_else(b, NULL);\n"
            "   {\n"
            "      /* Non-indexed: the vertex index is the attribute index. */\n"
            "   }\n"
            "   nir_pop_if(b, NULL);\n"
            "\n"
            "   ids->attrib = nir_if_phi(b, from_index, input);\n"
            "}\n"
            "\n"
            "static bool\n"
            "panvk_lower_xfb_dispatch_ids(nir_shader *nir)\n"
            "{\n"
            "   nir_function_impl *impl = nir_shader_get_entrypoint(nir);\n"
            "   bool found = false;\n"
            "\n"
            "   nir_foreach_block(block, impl) {\n"
            "      nir_foreach_instr(instr, block) {\n"
            "         if (instr->type != nir_instr_type_intrinsic)\n"
            "            continue;\n"
            "\n"
            "         switch (nir_instr_as_intrinsic(instr)->intrinsic) {\n"
            "         case nir_intrinsic_load_raw_vertex_id:\n"
            "         case nir_intrinsic_load_vertex_id:\n"
            "         case nir_intrinsic_load_instance_id:\n"
            "            found = true;\n"
            "            break;\n"
            "         default:\n"
            "            break;\n"
            "         }\n"
            "\n"
            "         if (found)\n"
            "            break;\n"
            "      }\n"
            "      if (found)\n"
            "         break;\n"
            "   }\n"
            "\n"
            "   if (!found)\n"
            "      return nir_no_progress(impl);\n"
            "\n"
            "   /* Built once at the very top of main: this introduces control flow, and\n"
            "    * adding that at an arbitrary cursor from inside an intrinsics-pass\n"
            "    * callback - while the pass is walking the block being split - is not safe.\n"
            "    */\n"
            "   nir_builder b = nir_builder_at(nir_before_impl(impl));\n"
            "   struct panvk_xfb_ids ids;\n"
            "   build_xfb_dispatch_ids(&b, &ids);\n"
            "\n"
            "   nir_foreach_block_safe(block, impl) {\n"
            "      nir_foreach_instr_safe(instr, block) {\n"
            "         if (instr->type != nir_instr_type_intrinsic)\n"
            "            continue;\n"
            "\n"
            "         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);\n"
            "         nir_def *val;\n"
            "\n"
            "         switch (intr->intrinsic) {\n"
            "         case nir_intrinsic_load_raw_vertex_id:\n"
            "            val = ids.vertex;\n"
            "            break;\n"
            "         case nir_intrinsic_load_vertex_id:\n"
            "            val = ids.attrib;\n"
            "            break;\n"
            "         case nir_intrinsic_load_instance_id:\n"
            "            val = ids.instance;\n"
            "            break;\n"
            "         default:\n"
            "            continue;\n"
            "         }\n"
            "\n"
            "         if (val->bit_size != intr->def.bit_size) {\n"
            "            b.cursor = nir_before_instr(instr);\n"
            "            val = nir_u2uN(&b, val, intr->def.bit_size);\n"
            "         }\n"
            "         nir_def_replace(&intr->def, val);\n"
            "      }\n"
            "   }\n"
            "\n"
            "   return nir_progress(true, impl, nir_metadata_none);\n"
            "}\n",
        ),
        (
            "panvk_compile_nir(struct panvk_device *dev, nir_shader *nir,\n"
            "                  VkShaderCreateFlagsEXT shader_flags,\n"
            "                  const struct pan_compile_inputs *compile_input,\n"
            "                  const struct vk_graphics_pipeline_state *state,\n"
            "                  const uint32_t *noperspective_varyings,\n"
            "                  struct panvk_shader_desc_info *desc_info,\n"
            "                  struct panvk_shader_variant *shader)\n"
            "{\n"
            "   const bool dump_asm =\n"
            "      shader_flags & VK_SHADER_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_MESA;\n"
            "\n"
            "   /* We're going to modify this so make our own copy to be nicer to callers */\n"
            "   struct pan_compile_inputs input = *compile_input;\n"
            "\n"
            "   if (nir->info.stage == MESA_SHADER_VERTEX)\n"
            "      NIR_PASS(_, nir, nir_shader_intrinsics_pass, panvk_lower_load_vs_input,\n"
            "               nir_metadata_control_flow, NULL);\n",
            "panvk_compile_nir(struct panvk_device *dev, nir_shader *nir,\n"
            "                  VkShaderCreateFlagsEXT shader_flags,\n"
            "                  const struct pan_compile_inputs *compile_input,\n"
            "                  const struct vk_graphics_pipeline_state *state,\n"
            "                  const uint32_t *noperspective_varyings,\n"
            "                  struct panvk_shader_desc_info *desc_info,\n"
            "                  struct panvk_shader_variant *shader,\n"
            "                  bool lower_compute_dispatch_ids)\n"
            "{\n"
            "   const bool dump_asm =\n"
            "      shader_flags & VK_SHADER_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_MESA;\n"
            "\n"
            "   /* We're going to modify this so make our own copy to be nicer to callers */\n"
            "   struct pan_compile_inputs input = *compile_input;\n"
            "\n"
            "   if (nir->info.stage == MESA_SHADER_VERTEX)\n"
            "      NIR_PASS(_, nir, nir_shader_intrinsics_pass, panvk_lower_load_vs_input,\n"
            "               nir_metadata_control_flow, NULL);\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: see\n"
            "    * panvk_lower_xfb_compute_dispatch_ids()'s comment. Must run after\n"
            "    * panvk_lower_load_vs_input just above (attribute fetch) and after\n"
            "    * nir_lower_xfb_to_stores, already applied to this nir before it\n"
            "    * reached here - see the MESA_SHADER_VERTEX case in\n"
            "    * panvk_compile_shader().\n"
            "    */\n"
            "   if (lower_compute_dispatch_ids)\n"
            "      NIR_PASS(_, nir, panvk_lower_xfb_dispatch_ids);\n",
        ),
        (
            "         variant->own_bin = true;\n"
            "\n"
            "         result = panvk_compile_nir(dev, nir, info->flags, &inputs, state,\n"
            "                                    noperspective_varyings,\n"
            "                                    &shader->desc_info, variant);\n"
            "\n"
            "         /* If we cloned, it's our job to clean up */\n",
            "         variant->own_bin = true;\n"
            "\n"
            "         result = panvk_compile_nir(dev, nir, info->flags, &inputs, state,\n"
            "                                    noperspective_varyings,\n"
            "                                    &shader->desc_info, variant, false);\n"
            "\n"
            "         /* If we cloned, it's our job to clean up */\n",
        ),
        # The remaining two panvk_compile_nir call sites (FS-shaped, byte-identical
        # surrounding text at both) - list the same tuple twice so the second
        # patch_file() pass consumes the second occurrence too.
        (
            "      variant->own_bin = true;\n"
            "\n"
            "      result = panvk_compile_nir(dev, nir, info->flags, &inputs, state,\n"
            "                                 noperspective_varyings,\n"
            "                                 &shader->desc_info, variant);\n"
            "      if (result != VK_SUCCESS) {\n"
            "         panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n",
            "      variant->own_bin = true;\n"
            "\n"
            "      result = panvk_compile_nir(dev, nir, info->flags, &inputs, state,\n"
            "                                 noperspective_varyings,\n"
            "                                 &shader->desc_info, variant, false);\n"
            "      if (result != VK_SUCCESS) {\n"
            "         panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n",
        ),
        (
            "      variant->own_bin = true;\n"
            "\n"
            "      result = panvk_compile_nir(dev, nir, info->flags, &inputs, state,\n"
            "                                 noperspective_varyings,\n"
            "                                 &shader->desc_info, variant);\n"
            "      if (result != VK_SUCCESS) {\n"
            "         panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n",
            "      variant->own_bin = true;\n"
            "\n"
            "      result = panvk_compile_nir(dev, nir, info->flags, &inputs, state,\n"
            "                                 noperspective_varyings,\n"
            "                                 &shader->desc_info, variant, false);\n"
            "      if (result != VK_SUCCESS) {\n"
            "         panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n",
        ),
    ],
    done_marker="panvk_lower_xfb_dispatch_ids",
)

# 3b. panvk_vX_shader.c: freeing xfb_variant on destroy, and the two-variant
#    compile block itself (XFB variant compiled before the render-variant
#    loop mutates info->nir in place).
patch_file(
    "panvk_vX_shader.c",
    [
        (
            "   panvk_shader_foreach_variant(shader, variant) {\n"
            "      panvk_shader_variant_destroy(variant);\n"
            "   }\n"
            "\n"
            "#if PAN_ARCH < 9\n"
            "   panvk_pool_free_mem(&shader->desc_info.others.map);\n"
            "#endif\n",
            "   panvk_shader_foreach_variant(shader, variant) {\n"
            "      panvk_shader_variant_destroy(variant);\n"
            "   }\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: xfb_variant is a separate,\n"
            "    * optional allocation - not part of variants[], see panvk_shader.h.\n"
            "    */\n"
            "   if (shader->xfb_variant) {\n"
            "      panvk_shader_variant_destroy(shader->xfb_variant);\n"
            "      free(shader->xfb_variant);\n"
            "   }\n"
            "\n"
            "#if PAN_ARCH < 9\n"
            "   panvk_pool_free_mem(&shader->desc_info.others.map);\n"
            "#endif\n",
        ),
        (
            "   switch (info->stage) {\n"
            "   case MESA_SHADER_VERTEX: {\n"
            "      const enum panvk_vs_variant last_variant = PANVK_VS_VARIANT_HW;\n",
            "   switch (info->stage) {\n"
            "   case MESA_SHADER_VERTEX: {\n"
            "      /* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess.\n"
            "       *\n"
            "       * Compile the XFB-capture variant FIRST, cloning info->nir while\n"
            "       * it is still pristine - the loop below mutates info->nir in place\n"
            "       * for its last (non-cloned) variant, so doing this after the loop\n"
            "       * would clone an already-descriptor-lowered shader instead of the\n"
            "       * original. Mirrors the per-variant setup steps the loop below does\n"
            "       * (attribute driver_location assignment, io var locations) since\n"
            "       * this variant reads the same vertex attributes as the render\n"
            "       * variant and must agree on their layout - see docs/kbase-notes.md\n"
            "       * for the full design (mirrors Panfrost GL's csf_launch_xfb).\n"
            "       */\n"
            "      if (info->nir->xfb_info) {\n"
            "         shader->xfb_variant = calloc(1, sizeof(*shader->xfb_variant));\n"
            "         if (!shader->xfb_variant) {\n"
            "            panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n"
            "            return panvk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);\n"
            "         }\n"
            "\n"
            "         /* Grab strides before any lowering touches xfb_info. */\n"
            "         shader->xfb_buffers_written = info->nir->xfb_info->buffers_written;\n"
            "         u_foreach_bit(b, shader->xfb_buffers_written) {\n"
            "            assert(b < MAX_XFB_BUFFERS);\n"
            "            shader->xfb_strides[b] = info->nir->xfb_info->buffers[b].stride;\n"
            "         }\n"
            "\n"
            "         nir_shader *xfb_nir = nir_shader_clone(NULL, info->nir);\n"
            "\n"
            "         panvk_lower_nir(dev, xfb_nir, info->set_layout_count,\n"
            "                         info->set_layouts, info->robustness,\n"
            "                         state, &shader->desc_info, false);\n"
            "\n"
            "         nir_foreach_shader_in_variable(var, xfb_nir) {\n"
            "            assert(var->data.location >= VERT_ATTRIB_GENERIC0 &&\n"
            "                   var->data.location <= VERT_ATTRIB_GENERIC15);\n"
            "            var->data.driver_location =\n"
            "               var->data.location - VERT_ATTRIB_GENERIC0;\n"
            "         }\n"
            "         nir_assign_io_var_locations(xfb_nir, nir_var_shader_out);\n"
            "         panvk_lower_nir_io(xfb_nir);\n"
            "         NIR_PASS(_, xfb_nir, nir_opt_constant_folding);\n"
            "\n"
            "         NIR_PASS(_, xfb_nir, nir_io_add_intrinsic_xfb_info);\n"
            "         NIR_PASS(_, xfb_nir, nir_lower_xfb_to_stores,\n"
            "                  &(nir_lower_xfb_to_stores_options){\n"
            "                     .address_format = nir_address_format_64bit_global,\n"
            "                     .keep_outputs = false,\n"
            "                  });\n"
            "\n"
            "         /* bifrost_postprocess_nir() unconditionally requires a non-NULL\n"
            "          * varying_layout for any MESA_SHADER_VERTEX compile (it asserts,\n"
            "          * but that assert compiles out in release builds and it then\n"
            "          * memcpy()s from NULL instead - confirmed on real hardware, see\n"
            "          * docs/kbase-notes.md). The XFB variant doesn't rasterize or\n"
            "          * produce meaningful varyings, but still needs a valid (if\n"
            "          * trivial) layout to satisfy this.\n"
            "          */\n"
            "         struct pan_varying_layout xfb_varying_layout;\n"
            "         /* inputs.trust_varying_flat_highp_types isn't set until the loop\n"
            "          * below (v == PANVK_VS_VARIANT_HW) - this block runs before it,\n"
            "          * so pass the same literal true it ends up with directly rather\n"
            "          * than reading the not-yet-set struct field.\n"
            "          */\n"
            "         pan_varying_collect_formats(&xfb_varying_layout, xfb_nir,\n"
            "                                     inputs.gpu_id, true, true);\n"
            "         pan_build_varying_layout_compact(&xfb_varying_layout, xfb_nir,\n"
            "                                          inputs.gpu_id);\n"
            "\n"
            "         struct pan_compile_inputs xfb_inputs = inputs;\n"
            "         xfb_inputs.no_idvs = true;\n"
            "         xfb_inputs.varying_layout = &xfb_varying_layout;\n"
            "\n"
            "         shader->xfb_variant->own_bin = true;\n"
            "         result = panvk_compile_nir(dev, xfb_nir, info->flags, &xfb_inputs,\n"
            "                                    state, noperspective_varyings,\n"
            "                                    &shader->desc_info, shader->xfb_variant,\n"
            "                                    true);\n"
            "         ralloc_free(xfb_nir);\n"
            "\n"
            "         if (result != VK_SUCCESS) {\n"
            "            free(shader->xfb_variant);\n"
            "            shader->xfb_variant = NULL;\n"
            "            panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n"
            "            return result;\n"
            "         }\n"
            "      }\n"
            "\n"
            "      const enum panvk_vs_variant last_variant = PANVK_VS_VARIANT_HW;\n",
        ),
    ],
    done_marker="xfb_variant = calloc(1, sizeof(*shader->xfb_variant))",
)

# 3c. panvk_vX_shader.c: upload the XFB variant's compiled binary.
#     THE fix for the "capture writes nothing" bug - see the module
#     docstring and docs/kbase-notes.md. panvk_shader_upload() is what
#     uploads a variant's code and sets code_mem, and it is driven by
#     panvk_shader_foreach_variant(), which walks variants[] - and
#     xfb_variant is deliberately not in that array.
patch_file(
    "panvk_vX_shader.c",
    [
        (
            "   panvk_shader_foreach_variant(shader, variant) {\n"
            "      result = panvk_shader_upload(dev, variant, pAllocator);\n"
            "      if (result != VK_SUCCESS) {\n"
            "         panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n"
            "         return result;\n"
            "      }\n"
            "   }\n"
            "\n"
            "   *shader_out = &shader->vk;\n",
            "   panvk_shader_foreach_variant(shader, variant) {\n"
            "      result = panvk_shader_upload(dev, variant, pAllocator);\n"
            "      if (result != VK_SUCCESS) {\n"
            "         panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n"
            "         return result;\n"
            "      }\n"
            "   }\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: xfb_variant is a separate\n"
            "    * allocation, not part of variants[], so the loop above does not cover\n"
            "    * it - and panvk_shader_upload() is what uploads a variant's binary and\n"
            "    * sets code_mem. Without this the XFB variant's code was never uploaded,\n"
            "    * code_mem stayed {0}, and the SHADER_PROGRAM descriptor built in\n"
            "    * dispatch_one_xfb_capture() pointed cfg.binary at device address 0 - a\n"
            "    * dispatch of a shader with no code, which silently wrote nothing and\n"
            "    * never faulted. See docs/kbase-notes.md.\n"
            "    *\n"
            "    * Upload the code directly rather than calling panvk_shader_upload():\n"
            "    * that would also build IDVS-shaped MALI_SHADER_STAGE_VERTEX SPDs for\n"
            "    * this variant (it branches on info.stage, still MESA_SHADER_VERTEX\n"
            "    * here), which must never be used - the capture dispatch builds its own\n"
            "    * MALI_SHADER_STAGE_COMPUTE descriptor. code_mem is all it needs.\n"
            "    */\n"
            "   if (shader->xfb_variant && shader->xfb_variant->bin_size) {\n"
            "      shader->xfb_variant->code_mem = panvk_pool_upload_aligned(\n"
            "         &dev->mempools.exec, shader->xfb_variant->bin_ptr,\n"
            "         shader->xfb_variant->bin_size, 128);\n"
            "      if (!panvk_priv_mem_check_alloc(shader->xfb_variant->code_mem)) {\n"
            "         panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);\n"
            "         return panvk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);\n"
            "      }\n"
            "   }\n"
            "\n"
            "   *shader_out = &shader->vk;\n",
        ),
    ],
    done_marker="xfb_variant->code_mem",
)

# 4. panvk_vX_cmd_draw.c (common, arch-independent file):
#    CmdBindTransformFeedbackBuffersEXT.
patch_file(
    "panvk_vX_cmd_draw.c",
    [
        (
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdBindIndexBuffer2)(",
            "/* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess - see\n"
            " * docs/kbase-notes.md for the full phase-1 scope (non-indexed,\n"
            " * non-indirect vkCmdDraw only; pCounterBuffers must be NULL at Begin).\n"
            " */\n"
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdBindTransformFeedbackBuffersEXT)(\n"
            "   VkCommandBuffer commandBuffer, uint32_t firstBinding,\n"
            "   uint32_t bindingCount, const VkBuffer *pBuffers,\n"
            "   const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes)\n"
            "{\n"
            "   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);\n"
            "\n"
            "   assert(firstBinding + bindingCount <= MAX_XFB_BUFFERS);\n"
            "   assert(!cmdbuf->state.gfx.xfb.active &&\n"
            "          \"cannot rebind XFB buffers while transform feedback is active\");\n"
            "\n"
            "   for (uint32_t i = 0; i < bindingCount; i++) {\n"
            "      VK_FROM_HANDLE(panvk_buffer, buffer, pBuffers[i]);\n"
            "      unsigned idx = firstBinding + i;\n"
            "\n"
            "      if (buffer) {\n"
            "         cmdbuf->state.gfx.xfb.bufs[idx].address =\n"
            "            panvk_buffer_gpu_ptr(buffer, pOffsets[i]);\n"
            "         cmdbuf->state.gfx.xfb.bufs[idx].size = panvk_buffer_range(\n"
            "            buffer, pOffsets[i], pSizes ? pSizes[i] : VK_WHOLE_SIZE);\n"
            "      } else {\n"
            "         cmdbuf->state.gfx.xfb.bufs[idx].address = 0;\n"
            "         cmdbuf->state.gfx.xfb.bufs[idx].size = 0;\n"
            "      }\n"
            "   }\n"
            "\n"
            "   cmdbuf->state.gfx.xfb.bound_count =\n"
            "      MAX2(cmdbuf->state.gfx.xfb.bound_count, firstBinding + bindingCount);\n"
            "}\n"
            "\n"
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdBindIndexBuffer2)(",
        ),
    ],
    done_marker="CmdBindTransformFeedbackBuffersEXT",
)

# 5. csf/panvk_vX_cmd_draw.c: queue pending XFB draws in CmdDraw (only
#    non-indexed, non-indirect draws - CmdDrawIndexed and indirect draws
#    deliberately do not queue), and flush them in CmdEndRendering right
#    after flush_tiling() - the capture dispatch cannot run any earlier,
#    see the pending_draws comment in panvk_cmd_draw.h and
#    docs/kbase-notes.md for the investigation that found this.
patch_file(
    "csf/panvk_vX_cmd_draw.c",
    [
        (
            "   struct panvk_draw_info draw = {\n"
            "      .vertex.base = firstVertex,\n"
            "      .vertex.count = vertexCount,\n"
            "      .instance.base = firstInstance,\n"
            "      .instance.count = instanceCount,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "}",
            "   struct panvk_draw_info draw = {\n"
            "      .vertex.base = firstVertex,\n"
            "      .vertex.count = vertexCount,\n"
            "      .instance.base = firstInstance,\n"
            "      .instance.count = instanceCount,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "\n"
            "   /* VK_EXT_transform_feedback, phase 1: only non-indexed, non-indirect\n"
            "    * vkCmdDraw is supported - CmdDrawIndexed and indirect draws\n"
            "    * deliberately do not call this. The actual capture dispatch cannot\n"
            "    * run here: flush_tiling() (CmdEndRendering) hasn't signalled this\n"
            "    * render pass's VERTEX_TILER syncobj yet, so there's nothing valid for\n"
            "    * the compute dispatch to wait on. Queue it instead - see\n"
            "    * docs/kbase-notes.md and the xfb.pending_draws comment in\n"
            "    * panvk_cmd_draw.h.\n"
            "    */\n"
            "   if (cmdbuf->state.gfx.xfb.active) {\n"
            "      struct panvk_xfb_pending_draw *d = xfb_queue_draw(cmdbuf);\n"
            "\n"
            "      if (d != NULL) {\n"
            "         d->vertex_count = vertexCount;\n"
            "         d->instance_count = instanceCount;\n"
            "         d->vertex_base = firstVertex;\n"
            "         d->index_count_bound = indexCount;\n"
            "         d->query_ptr = cmdbuf->state.gfx.xfb_query.ptr;\n"
            "         d->xfb_topology = xfb_topology(cmdbuf);\n"
            "      }\n"
            "   }\n"
            "}",
        ),
        # Phase 2: indexed draws capture too.
        (
            "   struct panvk_draw_info draw = {\n"
            "      .index = panvk_draw_info_index(cmdbuf, firstIndex),\n"
            "      .vertex.base = vertexOffset,\n"
            "      .vertex.count = indexCount,\n"
            "      .instance.count = instanceCount,\n"
            "      .instance.base = firstInstance,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "}\n",
            "   struct panvk_draw_info draw = {\n"
            "      .index = panvk_draw_info_index(cmdbuf, firstIndex),\n"
            "      .vertex.base = vertexOffset,\n"
            "      .vertex.count = indexCount,\n"
            "      .instance.count = instanceCount,\n"
            "      .instance.base = firstInstance,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "\n"
            "   /* VK_EXT_transform_feedback phase 2: indexed draws capture too. The\n"
            "    * shader reads its attribute index from the index buffer while the XFB\n"
            "    * store slot stays sequential - see panvk_lower_xfb_vertex_id() in\n"
            "    * panvk_vX_shader.c, and the xfb.pending_draws comment in\n"
            "    * panvk_cmd_draw.h for why this is queued rather than dispatched here.\n"
            "    */\n"
            "   if (cmdbuf->state.gfx.xfb.active) {\n"
            "      const struct panvk_cmd_graphics_state *gfx = &cmdbuf->state.gfx;\n"
            "\n"
            "      /* Primitive restart removes vertices from the stream, so the capture\n"
            "       * would need a GPU-side compacted count rather than a host-known one.\n"
            "       * Out of scope with the rest of the indirect/counter work.\n"
            "       */\n"
            "      assert(!cmdbuf->vk.dynamic_graphics_state.ia.primitive_restart_enable &&\n"
            "             \"VK_EXT_transform_feedback: primitive restart with transform \"\n"
            "             \"feedback active is not supported yet\");\n"
            "\n"
            "      struct panvk_xfb_pending_draw *d = xfb_queue_draw(cmdbuf);\n"
            "\n"
            "      if (d != NULL) {\n"
            "         d->vertex_count = indexCount;\n"
            "         d->instance_count = instanceCount;\n"
            "         d->vertex_base = vertexOffset;\n"
            "         d->index_buffer =\n"
            "            gfx->ib.dev_addr + ((uint64_t)firstIndex * gfx->ib.index_size);\n"
            "         d->index_size = gfx->ib.index_size;\n"
            "         /* The restart sentinel is all-ones at the index width. */\n"
            "         d->restart_index =\n"
            "            cmdbuf->vk.dynamic_graphics_state.ia.primitive_restart_enable\n"
            "               ? (gfx->ib.index_size == 4   ? 0xffffffffu\n"
            "                  : gfx->ib.index_size == 2 ? 0xffffu\n"
            "                                            : 0xffu)\n"
            "               : 0;\n"
            "         d->query_ptr = cmdbuf->state.gfx.xfb_query.ptr;\n"
            "         d->xfb_topology = xfb_topology(cmdbuf);\n"
            "      }\n"
            "   }\n"
            "}\n",
        ),
        # VK_EXT_transform_feedback: vkCmdDrawIndirectByteCountEXT. A kernel
        # turns the captured byte count into an ordinary
        # VkDrawIndirectCommand, so both the draw and its own capture go
        # through the existing indirect paths unchanged.
        (
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDrawIndirect)(VkCommandBuffer commandBuffer, VkBuffer _buffer,\n",
            "/* VK_EXT_transform_feedback: draw as many vertices as a previous capture wrote.\n"
            " *\n"
            " * The count lives in a counter buffer, so a kernel turns it into an ordinary\n"
            " * VkDrawIndirectCommand and everything downstream - the draw itself and the XFB\n"
            " * capture - goes through the existing indirect paths unchanged.\n"
            " *\n"
            " * This is the one place in this feature where compute has to be ordered\n"
            " * *before* vertex/tiler rather than after it: the draw reads what the kernel\n"
            " * wrote. Hence the cache flush (a barrier only waits, it does not write stores\n"
            " * back) followed by a signal on compute and a matching wait on vertex/tiler,\n"
            " * mirroring emit_barrier_insert_waits().\n"
            " */\n"
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDrawIndirectByteCountEXT)(\n"
            "   VkCommandBuffer commandBuffer, uint32_t instanceCount,\n"
            "   uint32_t firstInstance, VkBuffer counterBuffer,\n"
            "   VkDeviceSize counterBufferOffset, uint32_t counterOffset,\n"
            "   uint32_t vertexStride)\n"
            "{\n"
            "   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);\n"
            "   VK_FROM_HANDLE(panvk_buffer, counter, counterBuffer);\n"
            "\n"
            "   if (instanceCount == 0 || vertexStride == 0)\n"
            "      return;\n"
            "\n"
            "   struct pan_ptr cmd = panvk_cmd_alloc_dev_mem(\n"
            "      cmdbuf, desc, sizeof(VkDrawIndirectCommand), sizeof(uint32_t));\n"
            "   if (!cmd.gpu) {\n"
            "      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);\n"
            "      return;\n"
            "   }\n"
            "\n"
            "   {\n"
            "      struct panvk_precomp_ctx pctx = panvk_per_arch(precomp_cs)(cmdbuf);\n"
            "      struct panlib_xfb_byte_count_draw_args args = {\n"
            "         .cmd = cmd.gpu,\n"
            "         .counter = panvk_buffer_gpu_ptr(counter, counterBufferOffset),\n"
            "         .counter_offset = counterOffset,\n"
            "         .vertex_stride = vertexStride,\n"
            "         .instance_count = instanceCount,\n"
            "         .first_instance = firstInstance,\n"
            "      };\n"
            "\n"
            "      panlib_xfb_byte_count_draw_struct(&pctx, panlib_1d(1),\n"
            "                                        PANLIB_BARRIER_CSF_WAIT, args);\n"
            "   }\n"
            "\n"
            "   {\n"
            "      /* Write the kernel's stores back, then order vertex/tiler after compute\n"
            "       * so the draw below sees the synthesised command.\n"
            "       */\n"
            "      struct cs_builder *cb =\n"
            "         panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);\n"
            "      struct cs_index flush_id = cs_scratch_reg32(cb, 4);\n"
            "\n"
            "      cs_move32_to(cb, flush_id, 0);\n"
            "      cs_flush_caches(cb, MALI_CS_FLUSH_MODE_CLEAN, MALI_CS_FLUSH_MODE_CLEAN,\n"
            "                      MALI_CS_OTHER_FLUSH_MODE_NONE, flush_id,\n"
            "                      cs_defer(SB_IMM_MASK, SB_ID(DEFERRED_FLUSH)));\n"
            "      cs_wait_slot(cb, SB_ID(DEFERRED_FLUSH));\n"
            "\n"
            "      panvk_per_arch(cmd_signal_barrier)(cmdbuf, PANVK_CSF_BARRIER_SYNC);\n"
            "\n"
            "      struct cs_builder *vb =\n"
            "         panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);\n"
            "      struct cs_index sync_addr = cs_scratch_reg64(vb, 0);\n"
            "      struct cs_index wait_val = cs_scratch_reg64(vb, 2);\n"
            "\n"
            "      cs_load64_to(vb, sync_addr, cs_subqueue_ctx_reg(vb),\n"
            "                   offsetof(struct panvk_cs_subqueue_context, syncobjs));\n"
            "      cs_add_imm64(vb, sync_addr, sync_addr,\n"
            "                   sizeof(struct panvk_cs_sync64) * PANVK_SUBQUEUE_COMPUTE);\n"
            "      cs_add_imm64(vb, wait_val,\n"
            "                   cs_progress_seqno_reg(vb, PANVK_SUBQUEUE_COMPUTE),\n"
            "                   cmdbuf->state.cs[PANVK_SUBQUEUE_COMPUTE].relative_sync_point);\n"
            "\n"
            "      panvk_instr_sync64_wait(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER, false,\n"
            "                              MALI_CS_CONDITION_GREATER, wait_val, sync_addr);\n"
            "   }\n"
            "\n"
            "   struct panvk_draw_info draw = {\n"
            "      .indirect.buffer_dev_addr = cmd.gpu,\n"
            "      .indirect.draw_count = 1,\n"
            "      .indirect.stride = sizeof(VkDrawIndirectCommand),\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "\n"
            "   /* The capture needs no special case: the synthesised command is an ordinary\n"
            "    * indirect command, so this is just an indirect draw as far as it cares.\n"
            "    */\n"
            "   xfb_queue_indirect_captures(cmdbuf, cmd.gpu, 1,\n"
            "                               sizeof(VkDrawIndirectCommand), 0, 0);\n"
            "}\n"
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDrawIndirect)(VkCommandBuffer commandBuffer, VkBuffer _buffer,\n",
        ),
        # Phase 2: vkCmdDrawIndirect. The counts live in GPU memory, so the
        # queued capture carries the buffer address and panlib_xfb_setup()
        # reads vertexCount/instanceCount from it.
        (
            "   struct panvk_draw_info draw = {\n"
            "      .indirect.buffer_dev_addr = panvk_buffer_gpu_ptr(buffer, offset),\n"
            "      .indirect.draw_count = drawCount,\n"
            "      .indirect.stride = stride,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "}\n",
            "   struct panvk_draw_info draw = {\n"
            "      .indirect.buffer_dev_addr = panvk_buffer_gpu_ptr(buffer, offset),\n"
            "      .indirect.draw_count = drawCount,\n"
            "      .indirect.stride = stride,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "\n"
            "   /* VK_EXT_transform_feedback: the counts live in the indirect buffer, so\n"
            "    * panlib_xfb_setup() reads them there - see the indirect_buffer comment in\n"
            "    * panvk_cmd_draw.h. stride only matters when there is more than one\n"
            "    * command; Vulkan lets it be anything for a single draw.\n"
            "    */\n"
            "   xfb_queue_indirect_captures(\n"
            "      cmdbuf, panvk_buffer_gpu_ptr(buffer, offset), drawCount,\n"
            "      drawCount > 1 ? stride : sizeof(VkDrawIndirectCommand), 0, 0);\n"
            "}\n"
            "\n",
        ),
        # Phase 2: vkCmdDrawIndexedIndirect. The index-buffer address is
        # queued *unbiased* - firstIndex is in the indirect command, so
        # panlib_xfb_setup() applies the bias. index_size alongside
        # indirect_buffer is what marks this as an indexed indirect draw.
        (
            "   struct panvk_draw_info draw = {\n"
            "      .index = panvk_draw_info_index(cmdbuf, 0),\n"
            "      .indirect.buffer_dev_addr = panvk_buffer_gpu_ptr(buffer, offset),\n"
            "      .indirect.draw_count = drawCount,\n"
            "      .indirect.stride = stride,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "}\n",
            "   struct panvk_draw_info draw = {\n"
            "      .index = panvk_draw_info_index(cmdbuf, 0),\n"
            "      .indirect.buffer_dev_addr = panvk_buffer_gpu_ptr(buffer, offset),\n"
            "      .indirect.draw_count = drawCount,\n"
            "      .indirect.stride = stride,\n"
            "      .prim = panvk_get_client_prim(cmdbuf),\n"
            "   };\n"
            "\n"
            "   panvk_cmd_draw(cmdbuf, draw);\n"
            "\n"
            "   /* VK_EXT_transform_feedback: like CmdDrawIndirect, but the index-buffer\n"
            "    * address is passed *unbiased* - firstIndex is in the indirect command, so\n"
            "    * panlib_xfb_setup() applies the bias.\n"
            "    */\n"
            "   xfb_queue_indirect_captures(\n"
            "      cmdbuf, panvk_buffer_gpu_ptr(buffer, offset), drawCount,\n"
            "      drawCount > 1 ? stride : sizeof(VkDrawIndexedIndirectCommand),\n"
            "      cmdbuf->state.gfx.ib.dev_addr, cmdbuf->state.gfx.ib.index_size);\n"
            "}\n"
            "\n",
        ),
        # The topology helper both queueing sites use.
        (
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount,",
            "/* VK_EXT_transform_feedback: queue one draw for capture at CmdEndRendering.\n"
            " *\n"
            " * Returns the slot to fill in, or NULL if it could not be allocated - in which\n"
            " * case the error is already recorded on the command buffer and the caller just\n"
            " * skips this capture.\n"
            " */\n"
            "static struct panvk_xfb_pending_draw *\n"
            "xfb_queue_draw(struct panvk_cmd_buffer *cmdbuf)\n"
            "{\n"
            "   struct util_dynarray *draws = &cmdbuf->state.gfx.xfb.pending_draws;\n"
            "   struct panvk_xfb_pending_draw *d = util_dynarray_grow(\n"
            "      draws, struct panvk_xfb_pending_draw, 1);\n"
            "\n"
            "   if (d == NULL) {\n"
            "      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);\n"
            "      return NULL;\n"
            "   }\n"
            "\n"
            "   memset(d, 0, sizeof(*d));\n"
            "   return d;\n"
            "}\n"
            "\n"
            "/* VK_EXT_transform_feedback: the current topology, as the capture path sees it.\n"
            " *\n"
            " * Strips and fans are supported now: the kernel derives both verts_per_prim\n"
            " * and the primitive count from this, and the capture shader uses it to map\n"
            " * each capture slot back to the input vertex it should read (transform\n"
            " * feedback captures assembled primitives, so shared vertices are emitted once\n"
            " * per primitive that uses them).\n"
            " *\n"
            " * Patch lists are the remaining hole, and they need tessellation anyway.\n"
            " */\n"
            "static uint32_t\n"
            "xfb_topology(const struct panvk_cmd_buffer *cmdbuf)\n"
            "{\n"
            "   switch (cmdbuf->vk.dynamic_graphics_state.ia.primitive_topology) {\n"
            "   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:\n"
            "      return PANVK_XFB_TOPO_POINT_LIST;\n"
            "   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:\n"
            "      return PANVK_XFB_TOPO_LINE_LIST;\n"
            "   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:\n"
            "      return PANVK_XFB_TOPO_LINE_STRIP;\n"
            "   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:\n"
            "      return PANVK_XFB_TOPO_TRIANGLE_LIST;\n"
            "   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:\n"
            "      return PANVK_XFB_TOPO_TRIANGLE_STRIP;\n"
            "   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:\n"
            "      return PANVK_XFB_TOPO_TRIANGLE_FAN;\n"
            "   default:\n"
            "      assert(!\"VK_EXT_transform_feedback: unsupported primitive topology \"\n"
            "              \"(adjacency and patch lists are not handled)\");\n"
            "      return PANVK_XFB_TOPO_TRIANGLE_LIST;\n"
            "   }\n"
            "}\n"
            "\n"
            "/* VK_EXT_transform_feedback: queue one capture per indirect command.\n"
            " *\n"
            " * Each command is an independent draw and panlib_xfb_setup() advances the\n"
            " * GPU-resident write position once per capture, so N captures queued here land\n"
            " * back-to-back in the XFB buffer: they execute in order on\n"
            " * PANVK_SUBQUEUE_COMPUTE and each one's setup kernel picks up where the\n"
            " * previous left off. Nothing in the kernel needs to know about multi-draw.\n"
            " *\n"
            " * index_buffer is the *unbiased* index-buffer base for an indexed indirect\n"
            " * draw (the kernel applies firstIndex from the command), or 0 for a\n"
            " * non-indexed one.\n"
            " */\n"
            "static void\n"
            "xfb_queue_indirect_captures(struct panvk_cmd_buffer *cmdbuf, uint64_t cmd_addr,\n"
            "                            uint32_t draw_count, uint32_t stride,\n"
            "                            uint64_t index_buffer, uint32_t index_size)\n"
            "{\n"
            "   struct panvk_cmd_graphics_state *gfx = &cmdbuf->state.gfx;\n"
            "\n"
            "   if (!gfx->xfb.active)\n"
            "      return;\n"
            "\n"
            "   uint32_t topo = xfb_topology(cmdbuf);\n"
            "\n"
            "   /* Primitive restart only means anything with an index buffer, and the\n"
            "    * sentinel is all-ones at the index width.\n"
            "    */\n"
            "   uint32_t restart_index =\n"
            "      index_size && cmdbuf->vk.dynamic_graphics_state.ia.primitive_restart_enable\n"
            "         ? (index_size == 4 ? 0xffffffffu : index_size == 2 ? 0xffffu : 0xffu)\n"
            "         : 0;\n"
            "\n"
            "   /* The real index count lives in the indirect command, which only the GPU\n"
            "    * can read. The slot table only needs an upper bound, though, and the\n"
            "    * bound index buffer is one: the draw cannot consume more indices than it\n"
            "    * holds.\n"
            "    */\n"
            "   uint32_t index_count_bound =\n"
            "      index_size ? (uint32_t)(gfx->ib.size / index_size) : 0;\n"
            "\n"
            "   for (uint32_t i = 0; i < draw_count; i++) {\n"
            "      struct panvk_xfb_pending_draw *d = xfb_queue_draw(cmdbuf);\n"
            "\n"
            "      if (d == NULL)\n"
            "         return;\n"
            "\n"
            "      /* vertex_count/instance_count stay 0: the kernel reads the real\n"
            "       * counts out of the indirect command instead.\n"
            "       */\n"
            "      d->index_buffer = index_buffer;\n"
            "      d->index_size = index_size;\n"
            "      d->query_ptr = gfx->xfb_query.ptr;\n"
            "      d->xfb_topology = topo;\n"
            "      d->restart_index = restart_index;\n"
            "      d->index_count_bound = index_count_bound;\n"
            "      d->indirect_buffer = cmd_addr + (uint64_t)i * stride;\n"
            "   }\n"
            "}\n"
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount,\n",
        ),
        (
            "      if (cmdbuf->state.gfx.render.fbds.gpu || inherits_render_ctx(cmdbuf)) {\n"
            "         flush_tiling(cmdbuf);\n"
            "         issue_fragment_jobs(cmdbuf);\n"
            "\n"
            "         handle_deferred_queries(cmdbuf);\n"
            "      }",
            "      if (cmdbuf->state.gfx.render.fbds.gpu || inherits_render_ctx(cmdbuf)) {\n"
            "         flush_tiling(cmdbuf);\n"
            "\n"
            "         /* VK_EXT_transform_feedback, phase 1: now that flush_tiling() has\n"
            "          * signalled this render pass's VERTEX_TILER syncobj, the queued\n"
            "          * capture dispatches have something valid to wait on. See\n"
            "          * docs/kbase-notes.md and the xfb.pending_draws comment in\n"
            "          * panvk_cmd_draw.h.\n"
            "          */\n"
            "         if (util_dynarray_num_elements(\n"
            "                &cmdbuf->state.gfx.xfb.pending_draws,\n"
            "                struct panvk_xfb_pending_draw))\n"
            "            panvk_per_arch(cmd_flush_pending_xfb_captures)(cmdbuf);\n"
            "\n"
            "         issue_fragment_jobs(cmdbuf);\n"
            "\n"
            "         handle_deferred_queries(cmdbuf);\n"
            "      }",
        ),
    ],
    done_marker="cmd_flush_pending_xfb_captures)(cmdbuf);",
)

# 6. panvk_vX_physical_device.c: extension table entry (explicitly disabled
#    pending the cross-subqueue sync fix - see module docstring), features,
#    and properties.
patch_file(
    "panvk_vX_physical_device.c",
    [
        (
            "      .EXT_vertex_input_dynamic_state = true,\n",
            "      .EXT_vertex_input_dynamic_state = true,\n"
            "      /* VK_EXT_transform_feedback, single-stream, no GS/tess.\n"
            "       *\n"
            "       * The capture itself works: non-indexed, indexed, instanced,\n"
            "       * indirect and indexed-indirect draws all capture correctly, with\n"
            "       * counter-buffer resume, the XFB stream query, and bounds clamping\n"
            "       * against the bound buffer sizes (csf/panvk_vX_cmd_xfb.c plus\n"
            "       * panlib_xfb_setup in libpan/draw_helper.cl).\n"
            "       *\n"
            "       * It stays disabled because the gaps that remain are asserts, not\n"
            "       * graceful failures: strip/fan topologies, primitive restart with\n"
            "       * XFB active, multi-draw indirect, and\n"
            "       * vkCmdDrawIndirectByteCountEXT. Advertising the extension would\n"
            "       * turn \"unsupported\" into \"abort\" for applications that use any of\n"
            "       * them. See docs/kbase-notes.md.\n"
            "       */\n"
            "      .EXT_transform_feedback = " + XFB_EXT_ENABLED + ",\n",
        ),
        (
            "      /* VK_EXT_provoking_vertex */\n"
            "      .provokingVertexLast = true,\n"
            "      .transformFeedbackPreservesProvokingVertex = false,\n",
            "      /* VK_EXT_provoking_vertex */\n"
            "      .provokingVertexLast = true,\n"
            "      .transformFeedbackPreservesProvokingVertex = false,\n"
            "\n"
            "      /* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess -\n"
            "       * only the render-VS-epilogue capture path exists (no geometry\n"
            "       * shader emulation), so geometryStreams stays false. See\n"
            "       * docs/kbase-notes.md.\n"
            "       */\n"
            "      .transformFeedback = PAN_ARCH >= 10,\n"
            "      .geometryStreams = false,\n",
        ),
        (
            "      /* VK_EXT_provoking_vertex */\n"
            "      .provokingVertexModePerPipeline = false,\n"
            "      .transformFeedbackPreservesTriangleFanProvokingVertex = false,\n",
            "      /* VK_EXT_provoking_vertex */\n"
            "      .provokingVertexModePerPipeline = false,\n"
            "      .transformFeedbackPreservesTriangleFanProvokingVertex = false,\n"
            "\n"
            "      /* VK_EXT_transform_feedback, phase 1: single stream, no queries,\n"
            "       * no indirect-draw-from-XFB-counter, no counter-buffer resume yet.\n"
            "       */\n"
            "      .maxTransformFeedbackStreams = 1,\n"
            "      .maxTransformFeedbackBuffers = MAX_XFB_BUFFERS,\n"
            "      .maxTransformFeedbackBufferSize = UINT32_MAX,\n"
            "      .maxTransformFeedbackStreamDataSize = UINT32_MAX,\n"
            "      .maxTransformFeedbackBufferDataSize = UINT32_MAX,\n"
            "      .maxTransformFeedbackBufferDataStride = UINT32_MAX,\n"
            "      .transformFeedbackQueries = true,\n"
            "      .transformFeedbackStreamsLinesTriangles = false,\n"
            "      .transformFeedbackRasterizationStreamSelect = false,\n"
            "      .transformFeedbackDraw = true,\n",
        ),
    ],
    done_marker="EXT_transform_feedback",
)

# 7. VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: two uint64 reports per query,
#    [0] primitives written and [1] primitives generated. They differ only when
#    the bound XFB buffer was too small for a primitive, which is exactly what
#    dispatch_one_xfb_capture()'s bounds clamp computes.
patch_file(
    "panvk_vX_query_pool.c",
    [
        (
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "      reports_per_query = 1;\n"
            "      break;\n"
            "   }\n",
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "      reports_per_query = 1;\n"
            "      break;\n"
            "   }\n"
            "   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: {\n"
            "      /* [0] = primitives written, [1] = primitives generated. */\n"
            "      reports_per_query = 2;\n"
            "      break;\n"
            "   }\n",
        ),
        (
            "         case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "            if (write_results)\n"
            "               cpu_write_query_result(dst, 0, flags, src[0].value);\n"
            "            break;\n"
            "         }\n",
            "         case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "            if (write_results)\n"
            "               cpu_write_query_result(dst, 0, flags, src[0].value);\n"
            "            break;\n"
            "         }\n"
            "         case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: {\n"
            "            if (write_results) {\n"
            "               cpu_write_query_result(dst, 0, flags, src[0].value);\n"
            "               cpu_write_query_result(dst, 1, flags, src[1].value);\n"
            "            }\n"
            "            break;\n"
            "         }\n",
        ),
    ],
    done_marker="reports_per_query = 2;",
)

# 8. panvk_cmd_query.h: XFB query state + the availability helper shared with
#    the capture flush.
patch_file(
    "panvk_cmd_query.h",
    [
        (
            "struct panvk_prims_generated_query_state {\n"
            "   uint64_t syncobj;\n"
            "   uint64_t ptr;\n"
            "};\n",
            "struct panvk_prims_generated_query_state {\n"
            "   uint64_t syncobj;\n"
            "   uint64_t ptr;\n"
            "};\n"
            "\n"
            "struct panvk_cmd_buffer;\n"
            "\n"
            "/* VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT.\n"
            " *\n"
            " * deferred_syncobj carries the availability address from EndQuery to\n"
            " * cmd_flush_pending_xfb_captures() when the query is ended while captures\n"
            " * are still queued - the counters are only accumulated at flush time, so\n"
            " * signalling availability at EndQuery would mark the query ready before\n"
            " * its counts existed.\n"
            " */\n"
            "struct panvk_xfb_query_state {\n"
            "   uint64_t syncobj;\n"
            "   uint64_t ptr;\n"
            "   uint64_t deferred_syncobj;\n"
            "};\n"
            "\n"
            "void panvk_per_arch(cmd_signal_xfb_query_available)(\n"
            "   struct panvk_cmd_buffer *cmd, uint64_t syncobj);\n",
        ),
    ],
    done_marker="panvk_xfb_query_state",
)

# 9. panvk_cmd_draw.h: the live XFB query state on the graphics state.
patch_file(
    "panvk_cmd_draw.h",
    [
        (
            "   struct panvk_prims_generated_query_state prims_generated_query;\n",
            "   struct panvk_prims_generated_query_state prims_generated_query;\n"
            "   struct panvk_xfb_query_state xfb_query;\n",
        ),
    ],
    done_marker="panvk_xfb_query_state xfb_query;",
)

# 10. csf/panvk_vX_cmd_query.c: begin/end/reset for the XFB stream query.
patch_file(
    "csf/panvk_vX_cmd_query.c",
    [
        (
            "static void\n"
            "panvk_cmd_begin_prims_generated_query(\n",
            "static void\n"
            "panvk_cmd_begin_xfb_query(struct panvk_cmd_buffer *cmd,\n"
            "                          struct panvk_query_pool *pool, uint32_t query)\n"
            "{\n"
            "   uint64_t report_addr = panvk_query_report_dev_addr(pool, query);\n"
            "\n"
            "   cmd->state.gfx.xfb_query.ptr = report_addr;\n"
            "   cmd->state.gfx.xfb_query.syncobj =\n"
            "      panvk_query_available_dev_addr(pool, query);\n"
            "   cmd->state.gfx.xfb_query.deferred_syncobj = 0;\n"
            "\n"
            "   /* Both counters start from zero. */\n"
            "   struct cs_builder *b = panvk_get_cs_builder(cmd, PANVK_SUBQUEUE_COMPUTE);\n"
            "   struct cs_index addr = cs_scratch_reg64(b, 0);\n"
            "   struct cs_index zero = cs_scratch_reg64(b, 2);\n"
            "\n"
            "   cs_move64_to(b, addr, report_addr);\n"
            "   cs_move64_to(b, zero, 0);\n"
            "   cs_store64(b, zero, addr, 0);\n"
            "   cs_store64(b, zero, addr, sizeof(struct panvk_query_report));\n"
            "   cs_flush_stores(b);\n"
            "}\n"
            "\n"
            "void\n"
            "panvk_per_arch(cmd_signal_xfb_query_available)(\n"
            "   struct panvk_cmd_buffer *cmd, uint64_t syncobj)\n"
            "{\n"
            "   struct cs_builder *b = panvk_get_cs_builder(cmd, PANVK_SUBQUEUE_COMPUTE);\n"
            "   struct cs_index query_syncobj = cs_scratch_reg64(b, 0);\n"
            "   struct cs_index val = cs_scratch_reg32(b, 2);\n"
            "\n"
            "   /* Same shape as panvk_cmd_end_prims_generated_query(): the counters live\n"
            "    * in cached memory, so clean the caches before signalling availability.\n"
            "    */\n"
            "   cs_move32_to(b, val, 0);\n"
            "   cs_flush_caches(\n"
            "      b, MALI_CS_FLUSH_MODE_CLEAN, MALI_CS_FLUSH_MODE_CLEAN,\n"
            "      MALI_CS_OTHER_FLUSH_MODE_NONE, val,\n"
            "      cs_defer(SB_IMM_MASK, SB_ID(DEFERRED_FLUSH)));\n"
            "\n"
            "   cs_move32_to(b, val, 1);\n"
            "   cs_move64_to(b, query_syncobj, syncobj);\n"
            "   cs_sync32_set(b, true, MALI_CS_SYNC_SCOPE_CSG, val, query_syncobj,\n"
            "                 cs_defer(SB_MASK(DEFERRED_FLUSH), SB_ID(DEFERRED_SYNC)));\n"
            "}\n"
            "\n"
            "static void\n"
            "panvk_cmd_end_xfb_query(struct panvk_cmd_buffer *cmd,\n"
            "                        struct panvk_query_pool *pool, uint32_t query)\n"
            "{\n"
            "   uint64_t syncobj = panvk_query_available_dev_addr(pool, query);\n"
            "\n"
            "   cmd->state.gfx.xfb_query.ptr = 0;\n"
            "   cmd->state.gfx.xfb_query.syncobj = 0;\n"
            "\n"
            "   /* Captures queued but not yet flushed still have to add their counts, and\n"
            "    * that only happens at CmdEndRendering. Signalling availability now would\n"
            "    * publish the query before its counters were written, so hand the\n"
            "    * availability write to the flush instead.\n"
            "    */\n"
            "   if (util_dynarray_num_elements(&cmd->state.gfx.xfb.pending_draws,\n"
            "                                  struct panvk_xfb_pending_draw)) {\n"
            "      cmd->state.gfx.xfb_query.deferred_syncobj = syncobj;\n"
            "      return;\n"
            "   }\n"
            "\n"
            "   panvk_per_arch(cmd_signal_xfb_query_available)(cmd, syncobj);\n"
            "}\n"
            "\n"
            "static void\n"
            "panvk_cmd_begin_prims_generated_query(\n",
        ),
        (
            "   case VK_QUERY_TYPE_OCCLUSION:\n"
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "      panvk_cmd_reset_queries(cmd, pool, firstQuery, queryCount);\n"
            "      break;\n"
            "   }\n",
            "   case VK_QUERY_TYPE_OCCLUSION:\n"
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT:\n"
            "   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: {\n"
            "      panvk_cmd_reset_queries(cmd, pool, firstQuery, queryCount);\n"
            "      break;\n"
            "   }\n",
        ),
        (
            "   /* TODO: transform feedback */\n"
            "   assert(index == 0);\n"
            "\n"
            "   switch (pool->vk.query_type) {\n"
            "   case VK_QUERY_TYPE_OCCLUSION: {\n"
            "      panvk_cmd_begin_occlusion_query(cmd, pool, query, flags);\n"
            "      break;\n"
            "   }\n"
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "      panvk_cmd_begin_prims_generated_query(cmd, pool, query, flags);\n"
            "      break;\n"
            "   }\n",
            "   /* Only stream 0 exists without a geometry shader, and geometryStreams is\n"
            "    * reported as false, so a nonzero index is invalid usage.\n"
            "    */\n"
            "   assert(index == 0);\n"
            "\n"
            "   switch (pool->vk.query_type) {\n"
            "   case VK_QUERY_TYPE_OCCLUSION: {\n"
            "      panvk_cmd_begin_occlusion_query(cmd, pool, query, flags);\n"
            "      break;\n"
            "   }\n"
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "      panvk_cmd_begin_prims_generated_query(cmd, pool, query, flags);\n"
            "      break;\n"
            "   }\n"
            "   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: {\n"
            "      panvk_cmd_begin_xfb_query(cmd, pool, query);\n"
            "      break;\n"
            "   }\n",
        ),
        (
            "   /* TODO: transform feedback */\n"
            "   assert(index == 0);\n"
            "\n"
            "   switch (pool->vk.query_type) {\n"
            "   case VK_QUERY_TYPE_OCCLUSION: {\n"
            "      panvk_cmd_end_occlusion_query(cmd, pool, query);\n"
            "      break;\n"
            "   }\n"
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "      panvk_cmd_end_prims_generated_query(cmd, pool, query);\n"
            "      break;\n"
            "   }\n",
            "   assert(index == 0);\n"
            "\n"
            "   switch (pool->vk.query_type) {\n"
            "   case VK_QUERY_TYPE_OCCLUSION: {\n"
            "      panvk_cmd_end_occlusion_query(cmd, pool, query);\n"
            "      break;\n"
            "   }\n"
            "   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT: {\n"
            "      panvk_cmd_end_prims_generated_query(cmd, pool, query);\n"
            "      break;\n"
            "   }\n"
            "   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: {\n"
            "      panvk_cmd_end_xfb_query(cmd, pool, query);\n"
            "      break;\n"
            "   }\n",
        ),
    ],
    done_marker="panvk_cmd_begin_xfb_query",
)

# 11. meson.build: register csf/panvk_vX_cmd_xfb.c (copied in by mesa-backend-sync).
patch_file(
    "meson.build",
    [
        (
            "  'csf/panvk_vX_cmd_precomp.c',\n",
            "  'csf/panvk_vX_cmd_precomp.c',\n"
            "  'csf/panvk_vX_cmd_xfb.c',\n",
        ),
    ],
    done_marker="csf/panvk_vX_cmd_xfb.c",
)

# 12. libpan: the helper kernel that clamps a capture to what actually fits.
#     The clamp has to consult a GPU-resident write position, which means
#     dividing by a stride at dispatch time - and on PAN_ARCH 10 the command
#     stream cannot divide (cs_udiv32 and the rest of the register-register
#     arithmetic are #if PAN_ARCH >= 13 in genxml/cs_builder.h). So the
#     arithmetic lives in a single-invocation kernel and the command stream
#     only loads the result into JOB_SIZE_X. Mirrors Asahi hk's
#     setup_xfb_buffer(); see docs/kbase-notes.md.
#
#     draw_helper.cl is already in libpan/meson.build, so no build plumbing is
#     needed - the panlib_xfb_setup_args struct and dispatch macro generate
#     themselves.
patch_file(
    "draw_helper.h",
    [
        (
            "#pragma once\n",
            "#pragma once\n"
            "\n"
            "/* One bound XFB buffer, as panlib_xfb_setup() sees it.\n"
            " *\n"
            " * size_bytes is the bound size of the buffer; the *used* part of it lives in\n"
            " * GPU memory, so the clamp has to happen on the GPU. Bytes rather than capture\n"
            " * slots because that is the unit a counter buffer uses, which keeps\n"
            " * Begin/End's copies free of arithmetic the command stream cannot do.\n"
            " * push_uniform is where to write the resolved capture base address\n"
            " * (base + offset) so the capture shader's xfb.buffer_addrs[] sysval picks it\n"
            " * up - 0 to skip.\n"
            " */\n"
            "struct panlib_xfb_buffer_desc {\n"
            "   uint64_t base;\n"
            "   uint64_t push_uniform;\n"
            "   uint32_t size_bytes;\n"
            "   uint32_t stride;\n"
            "};\n"
            "\n",
        ),
    ],
    done_marker="panlib_xfb_buffer_desc",
    base=LIBPAN_DIR,
)

patch_file(
    "draw_helper.cl",
    [
        (
            "KERNEL(1)\n"
            "panlib_update_prims_generated_query_indirect(\n",
            "/* vkCmdDrawIndirectByteCountEXT: turn a captured byte count into a draw.\n"
            " *\n"
            " * vertexCount = (counter - counterOffset) / vertexStride. The division is by a\n"
            " * runtime value, which the command stream cannot do on this arch, so it\n"
            " * happens here and the result is written as an ordinary VkDrawIndirectCommand\n"
            " * for the normal indirect draw path to consume.\n"
            " */\n"
            "KERNEL(1)\n"
            "panlib_xfb_byte_count_draw(global uint32_t *cmd, constant uint32_t *counter,\n"
            "                           uint32_t counter_offset, uint32_t vertex_stride,\n"
            "                           uint32_t instance_count, uint32_t first_instance)\n"
            "{\n"
            "   uint32_t bytes = *counter;\n"
            "   uint32_t avail = bytes > counter_offset ? bytes - counter_offset : 0;\n"
            "\n"
            "   cmd[0] = vertex_stride ? avail / vertex_stride : 0; /* vertexCount */\n"
            "   cmd[1] = instance_count;\n"
            "   cmd[2] = 0;                                         /* firstVertex */\n"
            "   cmd[3] = first_instance;\n"
            "}\n"
            "\n"
            "/* One index-buffer element, whatever its width. */\n"
            "static uint32_t\n"
            "xfb_load_index(uint64_t base, uint32_t i, uint32_t index_size)\n"
            "{\n"
            "   if (index_size == 4)\n"
            "      return ((constant uint32_t *)base)[i];\n"
            "   if (index_size == 2)\n"
            "      return ((constant uint16_t *)base)[i];\n"
            "   return ((constant uchar *)base)[i];\n"
            "}\n"
            "\n"
            "/* Walk the index buffer once, resolving every capture slot to the input vertex\n"
            " * it should read, and return the number of primitives assembled.\n"
            " *\n"
            " * Primitive restart splits the stream into runs; within a run the topology\n"
            " * rules apply exactly as they do without restart, including the triangle-strip\n"
            " * parity - which restarts with each run, hence tracking the run length rather\n"
            " * than the global position.\n"
            " */\n"
            "static uint32_t\n"
            "xfb_build_restart_table(global uint32_t *table, uint64_t index_base,\n"
            "                        uint32_t index_count, uint32_t index_size,\n"
            "                        uint32_t restart_index, uint32_t topology,\n"
            "                        uint32_t vpp)\n"
            "{\n"
            "   uint32_t prims = 0;\n"
            "   uint32_t run = 0;        /* vertices seen in the current run */\n"
            "   uint32_t run_first = 0;  /* index position the run started at */\n"
            "\n"
            "   for (uint32_t i = 0; i < index_count; i++) {\n"
            "      uint32_t idx = xfb_load_index(index_base, i, index_size);\n"
            "\n"
            "      if (idx == restart_index) {\n"
            "         run = 0;\n"
            "         continue;\n"
            "      }\n"
            "\n"
            "      if (run == 0)\n"
            "         run_first = i;\n"
            "      run++;\n"
            "\n"
            "      bool emit = false;\n"
            "      uint32_t a = 0, b = 0, c = 0;\n"
            "\n"
            "      switch (topology) {\n"
            "      case 0: /* POINT_LIST */\n"
            "         emit = true;\n"
            "         a = i;\n"
            "         break;\n"
            "      case 1: /* LINE_LIST */\n"
            "         emit = (run % 2) == 0;\n"
            "         a = i - 1; b = i;\n"
            "         break;\n"
            "      case 2: /* LINE_STRIP */\n"
            "         emit = run >= 2;\n"
            "         a = i - 1; b = i;\n"
            "         break;\n"
            "      case 3: /* TRIANGLE_LIST */\n"
            "         emit = (run % 3) == 0;\n"
            "         a = i - 2; b = i - 1; c = i;\n"
            "         break;\n"
            "      case 4: /* TRIANGLE_STRIP - odd triangles swap their first two */\n"
            "         emit = run >= 3;\n"
            "         if ((run & 1) == 0) { a = i - 1; b = i - 2; }\n"
            "         else                { a = i - 2; b = i - 1; }\n"
            "         c = i;\n"
            "         break;\n"
            "      default: /* TRIANGLE_FAN - every triangle starts at the run's first */\n"
            "         emit = run >= 3;\n"
            "         a = run_first; b = i - 1; c = i;\n"
            "         break;\n"
            "      }\n"
            "\n"
            "      if (!emit)\n"
            "         continue;\n"
            "\n"
            "      uint32_t slot = prims * vpp;\n"
            "      table[slot] = xfb_load_index(index_base, a, index_size);\n"
            "      if (vpp > 1)\n"
            "         table[slot + 1] = xfb_load_index(index_base, b, index_size);\n"
            "      if (vpp > 2)\n"
            "         table[slot + 2] = xfb_load_index(index_base, c, index_size);\n"
            "\n"
            "      prims++;\n"
            "   }\n"
            "\n"
            "   return prims;\n"
            "}\n"
            "\n"
            "/* VK_EXT_transform_feedback: clamp one capture to what actually fits, and do\n"
            " * the bookkeeping that depends on the result.\n"
            " *\n"
            " * Single invocation on purpose. Everything here reads and writes GPU-resident\n"
            " * counters that only this dispatch touches, so plain += is correct and no\n"
            " * atomics are needed - the same reason Asahi's setup_xfb_buffer() can do it.\n"
            " *\n"
            " * Transform feedback discards whole primitives rather than truncating them,\n"
            " * hence the round-down by verts_per_prim. The write position is in capture\n"
            " * slots rather than bytes so that advancing it is a plain add; only the\n"
            " * resolved base address needs the stride multiply.\n"
            " */\n"
            "KERNEL(1)\n"
            "panlib_xfb_setup(global uint32_t *offsets,\n"
            "                 constant struct panlib_xfb_buffer_desc *descs,\n"
            "                 uint32_t desc_count, uint32_t direct_vertex_count,\n"
            "                 uint32_t direct_instance_count, uint32_t topology,\n"
            "                 global uint32_t *out_slots, global uint64_t *query,\n"
            "                 constant uint32_t *indirect,\n"
            "                 global uint32_t *num_vertices_pu,\n"
            "                 global uint64_t *index_buffer_pu, uint64_t index_buffer_base,\n"
            "                 uint32_t index_size, global uint32_t *slot_table,\n"
            "                 uint32_t restart_index)\n"
            "{\n"
            "   /* Keep in sync with enum panvk_xfb_topology in panvk_shader.h - the\n"
            "    * ordering is what makes verts_per_prim fall out of the value.\n"
            "    */\n"
            "   uint32_t vpp = topology == 0 ? 1 : (topology <= 2 ? 2 : 3);\n"
            "\n"
            "   /* VkDrawIndirectCommand and VkDrawIndexedIndirectCommand both start with\n"
            "    * the vertex/index count followed by the instance count, so one read\n"
            "    * serves either.\n"
            "    */\n"
            "   uint32_t vertex_count = indirect ? indirect[0] : direct_vertex_count;\n"
            "   uint32_t instance_count = indirect ? indirect[1] : direct_instance_count;\n"
            "\n"
            "   /* Primitives assembled from vertex_count inputs. Strips and fans share\n"
            "    * vertices between adjacent primitives, so this is not a simple divide.\n"
            "    */\n"
            "   uint32_t prims;\n"
            "   if (slot_table) {\n"
            "      /* Primitive restart: the count and the per-slot vertex mapping both\n"
            "       * depend on where the restart indices fall, so resolve them together.\n"
            "       */\n"
            "      uint64_t walk_base =\n"
            "         index_buffer_base +\n"
            "         (indirect ? (uint64_t)indirect[2] * index_size : 0);\n"
            "\n"
            "      prims = xfb_build_restart_table(slot_table, walk_base, vertex_count,\n"
            "                                      index_size, restart_index, topology,\n"
            "                                      vpp);\n"
            "   } else if (topology == 2) {    /* LINE_STRIP */\n"
            "      prims = vertex_count >= 2 ? vertex_count - 1 : 0;\n"
            "   } else if (topology == 4 || topology == 5) {  /* TRIANGLE_STRIP / _FAN */\n"
            "      prims = vertex_count >= 3 ? vertex_count - 2 : 0;\n"
            "   } else {                       /* the LIST topologies */\n"
            "      prims = vertex_count / vpp;\n"
            "   }\n"
            "\n"
            "   /* Transform feedback captures assembled primitives, so a strip emits each\n"
            "    * shared vertex once per primitive that uses it - this is larger than\n"
            "    * vertex_count for strips and fans.\n"
            "    */\n"
            "   uint32_t captured_per_instance = prims * vpp;\n"
            "   uint32_t generated_slots = captured_per_instance * instance_count;\n"
            "   uint32_t slots = generated_slots;\n"
            "\n"
            "   /* The capture shader recovers instance = slot / num_vertices and then maps\n"
            "    * the remainder back to an input vertex, so this sysval is the *captured*\n"
            "    * count per instance. The host cannot supply it for a strip any more than\n"
            "    * it can for an indirect draw, so it is always patched from here.\n"
            "    */\n"
            "   if (num_vertices_pu)\n"
            "      *num_vertices_pu = captured_per_instance;\n"
            "\n"
            "   /* Indexed indirect: firstIndex is word 2 of VkDrawIndexedIndirectCommand,\n"
            "    * and the capture shader expects xfb.index_buffer to already point at the\n"
            "    * draw's first index. Only set for an indexed indirect draw.\n"
            "    */\n"
            "   if (indirect && index_buffer_pu)\n"
            "      *index_buffer_pu = index_buffer_base + (uint64_t)indirect[2] * index_size;\n"
            "\n"
            "   /* Tightest constraint across every buffer this capture writes. The write\n"
            "    * position is a byte offset, so converting it to capture slots is a\n"
            "    * division - which is why this lives in a kernel rather than the command\n"
            "    * stream.\n"
            "    */\n"
            "   for (uint32_t i = 0; i < desc_count; i++) {\n"
            "      uint32_t used = offsets[i];\n"
            "      uint32_t cap = descs[i].size_bytes;\n"
            "      uint32_t remaining_bytes = cap > used ? cap - used : 0;\n"
            "      uint32_t remaining = remaining_bytes / descs[i].stride;\n"
            "\n"
            "      remaining -= remaining % vpp;\n"
            "      slots = min(slots, remaining);\n"
            "   }\n"
            "\n"
            "   *out_slots = slots;\n"
            "\n"
            "   for (uint32_t i = 0; i < desc_count; i++) {\n"
            "      /* Resolve the capture base *before* advancing: this draw starts where\n"
            "       * the previous one stopped.\n"
            "       */\n"
            "      if (descs[i].push_uniform) {\n"
            "         global uint64_t *dst = (global uint64_t *)descs[i].push_uniform;\n"
            "         *dst = descs[i].base + offsets[i];\n"
            "      }\n"
            "\n"
            "      offsets[i] += slots * descs[i].stride;\n"
            "   }\n"
            "\n"
            "   /* [0] primitives written, [1] primitives generated. They differ exactly\n"
            "    * when the capture did not fit, which is what an application uses this\n"
            "    * query to detect.\n"
            "    */\n"
            "   if (query) {\n"
            "      query[0] += slots / vpp;\n"
            "      query[1] += generated_slots / vpp;\n"
            "   }\n"
            "}\n"
            "\n"
            "KERNEL(1)\n"
            "panlib_update_prims_generated_query_indirect(\n",
        ),
    ],
    done_marker="panlib_xfb_setup",
    base=LIBPAN_DIR,
)


# 12. csf/panvk_vX_cmd_buffer.c: free the growable capture queue. Reset
#     memsets the whole state struct, which would otherwise strand the
#     allocation; a zeroed util_dynarray is a valid empty one, so nothing
#     needs re-initialising afterwards.
patch_file(
    "csf/panvk_vX_cmd_buffer.c",
    [
        (
            "   memset(&cmdbuf->state, 0, sizeof(cmdbuf->state));\n"
            "   init_cs_builders(cmdbuf);",
            "   /* Frees the heap allocation before the memset below strands it. A\n"
            "    * zeroed util_dynarray is a valid empty one, so nothing re-inits it.\n"
            "    */\n"
            "   util_dynarray_fini(&cmdbuf->state.gfx.xfb.pending_draws);\n"
            "\n"
            "   memset(&cmdbuf->state, 0, sizeof(cmdbuf->state));\n"
            "   init_cs_builders(cmdbuf);",
        ),
        (
            "   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++)\n"
            "      cs_builder_fini(&cmdbuf->state.cs[i].builder);\n"
            "\n"
            "   panvk_pool_cleanup",
            "   util_dynarray_fini(&cmdbuf->state.gfx.xfb.pending_draws);\n"
            "\n"
            "   for (uint32_t i = 0; i < ARRAY_SIZE(cmdbuf->state.cs); i++)\n"
            "      cs_builder_fini(&cmdbuf->state.cs[i].builder);\n"
            "\n"
            "   panvk_pool_cleanup",
        ),
    ],
    done_marker="util_dynarray_fini(&cmdbuf->state.gfx.xfb.pending_draws)",
)


print("panvk xfb-phase1 patch applied")
