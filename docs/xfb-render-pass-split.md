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

---

# (unrelated) APK milestone 2: where presentation crashes

Recorded here only because this file was open; belongs in kbase-notes.md.

`vkCreateImage(VkNativeBufferANDROID)` SIGSEGVs on this device. Exact stack,
from the crash buffer (`adb logcat -b crash`):

    signal 11 (SIGSEGV), SEGV_MAPERR, fault addr 0x10   null pointer deref
    #00 pan_image_layout_init +212
    #01 panvk_image_init +2004
    #02 panvk_android_create_gralloc_image +368
    #03 present_frames                (src/android/swapchain_app)

The path is `panvk_android_anb_init()` (panvk_android.c:115):
`vk_android_get_anb_layout()` turns the gralloc handle into a
`VkImageDrmFormatModifierExplicitCreateInfoEXT` via u_gralloc, then
`panvk_image_init()` consumes it. Something in that layout is NULL or
otherwise not what `pan_image_layout_init` expects.

Prime suspect: u_gralloc's *fallback* backend is what runs on this device
(MediaTek gralloc, no dedicated backend), and it is the same component that
needed `patch-panvk-android-gralloc-fd.py` for the fd index. A fallback that
cannot determine the modifier would plausibly hand back
`DRM_FORMAT_MOD_INVALID` or a zeroed plane layout.

**Next step:** log what `vk_android_get_anb_layout()` actually produces -
`mod_info.drmFormatModifier`, `drmFormatModifierPlaneCount`, and
`layouts[0].{offset,size,rowPitch}` - before `panvk_image_init()` sees them.
That distinguishes "u_gralloc fallback cannot describe this buffer" from
"pan_image_layout_init does not handle a case it should". Do not guess
between those two; the fixes are in different projects.

Note this path had never executed before 2026-08-07: it needs a swapchain,
which needs a window, which needs an APK. The same blind spot hid the
`anb->handle->data[0]` bug fixed in the same session.

## Presentation: the -13 is the queue submit, not the export

Measured 2026-08-07 by logging either side of the submit in
`vk_common_QueueSignalReleaseImageANDROID` (`vk_android.c:578-602`):

    ANB-RELEASE submit result=-13

The export line never printed, so `GetSemaphoreFdKHR` is never reached.
`VK_ERROR_UNKNOWN` comes out of the `QueueSubmit2` (or
`vk_device_copy_semaphore_payloads`) that signals the SYNC_FD-exportable
semaphore `vk_anb_semaphore_init_once()` created.

Ruled out, each by direct measurement rather than inference:

- semaphore *creation* with `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT`
  succeeds (the app creates one itself: returns 0)
- device creation with `VK_KHR_external_semaphore_fd` / `external_fence_fd` /
  `external_memory_fd` enabled succeeds (`vkCreateDevice -> 0`)
- the fd export is not the failure - it never runs

Also worth noting: `panvk_kbase_sync.c` and `panvk_vX_kbase_queue.c` contain
no `VK_ERROR_UNKNOWN` of their own, so the error is being produced by the
shared runtime's submit path rather than returned directly by the kbase
backend. That points at a `vk_sync_type` capability check - the exportable
semaphore's sync type likely lacks a feature the submit path requires -
rather than at an outright "not implemented" in the backend.

**Next:** instrument or step `vk_queue_submit` / the panvk `QueueSubmit2`
entrypoint to find which check produces `VK_ERROR_UNKNOWN` for a semaphore
whose sync type is the kbase one. If it is a missing `vk_sync_features` bit
on the kbase sync type, that is a small, local fix; if the submit path
genuinely requires a real exportable fence, this runs into the documented
`KBASE_IOCTL_STREAM_CREATE` limitation and presentation needs a different
design on this kernel driver.

## The Eden freeze: the LINEAR assumption is the prime suspect

2026-08-07, after presentation started working.

Ruled out first, so this is not guesswork by elimination alone:

- **Not a per-present leak.** 600/600 frames with objects recycled through
  a 4-slot ring, device healthy. The earlier stop at frame 127 was the
  harness holding 127 images and 254 semaphores at once.
- **Not an unsignalled fence of ours.** `panvk_kbase_sync_export_sync_file()`
  blocks until the GPU is actually done and only then returns -1
  ("already signalled"), which is spec-legal *because* the wait happened.
  It costs a CPU block per present; it does not hand out live fences.

The fences the system hung on were `mali-0-1282_2-74-kcpu...` - pid 1282 is
**SurfaceFlinger**, i.e. the vendor blob driver's own context, not ours.
SurfaceFlinger's GPU work hung while compositing, with
`kworker/mali_mmu2` at ~98% kernel.

