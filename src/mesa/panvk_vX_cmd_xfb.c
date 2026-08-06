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
 * as closely as PanVK's multi-subqueue CSF architecture allows.
 *
 * IMPORTANT: the dispatch cannot fire immediately in CmdDraw. Draws are
 * queued (panvk_cmd_graphics_state::xfb.pending_draws, panvk_cmd_draw.h)
 * and the actual capture dispatches only get emitted from
 * panvk_per_arch(cmd_flush_pending_xfb_captures)(), called from
 * CmdEndRendering right after flush_tiling() - see that function's
 * comment for why.
 */

#include "bifrost/bifrost_compile.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "pan_desc.h"
#include "pan_encoder.h"
#include "pan_shader.h"
#include "panvk_buffer.h"
#include "panvk_cmd_alloc.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_push_constant.h"
#include "panvk_entrypoints.h"
#include "panvk_instr.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"
#include "panvk_query_pool.h"

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

   state->xfb.pending_draw_count = 0;
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
    * state->xfb.buffer_offset[] by dispatch_one_xfb_capture below, once
    * the queued draws are flushed at CmdEndRendering), so the counter
    * writeback is just a plain GPU store of a host-computed constant - no
    * compute pass. If EndTransformFeedback is called before the render
    * pass ends (legal, if unusual), buffer_offset[] simply doesn't yet
    * include draws still queued - phase 1 doesn't handle that ordering,
    * matching the rest of its non-indirect-only scope.
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

/* VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: report [0] = primitives
 * written, [1] = primitives generated.
 *
 * Must be called even when the capture was clamped away to nothing - a
 * primitive that did not fit is still *generated*, and the whole point of the
 * query is to let an application detect exactly that case by seeing written
 * fall behind generated.
 */
static void
accumulate_xfb_query(struct panvk_cmd_buffer *cmdbuf, uint64_t query_ptr,
                     uint64_t written_prims, uint64_t generated_prims)
{
   if (!query_ptr)
      return;

   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);
   struct cs_index q_addr = cs_scratch_reg64(b, 6);
   struct cs_index q_val = cs_scratch_reg64(b, 8);

   cs_move64_to(b, q_addr, query_ptr);

   cs_load64_to(b, q_val, q_addr, 0);
   cs_add_imm64(b, q_val, q_val, written_prims);
   cs_store64(b, q_val, q_addr, 0);

   cs_load64_to(b, q_val, q_addr, sizeof(struct panvk_query_report));
   cs_add_imm64(b, q_val, q_val, generated_prims);
   cs_store64(b, q_val, q_addr, sizeof(struct panvk_query_report));

   cs_flush_stores(b);
}

/* Launches shader->xfb_variant as a plain compute job on
 * PANVK_SUBQUEUE_COMPUTE, one thread per (vertex, instance) pair, mirroring
 * GENX(csf_launch_xfb)'s register setup exactly where the two subqueue
 * models allow it to translate directly. Only called from
 * panvk_per_arch(cmd_flush_pending_xfb_captures)() below, never directly
 * from CmdDraw - see that function and the module comment for why.
 */
