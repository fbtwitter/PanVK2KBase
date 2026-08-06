/*
 * Copyright © 2026 PanVK2KBase contributors
 * SPDX-License-Identifier: MIT
 */

/* VK_EXT_transform_feedback, phase 1: single-stream, no GS/tess.
 *
 * Scope (see docs/kbase-notes.md and ROADMAP.md for the full writeup):
 * only non-indexed, non-indirect vkCmdDraw is supported, so the captured
 * vertex count is a host-known constant at record time and the
 * counter-buffer writeback needs no compute pass of its own.
 * pCounterBuffers must be NULL at Begin (no resume-from-offset support
 * yet). Indexed/indirect draws, multiple streams, and
 * VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT are all out of scope for
 * this phase.
 *
 * The capture itself is a second, monolithic vertex-shader variant
 * (panvk_shader::xfb_variant, compiled in panvk_vX_shader.c) launched as
 * a plain compute job on PANVK_SUBQUEUE_COMPUTE, reusing the render VS's
 * vs_desc_state->res_table for hardware attribute fetch - mirroring
 * Panfrost GL's GENX(csf_launch_xfb) in gallium/drivers/panfrost/pan_csf.c
 * as closely as possible. NOT independently verified beyond compiling
 * clean; the first real test is tests/render_xfb_probe on real hardware.
 */

#include "bifrost/bifrost_compile.h"
#include "pan_desc.h"
#include "pan_encoder.h"
#include "panvk_buffer.h"
#include "panvk_cmd_alloc.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_push_constant.h"
#include "panvk_entrypoints.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindTransformFeedbackBuffersEXT)(
   VkCommandBuffer commandBuffer, uint32_t firstBinding,
   uint32_t bindingCount, const VkBuffer *pBuffers,
   const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes);

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBeginTransformFeedbackEXT)(
   VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
   uint32_t counterBufferCount, const VkBuffer *pCounterBuffers,
   const VkDeviceSize *pCounterBufferOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;

   assert(!state->xfb.active);

   /* Phase 1: every Begin starts at offset 0. Resuming from a nonzero
    * counter-buffer offset needs either a stall-and-readback or the same
    * GPU-side counter machinery indirect/restart draws will need - not
    * implemented yet.
    */
   assert(counterBufferCount == 0 &&
          "VK_EXT_transform_feedback: resuming from a counter buffer is "
          "not yet supported (phase 1)");
   (void)firstCounterBuffer;
   (void)pCounterBuffers;
   (void)pCounterBufferOffsets;

   for (uint32_t i = 0; i < state->xfb.bound_count; i++) {
      state->xfb.counter_buffers[i].present = false;
      state->xfb.counter_buffers[i].dev_addr = 0;
      state->xfb.buffer_offset[i] = 0;
   }

   state->xfb.active = true;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdEndTransformFeedbackEXT)(
   VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
   uint32_t counterBufferCount, const VkBuffer *pCounterBuffers,
   const VkDeviceSize *pCounterBufferOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;

   assert(state->xfb.active);

   /* vertexCount * instanceCount * stride is known on the host for phase
    * 1's non-indexed, non-indirect draws (accumulated into
    * state->xfb.buffer_offset[] by cmd_dispatch_xfb_capture below on
    * every vkCmdDraw while active), so the counter writeback is just a
    * plain GPU store of a host-computed constant - no compute pass.
    */
   if (counterBufferCount > 0) {
      struct cs_builder *b =
         panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);

      for (uint32_t i = 0; i < counterBufferCount; i++) {
         VK_FROM_HANDLE(panvk_buffer, buffer, pCounterBuffers[i]);
         if (!buffer)
            continue;

         unsigned buf_idx = firstCounterBuffer + i;
         uint64_t counter_addr =
            panvk_buffer_gpu_ptr(buffer, pCounterBufferOffsets[i]);
         uint32_t final_offset = (uint32_t)state->xfb.buffer_offset[buf_idx];

         struct cs_index addr_reg = cs_scratch_reg64(b, 0);
         struct cs_index val_reg = cs_scratch_reg32(b, 2);

         cs_move64_to(b, addr_reg, counter_addr);
         cs_move32_to(b, val_reg, final_offset);
         cs_store32(b, val_reg, addr_reg, 0);
      }

      cs_flush_stores(b);
   }

   state->xfb.active = false;
}

/* Called from panvk_per_arch(CmdDraw) (panvk_vX_cmd_draw.c) right after a
 * non-indexed, non-indirect draw, when transform feedback is active.
 * Launches shader->xfb_variant as a plain compute job on
 * PANVK_SUBQUEUE_COMPUTE, one thread per (vertex, instance) pair, mirroring
 * GENX(csf_launch_xfb)'s register setup exactly where the two subqueue
 * models allow it to translate directly.
 */