**Suspect: the modifier.** `panvk_android_anb_init()` now maps
`DRM_FORMAT_MOD_INVALID` to `DRM_FORMAT_MOD_LINEAR`, because u_gralloc's
fallback cannot determine the real modifier and the measured layout looked
linear (one plane, rowPitch exactly width * 4). That fixed the null-deref
and made presentation work - but it is an *assumption*, and Mali gralloc
commonly allocates **AFBC** for `GRALLOC_USAGE_HW_RENDER` buffers, which is
the usage the driver asks for (0x200).

If the buffer really is AFBC and we render to it as linear, the content is
wrong in exactly the way that makes the compositor's AFBC decoder fault -
which fits a spinning MMU kworker far better than anything on our side.
A clear-only test app would not notice: the image still shows *something*,
and Eden "ran, a bit laggy, then froze" fits a compositor progressively
choking on malformed buffers.

**Next, in order:**

1. Determine the real modifier rather than assuming. Options: query gralloc
   for it properly (a u_gralloc backend for this MediaTek/Arm gralloc rather
   than the fallback), or read it from the handle's metadata fds.
2. Failing that, force the assumption true: request a linear buffer by
   adding CPU-access usage bits, and see whether Eden stops freezing. That
   is a diagnostic, not a fix - it would cost AFBC's bandwidth savings.
3. If AFBC is confirmed, the LINEAR mapping must become a real modifier
   rather than a guess, or the ANB path should refuse buffers whose
   modifier it cannot determine instead of silently mis-describing them.

Note the shape of this mistake for next time: mapping INVALID to LINEAR
made the crash go away and the frames appear, so it looked like a fix. The
evidence supported "linear" only weakly - a stride consistent with linear
is also consistent with AFBC. "The symptom went away" is not the same as
"the assumption is true".

### Root cause of the modifier problem: IMapper is compiled out

