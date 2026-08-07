#!/usr/bin/env python3
"""VK_EXT_transform_feedback: split a render pass at a backward dependency.

Fixes dEQP-VK.transform_feedback.simple.backward_dependency* (8 cases) and
draw_indirect_counter_resubmit (1). Design and the evidence behind every
choice here: docs/xfb-render-pass-split.md.

THE PROBLEM. These tests record, inside one render pass:

    Begin -> draw -> End(tfc) -> barrier -> Begin(tfc)
          -> vkCmdDrawIndirectByteCountEXT(tfc) -> End

vkCmdDrawIndirectByteCountEXT derives its OWN draw's vertex count from tfc,
on PANVK_SUBQUEUE_VERTEX_TILER, in the middle of the render pass. The value
it needs is produced by the first pair's capture, and a capture cannot run
until flush_tiling() has signalled the VERTEX_TILER syncobj - which happens
once, at CmdEndRendering. The consumer runs before the producer, so the draw
reads zero and draws nothing. This is circular within one tiling batch; no
reordering fixes it, the render pass has to be cut at the dependency.

THE FIX. On recording a vkCmdDrawIndirectByteCountEXT whose counter buffer
still has a writeback pending from earlier in this same render pass: close
the tiling batch early (flush_tiling + flush every pending capture and
counter op, so tfc becomes final), run fragment work for the batch just
closed storing to spill buffers instead of the render pass's real targets,
then reopen a fresh batch that resumes from what was just stored. The next
lazy FBD rebuild reads render->fb.load, so pointing that at the spill load
config is enough to make it happen - no new FBD-building code needed.

WHAT THIS DOES NOT NEED, having read the code it was assumed to need
surgery on:

  - No CS-level tiler-descriptor reset. issue_fragment_jobs() already calls
    cs_finish_fragment() on every tiler descriptor in its own ordinary tail,
    unconditionally, on every call - including this one. The extra
    polygon-list zeroing the tiler-OOM exception handler does on top of
    that exists so ONE physical descriptor can be reused for a second
    incremental pass within the SAME fragment job; this split does not
    reuse one - it gets an entirely fresh tiler context from the ordinary
    render->tiler == 0 path, exactly like a brand new render pass would.
  - No tiler-OOM context bookkeeping. setup_tiler_oom_ctx() - called
    unconditionally from inside issue_fragment_jobs() - rebuilds its whole
    context fresh on every call: counter reset to 0, layer_fbd_ptr derived
    from whatever FBD_POINTER currently holds. It has no memory of an
    earlier IR pass for a split to be wrong about.

Both of those were the design doc's two biggest identified risks. Neither
turned out to be real, once read rather than assumed - which is the entire
reason this file exists as a separate, careful reading pass rather than a
direct translation of that doc into code.

Usage: patch-panvk-xfb-render-pass-split.py [<mesa-src-dir>]

Depends on patch-panvk-xfb-phase1.py having already run (anchors on text it
inserts) and on panvk_vX_cmd_xfb.c already carrying
cmd_xfb_counter_write_pending() (added directly to that hand-maintained
file, not by a patch script - see its own comment there).
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"
DRAW_H = os.path.join(mesa, "src/panfrost/vulkan/panvk_cmd_draw.h")
DRAW_C = os.path.join(mesa, "src/panfrost/vulkan/panvk_vX_cmd_draw.c")
CSF_DRAW_C = os.path.join(mesa, "src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c")


def patch_file(path, replacements, done_marker):
    src = open(path).read()
    name = os.path.basename(path)

    if done_marker in src:
        print(f"    {name}: already patched")
        return

    for old, new in replacements:
        assert old in src, f"{name}: anchor not found - upstream moved:\n{old[:200]}"
        src = src.replace(old, new, 1)

    assert done_marker in src, f"{name}: patch did not produce the done_marker"
    open(path, "w").write(src)
    print(f"    {name}: patched")


# ------------------------------------------------------- panvk_cmd_draw.h
#
# 1. The declaration, next to cmd_flush_pending_xfb_captures's - both are
#    defined in csf/panvk_vX_cmd_xfb.c (well, this one in the hand-written
#    panvk_vX_cmd_xfb.c, not patch-script-generated, but same file either
#    way) and called from csf/panvk_vX_cmd_draw.c.
# 2. The new per-render-pass counter, in panvk_rendering_state next to `ir`
#    since it exists for exactly the reason `ir` does - PAN_ARCH >= 10 only.
patch_file(
    DRAW_H,
    [
        (
            "void panvk_per_arch(cmd_flush_pending_xfb_captures)(\n"
            "   struct panvk_cmd_buffer *cmdbuf);\n",
            "void panvk_per_arch(cmd_flush_pending_xfb_captures)(\n"
            "   struct panvk_cmd_buffer *cmdbuf);\n"
            "\n"
            "/* True if a counter buffer this render pass owes a write to has not been\n"
            " * written yet - see docs/xfb-render-pass-split.md. Defined in\n"
            " * panvk_vX_cmd_xfb.c.\n"
            " */\n"
            "bool panvk_per_arch(cmd_xfb_counter_write_pending)(\n"
            "   struct panvk_cmd_buffer *cmdbuf, uint64_t dev_addr);\n",
        ),
        (
            "   struct {\n"
            "      uint64_t fbds[3];\n"
            "   } ir;\n"
            "#endif\n"
            "};",
            "   struct {\n"
            "      uint64_t fbds[3];\n"
            "   } ir;\n"
            "\n"
            "   /* VK_EXT_transform_feedback: how many times split_render_pass_for_xfb()\n"
            "    * (csf/panvk_vX_cmd_draw.c) has split this render pass so far. 0 means\n"
            "    * not yet - the first split uses PANVK_IR_FIRST_PASS, same as ordinary\n"
            "    * tiler-OOM incremental rendering would; any split after that uses\n"
            "    * PANVK_IR_MIDDLE_PASS, the same choice the GPU-side tiler-OOM handler\n"
            "    * makes via its own counter, mirrored here as an explicit host-side one\n"
            "    * since this decision is made on the host, not the GPU. Reset in\n"
            "    * panvk_per_arch(cmd_init_render_state)().\n"
            "    */\n"
            "   uint32_t xfb_split_count;\n"
            "#endif\n"
            "};",
        ),
    ],
    done_marker="cmd_xfb_counter_write_pending)(\n   struct panvk_cmd_buffer *cmdbuf, uint64_t dev_addr);",
)

# ----------------------------------------------------- panvk_vX_cmd_draw.c
#
# Reset the new counter at BeginRendering, in the same block that resets
# every other per-render-pass render-state field.
patch_file(
    DRAW_C,
    [
        (
            "   memset(&render->color_attachments, 0,\n"
            "          sizeof(render->color_attachments));\n"
            "   memset(&render->z_attachment, 0, sizeof(render->z_attachment));\n"
            "   memset(&render->s_attachment, 0, sizeof(render->s_attachment));\n"
            "   memset(&render->fb, 0, sizeof(render->fb));\n"
            "   render->bound_attachments = 0;\n",
            "   memset(&render->color_attachments, 0,\n"
            "          sizeof(render->color_attachments));\n"
            "   memset(&render->z_attachment, 0, sizeof(render->z_attachment));\n"
            "   memset(&render->s_attachment, 0, sizeof(render->s_attachment));\n"
            "   memset(&render->fb, 0, sizeof(render->fb));\n"
            "   render->bound_attachments = 0;\n"
            "#if PAN_ARCH >= 10\n"
            "   /* VK_EXT_transform_feedback: this render pass has not been split yet.\n"
            "    * See the field's own comment in panvk_cmd_draw.h.\n"
            "    */\n"
            "   render->xfb_split_count = 0;\n"
            "#endif\n",
        ),
    ],
    done_marker="render->xfb_split_count = 0;",
)

# ------------------------------------------------- csf/panvk_vX_cmd_draw.c
#
# 1. Forward declaration, right before CmdDrawIndirectByteCountEXT's own
#    definition (which patch-panvk-xfb-phase1.py already inserted, hence
#    anchoring on ITS text rather than anything from pristine Mesa - by the
#    time this script runs, the pristine anchor phase1 itself used is gone).
#    Needed because the real body goes after issue_fragment_jobs(), which is
#    defined later in the file than this call site.
# 2. The trigger: check pending writebacks before recording the draw.
# 3. The real function body, placed after issue_fragment_jobs() - the last
#    of its dependencies (flush_tiling, wrap_prev_oq, cmd_select_tile_size,
#    handle_deferred_queries, inherits_render_ctx) to become available in
#    this file's top-to-bottom definition order.
patch_file(
    CSF_DRAW_C,
    [
        (
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDrawIndirectByteCountEXT)(\n"
            "   VkCommandBuffer commandBuffer, uint32_t instanceCount,\n",
            "/* VK_EXT_transform_feedback: defined after issue_fragment_jobs() and\n"
            " * handle_deferred_queries() below, both of which it calls -\n"
            " * forward-declared here because CmdDrawIndirectByteCountEXT, which needs\n"
            " * to call it, is defined earlier in this file than either of them.\n"
            " */\n"
            "static void handle_deferred_queries(struct panvk_cmd_buffer *cmdbuf);\n"
            "static VkResult split_render_pass_for_xfb(struct panvk_cmd_buffer *cmdbuf);\n"
            "\n"
            "VKAPI_ATTR void VKAPI_CALL\n"
            "panvk_per_arch(CmdDrawIndirectByteCountEXT)(\n"
            "   VkCommandBuffer commandBuffer, uint32_t instanceCount,\n",
        ),
        (
            "   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);\n"
            "   VK_FROM_HANDLE(panvk_buffer, counter, counterBuffer);\n"
            "\n"
            "   if (instanceCount == 0 || vertexStride == 0)\n"
            "      return;\n"
            "\n"
            "   struct pan_ptr cmd = panvk_cmd_alloc_dev_mem(\n",
            "   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);\n"
            "   VK_FROM_HANDLE(panvk_buffer, counter, counterBuffer);\n"
            "\n"
            "   if (instanceCount == 0 || vertexStride == 0)\n"
            "      return;\n"
            "\n"
            "   /* This draw's own vertex count comes from `counter`, which only\n"
            "    * becomes final once the capture that wrote it has been dispatched -\n"
            "    * normally deferred all the way to CmdEndRendering. If a writeback to\n"
            "    * this exact counter buffer is still pending from earlier in this same\n"
            "    * render pass, waiting that long is too late: the draw below would read\n"
            "    * a stale value. Close the batch now instead. See\n"
            "    * docs/xfb-render-pass-split.md.\n"
            "    */\n"
            "   if (panvk_per_arch(cmd_xfb_counter_write_pending)(\n"
            "          cmdbuf, panvk_buffer_gpu_ptr(counter, counterBufferOffset))) {\n"
            "      VkResult split_result = split_render_pass_for_xfb(cmdbuf);\n"
            "      if (split_result != VK_SUCCESS) {\n"
            "         vk_command_buffer_set_error(&cmdbuf->vk, split_result);\n"
            "         return;\n"
            "      }\n"
            "   }\n"
            "\n"
            "   struct pan_ptr cmd = panvk_cmd_alloc_dev_mem(\n",
        ),
        (
            "   /* Update the frag seqno. */\n"
            "   ++cmdbuf->state.cs[PANVK_SUBQUEUE_FRAGMENT].relative_sync_point;\n"
            "\n"
            "\n"
            "   return VK_SUCCESS;\n"
            "}\n",
            "   /* Update the frag seqno. */\n"
            "   ++cmdbuf->state.cs[PANVK_SUBQUEUE_FRAGMENT].relative_sync_point;\n"
            "\n"
            "\n"
            "   return VK_SUCCESS;\n"
            "}\n"
            "\n"
            "/* VK_EXT_transform_feedback: close the current tiling batch early, so a\n"
            " * counter buffer this render pass wrote earlier is observed by a draw\n"
            " * that reads it later in the same render pass, then reopen the batch so\n"
            " * ordinary recording continues. See docs/xfb-render-pass-split.md for the\n"
            " * design and the evidence for why each step here is either necessary, or,\n"
            " * in two cases the original design assumed needed new code, is not.\n"
            " *\n"
            " * Mirrors CmdEndRendering's non-suspending path down through\n"
            " * issue_fragment_jobs() exactly - same functions, same order - then,\n"
            " * instead of finishing the render pass, resets only what CmdEndRendering\n"
            " * itself resets (fbds/tiler/oq) and points the next lazy FBD rebuild at\n"
            " * the spill load config rather than the render pass's original one.\n"
            " */\n"
            "static VkResult\n"
            "split_render_pass_for_xfb(struct panvk_cmd_buffer *cmdbuf)\n"
            "{\n"
            "   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;\n"
            "   struct panvk_rendering_state *render = &state->render;\n"
            "\n"
            "   /* Nothing recorded yet - nothing to split. Mirrors the same check\n"
            "    * CmdEndRendering makes before calling flush_tiling().\n"
            "    */\n"
            "   if (!render->fbds.gpu && !inherits_render_ctx(cmdbuf))\n"
            "      return VK_SUCCESS;\n"
            "\n"
            "   panvk_per_arch(cmd_select_tile_size)(cmdbuf);\n"
            "\n"
            "   if (render->oq.last != state->occlusion_query.syncobj) {\n"
            "      VkResult result = wrap_prev_oq(cmdbuf);\n"
            "      if (result != VK_SUCCESS)\n"
            "         return result;\n"
            "   }\n"
            "\n"
            "   /* The point of the exercise: end the tiler batch and signal\n"
            "    * VERTEX_TILER, the only thing a queued capture can validly wait on.\n"
            "    */\n"
            "   flush_tiling(cmdbuf);\n"
            "\n"
            "   /* Every capture and counter op recorded so far now runs, so any\n"
            "    * counter buffer this render pass has written becomes final. Already\n"
            "    * ordered correctly among themselves by draw_pos.\n"
            "    */\n"
            "   if (util_dynarray_num_elements(&state->xfb.pending_draws,\n"
            "                                  struct panvk_xfb_pending_draw) ||\n"
            "       util_dynarray_num_elements(&state->xfb.pending_counter_ops,\n"
            "                                  struct panvk_xfb_counter_op))\n"
            "      panvk_per_arch(cmd_flush_pending_xfb_captures)(cmdbuf);\n"
            "\n"
            "   /* Fragment work for the batch just closed, storing to the spill\n"
            "    * buffers rather than the render pass's real store targets - those\n"
            "    * still belong to whichever split (or the final CmdEndRendering)\n"
            "    * actually finishes the render pass. Override FBD_POINTER to the\n"
            "    * incremental-rendering FBD right before calling the ordinary\n"
            "    * issue_fragment_jobs(): that function never loads FBD_POINTER itself,\n"
            "    * only reads whatever get_fb_descs() already put there at record time,\n"
            "    * so this override in program order on the same subqueue is enough -\n"
            "    * no new parameter to that function needed.\n"
            "    *\n"
            "    * FIRST_PASS the first time this render pass is split, MIDDLE_PASS for\n"
            "    * any split after that - the same choice the GPU-side tiler-OOM\n"
            "    * handler makes via its own counter (panvk_vX_exception_handler.c),\n"
            "    * mirrored here as an explicit host-side one.\n"
            "    */\n"
            "   enum panvk_incremental_rendering_pass ir_pass =\n"
            "      render->xfb_split_count == 0 ? PANVK_IR_FIRST_PASS\n"
            "                                   : PANVK_IR_MIDDLE_PASS;\n"
            "   render->xfb_split_count++;\n"
            "\n"
            "   {\n"
            "      struct cs_builder *b =\n"
            "         panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_FRAGMENT);\n"
            "\n"
            "#if PAN_ARCH >= 14\n"
            "      cs_update_frag_ctx(b) {\n"
            "         cs_move64_to(b, cs_sr_reg64(b, FRAGMENT, FBD_POINTER),\n"
            "                      render->ir.fbds[ir_pass]);\n"
            "      }\n"
            "#else\n"
            "      /* Same tag-bit reconstruction setup_tiler_oom_ctx() uses for the\n"
            "       * same purpose, a few hundred lines above this function in the\n"
            "       * normal flow - the tag depends only on the framebuffer's own\n"
            "       * layout, not on which pass this is, so it is safe to recompute\n"
            "       * independently here rather than thread it through as state.\n"
            "       */\n"
            "      const bool has_zs_ext = pan_fb_has_zs(&render->fb.layout);\n"
            "      struct mali_framebuffer_pointer_packed fb_tag;\n"
            "      pan_pack(&fb_tag, FRAMEBUFFER_POINTER, cfg) {\n"
            "         cfg.zs_crc_extension_present = has_zs_ext;\n"
            "         cfg.render_target_count = render->fb.layout.rt_count;\n"
            "      }\n"
            "\n"
            "      cs_update_frag_ctx(b) {\n"
            "         cs_move64_to(b, cs_sr_reg64(b, FRAGMENT, FBD_POINTER),\n"
            "                      render->ir.fbds[ir_pass] | fb_tag.opaque[0]);\n"
            "      }\n"
            "#endif\n"
            "   }\n"
            "\n"
            "   VkResult result = issue_fragment_jobs(cmdbuf);\n"
            "   if (result != VK_SUCCESS)\n"
            "      return result;\n"
            "\n"
            "   handle_deferred_queries(cmdbuf);\n"
            "\n"
            "   /* Reset exactly what CmdEndRendering resets when it finishes a render\n"
            "    * pass. The next draw rebuilds fbds/tiler lazily, exactly as it would\n"
            "    * at the start of a brand new render pass.\n"
            "    */\n"
            "   memset(&render->fbds, 0, sizeof(render->fbds));\n"
            "   memset(&render->oq, 0, sizeof(render->oq));\n"
            "   render->tiler = 0;\n"
            "\n"
            "   /* Point the resumed batch's load at what this split just stored, so\n"
            "    * the next get_fb_descs() produces \"load spill, store real\" - exactly\n"
            "    * PANVK_IR_LAST_PASS's configuration - without new code to build it:\n"
            "    * the existing lazy FBD-build path already does this, driven purely by\n"
            "    * whatever render->fb.load currently says.\n"
            "    */\n"
            "   render->fb.load = render->fb.spill.load;\n"
            "\n"
            "   return VK_SUCCESS;\n"
            "}\n",
        ),
    ],
    done_marker="split_render_pass_for_xfb(struct panvk_cmd_buffer *cmdbuf)\n{",
)

print("panvk xfb render-pass-split patch applied")