static void
dispatch_one_xfb_capture(struct panvk_cmd_buffer *cmdbuf,
                         uint32_t vertex_count, uint32_t instance_count,
                         int32_t vertex_base, uint64_t index_buffer,
                         uint32_t index_size, uint64_t query_ptr,
                         uint32_t verts_per_prim)
{
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;
   const struct panvk_shader *shader = state->vs.shader;

   if (!shader || !shader->xfb_variant)
      return;

   const struct panvk_shader_variant *xfb_variant = shader->xfb_variant;

   /* Kept for the XFB query: "generated" is what the draw asked for, before
    * the bounds clamp below reduces it to what actually fits ("written").
    */
   const uint64_t generated_verts = (uint64_t)vertex_count * instance_count;

   /* The dispatch is a flat 1D grid: one invocation per captured vertex,
    * across all instances. workgroup_id.x is the capture slot, and the shader
    * recovers vertex/instance from it using num_vertices (see
    * panvk_lower_xfb_dispatch_ids() in panvk_vX_shader.c). num_vertices stays
    * the *unclamped* per-instance count so that decomposition survives the
    * clamp below.
    */
   uint64_t capture_slots = generated_verts;

   /* Clamp the capture to what actually fits in the bound XFB buffers.
    * Without this a draw bigger than its capture buffer writes past the end
    * of it - an out-of-bounds GPU write, not merely wrong data.
    *
    * Capacity is the tightest constraint across every buffer this shader
    * writes, measured from each buffer's current offset. With a flat grid this
    * is a single min, and a partial instance can be captured correctly rather
    * than dropped.
    */
   uint64_t xfb_capture_capacity = UINT64_MAX;
   u_foreach_bit(i, shader->xfb_buffers_written) {
      if (i >= state->xfb.bound_count || !shader->xfb_strides[i])
         continue;

      uint64_t used = state->xfb.buffer_offset[i];
      uint64_t size = state->xfb.bufs[i].size;
      uint64_t avail = size > used ? size - used : 0;

      xfb_capture_capacity =
         MIN2(xfb_capture_capacity, avail / shader->xfb_strides[i]);
   }

   capture_slots = MIN2(capture_slots, xfb_capture_capacity);

   /* Transform feedback discards whole primitives, not individual vertices: a
    * triangle that only half fits is dropped entirely rather than captured as
    * a partial primitive. This also keeps the XFB query self-consistent -
    * "written" is always a whole number of primitives that really were
    * captured.
    */
   if (verts_per_prim > 1)
      capture_slots -= capture_slots % verts_per_prim;

   if (!capture_slots) {
      /* Nothing fits, but the primitives were still generated. */
      if (verts_per_prim)
         accumulate_xfb_query(cmdbuf, query_ptr, 0,
                              generated_verts / verts_per_prim);
      return;
   }

   /* Populate the sysvals the XFB variant's shader body reads
    * (nir_load_num_vertices / nir_load_xfb_address, wired in
    * panvk_lower_sysvals()) directly - this dispatch always rebuilds its
    * own push-uniforms below, so there's no need to route through the
    * per-draw dirty-bit tracking the render VS/FS sysvals use.
    */
   state->sysvals.xfb.num_vertices = vertex_count;
   state->sysvals.xfb.index_buffer = index_buffer;
   state->sysvals.xfb.index_size = index_size;
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
      .x = (uint32_t)capture_slots,
      .y = 1,
      .z = 1,
   };
   uint64_t tsd = panvk_per_arch(cmd_dispatch_prepare_tls)(
      cmdbuf, xfb_variant, &dim, false);
   if (!tsd) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   /* ROOT CAUSE OF THE ORIGINAL VK_ERROR_DEVICE_LOST (found 2026-08-06,
    * see docs/kbase-notes.md): xfb_variant->spds.* (built by
    * panvk_shader_upload()) declares MALI_SHADER_STAGE_VERTEX, because
    * panvk_shader_upload() branches purely on shader->info.stage, and
    * xfb_variant is still MESA_SHADER_VERTEX-stage NIR (required for
    * bifrost_postprocess_nir()'s VS-specific lowering to run at all).
    * Running a VERTEX-declared SPD via cs_run_compute (a COMPUTE job)
    * faulted every time - confirmed by isolation testing on real
    * hardware. Building a plain SHADER_PROGRAM descriptor here that
    * declares MALI_SHADER_STAGE_COMPUTE explicitly, pointing at the same
    * compiled binary, fixes it. GL's csf_launch_xfb does not need this
    * because it isn't subject to PanVK's SPD-stage/job-type check the
    * same way - not fully understood, but empirically necessary here.
    */
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_priv_mem compute_spd =
      panvk_pool_alloc_desc(&dev->mempools.rw, SHADER_PROGRAM);
   panvk_priv_mem_write_desc(compute_spd, 0, SHADER_PROGRAM, cfg) {
      cfg.stage = MALI_SHADER_STAGE_COMPUTE;
      cfg.register_allocation =
         pan_register_allocation(xfb_variant->info.work_reg_count);
      cfg.binary = panvk_shader_variant_get_dev_addr(xfb_variant);
      cfg.preload.r48_r63 = (xfb_variant->info.preload >> 48);
      cfg.flush_to_zero_mode =
         xfb_variant->info.ftz_fp32
            ? (xfb_variant->info.ftz_fp16 ? MALI_FLUSH_TO_ZERO_MODE_ALWAYS
                                          : MALI_FLUSH_TO_ZERO_MODE_DX11)
            : MALI_FLUSH_TO_ZERO_MODE_PRESERVE_SUBNORMALS;
   }
   uint64_t spd_addr = panvk_priv_mem_dev_addr(compute_spd);

   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);
   const struct cs_tracing_ctx *tracing_ctx =
      &cmdbuf->state.cs[PANVK_SUBQUEUE_COMPUTE].tracing;

   /* Cross-subqueue dependency: this job reads the vertex attribute state
    * (vs_desc_state->res_table) and render/TLS state the draw on
    * PANVK_SUBQUEUE_VERTEX_TILER set up. PanVK never synchronizes
    * subqueues automatically (see panvk_per_arch(emit_barrier) /
    * collect_cs_deps in panvk_vX_cmd_buffer.c - it only fires off explicit
    * VkMemoryBarrier2-family calls), so this has to insert its own wait,
    * mirroring emit_barrier_insert_waits()'s exact primitives. This is
    * only valid to do here, after flush_tiling() has just signalled
    * VERTEX_TILER's syncobj and incremented its relative_sync_point - see
    * docs/kbase-notes.md and cmd_flush_pending_xfb_captures() below.
    */
   {
      struct cs_index sync_addr = cs_scratch_reg64(b, 0);
      struct cs_index wait_val = cs_scratch_reg64(b, 2);

      cs_load64_to(b, sync_addr, cs_subqueue_ctx_reg(b),
                   offsetof(struct panvk_cs_subqueue_context, syncobjs));
      cs_add_imm64(b, sync_addr, sync_addr,
                   sizeof(struct panvk_cs_sync64) * PANVK_SUBQUEUE_VERTEX_TILER);

      cs_add_imm64(b, wait_val,
                   cs_progress_seqno_reg(b, PANVK_SUBQUEUE_VERTEX_TILER),
                   cmdbuf->state.cs[PANVK_SUBQUEUE_VERTEX_TILER].relative_sync_point);

      panvk_instr_sync64_wait(cmdbuf, PANVK_SUBQUEUE_COMPUTE, false,
                              MALI_CS_CONDITION_GREATER, wait_val, sync_addr);
   }

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

      /* Bias attribute fetch by the draw's firstVertex (non-indexed) or
       * vertexOffset (indexed), matching VERTEX_OFFSET in the real IDVS
       * draw (launch_draw()). This is why the indexed path needs no
       * vertexOffset maths in the shader.
       */
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, GLOBAL_ATTRIBUTE_OFFSET),
                   (uint32_t)vertex_base);

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

      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_X),
                   (uint32_t)capture_slots);
      cs_move32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_Y), 1);
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

   if (verts_per_prim) {
      accumulate_xfb_query(cmdbuf, query_ptr, capture_slots / verts_per_prim,
                           generated_verts / verts_per_prim);
   }

   /* Accumulate this draw's contribution to the counter-buffer writeback
    * CmdEndTransformFeedbackEXT will perform - see the comment there.
    */
   for (uint32_t i = 0; i < state->xfb.bound_count; i++) {
      if (!(shader->xfb_buffers_written & BITFIELD_BIT(i)))
         continue;

      state->xfb.buffer_offset[i] +=
         (uint64_t)shader->xfb_strides[i] * capture_slots;
   }
}

