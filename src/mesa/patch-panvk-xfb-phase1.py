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

What's left: with the crash fixed, the capture runs but writes nothing -
nir_load_raw_vertex_id compiles to a hardware-preloaded register
(BI_PRELOAD_VERTEX_ID) that firmware only populates correctly for real
VERTEX/IDVS jobs; a COMPUTE-declared job gets something else preloaded
into that slot, so the shader's own vertex/instance index - and therefore
both its attribute-fetch address and its XFB store address - is wrong.
This needs a NIR lowering pass sourcing vertex/instance index from
whatever registers a COMPUTE job actually receives its invocation index
in, replacing the compiler's default (VERTEX-hardware-only) handling for
this variant specifically. See docs/kbase-notes.md for the full trace.
.EXT_transform_feedback stays false until this lands and
tests/render_xfb_probe passes end to end.

Not kbase-specific - like patch-panvk-null-device-destroy.py, this is
genuine upstream PanVK/Mesa capability work, a candidate for upstreaming
once the remaining issue is found and fixed and this is verified
end-to-end via tests/render_xfb_probe. Applied as a script rather than a
diff because upstream moves. Idempotent.

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
            "      uint32_t num_vertices;\n"
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
            "         uint32_t vertex_count, instance_count, vertex_base;\n"
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
            "                                    &shader->desc_info, shader->xfb_variant);\n"
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
    done_marker="load_xfb_address",
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
            "      cmdbuf->state.gfx.xfb.pending_draw_count = n + 1;\n"
            "   }\n"
            "}",
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
            "      .transformFeedbackQueries = false,\n"
            "      .transformFeedbackStreamsLinesTriangles = false,\n"
            "      .transformFeedbackRasterizationStreamSelect = false,\n"
            "      .transformFeedbackDraw = false,\n",
        ),
    ],
    done_marker="EXT_transform_feedback",
)

# 7. meson.build: register csf/panvk_vX_cmd_xfb.c (copied in by mesa-backend-sync).
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
