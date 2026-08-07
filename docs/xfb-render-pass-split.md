# Splitting a render pass so XFB captures can be consumed inside it

Design note, 2026-08-07. Not implemented — this is the plan, written while
the surrounding code was fresh, plus the evidence behind it.

## The problem

`dEQP-VK.transform_feedback.simple.backward_dependency*` (8 cases) and
`draw_indirect_counter_resubmit` (1) record, inside **one** render pass:

```
Begin -> draw -> End(tfc) -> barrier -> Begin(tfc)
      -> vkCmdDrawIndirectByteCountEXT(tfc) -> End
```

`vkCmdDrawIndirectByteCountEXT` derives *its own draw's vertex count* from
`tfc`, on `PANVK_SUBQUEUE_VERTEX_TILER`, in the middle of the render pass.
The value it needs is produced by the first pair's capture, and a capture
cannot run until `flush_tiling()` has signalled the VERTEX_TILER syncobj —
which happens once, at `CmdEndRendering`. The consumer runs before the
producer, so the draw reads zero and draws nothing (`received:0`).

This is circular within one tiling batch. No reordering fixes it; the
render pass has to be cut at the dependency.

## Why not the cheaper alternatives

**Reorder the counter ops.** Already done, and it fixed `primitive_restart`
(1/4 -> 4/4). It does not help here: the consumer is a draw on another
subqueue, not a command-stream load that can be moved.

**Read the write position instead of the counter buffer.** Same value with
the same dependency, sourced differently. No help.

**Dispatch the capture inline, without waiting on the tiler.** Plausible on
paper — `docs/kbase-notes.md:4189` says the deferral exists because the
*wait* had nothing valid to wait on, not because of a data dependency, and
what a capture reads (`res_table`, descriptors, push uniforms) is CPU-written
at record time.

**Tried on hardware, 2026-08-07. Half-answered, and worth continuing.**
A `wait_vt` flag was threaded through `cmd_flush_pending_xfb_captures()` /
`dispatch_one_xfb_capture()` to skip the cross-subqueue wait, and
`CmdDrawIndirectByteCountEXT` called it inline when
`cmd_xfb_counter_write_pending()` said this draw's counter buffer was still
owed a write. The existing COMPUTE -> VERTEX_TILER barrier in that entry
point supplies the ordering for the draw.

- **No fault.** No `VK_ERROR_DEVICE_LOST`, no hang, no reboot. A capture
  dispatched mid-render-pass with no VERTEX_TILER wait left the device
  healthy. That was the outcome most expected to kill the idea, and it did
  not. The TLS hazard did not bite either, at least not here.
- **No fix.** `backward_dependency` stayed at 9/12 with a byte-identical
  `received:0 expected:64`. The draw still saw a zero counter.

Which of "the hypothesis is wrong" and "the inline flush has its own
ordering bug" that shows is **not yet determined**. The specific suspect is
that a capture ends in an asynchronous `cs_trace_run_compute`, and the
counter writeback emitted immediately after it does its cache flush and load
without waiting for that dispatch to retire — a gap that is masked in the
normal path, where the writeback sits at the end of the render pass with
everything else already drained. If that is it, the fix is a scoreboard wait
between the capture dispatch and a following counter op, not a render pass
split. **Check that before building the split**; it is a much smaller change
and this experiment is cheap to reconstruct (it was reverted rather than
committed, precisely because it is unproven).

## What already exists

PanVK already stores and reloads attachments mid-render-pass, for tiler heap
exhaustion — *incremental rendering*:

- `render->fb.spill.load` / `render->fb.spill.store`
  (`panvk_vX_cmd_draw.c:179-180, 416-435`) — "load every attachment from
  memory" / "store every attachment to memory", built with
  `pan_fb_load_iview()` / `pan_fb_store_iview()` for every render pass.
- `render.ir.fbds[PANVK_IR_{FIRST,MIDDLE,LAST}_PASS]`
  (`csf/panvk_vX_cmd_draw.c:1564-1614`) — three complete FBD sets:
  - FIRST: load `fb.load` (real loads/clears), store `spill.store`
  - MIDDLE: load `spill.load`, store `spill.store`
  - LAST: load `spill.load`, store `fb.store` (real stores + resolves)

So the attachment preload problem a split creates is already solved. What
does **not** exist is a host-side trigger: incremental rendering is driven
entirely from the tiler-OOM exception handler on the FRAGMENT subqueue
(`csf/panvk_vX_exception_handler.c:160-270`), and it never signals the
VERTEX_TILER syncobj, which is the one thing the capture needs.

