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
 *
 * Because of that deferral, a capture is built in two halves at two
 * different times. Everything host-side happens at record time, in
 * panvk_per_arch(cmd_prepare_xfb_capture)(), while the state it depends on
 * is still this draw's; only the command stream is emitted at
 * CmdEndRendering. Anything moved across that line reintroduces the bug the
 * split exists to fix, where every capture in a render pass was built from
 * the last draw's state.
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

/* Records one counter-buffer transfer for CmdEndRendering to replay, stamped
 * with its position in the capture queue so the replay keeps record order.
 * Returns false if it could not be queued, with the error already on the
 * command buffer.
 */
static bool
xfb_queue_counter_op(struct panvk_cmd_buffer *cmdbuf,
                     enum panvk_xfb_counter_op_type type, uint64_t offsets,
                     uint32_t buf_idx, uint64_t dev_addr)
{
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;
   struct panvk_xfb_counter_op *op = util_dynarray_grow(
      &state->xfb.pending_counter_ops, struct panvk_xfb_counter_op, 1);

   if (op == NULL) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return false;
   }

   *op = (struct panvk_xfb_counter_op){
      .type = type,
      .offsets = offsets,
      .dev_addr = dev_addr,
      .buf_idx = buf_idx,
      .draw_pos = util_dynarray_num_elements(&state->xfb.pending_draws,
                                             struct panvk_xfb_pending_draw),
   };

   return true;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBeginTransformFeedbackEXT)(
   VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
   uint32_t counterBufferCount, const VkBuffer *pCounterBuffers,
   const VkDeviceSize *pCounterBufferOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;

   assert(!state->xfb.active);

   /* One write position per buffer, a byte offset, zeroed here. It has to
    * live in GPU memory because panlib_xfb_setup() clamps against it.
    *
    * Allocated per Begin rather than per render pass: each Begin/End pair
    * restarts capture from its own base, and pairs from earlier in this
    * render pass still have captures queued that refer to their own
    * allocation. They snapshot the address, so replacing it here is safe.
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
    *
    * The copy is recorded, not emitted. If it were emitted here it would run
    * before the captures and the End writeback that produce the value it
    * reads - those are all deferred to CmdEndRendering - and every Begin in
    * the render pass would resume from the same stale counter. Recording it
    * lets the flush replay it in the position it was written in.
    */
   for (uint32_t i = 0; pCounterBuffers && i < counterBufferCount; i++) {
      uint32_t buf_idx = firstCounterBuffer + i;

      if (buf_idx >= MAX_XFB_BUFFERS || !pCounterBuffers[i])
         continue;

      VK_FROM_HANDLE(panvk_buffer, cbuf, pCounterBuffers[i]);

      if (!xfb_queue_counter_op(
             cmdbuf, PANVK_XFB_COUNTER_SEED, offsets.gpu, buf_idx,
             panvk_buffer_gpu_ptr(
                cbuf, pCounterBufferOffsets ? pCounterBufferOffsets[i] : 0)))
         break;
   }

   /* pending_draws is deliberately NOT cleared here. A render pass may contain
    * several Begin/End pairs, and captures queued by an earlier pair are still
    * waiting for CmdEndRendering to dispatch them - clearing would silently
    * drop them. Only cmd_flush_pending_xfb_captures() clears the queue, once
    * it has emitted everything in it.
    */
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
    *
    * Each entry carries state->xfb.offsets_gpu as it is *now*, because a later
    * Begin in this same render pass will have replaced it by the time the
    * copies are emitted. Two pairs writing back the same buffer index are two
    * independent entries, so this is a queue rather than a per-buffer array.
    */
   for (uint32_t i = 0; pCounterBuffers && i < counterBufferCount; i++) {
      uint32_t buf_idx = firstCounterBuffer + i;

      if (buf_idx >= MAX_XFB_BUFFERS || !pCounterBuffers[i])
         continue;

      VK_FROM_HANDLE(panvk_buffer, cbuf, pCounterBuffers[i]);

      if (!xfb_queue_counter_op(
             cmdbuf, PANVK_XFB_COUNTER_WRITEBACK, state->xfb.offsets_gpu,
             buf_idx,
             panvk_buffer_gpu_ptr(
                cbuf, pCounterBufferOffsets ? pCounterBufferOffsets[i] : 0)))
         break;
   }

   /* offsets_gpu deliberately survives here: End runs before CmdEndRendering,
    * which is where the captures this End closed are actually dispatched.
    * cmd_flush_pending_xfb_captures() clears it once they have been emitted.
    */

   state->xfb.active = false;
}