void
panvk_per_arch(cmd_dispatch_xfb_capture)(struct panvk_cmd_buffer *cmdbuf,
                                         const struct panvk_draw_info *draw)
{
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;
   const struct panvk_shader *shader = state->vs.shader;

   if (!shader || !shader->xfb_variant)
      return;

   const struct panvk_shader_variant *xfb_variant = shader->xfb_variant;

   /* Populate the sysvals the XFB variant's shader body reads
    * (nir_load_num_vertices / nir_load_xfb_address, wired in
    * panvk_lower_sysvals()) directly - this dispatch always rebuilds its
    * own push-uniforms below, so there's no need to route through the
    * per-draw dirty-bit tracking the render VS/FS sysvals use.
    */
   state->sysvals.xfb.num_vertices = draw->vertex.count;
   for (uint32_t i = 0; i < state->xfb.bound_count; i++) {
      state->sysvals.xfb.buffer_addrs[i] =
         state->xfb.bufs[i].address + state->xfb.buffer_offset[i];
   }

   struct pan_ptr push_uniforms;
   VkResult result = panvk_per_arch(cmd_prepare_gfx_push_uniforms)(
      cmdbuf, xfb_variant, &push_uniforms, 1);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmdbuf->vk, result);
      return;
   }

   struct pan_compute_dim dim = {
      .x = draw->vertex.count,
      .y = draw->instance.count,
      .z = 1,
   };
   uint64_t tsd = panvk_per_arch(cmd_dispatch_prepare_tls)(
      cmdbuf, xfb_variant, &dim, false);
   if (!tsd) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

#if PAN_ARCH >= 12
   uint64_t spd_addr = panvk_priv_mem_dev_addr(xfb_variant->spds.all_triangles);
#else
   /* no_idvs compilation never splits into position/varying binaries, so
    * pos_triangles is the shader's one and only entry point - see
    * docs/kbase-notes.md.
    */
   uint64_t spd_addr = panvk_priv_mem_dev_addr(xfb_variant->spds.pos_triangles);
#endif

   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);
   const struct cs_tracing_ctx *tracing_ctx =
      &cmdbuf->state.cs[PANVK_SUBQUEUE_COMPUTE].tracing;

   if (xfb_variant->info.tls_size) {
      cs_move64_to(b, cs_scratch_reg64(b, 0), cmdbuf->state.tls.desc.gpu);
      cs_load64_to(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 0), 8);
      cs_move64_to(b, cs_scratch_reg64(b, 0), tsd);
      cs_store64(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 0), 8);
      cs_flush_stores(b);
   }

   cs_update_compute_ctx(b) {
      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_SRT),
                   state->vs.desc.res_table);

      uint64_t fau_count = xfb_variant->fau.total_count;
      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_FAU),
                   push_uniforms.gpu | (fau_count << 56));

      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_SPD), spd_addr);
      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_TSD), tsd);

      /* Bias attribute fetch by the draw's firstVertex, matching
       * VERTEX_OFFSET in the real IDVS draw (launch_draw()).
       */
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, GLOBAL_ATTRIBUTE_OFFSET),
                   draw->vertex.base);

      struct mali_compute_size_workgroup_packed wg_size;
      pan_pack(&wg_size, COMPUTE_SIZE_WORKGROUP, cfg) {
         cfg.workgroup_size_x = 1;
         cfg.workgroup_size_y = 1;
         cfg.workgroup_size_z = 1;
         /* XFB capture kernels use no barriers/shared memory, so
          * workgroups can be freely merged - mirrors csf_launch_xfb.
          */
         cfg.allow_merging_workgroups = true;
      }
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, WG_SIZE), wg_size.opaque[0]);

      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_OFFSET_X), 0);
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_OFFSET_Y), 0);
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_OFFSET_Z), 0);

      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_X), draw->vertex.count);
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_Y),
                   draw->instance.count);
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_Z), 1);
   }

   cs_next_iter_sb(cmdbuf, PANVK_SUBQUEUE_COMPUTE,
                   cs_scratch_reg_tuple(b, 0, 2));

   /* One thread per (vertex, instance) - matches GENX(csf_launch_xfb)'s
    * cs_run_compute(b, 1, MALI_TASK_AXIS_Z, ...) exactly rather than using
    * the throughput-oriented task-axis/increment search dispatch_precomp
    * uses for real compute workloads, which doesn't fit this shape.
    */
   cs_trace_run_compute(b, tracing_ctx, cs_scratch_reg_tuple(b, 0, 4), 1,
                        MALI_TASK_AXIS_Z, PANVK_PRECOMP_RES_SEL);

   /* Accumulate this draw's contribution to the counter-buffer writeback
    * CmdEndTransformFeedbackEXT will perform - see the comment there.
    */
   for (uint32_t i = 0; i < state->xfb.bound_count; i++) {
      if (!(shader->xfb_buffers_written & BITFIELD_BIT(i)))
         continue;

      state->xfb.buffer_offset[i] +=
         (uint64_t)shader->xfb_strides[i] * draw->vertex.count *
         draw->instance.count;
   }
}
