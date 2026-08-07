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
#include "panvk_cmd_precomp.h"
#include "libpan/draw_helper.h"
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

   for (uint32_t i = 0; i < MAX_XFB_BUFFERS; i++) {
      state->xfb.counter_buffers[i].present = false;
      state->xfb.counter_buffers[i].dev_addr = 0;
   }

   /* One write position per buffer, a byte offset, zeroed here. It has to
    * live in GPU memory because panlib_xfb_setup() clamps against it.
    */
   struct pan_ptr offsets = panvk_cmd_alloc_dev_mem(
      cmdbuf, desc, sizeof(uint32_t) * MAX_XFB_BUFFERS, sizeof(uint32_t));
   if (!offsets.gpu) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   memset(offsets.cpu, 0, sizeof(uint32_t) * MAX_XFB_BUFFERS);
   state->xfb.offsets_gpu = offsets.gpu;

   /* Resume: a counter buffer holds a byte offset, which is exactly the unit
    * the write position uses, so seeding it is a plain copy. Buffers without
    * a counter buffer keep the zero written above.
    */
   if (counterBufferCount) {
      struct cs_builder *b =
         panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);
      struct cs_index dst = cs_scratch_reg64(b, 0);
      struct cs_index src = cs_scratch_reg64(b, 2);
      struct cs_index val = cs_scratch_reg32(b, 4);

      cs_move64_to(b, dst, offsets.gpu);

      for (uint32_t i = 0; i < counterBufferCount; i++) {
         uint32_t buf_idx = firstCounterBuffer + i;

         if (buf_idx >= MAX_XFB_BUFFERS || !pCounterBuffers[i])
            continue;

         VK_FROM_HANDLE(panvk_buffer, cbuf, pCounterBuffers[i]);
         uint64_t caddr = panvk_buffer_gpu_ptr(
            cbuf, pCounterBufferOffsets ? pCounterBufferOffsets[i] : 0);

         cs_move64_to(b, src, caddr);
         cs_load32_to(b, val, src, 0);
         cs_store32(b, val, dst, buf_idx * sizeof(uint32_t));
      }

      cs_flush_stores(b);
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

   /* The write position is only final once CmdEndRendering has dispatched the
    * captures this End closed, so the writeback cannot happen here. Record the
    * targets and let cmd_flush_pending_xfb_captures() emit the copies - the
    * same deferral the XFB query's availability uses.
    */
   for (uint32_t i = 0; i < counterBufferCount; i++) {
      uint32_t buf_idx = firstCounterBuffer + i;

      if (buf_idx >= MAX_XFB_BUFFERS || !pCounterBuffers[i])
         continue;

      VK_FROM_HANDLE(panvk_buffer, cbuf, pCounterBuffers[i]);

      state->xfb.counter_buffers[buf_idx].present = true;
      state->xfb.counter_buffers[buf_idx].dev_addr = panvk_buffer_gpu_ptr(
         cbuf, pCounterBufferOffsets ? pCounterBufferOffsets[i] : 0);
   }

   /* offsets_gpu deliberately survives here: End runs before CmdEndRendering,
    * which is where the captures this End closed are actually dispatched.
    * cmd_flush_pending_xfb_captures() clears it once they have been emitted.
    */

   state->xfb.active = false;
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
                         uint32_t xfb_topology, uint64_t indirect_buffer,
                         uint32_t restart_index)
{
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;
   const struct panvk_shader *shader = state->vs.shader;

   if (!shader || !shader->xfb_variant)
      return;

   const struct panvk_shader_variant *xfb_variant = shader->xfb_variant;

   /* Kept for the XFB query: "generated" is what the draw asked for, before
    * the bounds clamp below reduces it to what actually fits ("written").
    */
   /* Only meaningful for a direct draw; for an indirect one the kernel reads
    * the counts out of the indirect buffer itself.
    */
   const uint64_t generated_verts = (uint64_t)vertex_count * instance_count;

   /* The dispatch is a flat 1D grid: one invocation per captured vertex,
    * across all instances. workgroup_id.x is the capture slot, and the shader
    * recovers vertex/instance from it using num_vertices (see
    * panvk_lower_xfb_dispatch_ids() in panvk_vX_shader.c). num_vertices stays
    * the *unclamped* per-instance count so that decomposition survives the
    * clamp below.
    */
   /* Only an upper bound now: for a strip the captured count is larger than
    * the input vertex count, and for an indirect draw the host has neither.
    * The kernel computes the real figure; this just sizes TLS below.
    */
   const uint64_t capture_slots = generated_verts;

   if (!state->xfb.offsets_gpu)
      return;

   state->sysvals.xfb.num_vertices = vertex_count;
   state->sysvals.xfb.index_buffer = index_buffer;
   state->sysvals.xfb.index_size = index_size;
   state->sysvals.xfb.topology = xfb_topology;
   state->sysvals.xfb.slot_table = 0;

   /* Primitive restart makes slot -> input vertex data-dependent, so the
    * kernel resolves it into this table and the shader reads it directly.
    * Sized by the worst case: every index could complete a primitive.
    */
   struct pan_ptr slot_table = {0};
   if (restart_index) {
      uint32_t vpp = xfb_topology == PANVK_XFB_TOPO_POINT_LIST      ? 1
                     : xfb_topology <= PANVK_XFB_TOPO_LINE_STRIP    ? 2
                                                                    : 3;
      slot_table = panvk_cmd_alloc_dev_mem(
         cmdbuf, desc, (uint64_t)vertex_count * vpp * sizeof(uint32_t),
         sizeof(uint32_t));
      if (!slot_table.gpu) {
         vk_command_buffer_set_error(&cmdbuf->vk,
                                     VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }
      state->sysvals.xfb.slot_table = slot_table.gpu;
   }



   /* Base only; panlib_xfb_setup() overwrites this slot in the uploaded
    * push-uniform buffer with base + offset*stride, since only it knows the
    * GPU-resident write position.
    */
   for (uint32_t i = 0; i < state->xfb.bound_count; i++)
      state->sysvals.xfb.buffer_addrs[i] = state->xfb.bufs[i].address;

   struct pan_ptr push_uniforms;
   VkResult result = panvk_per_arch(cmd_prepare_gfx_push_uniforms)(
      cmdbuf, xfb_variant, &push_uniforms, 1);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmdbuf->vk, result);
      return;
   }

   uint32_t tls_slots = (uint32_t)capture_slots;
   if (indirect_buffer) {
      /* No host count: size TLS for the most the bound buffers could hold,
       * which is the most the clamp can ever let through.
       */
      tls_slots = 0;
      u_foreach_bit(i, shader->xfb_buffers_written) {
         if (i >= state->xfb.bound_count || !shader->xfb_strides[i])
            continue;
         tls_slots = MAX2(tls_slots, (uint32_t)(state->xfb.bufs[i].size /
                                                shader->xfb_strides[i]));
      }
   }

   struct pan_compute_dim dim = {
      .x = tls_slots,
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

   /* Clamp the capture to what fits, in a helper kernel.
    *
    * The write position lives in GPU memory, so deciding how much fits means
    * dividing by the stride at dispatch time - which the command stream
    * cannot do on this arch. The kernel also owns the XFB query counters and
    * advancing the write position, since both depend on the clamped result.
    * Same division of labour as Asahi hk's setup_xfb_buffer().
    */
   struct pan_ptr xfb_descs = panvk_cmd_alloc_dev_mem(
      cmdbuf, desc, sizeof(struct panlib_xfb_buffer_desc) * MAX_XFB_BUFFERS,
      sizeof(uint64_t));
   struct pan_ptr out_slots =
      panvk_cmd_alloc_dev_mem(cmdbuf, desc, sizeof(uint32_t), sizeof(uint32_t));

   if (!xfb_descs.gpu || !out_slots.gpu) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   struct panlib_xfb_buffer_desc *descs = xfb_descs.cpu;
   uint32_t desc_count = 0;

   u_foreach_bit(i, shader->xfb_buffers_written) {
      if (i >= state->xfb.bound_count || !shader->xfb_strides[i])
         continue;

      /* The kernel resolves base + offset*stride straight into the
       * push-uniform slot the capture shader reads xfb.buffer_addrs[i] from,
       * so the shader needs no knowledge of the write position.
       */
      uint64_t pu = 0;
      if (push_uniforms.gpu &&
          shader_uses_sysval_entry(xfb_variant, graphics, xfb.buffer_addrs, i))
         pu = push_uniforms.gpu +
              shader_remapped_sysval_offset(
                 xfb_variant,
                 sysval_entry_offset(graphics, xfb.buffer_addrs, i));

      descs[desc_count++] = (struct panlib_xfb_buffer_desc){
         .base = state->xfb.bufs[i].address,
         .push_uniform = pu,
         .size_bytes = (uint32_t)state->xfb.bufs[i].size,
         .stride = shader->xfb_strides[i],
      };
   }

   {
      struct panvk_precomp_ctx pctx = panvk_per_arch(precomp_cs)(cmdbuf);
      struct panlib_xfb_setup_args args = {
         .offsets = state->xfb.offsets_gpu,
         .descs = xfb_descs.gpu,
         .desc_count = desc_count,
         .direct_vertex_count = vertex_count,
         .direct_instance_count = instance_count,
         .topology = xfb_topology,
         .out_slots = out_slots.gpu,
         .query = query_ptr,
         .indirect = indirect_buffer,
         .index_buffer_base = index_buffer,
         .index_size = index_size,
         .slot_table = slot_table.gpu,
         .restart_index = restart_index,
         .index_buffer_pu =
            indirect_buffer && index_size && push_uniforms.gpu &&
                  shader_uses_sysval(xfb_variant, graphics, xfb.index_buffer)
               ? push_uniforms.gpu +
                    shader_remapped_sysval_offset(
                       xfb_variant, sysval_offset(graphics, xfb.index_buffer))
               : 0,
         .num_vertices_pu =
            push_uniforms.gpu &&
                  shader_uses_sysval(xfb_variant, graphics, xfb.num_vertices)
               ? push_uniforms.gpu +
                    shader_remapped_sysval_offset(
                       xfb_variant, sysval_offset(graphics, xfb.num_vertices))
               : 0,
      };

      panlib_xfb_setup_struct(&pctx, panlib_1d(1), PANLIB_BARRIER_CSF_WAIT,
                              args);
   }


   /* The barrier above only waits for the kernel to finish - it does not write
    * its stores back, so the CS load below would otherwise read stale memory.
    */
   {
      struct cs_index flush_id = cs_scratch_reg32(b, 4);

      cs_move32_to(b, flush_id, 0);
      cs_flush_caches(b, MALI_CS_FLUSH_MODE_CLEAN, MALI_CS_FLUSH_MODE_CLEAN,
                      MALI_CS_OTHER_FLUSH_MODE_NONE, flush_id,
                      cs_defer(SB_IMM_MASK, SB_ID(DEFERRED_FLUSH)));
      cs_wait_slot(b, SB_ID(DEFERRED_FLUSH));
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
      if (indirect_buffer) {
         /* firstVertex is word 2 of VkDrawIndirectCommand, but the indexed
          * command has vertexOffset at word 3 instead. Reading either from an
          * application buffer needs no cache flush - nothing in our command
          * stream produced it.
          */
         cs_move64_to(b, cs_scratch_reg64(b, 8), indirect_buffer);
         cs_load32_to(b, cs_sr_reg32(b, COMPUTE, GLOBAL_ATTRIBUTE_OFFSET),
                      cs_scratch_reg64(b, 8), index_size ? 12 : 8);
      } else {
         cs_move32_to(b, cs_sr_reg32(b, COMPUTE, GLOBAL_ATTRIBUTE_OFFSET),
                      (uint32_t)vertex_base);
      }

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

      cs_move64_to(b, cs_scratch_reg64(b, 6), out_slots.gpu);
      cs_load32_to(b, cs_sr_reg32(b, COMPUTE, JOB_SIZE_X),
                   cs_scratch_reg64(b, 6), 0);
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

   /* The XFB query counters and the write-position advance both depend on the
    * clamped slot count, so panlib_xfb_setup() above owns them.
    */
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
                               state->xfb.pending_draws[i].xfb_topology,
                               state->xfb.pending_draws[i].indirect_buffer,
                               state->xfb.pending_draws[i].restart_index);
   }

   state->xfb.pending_draw_count = 0;

   /* Deferred counter-buffer writeback, now that every capture has run and the
    * write positions are final. They were last written by panlib_xfb_setup(),
    * so the caches have to be cleaned before the command stream can read them
    * - a barrier alone does not make kernel stores visible here.
    */
   if (state->xfb.offsets_gpu) {
      struct cs_builder *b =
         panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);
      bool counter_writeback = false;

      for (uint32_t i = 0; i < MAX_XFB_BUFFERS; i++)
         counter_writeback |= state->xfb.counter_buffers[i].present;

      if (counter_writeback) {
         struct cs_index flush_id = cs_scratch_reg32(b, 4);
         struct cs_index src = cs_scratch_reg64(b, 0);
         struct cs_index dst = cs_scratch_reg64(b, 2);
         struct cs_index val = cs_scratch_reg32(b, 5);

         cs_move32_to(b, flush_id, 0);
         cs_flush_caches(b, MALI_CS_FLUSH_MODE_CLEAN, MALI_CS_FLUSH_MODE_CLEAN,
                         MALI_CS_OTHER_FLUSH_MODE_NONE, flush_id,
                         cs_defer(SB_IMM_MASK, SB_ID(DEFERRED_FLUSH)));
         cs_wait_slot(b, SB_ID(DEFERRED_FLUSH));

         cs_move64_to(b, src, state->xfb.offsets_gpu);

         for (uint32_t i = 0; i < MAX_XFB_BUFFERS; i++) {
            if (!state->xfb.counter_buffers[i].present)
               continue;

            cs_load32_to(b, val, src, i * sizeof(uint32_t));
            cs_move64_to(b, dst, state->xfb.counter_buffers[i].dev_addr);
            cs_store32(b, val, dst, 0);
         }

         cs_flush_stores(b);
      }
   }

   for (uint32_t i = 0; i < MAX_XFB_BUFFERS; i++)
      state->xfb.counter_buffers[i].present = false;

   /* Safe to release only now: End runs before CmdEndRendering, so the write
    * positions have to outlive it and survive until the captures that use
    * them have actually been emitted.
    */
   state->xfb.offsets_gpu = 0;

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