/* Host-side half of one capture: snapshot the state it depends on and do
 * every allocation and upload it needs, at the point the draw is recorded.
 *
 * This exists because the command-stream half below runs at CmdEndRendering,
 * by which point the live state has moved on - a second Begin/End pair, a
 * different pipeline, or just different push constants, and every capture in
 * the render pass would otherwise be built from the last draw's inputs. The
 * split is on that line and no other: anything read from cmdbuf->state here
 * is per-draw, anything read there is per-render-pass or per-command-buffer.
 *
 * Failures are recorded on the command buffer and leave draw->prepared false,
 * which the flush skips.
 */
void
panvk_per_arch(cmd_prepare_xfb_capture)(struct panvk_cmd_buffer *cmdbuf,
                                        struct panvk_xfb_pending_draw *draw)
{
   const uint32_t vertex_count = draw->vertex_count;
   const uint32_t instance_count = draw->instance_count;
   const uint64_t index_buffer = draw->index_buffer;
   const uint32_t index_size = draw->index_size;
   const uint32_t xfb_topology = draw->xfb_topology;
   const uint64_t indirect_buffer = draw->indirect_buffer;
   const uint32_t restart_index = draw->restart_index;
   const uint32_t index_count_bound = draw->index_count_bound;

   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;
   const struct panvk_shader *shader = state->vs.shader;

   draw->prepared = false;

   if (!shader || !shader->xfb_variant || !state->xfb.offsets_gpu)
      return;

   const struct panvk_shader_variant *xfb_variant = shader->xfb_variant;

   /* The three things the dispatch used to read live, and the reason this
    * function exists: which shader captures, which descriptor table it fetches
    * attributes through, and which Begin/End pair's write positions it
    * advances.
    */
   draw->xfb_variant = xfb_variant;
   draw->res_table = state->vs.desc.res_table;
   draw->offsets = state->xfb.offsets_gpu;

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