## The plan

### Trigger

In `panvk_per_arch(CmdDrawIndirectByteCountEXT)`, before recording the draw:
split if any pending counter op is a `PANVK_XFB_COUNTER_WRITEBACK` whose
`dev_addr` equals this draw's counter-buffer address. That is exactly the
dependency and nothing else, so no non-XFB workload changes behaviour.

### The split itself

A new static function in `csf/panvk_vX_cmd_draw.c` (it needs the file's
statics), mirroring `CmdEndRendering`'s non-suspending path down to
`issue_fragment_jobs()` and then re-opening rather than finishing:

1. Bail out if nothing has been recorded yet (`!render->fbds.gpu &&
   !inherits_render_ctx(cmdbuf)`) — nothing to split.
2. `panvk_per_arch(cmd_select_tile_size)(cmdbuf)`.
3. Wrap any in-flight occlusion query, as `CmdEndRendering` does
   (`render->oq.last != state->occlusion_query.syncobj` -> `wrap_prev_oq()`).
4. `flush_tiling(cmdbuf)` — **this is the point of the exercise**: it ends
   the tiler batch and signals the VERTEX_TILER syncobj.
5. `panvk_per_arch(cmd_flush_pending_xfb_captures)(cmdbuf)` — the captures
   and counter ops of every pair recorded so far now run, so `tfc` becomes
   final. Already ordered correctly among themselves (`draw_pos`).
6. Fragment work for the batch just closed, using
   `render.ir.fbds[PANVK_IR_FIRST_PASS]` rather than `render.fbds`, so that
   every attachment is stored to memory even where the render pass's real
   `storeOp` is `DONT_CARE`. `issue_fragment_jobs()` needs a parameter for
   which FBD set to point `FRAGMENT.FBD_POINTER` at.
7. Reset the tiler state the way the OOM handler does
   (`panvk_vX_exception_handler.c:228-245`): per tiler descriptor,
   `cs_finish_fragment` then zero the polygon list, `completed_top` and
   `completed_bottom`.
8. Reset the host-side batch state so the next draw rebuilds lazily:
   `render->fbds = {0}`, `render->tiler = 0`, `render->oq = {0}`.
9. Point the resumed batch at the spill loads: `render->fb.load =
   render->fb.spill.load`. The next `get_fb_descs()` then produces exactly
   the `LAST_PASS` configuration (load spill, store real), which is what the
   final `CmdEndRendering` should run.

A second split in the same render pass must use `MIDDLE_PASS` for step 6,
by the same reasoning the OOM handler uses `ir_count` to choose.

### Where it will go wrong

- **Step 7 is the risky one.** It is CS surgery against tiler descriptors,
  and getting it wrong corrupts rendering in ways that will not look like an
  XFB bug. Copy the handler's sequence exactly rather than paraphrasing it.
- **`setup_tiler_oom_ctx()`** is called from `issue_fragment_jobs()` and
  describes the render pass to the OOM handler. After a split the handler
  must not still believe it is in the first IR pass; check `ir_count` /
  `counter` handling.
- **Interaction with real tiler OOM.** A render pass can now be split by the
  host *and* by the GPU. The two must agree on which IR pass configuration
  is current.
- **Multiview / layered rendering.** Steps 6-7 are per layer and per tiler
  descriptor; `calc_enabled_layer_count()` and `MAX_LAYERS_PER_TILER_DESC`
  both matter.

### Testing

Order matters — the render path is downstream of this, not just XFB:

1. `make regress` (30 probes) and the render probes with `--with-render`.
   A split that breaks ordinary rendering will show up here first.
2. `dEQP-VK.transform_feedback.simple.backward_dependency*` +
   `draw_indirect_counter_resubmit` (12 cases) — the target.
3. `simple.basic_*` (38), `primitive_restart` (4), `fuzz` (2,168) for
   regressions.
4. `dEQP-VK.renderpass.*` / `dEQP-VK.dynamic_rendering.*` — the groups that
   would actually catch a botched attachment reload. Not previously run
   against this driver, so expect to have to separate pre-existing failures
   from new ones: **measure them before making the change.**

Run everything in bounded chunks. ~170 cases is a safe slice for
execution-dense groups; the 668-case `fuzz` chunk froze the device and
needed `adb reboot`.