/* Called from panvk_per_arch(CmdEndRendering) (csf/panvk_vX_cmd_draw.c),
 * right after flush_tiling() - which is the only thing that signals
 * PANVK_SUBQUEUE_VERTEX_TILER's syncobj and gives a compute dispatch on
 * PANVK_SUBQUEUE_COMPUTE something valid to wait on. It runs once per
 * render pass, not per draw, so the capture dispatch for draws recorded
 * earlier in the same render pass (queued into xfb.pending_draws by
 * CmdDraw) has to wait until here too - firing it directly from CmdDraw
 * would wait on a stale (or nonexistent) sync point and, worse, inject a
 * job into the middle of a still-open vertex/tiler batch. See
 * docs/kbase-notes.md for the investigation that found this.
 */
void
panvk_per_arch(cmd_flush_pending_xfb_captures)(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;

   for (unsigned i = 0; i < state->xfb.pending_draw_count; i++) {
      dispatch_one_xfb_capture(cmdbuf, state->xfb.pending_draws[i].vertex_count,
                               state->xfb.pending_draws[i].instance_count,
                               state->xfb.pending_draws[i].vertex_base,
                               state->xfb.pending_draws[i].index_buffer,
                               state->xfb.pending_draws[i].index_size,
                               state->xfb.pending_draws[i].query_ptr,
                               state->xfb.pending_draws[i].verts_per_prim);
   }

   state->xfb.pending_draw_count = 0;

   /* An XFB query ended before the render pass did had its availability write
    * deferred to here, so it lands after the counts above - see
    * panvk_cmd_end_xfb_query() in csf/panvk_vX_cmd_query.c.
    */
   if (state->xfb_query.deferred_syncobj) {
      panvk_per_arch(cmd_signal_xfb_query_available)(
         cmdbuf, state->xfb_query.deferred_syncobj);
      state->xfb_query.deferred_syncobj = 0;
   }
}