   /* Primitive restart makes slot -> input vertex data-dependent, so the
    * kernel resolves it into this table and the shader reads it directly.
    * Sized by the worst case: every index could complete a primitive, so a
    * strip needs verts_per_prim entries per index.
    *
    * index_count_bound rather than vertex_count, because an indirect draw's
    * real count is only in the indirect command. It is the capacity of the
    * bound index buffer there, which over-allocates but is never too small.
    */
   struct pan_ptr slot_table = {0};
   if (restart_index && index_count_bound) {
      uint32_t vpp = xfb_topology == PANVK_XFB_TOPO_POINT_LIST      ? 1
                     : xfb_topology <= PANVK_XFB_TOPO_LINE_STRIP    ? 2
                                                                    : 3;
      slot_table = panvk_cmd_alloc_dev_mem(
         cmdbuf, desc, (uint64_t)index_count_bound * vpp * sizeof(uint32_t),
         sizeof(uint32_t));
      if (!slot_table.gpu) {
         vk_command_buffer_set_error(&cmdbuf->vk,
                                     VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }
   }
   draw->slot_table = slot_table.gpu;

   /* The push-uniform upload reads cmdbuf->state.gfx.sysvals, so this draw's
    * capture sysvals have to be in there across the call - and only across the
    * call. Saving and restoring the whole struct keeps that invisible to the
    * graphics state: the real draws around this one see exactly what they set,
    * and no dirty tracking is disturbed because nothing observably changed.
    */
   struct pan_ptr push_uniforms;
   {
      const struct panvk_graphics_sysvals saved = state->sysvals;

      /* Patched from the kernel for an indirect draw, whose base only exists
       * in the indirect command.
       */
      state->sysvals.vs.first_vertex = draw->vertex_base;

      state->sysvals.xfb.num_vertices = vertex_count;
      state->sysvals.xfb.index_buffer = index_buffer;
      state->sysvals.xfb.index_size = index_size;
      state->sysvals.xfb.topology = xfb_topology;
      state->sysvals.xfb.slot_table = slot_table.gpu;

      /* Base only; panlib_xfb_setup() overwrites this slot in the uploaded
       * push-uniform buffer with base + offset*stride, since only it knows the
       * GPU-resident write position.
       */
      for (uint32_t i = 0; i < state->xfb.bound_count; i++)
         state->sysvals.xfb.buffer_addrs[i] = state->xfb.bufs[i].address;

      VkResult result = panvk_per_arch(cmd_prepare_gfx_push_uniforms)(
         cmdbuf, xfb_variant, &push_uniforms, 1);

      state->sysvals = saved;

      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmdbuf->vk, result);
         return;
      }
   }
   draw->push_uniforms = push_uniforms.gpu;

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
   draw->spd = panvk_priv_mem_dev_addr(compute_spd);
   draw->tsd = tsd;

   /* Clamp the capture to what fits, in a helper kernel.
    *
    * The write position lives in GPU memory, so deciding how much fits means
    * dividing by the stride at dispatch time - which the command stream
    * cannot do on this arch. The kernel also owns the XFB query counters and
    * advancing the write position, since both depend on the clamped result.
    * Same division of labour as Asahi hk's setup_xfb_buffer().
    *
    * Its inputs are all bound state, so they are gathered here; only the
    * launch itself is left to the command stream.
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

   draw->descs = xfb_descs.gpu;
   draw->desc_count = desc_count;
   draw->out_slots = out_slots.gpu;

   draw->first_vertex_pu =
      indirect_buffer && push_uniforms.gpu &&
            shader_uses_sysval(xfb_variant, graphics, vs.first_vertex)
         ? push_uniforms.gpu +
              shader_remapped_sysval_offset(
                 xfb_variant, sysval_offset(graphics, vs.first_vertex))
         : 0;
   draw->index_buffer_pu =
      indirect_buffer && index_size && push_uniforms.gpu &&
            shader_uses_sysval(xfb_variant, graphics, xfb.index_buffer)
         ? push_uniforms.gpu +
              shader_remapped_sysval_offset(
                 xfb_variant, sysval_offset(graphics, xfb.index_buffer))
         : 0;
   draw->num_vertices_pu =
      push_uniforms.gpu &&
            shader_uses_sysval(xfb_variant, graphics, xfb.num_vertices)
         ? push_uniforms.gpu +
              shader_remapped_sysval_offset(
                 xfb_variant, sysval_offset(graphics, xfb.num_vertices))
         : 0;

   draw->prepared = true;
}

/* Launches the prepared capture as a plain compute job on
 * PANVK_SUBQUEUE_COMPUTE, one thread per (vertex, instance) pair, mirroring
 * GENX(csf_launch_xfb)'s register setup exactly where the two subqueue
 * models allow it to translate directly. Only called from
 * panvk_per_arch(cmd_flush_pending_xfb_captures)() below, never directly
 * from CmdDraw - see that function and the module comment for why.
 *
 * Everything host-side already happened in cmd_prepare_xfb_capture() above,
 * at record time. This function must therefore read nothing out of
 * cmdbuf->state.gfx: by now it describes the last draw of the render pass,
 * not this one. The two things it does read from cmdbuf->state are legitimate
 * - the TLS descriptor is per command buffer, and the VERTEX_TILER sync point
 * is per render pass, which is exactly the granularity of this flush.
 */
