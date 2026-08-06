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


def patch_file(relpath, edits, done_marker):
    path = os.path.join(VULKAN_DIR, relpath)
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
            "#define MAX_XFB_BUFFERS 4\n",
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
            "      uint32_t num_vertices;\n"
            "      uint32_t index_size;\n"
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
            "    * Only set on a MESA_SHADER_VERTEX panvk_shader when the SPIR-V\n"
            "    * module has XFB-decorated outputs (nir->xfb_info != NULL at\n"
            "    * compile time). A separate, heap-allocated variant rather than\n"
            "    * growing PANVK_VS_VARIANTS unconditionally, since every other\n"
            "    * vertex shader has no use for it - see docs/kbase-notes.md.\n"
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
            "    * Non-indexed, non-indirect vkCmdDraw only - see\n"
            "    * docs/kbase-notes.md for the full phase-1 scope.\n"
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
            "      /* Host-computed at record time for phase 1's non-indirect case. */\n"
            "      uint64_t buffer_offset[MAX_XFB_BUFFERS];\n"
            "\n"
            "      /* The capture compute dispatch cannot run immediately in CmdDraw:\n"
            "       * flush_tiling() - the only thing that signals PANVK_SUBQUEUE_VERTEX_TILER's\n"
            "       * syncobj and gives PANVK_SUBQUEUE_COMPUTE something valid to wait on -\n"
            "       * runs once per render pass, from CmdEndRendering, not per draw. Draws\n"
            "       * recorded while XFB is active are queued here and the actual dispatches\n"
            "       * fire from CmdEndRendering, after flush_tiling(). See docs/kbase-notes.md.\n"
            "       */\n"
            "      struct {\n"
            "         uint32_t vertex_count, instance_count;\n"
            "\n"
            "         /* Non-indexed: firstVertex. Indexed: vertexOffset. Either way it\n"
            "          * is what GLOBAL_ATTRIBUTE_OFFSET gets programmed with, which is\n"
            "          * exactly the bias hardware attribute fetch applies.\n"
            "          */\n"
            "         int32_t vertex_base;\n"
            "\n"
            "         /* Phase 2: 0 for a non-indexed draw, otherwise the address of the\n"
            "          * draw's first index (already biased by firstIndex).\n"
            "          */\n"
            "         uint64_t index_buffer;\n"
            "         uint32_t index_size;\n"
            "\n"
            "         /* VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: snapshotted per\n"
            "          * draw, because the query may be ended before CmdEndRendering\n"
            "          * flushes these captures and clears the live state. 0 = no query.\n"
            "          * verts_per_prim is 1/2/3 (point/line/triangle list; strips and\n"
            "          * fans are rejected at record time - see xfb_verts_per_prim()).\n"
            "          */\n"
            "         uint64_t query_ptr;\n"
            "         uint32_t verts_per_prim;\n"
            "      } pending_draws[16];\n"
            "      unsigned pending_draw_count;\n"
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
    ],
    done_marker="pending_draws[16]",
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
            "/* VK_EXT_transform_feedback, phase 1: the XFB-capture variant is compiled\n"
            " * as MESA_SHADER_VERTEX (required for bifrost_postprocess_nir()'s\n"
            " * VS-specific lowering) but launched via cs_run_compute as a\n"
            " * MALI_SHADER_STAGE_COMPUTE job (see docs/kbase-notes.md for why - this\n"
            " * fixed a real VK_ERROR_DEVICE_LOST). nir_load_vertex_id/raw_vertex_id/\n"
            " * instance_id all compile to hardware-preloaded registers\n"
            " * (BI_PRELOAD_VERTEX_ID/INSTANCE_ID) that firmware only populates\n"
            " * correctly for real VERTEX/IDVS jobs - a COMPUTE job gets a different\n"
            " * value preloaded into the same slot. Both attribute fetch\n"
            " * (panvk_lower_load_vs_input above, via nir_load_vertex_id) and\n"
            " * nir_lower_xfb_to_stores's generated store address (via\n"
            " * nir_load_raw_vertex_id/nir_load_instance_id) depend on this, so both\n"
            " * silently compute wrong addresses without ever faulting.\n"
            " *\n"
            " * Fix: replace all three with reads of nir_load_workgroup_id() instead.\n"
            " * The XFB capture dispatch fixes WG_SIZE at 1x1x1 (dispatch_one_xfb_capture(),\n"
            " * csf/panvk_vX_cmd_xfb.c) specifically so each workgroup is exactly one\n"
            " * invocation - which makes workgroup_id.x/.y directly equal the\n"
            " * vertex/instance index the JOB_SIZE_X/Y grid was set up to represent, no\n"
            " * further math needed. Must run after both nir_lower_xfb_to_stores and\n"
            " * panvk_lower_load_vs_input have generated the intrinsics being replaced.\n"
            " *\n"
            " * NOTE (2026-08-06): this fix is necessary but confirmed NOT sufficient -\n"
            " * even with this lowering applied, tests/render_xfb_probe's capture\n"
            " * buffer still reads back as untouched poison, and forcing a constant\n"
            " * (vertex-id-independent) store address makes no difference either. The\n"
            " * store never happens at all, pointing at something upstream of\n"
            " * addressing entirely - most likely nir_load_xfb_address's FAU/\n"
            " * push-uniforms resolution, or the compute job not executing real work\n"
            " * for this shader/dispatch combination. See docs/kbase-notes.md.\n"
            " */\n"
            "static bool\n"
            "panvk_lower_xfb_compute_dispatch_ids(nir_builder *b, nir_intrinsic_instr *intr,\n"
            "                                     UNUSED void *data)\n"
            "{\n"
            "   unsigned component;\n"
            "   switch (intr->intrinsic) {\n"
            "   /* NOTE: nir_intrinsic_load_vertex_id is deliberately NOT handled here.\n"
            "    * It is the attribute-fetch index, which for an indexed draw must come\n"
            "    * from the index buffer rather than the invocation number - see\n"
            "    * panvk_lower_xfb_vertex_id() below. raw_vertex_id is the XFB store\n"
            "    * slot and stays sequential for indexed and non-indexed draws alike.\n"
            "    */\n"
            "   case nir_intrinsic_load_raw_vertex_id:\n"
            "      component = 0;\n"
            "      break;\n"
            "   case nir_intrinsic_load_instance_id:\n"
            "      component = 1;\n"
            "      break;\n"
            "   default:\n"
            "      return false;\n"
            "   }\n"
            "\n"
            "   b->cursor = nir_before_instr(&intr->instr);\n"
            "   nir_def *wg_id = nir_load_workgroup_id(b);\n"
            "   nir_def *val = nir_channel(b, wg_id, component);\n"
            "   if (val->bit_size != intr->def.bit_size)\n"
            "      val = nir_u2uN(b, val, intr->def.bit_size);\n"
            "   nir_def_replace(&intr->def, val);\n"
            "   return true;\n"
            "}\n"
            "\n"
            "/* VK_EXT_transform_feedback phase 2: the attribute-fetch vertex index.\n"
            " *\n"
            " * For a non-indexed draw this is just the sequential invocation number\n"
            " * (workgroup_id.x), same as the XFB store slot. For an indexed draw it is\n"
            " * index_buffer[i] - the xfb.index_buffer sysval already points at the\n"
            " * draw's first index, so no firstIndex maths is needed here, and\n"
            " * vertexOffset is applied by GLOBAL_ATTRIBUTE_OFFSET in hardware rather\n"
            " * than in the shader.\n"
            " *\n"
            " * The same compiled variant serves both indexed and non-indexed draws, so\n"
            " * this is a real runtime branch on the sysval rather than a compile-time\n"
            " * choice - a bcsel would not do, since the index load must not happen at\n"
            " * all when there is no index buffer to load from.\n"
            " *\n"
            " * NOTE (PAN_ARCH < 9): there attribute fetch uses raw_vertex_id, which is\n"
            " * also the XFB store slot, so the two could not be told apart. Irrelevant\n"
            " * today - the whole XFB path is gated on PAN_ARCH >= 10.\n"
            " */\n"
            "static nir_def *\n"
            "build_xfb_attrib_vertex_id(nir_builder *b)\n"
            "{\n"
            "   nir_def *seq = nir_channel(b, nir_load_workgroup_id(b), 0);\n"
            "   nir_def *index_buf = load_sysval(b, graphics, 64, xfb.index_buffer);\n"
            "\n"
            "   /* Each arm needs its own nir_def *: nir_if_phi() takes the value produced\n"
            "    * by each side, so reusing one variable would feed it the same def twice.\n"
            "    */\n"
            "   nir_def *from_index, *v32, *v16, *v8, *narrow;\n"
            "\n"
            "   nir_push_if(b, nir_ine_imm(b, index_buf, 0));\n"
            "   {\n"
            "      nir_def *index_size = load_sysval(b, graphics, 32, xfb.index_size);\n"
            "      nir_def *addr =\n"
            "         nir_iadd(b, index_buf, nir_u2u64(b, nir_imul(b, seq, index_size)));\n"
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
            "      /* Non-indexed: the invocation number is the attribute index. */\n"
            "   }\n"
            "   nir_pop_if(b, NULL);\n"
            "\n"
            "   return nir_if_phi(b, from_index, seq);\n"
            "}\n"
            "\n"
            "static bool\n"
            "panvk_lower_xfb_vertex_id(nir_shader *nir)\n"
            "{\n"
            "   nir_function_impl *impl = nir_shader_get_entrypoint(nir);\n"
            "   bool has_vertex_id = false;\n"
            "\n"
            "   nir_foreach_block(block, impl) {\n"
            "      nir_foreach_instr(instr, block) {\n"
            "         if (instr->type != nir_instr_type_intrinsic)\n"
            "            continue;\n"
            "         if (nir_instr_as_intrinsic(instr)->intrinsic ==\n"
            "             nir_intrinsic_load_vertex_id) {\n"
            "            has_vertex_id = true;\n"
            "            break;\n"
            "         }\n"
            "      }\n"
            "      if (has_vertex_id)\n"
            "         break;\n"
            "   }\n"
            "\n"
            "   if (!has_vertex_id) {\n"
            "      nir_no_progress(impl);\n"
            "      return false;\n"
            "   }\n"
            "\n"
            "   /* Build the value once, at the very top of main: adding control flow at\n"
            "    * an arbitrary cursor from inside an intrinsics-pass callback, while\n"
            "    * that pass is walking the block being split, is not safe.\n"
            "    */\n"
            "   nir_builder b = nir_builder_at(nir_before_impl(impl));\n"
            "   nir_def *vertex_id = build_xfb_attrib_vertex_id(&b);\n"
            "\n"
            "   nir_foreach_block_safe(block, impl) {\n"
            "      nir_foreach_instr_safe(instr, block) {\n"
            "         if (instr->type != nir_instr_type_intrinsic)\n"
            "            continue;\n"
            "\n"
            "         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);\n"
            "         if (intr->intrinsic != nir_intrinsic_load_vertex_id)\n"
            "            continue;\n"
            "\n"
            "         nir_def *val = vertex_id;\n"
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
            "   if (lower_compute_dispatch_ids) {\n"
            "      NIR_PASS(_, nir, nir_shader_intrinsics_pass,\n"
            "               panvk_lower_xfb_compute_dispatch_ids, nir_metadata_control_flow,\n"
            "               NULL);\n"
            "      NIR_PASS(_, nir, panvk_lower_xfb_vertex_id);\n"
            "   }\n",
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
    done_marker="panvk_lower_xfb_compute_dispatch_ids",
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
            "      unsigned n = cmdbuf->state.gfx.xfb.pending_draw_count;\n"
            "      assert(n < ARRAY_SIZE(cmdbuf->state.gfx.xfb.pending_draws));\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].vertex_count = vertexCount;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].instance_count = instanceCount;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].vertex_base = firstVertex;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].index_buffer = 0;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].index_size = 0;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].query_ptr =\n"
            "         cmdbuf->state.gfx.xfb_query.ptr;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].verts_per_prim =\n"
            "         xfb_verts_per_prim(cmdbuf);\n"
            "      cmdbuf->state.gfx.xfb.pending_draw_count = n + 1;\n"
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
            "      unsigned n = cmdbuf->state.gfx.xfb.pending_draw_count;\n"
            "      assert(n < ARRAY_SIZE(cmdbuf->state.gfx.xfb.pending_draws));\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].vertex_count = indexCount;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].instance_count = instanceCount;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].vertex_base = vertexOffset;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].index_buffer =\n"
            "         gfx->ib.dev_addr + ((uint64_t)firstIndex * gfx->ib.index_size);\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].index_size = gfx->ib.index_size;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].query_ptr =\n"
            "         cmdbuf->state.gfx.xfb_query.ptr;\n"
            "      cmdbuf->state.gfx.xfb.pending_draws[n].verts_per_prim =\n"
            "         xfb_verts_per_prim(cmdbuf);\n"
            "      cmdbuf->state.gfx.xfb.pending_draw_count = n + 1;\n"
            "   }\n"
            "}\n",
        ),
        # The topology helper both queueing sites use.
        (
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount,",
            "/* VK_EXT_transform_feedback: vertices per primitive for the current\n"
            " * topology.\n"
            " *\n"
            " * The capture emits one captured vertex per *input* vertex, which matches\n"
            " * transform feedback semantics only for LIST topologies. A triangle strip\n"
            " * of N vertices assembles N-2 triangles and must capture 3*(N-2) vertices,\n"
            " * with vertices duplicated across primitives; this implementation would\n"
            " * capture N. Rather than silently writing wrong data, strips and fans are\n"
            " * rejected here. Supporting them needs the capture grid sized by assembled\n"
            " * primitives and the store slot derived from the primitive/vertex pair -\n"
            " * the same machinery indirect draws will need.\n"
            " *\n"
            " * Restricting to lists also keeps the primitive count the XFB query reports\n"
            " * a plain divide, with no gallium u_prim.h dependency in the CSF path.\n"
            " */\n"
            "static uint32_t\n"
            "xfb_verts_per_prim(const struct panvk_cmd_buffer *cmdbuf)\n"
            "{\n"
            "   switch (cmdbuf->vk.dynamic_graphics_state.ia.primitive_topology) {\n"
            "   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:\n"
            "      return 1;\n"
            "   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:\n"
            "      return 2;\n"
            "   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:\n"
            "      return 3;\n"
            "   default:\n"
            "      assert(!\"VK_EXT_transform_feedback: only point/line/triangle LIST \"\n"
            "              \"topologies are supported - strips and fans would need the \"\n"
            "              \"capture to be sized by assembled primitives\");\n"
            "      return 0;\n"
            "   }\n"
            "}\n"
            "\n"
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount,",
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
            "         if (cmdbuf->state.gfx.xfb.pending_draw_count)\n"
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
            "      /* VK_EXT_transform_feedback, phase 1 (single-stream, no GS/tess):\n"
            "       * command-buffer state, the two-variant shader compile, and the\n"
            "       * compute dispatch all exist (csf/panvk_vX_cmd_xfb.c), but the\n"
            "       * dispatch still faults (VK_ERROR_DEVICE_LOST) - suspected missing\n"
            "       * cross-subqueue synchronization between PANVK_SUBQUEUE_COMPUTE\n"
            "       * (where it runs) and PANVK_SUBQUEUE_VERTEX_TILER (where the draw\n"
            "       * it depends on runs). Leave disabled until fixed and verified on\n"
            "       * real hardware - see docs/kbase-notes.md.\n"
            "       */\n"
            "      .EXT_transform_feedback = false,\n",
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
            "      .transformFeedbackDraw = false,\n",
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
            "   if (cmd->state.gfx.xfb.pending_draw_count) {\n"
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

print("panvk xfb-phase1 patch applied")
