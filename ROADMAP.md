# Roadmap

Checkboxes are for tracking your own progress — nothing past Phase 1 is
done yet. Phases are ordered by dependency, not by how interesting they
are; resist the urge to jump to Phase 3.

Adapted from a companion scaffold repo's roadmap to match this repo's
actual progress: this project already has a real vendored kbase UAPI
header and a working standalone probe, so Phase 0 and Phase 1 are further
along here than a from-scratch roadmap assumes. Track deviations from the
original recommendations inline below.

**End goal this roadmap is aimed at:** a PanVK build that runs on real
Android hardware and can be packaged/loaded the way Turnip is on Adreno —
a standalone Vulkan driver a user can drop into a custom-driver picker
(Eden, Azahar, Skyline, Winlator, etc.) instead of the phone's stock Mali
blob driver. Phases 2–6 (device probe through WSI) are the prerequisite
for that to even be possible; Phase 6 below includes the actual Android
packaging step. This is a long path — see Phase 6 and Phase 7 notes for
why "headless triangle" (Phase 5) is nowhere near "usable in an emulator."

## Phase 0 — Ground yourself
- [x] Pick ONE target: **Mali-G615-MC2**, real device, UK interface 1.20
      (CSF) / driver release r44p0. Headers vendored in
      `third_party/kbase-uapi-r44p0/`, pulled
      from the `nest-open-source` mali-driver mirror — see that
      directory's README.md for exact commit/provenance.
      **Deviation to verify:** the original roadmap recommended
      Mali-G610 (Valhall v10) specifically because it's PanVK's only
      *conformant* target on the panthor side. G615 needs its
      arch/product major confirmed against
      `third_party/kbase-uapi-r44p0/gpu/mali_kbase_gpu_id.h`'s table
      (see `utils/parse_gpu_props.h`'s `gpu_model()`) and cross-checked
      against PanVK's actual conformance list before assuming G615 gets
      the same conformant treatment as G610.
- [ ] Get the device running the proprietary/vendor Mali blob driver
      first, so you have a correctness + performance oracle to diff
      against later.
- [x] Identify whether your kernel exposes JM (job-manager, older) or CSF
      (command-stream frontend, v10+) kbase ioctls. `first_test.c` builds
      with `-DMALI_USE_CSF=1` (see `makefile`), so this repo is already
      assuming CSF — confirmed for the Poco X8 Pro: `/vendor/firmware/
      mali_csffw.bin` (CSF-only firmware blob) exists on-device, and the
      probe's reported UK version (`major=1`) matches CSF's numbering
      scheme in the vendored headers, not JM's (`major=11`). See
      `docs/kbase-notes.md`.
- [x] Pull the exact kbase UAPI header for the target kernel — done,
      `third_party/kbase-uapi-r44p0/`. This is a **real** header, not a
      stub; keep it in sync if the target kernel's kbase version changes.

## Phase 1 — Standalone probe (no Mesa yet)
- [x] Build and run a standalone probe on-device —
      `tests/first_test/first_test.c` (`make` via the root `makefile`).
- [x] Confirm you can open the device node, query GPU properties, and
      close it cleanly. Current probe does: `KBASE_IOCTL_VERSION_CHECK`
      → `KBASE_IOCTL_SET_FLAGS` → `KBASE_IOCTL_GET_GPUPROPS` (two-step
      size probe + fetch) → `parse_gpuprops()` (`utils/parse_gpu_props.h`),
      which decodes the GPU ID, shader core count, L2 slice count, and
      max frequency from the raw property blob.
- [x] Diff the probe's decoded output against what the vendor blob driver
      reports for the same device (see Phase 0's blob-driver item) to
      sanity-check the decoding, not just that the ioctl round-trips. Done
      for the Poco X8 Pro — pulled `libGLES_mali.so` off-device and found
      it embeds both `r49p1` and `Mali-G720`, matching the probe's decode
      and the kbase UK version. See `docs/kbase-notes.md`.

## Phase 2 — pan_kmod backend skeleton
- **Signal, not progress (superseded below):** `.gitignore` gained a
  `/third_party/MESA-KMOD` entry in root main's `a1644be` ("added queue
  group generation") — a placeholder for a local Mesa clone, correctly
  excluded from git. Worth checking with upstream before duplicating a
  `pan_kmod_kbase` skeleton independently.
- [x] Clone Mesa — done into that same gitignored path (`git clone
      --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git
      third_party/MESA-KMOD`). Local/ephemeral to this machine only —
      gitignored, won't persist across clones of this repo, re-run the
      clone command if it's missing.
- [ ] Add a `pan_kmod_kbase` backend as a third `pan_kmod` backend
      alongside `panfrost` and `panthor`. Confirmed real shape from the
      clone (`src/panfrost/lib/kmod/`): `pan_kmod.c/h` (the vtable +
      dispatch), `pan_kmod_backend.h`, `panfrost_kmod.c` (JM),
      `panthor_kmod.c/h` (CSF, DRM) — mirror `panthor_kmod.*` since it's
      the CSF sibling, not `panfrost_kmod.c` (JM). **Scope correction:**
      the vtable only covers device/BO/VM, not submission — see Phase 4
      and `docs/architecture.md`'s "Correction" section for why a
      `pan_kmod_kbase` backend alone isn't sufficient.
- [ ] Get device probe + enumeration working — this is where the
      DRM-node-vs-misc-device mismatch in `docs/architecture.md` has to
      actually be solved. Look at how Turnip's kgsl path is special-cased
      in physical-device enumeration and mirror that shape. The
      GPU-properties parsing already working in `utils/parse_gpu_props.h`
      is the raw-decode half of this; the other half is wiring that into
      `pan_kmod_dev_props`.

## Phase 3 — Memory management
- [x] BO create through kbase's mem-alloc ioctl —
      `kbase_bo_create()` in `utils/memory.h`. Confirmed on-device (Poco
      X8 Pro, r49p1 headers): `KBASE_IOCTL_MEM_ALLOC` + `mmap()` both
      round-trip, decoded output flags include `SAME_VA` (kernel adds it
      even though the input flags leave it commented out — worth
      understanding why before relying on it).
- [x] BO free — `kbase_bo_free()` in `utils/memory.h`. Confirmed
      on-device: for these SAME_VA allocations, `munmap()` alone is the
      free (kbase tears the region down on `vm_close`); calling
      `KBASE_IOCTL_MEM_FREE` afterward fails `EINVAL` since the region's
      already gone by then, so it's `munmap()`-only. Wired into
      `memory2.c` and `queue_group.c`'s teardown.
- [x] mmap — confirmed working (see above).
- [ ] dma-buf import/export.
- [ ] Tiler heap / JIT growable memory — PanVK's current growth logic
      assumes panthor/panfrost conventions; expect to adapt it.

## Phase 4 — Submission and sync (highest risk)
- [x] First real submission-chain round-trip —
      `tests/queue_group/queue_group.c`. Confirmed on-device (Poco X8
      Pro, r49p1 headers): `CS_QUEUE_GROUP_CREATE` → BO alloc →
      `CS_QUEUE_REGISTER` → `CS_QUEUE_BIND` → `CS_QUEUE_KICK` all return
      `ret=0`, no error path hit. **Caveat:** this only confirms the
      ioctls succeed syscall-wise, not that the GPU actually executed or
      completed anything — the probe writes sentinel words into the
      queue buffer but never builds a real command stream, and the
      doorbell/ring-buffer region from `bind.out.mmap_handle` is never
      mapped (commented out in the source) or read back. Still open:
      confirming actual GPU-side completion.
      **Update:** the doorbell/ring-buffer mmap is now wired up (3 pages
      — `BASEP_QUEUE_NR_MMAP_USER_PAGES`: input/output/HW-doorbell — per
      `csf/mali_base_csf_kernel.h`) and confirmed mapping successfully
      on-device. Full teardown (`CS_QUEUE_TERMINATE` →
      `CS_QUEUE_GROUP_TERMINATE` → `kbase_bo_free`) also confirmed clean,
      no failed ioctls. Still doesn't prove GPU execution completed —
      just that the whole submit/bind/kick/teardown lifecycle round-trips
      without kernel-side rejection.
- [ ] Confirm the completion/fence signaling mechanism kbase exposes for
      a submitted queue — r49p1 (MediaTek fork) adds
      `KBASE_IOCTL_INTERNAL_FENCE_WAIT` under `CONFIG_MALI_MTK_DEBUG_DUMP`/
      `CONFIG_MALI_MTK_FENCE_DEBUG` that r44p0 doesn't have; worth
      checking whether that's usable before designing a generic shim
      (see `docs/kbase-notes.md`).
      **Resolved as a dead end, don't revisit:** `tests/fence_probe/
      fence_probe.c` confirmed the kernel implements the ioctl (`ret=0`,
      not `ENOTTY`), then tested it against a real bound CS queue (real
      GPU VA, real pid, every documented flag, before/after `KICK`,
      2-second requested timeout) — every variant returned in ~0.0ms.
      That's not a real wait mechanism for any input this repo's ioctl
      sequence can produce; most likely a kernel-internal MTK debug hook,
      not a userspace completion API. Full writeup in
      `docs/kbase-notes.md`.
      **Redirect tried, inconclusive (not another dead end):**
      `tests/event_probe/event_probe.c` polls the kbase fd and reads
      `struct base_csf_notification` when ready, at three points
      (idle, bound, after `KICK`). `poll()` never returned readable, not
      even 2s after kicking a queue filled with `0xdeadbeef`. Most
      likely cause: this repo's `KICK` never updates the insert offset
      in the queue's mmap'd input page, so firmware probably never sees
      it as real work to fault on — not evidence the notification
      channel itself doesn't work. Confirming it needs an actual minimal
      CS instruction stream (real CSF ISA encoding), which is arguably
      this checklist item's own scope ("map VkQueueSubmit onto kbase
      command-stream submission") rather than a quick probe. Full
      writeup in `docs/kbase-notes.md`.
      **CSF instruction encoder built and verified offline:**
      `tests/cs_encode_probe/cs_encode_probe.c` links Mesa's own
      `cs_builder.h` (not hand-encoded bytes) and confirms it produces
      correct instruction bytes for this device's architecture — see
      `docs/mesa-cs-builder.md` for the full build setup. Not yet wired
      into a live `KICK`; that's the next step, and the point where this
      moves from "verified offline" to "executed by real GPU firmware."
- [ ] Map VkQueueSubmit onto kbase atom/command-stream submission. Real
      target identified from the Mesa clone (`third_party/MESA-KMOD`,
      see `docs/architecture.md`): `src/panfrost/vulkan/csf/
      panvk_vX_gpu_queue.c` calls `DRM_IOCTL_PANTHOR_GROUP_CREATE` /
      `_SUBMIT` / `_DESTROY` / `_GET_STATE` and `DRM_IOCTL_PANTHOR_
      TILER_HEAP_CREATE` / `_DESTROY` directly — not through
      `pan_kmod_ops`. A kbase target needs a sibling file swapping those
      for `CS_QUEUE_GROUP_CREATE` / `CS_QUEUE_REGISTER` / `CS_QUEUE_BIND`
      / `CS_QUEUE_KICK` (already prototyped and confirmed working
      end-to-end on-device in `tests/queue_group/queue_group.c`) and
      `CS_TILER_HEAP_INIT`/`_TERM` for the tiler-heap half.
- [ ] Build the fence-translation shim between kbase's completion
      mechanism and whatever PanVK's sync code expects to wait/signal on.
      Confirmed harder than "translate the ioctls": the same file signals
      completion via libdrm `drmSyncobj*` calls on `dev->drm_fd`, which
      only work on an actual DRM fd — kbase's `/dev/mali0` is a misc
      device, so there's no DRM fd to hang a syncobj off of. This isn't a
      wrong-ioctl-number problem, it's "the mechanism PanVK's sync code
      assumes doesn't exist on this kernel driver at all."
- [ ] Budget the most time here. This was the long pole for kgsl too.

## Phase 5 — Headless triangle
- [ ] Render to a buffer, dump to PNG, diff pixels. No WSI, no display.

## Phase 6 — WSI and Android driver packaging
- [ ] Only after Phase 5 is solid. Android gralloc/ANativeWindow if
      targeting phones, or DRM/kmsro if targeting an embedded board still
      on kbase.
- [ ] Once WSI actually presents to a real `Surface`/`ANativeWindow`:
      cross-compile Mesa for Android via a Meson Android cross-file
      (NDK toolchain, same shape as the community's Turnip-for-Android
      builds), producing a standalone Vulkan ICD `.so` — not a full
      system image integration, a droppable driver file.
- [ ] Package that `.so` with a `meta.json` manifest matching the
      Adrenotool/Turnip convention that custom-driver pickers in
      Eden/Azahar/Skyline/Winlator already know how to consume — this is
      what actually makes the driver "swap in" usable rather than just
      "builds for Android."
- [ ] Sanity-test the packaged driver in at least one of those pickers
      before assuming the packaging step itself is correct — a `.so`
      that loads is not the same as a `.so` that gets recognized and
      selected correctly by a given emulator's driver manager.

## Phase 7 — CTS-driven hardening
- [ ] dEQP-VK in stages: smoke → rendering → sync → compute → multisample
      → extensions. Keep an xfail list. Land fixes in small batches.

## Phase 8 — Real-app validation
- [ ] apitrace/gfxreconstruct captures of actual apps/games once CTS is
      mostly green. CTS will not catch everything real apps hit.

## Phase 9 — Upstream conversation
- [ ] Raise the project on #panfrost (Matrix/IRC) or mesa-dev BEFORE
      you're deep into Phase 4. Kbase is not currently a stated upstream
      priority (Panthor/Tyr are) — find out early whether this would be
      accepted upstream or needs to live as a maintained fork.
- [ ] If accepted, land behind an env var gate (precedent:
      `PAN_USE_KRAID=1` for the new shader compiler), small reviewable
      MRs per phase, not one large dump.

## Phase 10 — Keep it alive
- [ ] Get a kbase device into CI so it doesn't silently bitrot when
      panfrost/panvk core code changes.
- [ ] Track kbase UAPI drift — Arm's blob kernel driver evolves
      independently of whatever version you pinned to (currently r44p0 /
      UK 1.20 CSF; a second target, r49p1 / UK 1.30 CSF, is also vendored
      in `third_party/kbase-uapi-r49p1/` — see `docs/kbase-notes.md`).