static void
dispatch_one_xfb_capture(struct panvk_cmd_buffer *cmdbuf,
                         const struct panvk_xfb_pending_draw *draw)
{
   if (!draw->prepared)
      return;

   const struct panvk_shader_variant *xfb_variant = draw->xfb_variant;

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
      cs_move64_to(b, cs_scratch_reg64(b, 0), draw->tsd);
      cs_store64(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 0), 8);
      cs_flush_stores(b);
   }

   /* Launch the clamp/counter kernel prepared above. */
   {
      struct panvk_precomp_ctx pctx = panvk_per_arch(precomp_cs)(cmdbuf);
      struct panlib_xfb_setup_args args = {
         .offsets = draw->offsets,
         .descs = draw->descs,
         .desc_count = draw->desc_count,
         .direct_vertex_count = draw->vertex_count,
         .direct_instance_count = draw->instance_count,
         .topology = draw->xfb_topology,
         .out_slots = draw->out_slots,
         .query = draw->query_ptr,
         .indirect = draw->indirect_buffer,
         .index_buffer_base = draw->index_buffer,
         .index_size = draw->index_size,
         .slot_table = draw->slot_table,
         .restart_index = draw->restart_index,
         .first_vertex_pu = draw->first_vertex_pu,
         .index_buffer_pu = draw->index_buffer_pu,
         .num_vertices_pu = draw->num_vertices_pu,
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
      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_SRT), draw->res_table);

      uint64_t fau_count = xfb_variant->fau.total_count;
      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_FAU),
                   draw->push_uniforms | (fau_count << 56));

      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_SPD), draw->spd);
      cs_move64_to(b, cs_reg64(b, PANVK_PRECOMP_TSD), draw->tsd);

      /* Bias attribute fetch by the draw's firstVertex (non-indexed) or
       * vertexOffset (indexed), matching VERTEX_OFFSET in the real IDVS
       * draw (launch_draw()). This is why the indexed path needs no
       * vertexOffset maths in the shader.
       */
      if (draw->indirect_buffer) {
         /* firstVertex is word 2 of VkDrawIndirectCommand, but the indexed
          * command has vertexOffset at word 3 instead. Reading either from an
          * application buffer needs no cache flush - nothing in our command
          * stream produced it.
          */
         cs_move64_to(b, cs_scratch_reg64(b, 8), draw->indirect_buffer);
         cs_load32_to(b, cs_sr_reg32(b, COMPUTE, GLOBAL_ATTRIBUTE_OFFSET),
                      cs_scratch_reg64(b, 8), draw->index_size ? 12 : 8);
      } else {
         cs_move32_to(b, cs_sr_reg32(b, COMPUTE, GLOBAL_ATTRIBUTE_OFFSET),
                      (uint32_t)draw->vertex_base);
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

      cs_move64_to(b, cs_scratch_reg64(b, 6), draw->out_slots);
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

/* Emits one recorded counter-buffer transfer, in either direction.
 *
 * Both directions read something the command stream itself produced earlier -
 * a WRITEBACK reads write positions last written by panlib_xfb_setup(), a
 * SEED reads a counter buffer last written by a WRITEBACK - so both need the
 * caches cleaned first. A barrier alone does not make those stores visible
 * here. The flush is per op rather than hoisted because the ops interleave
 * with capture dispatches that write in between.
 */
static void
emit_xfb_counter_op(struct panvk_cmd_buffer *cmdbuf,
                    const struct panvk_xfb_counter_op *op)
{
   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);
   struct cs_index src = cs_scratch_reg64(b, 0);
   struct cs_index dst = cs_scratch_reg64(b, 2);
   struct cs_index flush_id = cs_scratch_reg32(b, 4);
   struct cs_index val = cs_scratch_reg32(b, 5);

   cs_move32_to(b, flush_id, 0);
   cs_flush_caches(b, MALI_CS_FLUSH_MODE_CLEAN, MALI_CS_FLUSH_MODE_CLEAN,
                   MALI_CS_OTHER_FLUSH_MODE_NONE, flush_id,
                   cs_defer(SB_IMM_MASK, SB_ID(DEFERRED_FLUSH)));
   cs_wait_slot(b, SB_ID(DEFERRED_FLUSH));

   /* The only difference between the two directions is which end is the
    * counter buffer and which is the write position for this buffer index.
    */
   if (op->type == PANVK_XFB_COUNTER_SEED) {
      cs_move64_to(b, src, op->dev_addr);
      cs_load32_to(b, val, src, 0);
      cs_move64_to(b, dst, op->offsets);
      cs_store32(b, val, dst, op->buf_idx * sizeof(uint32_t));
   } else {
      cs_move64_to(b, src, op->offsets);
      cs_load32_to(b, val, src, op->buf_idx * sizeof(uint32_t));
      cs_move64_to(b, dst, op->dev_addr);
      cs_store32(b, val, dst, 0);
   }

   cs_flush_stores(b);
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

   /* Replay captures and counter-buffer ops in the order they were recorded.
    *
    * The two queues are separate but their order is one order: an op stamped
    * draw_pos == i was recorded before capture i, so emitting every such op
    * first reproduces the application's sequence. That matters because a
    * Begin resuming from a counter buffer must observe the End that wrote it,
    * with the captures of the intervening pair in between - which is the
    * whole reason both directions are deferred to here.
    */
   const uint32_t num_draws = util_dynarray_num_elements(
      &state->xfb.pending_draws, struct panvk_xfb_pending_draw);
   const uint32_t num_ops = util_dynarray_num_elements(
      &state->xfb.pending_counter_ops, struct panvk_xfb_counter_op);
   const struct panvk_xfb_pending_draw *draws =
      util_dynarray_begin(&state->xfb.pending_draws);
   const struct panvk_xfb_counter_op *ops =
      util_dynarray_begin(&state->xfb.pending_counter_ops);
   uint32_t next_op = 0;

   for (uint32_t i = 0; i < num_draws; i++) {
      while (next_op < num_ops && ops[next_op].draw_pos <= i)
         emit_xfb_counter_op(cmdbuf, &ops[next_op++]);

      dispatch_one_xfb_capture(cmdbuf, &draws[i]);
   }

   /* Everything after the last capture - in particular the End that closes the
    * final pair, which is where a single-pair render pass does all its work.
    */
   while (next_op < num_ops)
      emit_xfb_counter_op(cmdbuf, &ops[next_op++]);

   util_dynarray_clear(&state->xfb.pending_draws);
   util_dynarray_clear(&state->xfb.pending_counter_ops);

   /* Safe to release only if no Begin/End pair is currently open: End runs
    * before CmdEndRendering, so the write positions have to outlive it and
    * survive until the captures that use them have actually been emitted.
    * Captures and writebacks both hold their own copy of the address, so
    * this only drops the current pair's - the allocation itself lives in
    * the command buffer's pool either way.
    *
    * This function also runs mid-render-pass, from
    * split_render_pass_for_xfb() (csf/panvk_vX_cmd_draw.c) closing a
    * backward dependency - see docs/xfb-render-pass-split.md. There, the
    * pair that triggered the split (a Begin resuming from the counter
    * buffer this same flush's writeback just produced) is still open:
    * its own Begin already replaced offsets_gpu with its own allocation,
    * and cmd_prepare_xfb_capture() for its own draw runs moments after
    * this call returns, in the same CmdDrawIndirectByteCountEXT. Zeroing
    * unconditionally here would null out that live allocation before it
    * is ever read, and cmd_prepare_xfb_capture() silently leaves the draw
    * unprepared (offsets_gpu is one of its own early-return checks) -
    * found via dEQP-VK.transform_feedback.simple.backward_dependency*,
    * where pair 2 read as unprepared despite everything upstream of it
    * (the counter buffer's value, the byte-count draw's own kernel) being
    * correct.
    */
   if (!state->xfb.active)
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

/* True if this render pass still owes a write to the counter buffer at
 * dev_addr - i.e. reading it now, before CmdEndRendering, would read a value
 * the captures have not produced yet.
 *
 * This is exactly the vkCmdDrawIndirectByteCountEXT backward dependency
 * (dEQP-VK.transform_feedback.simple.backward_dependency*): that draw's own
 * vertex count comes from a counter buffer an earlier End in this same
 * render pass wrote, and normally nothing makes that write final until
 * CmdEndRendering flushes every capture - too late for a draw recorded
 * before it. See docs/xfb-render-pass-split.md for what the caller does
 * with a true answer.
 */
bool
panvk_per_arch(cmd_xfb_counter_write_pending)(struct panvk_cmd_buffer *cmdbuf,
                                              uint64_t dev_addr)
{
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;

   util_dynarray_foreach(&state->xfb.pending_counter_ops,
                         struct panvk_xfb_counter_op, op) {
      if (op->type == PANVK_XFB_COUNTER_WRITEBACK && op->dev_addr == dev_addr)
         return true;
   }

   return false;
}
