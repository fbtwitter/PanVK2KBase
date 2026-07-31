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
- [x] Add a `pan_kmod_kbase` backend as a third `pan_kmod` backend
      alongside `panfrost` and `panthor` — **skeleton landed** at
      `src/mesa/pan_kmod_kbase.c`. Note it lives in *this* repo, not in
      the gitignored Mesa clone, and is synced in via
      `make mesa-backend-sync`; see `src/mesa/README.md` for why and for
      the full status table.
      **It builds as part of Mesa** — verified under WSL Ubuntu 24.04
      against Mesa 26.3.0-devel (`7296f9a`), meson 1.11.2, LLVM 18.1.3:
      `ninja src/panfrost/lib/kmod/libpankmod_lib.a` succeeds with
      `pan_kmod_kbase.c.o` in the archive next to the three upstream
      backends, exporting `kbase_kmod_ops` and `pan_kmod_fd_is_kbase`.
      That means it survives Mesa's full warning set — the real build
      caught a `-Werror=missing-prototypes` violation the standalone
      compile didn't, fixed by adding `pan_kmod_kbase.h` (mirroring
      `panthor_kmod.h`). Reproducible via `src/mesa/wsl-install-deps.sh`
      + `src/mesa/wsl-build.sh`. It also cross-compiles clean for
      aarch64-android against both r49p1 and r44p0.
      **Windows can't build it:** any panfrost target forces CLC → LLVM
      (`meson.build:976`), and `-Dmesa-clc=system` just needs a prebuilt
      native `mesa_clc` instead (`meson.build:965`). On Linux that's an
      `apt install` (libclc version must match LLVM's); on Windows it's a
      substantial LLVM build. Hence the WSL route.
      **Still never executed** — building is not running.
      **Scope correction (unchanged):** the vtable only covers
      device/BO/VM, not submission — see Phase 4 and
      `docs/architecture.md`'s "Correction" section for why a
      `pan_kmod_kbase` backend alone isn't sufficient.
- [x] **Android cross-build of the full driver** — this is Phase 6's
      "cross-compile Mesa for Android via a Meson Android cross-file,
      producing a standalone Vulkan ICD `.so`" arriving early, because
      it fell out of getting the backend building. Produces
      `libvulkan_panfrost.so` (18.8 MB, aarch64), with every kbase
      backend symbol in it. Needs Mesa's host shader compilers built
      natively first (`mesa_clc`, `panfrost_compile`) — a cross build
      can't run the aarch64 binaries it produces. Scripts:
      `src/mesa/wsl-fetch-ndk.sh`, `wsl-build-host-tools.sh`,
      `wsl-build-android.sh`; cross-file `android-aarch64-wsl.cross`.
      **Confirmed loading on the real device** via
      `tests/driver_load_probe/` — `dlopen()` from `/data/local/tmp`
      (no `/vendor` changes, no root): all `NEEDED` deps resolve and it
      exposes a well-formed Android hwvulkan HAL module (`HMI`, tag
      `HARDWARE_MODULE_TAG`, "Mesa 3D Vulkan HAL"), the same interface
      the device's own `vulkan.mali.so` uses.
      **But it cannot drive the GPU** — see the two unchecked items
      immediately below, both of which sit *above* this backend.
- [x] Get device probe working — `dev_create` does
      `VERSION_CHECK` → `SET_FLAGS` → `GET_GPUPROPS` and decodes the
      property blob into `pan_kmod_dev_props`, which is exactly the
      "wire `parse_gpu_props.h` into `pan_kmod_dev_props`" half noted
      below. The DRM-node-vs-misc-device mismatch is solved the way
      Turnip solves it for kgsl: `pan_kmod_dev_create()` gets a kbase
      probe *before* `drmGetVersion()` (which fails on `/dev/mali0`),
      using `KBASE_IOCTL_VERSION_CHECK` as a side-effect-free test. Kept
      as a readable patch at `src/mesa/pan_kmod.c.kbase.patch` rather
      than auto-applied, since upstream `pan_kmod.c` moves.
- [x] **Enumeration above `pan_kmod` — done, and the backend now runs.**
      `src/mesa/patch-panvk-kbase-enumeration.py` adds a
      `physical_devices.enumerate` hook that opens `/dev/mali0`. Mesa's
      `vk_instance` calls it before DRM enumeration and falls through on
      `VK_ERROR_INCOMPATIBLE_DRIVER`, so one binary still works on
      panfrost/panthor. Confirmed on-device via
      `tests/driver_enum_probe/`: `kbase: SET_FLAGS ok` /
      `props ok, gpu_id=0xc8700010` — matching what `first_test` reads
      from the hardware. So `open` → `VERSION_CHECK` → `SET_FLAGS` →
      `GET_GPUPROPS` → `pan_kmod_dev_props` all execute inside
      `pan_kmod_kbase.c`.
      **Non-obvious rule found doing this:** `KBASE_IOCTL_VERSION_CHECK`
      may be issued only *once per fd* — a second call returns `-EPERM`
      while leaving the handshake in effect
      (`tests/double_handshake_probe/`). This broke the path twice: once
      inside the backend, and once because PanVK probed *and* the
      dispatch probed, so the dispatch saw the failing second call,
      decided it wasn't kbase, and silently fell through to
      `drmGetVersion()`. Only the dispatch may probe.
      **Next blocker is Phase 4's, arriving early:** device init now
      fails at `vk_drm_syncobj_get_type(dev->fd)`, which needs a real
      DRM fd — `-3 VK_ERROR_INITIALIZATION_FAILED`. Exactly what
      `docs/architecture.md` predicted.
- [ ] Give PanVK a non-DRM `vk_sync` implementation. This is the new
      blocker: `get_device_sync_types()` calls
      `vk_drm_syncobj_get_type(dev->fd)` unconditionally, so physical
      device creation cannot succeed on a misc device no matter what the
      backend does. Needs a `vk_sync` type backed by whatever kbase
      offers — which loops back to the unsolved completion-mechanism
      question in `docs/kbase-notes.md`.
      **Design answer found in Panfork:** there is no kbase fence object
      to translate. Panfork builds its own syncobj over GPU-visible event
      memory allocated with `BASE_MEM_CSF_EVENT` (a flag this repo has
      never set), signalled by the command stream itself and waited on via
      `base_csf_notification` reads off the kbase fd. Same answer serves
      Phase 4's fence-shim item. See "Finding 2" in `docs/kbase-notes.md`.
- [ ] Fill in the deliberately-stubbed ops: `bo_import`/`bo_export`
      (dma-buf, Phase 3 — and blocked above the backend too, since the
      common `pan_kmod_bo_import()` goes through `drmPrimeFDToHandle()`),
      `vm_create`/`vm_destroy`/`vm_bind` (kbase has no explicit VM
      object — needs a design decision, see `src/mesa/README.md`), and
      `bo_wait` (blocked on the unsolved fence mechanism).

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
      `docs/mesa-cs-builder.md` for the full build setup.
      **Wired into a live KICK — submitted cleanly, never executed:**
      `tests/live_kick_probe/live_kick_probe.c` encodes a real `MOVE32`
      into a bound queue's ring buffer, writes `CS_INSERT` (a byte
      offset, per `utils/csf_user_regs.h`), and kicks for real. `KICK`
      returns 0 but `CS_EXTRACT`/`CS_ACTIVE` both stay 0 — the GPU never
      runs it. No hang; device healthy afterward. Root cause located in
      the kernel source: the CS is only programmed/started by
      `onslot_csg_add_new_queue()` once the scheduler puts the group on
      a **CSG slot**, and that call is skipped for a freshly created
      group (`scheduler_group_schedule()` returns 0 unconditionally and
      only marks the group runnable). So `KICK` succeeding proves
      nothing about execution.
      **Ruled out so far:** CS interface index; endpoint masks (real
      `RAW_SHADER_PRESENT` mask, compute-only, and `~0ULL` all fail
      identically); ring-buffer validity; and firmware capacity —
      `tests/glb_iface_probe/glb_iface_probe.c` shows the firmware
      offers 8 CSG slots × 8 streams at interface v3.6.0, so there is
      no shortage of anything.
      **Vendor blob reverse-engineered for comparison.** Mapped
      `libGLES_mali.so`'s complete kbase ioctl surface (50 call sites
      resolved via `ioctl@plt` + `mov`/`movk` pairing — see
      `docs/kbase-notes.md` for the method and the non-obvious pitfalls).
      It calls several things this repo never does: `MEM_JIT_INIT`,
      `MEM_EXEC_INIT`, `CS_TILER_HEAP_INIT`, `CONTEXT_PRIORITY_CHECK`,
      `GET_CONTEXT_ID`, `STREAM_CREATE`. It also uses
      `CS_QUEUE_GROUP_CREATE_1_6` (nr 42), not the modern nr 58 these
      probes use.
      **That experiment is now run, and is another clean negative:**
      `live_kick_probe.c` does the full vendor-style setup
      (`MEM_JIT_INIT` + `MEM_EXEC_INIT` + `CS_TILER_HEAP_INIT`, all
      succeeding — the heap gets a real GPU VA `0x7ffc000000`) and tries
      group create via both nr 58 and nr 42. Result is unchanged:
      `CS_EXTRACT=0`, `CS_ACTIVE=0`, no notification, every time. So
      neither the missing context setup nor the group-create struct
      version is the blocker.
      **RESOLVED — everything above this line about the GPU never
      executing was a measurement error in this repo, not a kernel or
      firmware behaviour. The GPU runs the command stream.** The queue's
      3 user-IO pages are ordered `[doorbell][input][output]`, not
      `[input][output][doorbell]`. So `live_kick_probe` was writing
      `CS_INSERT` into the doorbell page and polling `CS_EXTRACT`/
      `CS_ACTIVE` out of the input page — which the kernel zeroes at bind
      and firmware never writes. Every `CS_EXTRACT=0`/`CS_ACTIVE=0`
      reading recorded above was a read of a dead page.
      Found by comparing against Panfork (`third_party/PANFORK`, a
      Panfrost-on-kbase driver that ran on real hardware —
      `pan_vX_base.c:1434`), then measured on-device rather than assumed:
      `tests/user_io_probe` writes `CS_INSERT` at each candidate page with
      a fresh group each time and diffs the whole 12KB mapping. 6/6 runs:
      page 0 changes nothing anywhere; page 1 makes page 2 + 0x00 advance
      to the CS size ~50ms later, unwritten by userspace. Corroborated by
      page 0 persisting across processes while pages 1 and 2 come up
      freshly zeroed.
      After the two-line offset fix and nothing else,
      `live_kick_probe` reports **3 of 3 configs actually executed on the
      GPU** (nr 58, `_1_6`/nr 42, and compute-only), `CS_EXTRACT`
      advancing to 8 after ~50ms in each.
      **Withdrawn as a result:** the CSG-slot theory, the
      `kbase_csf_mcu_shared_group_bind_csg_reg()` MCU-shared-region
      hypothesis, and the claim that this was blocked on root. None of the
      kernel-side visibility (`dmesg`, debugfs, `/proc/mtk_mali/*`) was
      needed. Full writeup in `docs/kbase-notes.md`.
      **Blocked on kernel visibility, and it needs root.** Verified on
      this device: `dmesg` → `Permission denied` (no `CAP_SYSLOG`),
      `/sys/kernel/debug/mali0/` absent, tracefs readable but event
      enable/read denied — and the registered `mali` tracepoints are
      memory/JIT only, with no CSG-scheduling ones. `ro.build.type=user`,
      `ro.debuggable=0`, SELinux enforcing as `u:r:shell:s0`. No
      developer-options toggle changes this. Full writeup in
      `docs/kbase-notes.md`.