The build has no `u_gralloc_imapper4/5` objects:

    build-android/src/util/u_gralloc/*.p/
      u_gralloc.c.o  u_gralloc_cros_api.c.o  u_gralloc_fallback.c.o
      u_gralloc_internal.c.o  u_gralloc_libdrm.c.o  u_gralloc_qcom.c.o

`src/util/u_gralloc/meson.build` only compiles the IMapper backends when
`dep_android_ui` or `dep_android_mapper4` is found. This project builds with
`-Dandroid-stub=true` (see `src/mesa/wsl-build-android.sh`), so neither is,
and the runtime backend search falls all the way through CROS -> GRALLOC4 ->
LIBDRM -> QCOM to FALLBACK.

The IMapper backends are the only ones that can ask gralloc for a buffer's
actual format modifier. Without them the driver *cannot* know whether a
buffer is linear or AFBC - the fallback has no way to find out, which is
exactly why it reports `DRM_FORMAT_MOD_INVALID`.

So this is not a missing line of code, it is a build configuration that
removes the capability. Options, roughly in order of honesty:

1. Build against the real AOSP mapper libs so `u_gralloc_imapper*` compiles.
   Needs `android.hardware.graphics.mapper@4.0` (or libui for IMapper5)
   headers and libs, which an NDK-only cross build does not have. This is
   the real fix and the largest piece of work.
2. Read the modifier out of the gralloc handle's ints directly. Vendor
   specific and fragile, but this repo already has precedent for handle
   archaeology (`patch-panvk-android-gralloc-fd.py` probes the fds).
3. Keep assuming, but stop assuming *silently*: if the modifier is unknown,
   either refuse the buffer or force a linear allocation by asking for
   CPU-access usage, rather than declaring a possibly-AFBC buffer linear.

Note this also means the AFBC hypothesis for the Eden freeze is currently
untestable from inside the driver - it cannot ask. Testing it needs (2) or
an external check of what gralloc actually allocated for usage 0x200.

### Asking gralloc for no AFBC does not work on this device

Tried and reverted, 2026-08-07. Arm gralloc spells "do not compress" as
`MALI_GRALLOC_USAGE_NO_AFBC` = `GRALLOC_USAGE_PRIVATE_1` = `0x20000000`, so
`vk_common_GetSwapchainGrallocUsageANDROID()` was patched to OR that into
the usage it returns.

The bit reached gralloc and was stored - the handle shows it:

    usage    0x20000200          (was 0x00000200)
    int[10]  0x20000b00          (was 0x00000b00)

but the allocation size did not move:

    int[08]  0x00dba400 = 14,394,880 bytes   unchanged, still the AFBC size

Linear would be 14,172,160. So this MediaTek gralloc records the private
usage bit and ignores it; Arm's convention does not apply here. Reverted
rather than left in place, because code that looks like a fix and does
nothing is worse than no code.

**That leaves declaring the real modifier as the only route.** The needed
value is a `DRM_FORMAT_MOD_ARM_AFBC(...)` with the block size and layout
bits gralloc actually used. What is known from the handle: 16x16 blocks
(the 221,440-byte header matches ceil(1280/16) * ceil(2768/16) * 16 exactly)
and RGBA_8888. What is not known: the sparse/split/YTR bits, which change
the modifier and which the header size cannot distinguish.

Guessing those is the same mistake as guessing LINEAR. Getting them
properly means the IMapper backend, i.e. building against real AOSP mapper
libs instead of `-Dandroid-stub=true` - which is now the critical path for
correct presentation, not an optimisation.

### Why IMapper is not reachable from this build, and what to do instead

`meson.build:1039-1059` only looks for `android.hardware.graphics.mapper`
(>= 4.0) and `ui` inside `if not with_android_stub`. This project builds
with `-Dandroid-stub=true`, so they are never probed, and the IMapper
backends are never compiled. Turning the stub off means supplying real
pkg-config deps for cutils/hardware/log/sync/nativewindow *and* the HIDL
mapper - an AOSP build environment, not an NDK cross build. That is exactly
what the stub exists to avoid, so "just enable IMapper" is a much larger
change than it sounds.

**A cheaper route that does not need IMapper: solve for the modifier.**

The allocation size is known exactly from the gralloc handle
(`int[08] = 14,394,880` for 1280x2768 RGBA_8888). PanVK already knows how
to compute the size of an AFBC image for a given modifier - it allocates
them itself. So instead of asking gralloc what the modifier is, enumerate
the handful of plausible Mali AFBC modifiers, compute the allocation size
each would imply, and keep the one that matches the measured figure.

    for each candidate modifier:
        size = pan_image_layout size for 1280x2768 RGBA8888 with that mod
        if size == 14,394,880: that is the modifier

Candidates are few: `DRM_FORMAT_MOD_ARM_AFBC(BLOCK_SIZE_16x16 | X)` where X
ranges over the usual combinations of `SPARSE`, `YTR`, `SPLIT`, `TILED`.
The 16x16 block size is already pinned by the header size (221,440 bytes =
ceil(1280/16) * ceil(2768/16) * 16).

Why this is not the LINEAR mistake again: that was an assumption with no
way to check it. This is a hypothesis with an exact numeric test, and a
candidate that does not reproduce the measured allocation size is rejected.
If two candidates produce the same size the method is ambiguous and must
say so rather than pick one - in which case fall back to IMapper.

This also generalises better than it looks: any device whose gralloc cannot
be queried still hands out a handle with a size in it.

### IMapper5: vtable confirmed, decode and import are the gap

Read the authoritative header (AOSP `hardware/interfaces`, stable-c
`IMapper.h`). `AIMapperV5` field order:

    1 importBuffer  2 freeBuffer  3 getTransportSize  4 lock  5 unlock
    6 flushLockedBuffer  7 rereadLockedBuffer  8 getMetadata
    9 getStandardMetadata  10 setMetadata  11 setStandardMetadata
    12 listSupportedMetadataTypes  13 dumpBuffer  14 dumpAllBuffers
    15 getReservedRegion

The hand-written declaration in `src/android/swapchain_app/main.c` matches
exactly through all nine fields it declares. So the vtable offsets are right
and that explanation for the `n=77` result is dead.

Two candidates remain, in the order worth testing:

1. **The buffer is probably not imported.** `getStandardMetadata` expects a
   buffer that has been through `importBuffer()`; the probe passes the raw
   `ANativeWindowBuffer` handle straight from `dequeueBuffer`. An invalid
   handle alone could produce a nonsense length, and this is the cheaper
   test.
2. **The payload is encoded.** The header says the return is "the number of
   bytes written, or which would have been written", and directs callers to
   the encode/decode helpers in `gralloctypes/Gralloc4.h` rather than
   treating the output as a scalar. So reading the trailing 8 bytes - the
   next thing that was about to be tried - would have been another
   plausible-looking guess.

Do (1) first: if the handle was simply invalid, (2) may not arise.

Two things recorded so nobody repeats them:

- Mesa's own `u_gralloc_imapper5_api.cpp` is **not** a model for this route.
  It includes `<ui/GraphicBufferMapper.h>` and AIDL headers, i.e. the C++
  AOSP path needing generated headers. The stable-C dlopen approach has no
  in-tree reference.
- `andlabs/libui` is an unrelated desktop GUI toolkit that happens to share
  the name. The real source is `platform/frameworks/native/libs/ui`, with
  `Gralloc5.cpp` as AOSP's own stable-C consumer.