- [ ] Map VkQueueSubmit onto kbase atom/command-stream submission. Real
      target identified from the Mesa clone (`third_party/MESA-KMOD`,
      see `docs/architecture.md`): `src/panfrost/vulkan/csf/
      panvk_vX_gpu_queue.c` calls `DRM_IOCTL_PANTHOR_GROUP_CREATE` /
      `_SUBMIT` / `_DESTROY` / `_GET_STATE` and `DRM_IOCTL_PANTHOR_
      TILER_HEAP_CREATE` / `_DESTROY` directly — not through
      `pan_kmod_ops`. A kbase target needs a sibling file swapping those
      for `CS_QUEUE_GROUP_CREATE` / `CS_QUEUE_REGISTER` / `CS_QUEUE_BIND`
      / `CS_QUEUE_KICK` (prototyped in `tests/queue_group/queue_group.c`
      — every ioctl round-trips, but see the live-KICK finding above:
      that is *not* the same as the GPU executing anything, and getting
      a group actually scheduled onto a CSG slot is still unsolved) and
      `CS_TILER_HEAP_INIT`/`_TERM` for the tiler-heap half. The tiler
      heap may in fact be part of the answer — one hypothesis for the
      unscheduled group is that it needs more setup before the scheduler
      considers it schedulable.
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
