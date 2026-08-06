# Roadmap

Checkboxes are for tracking your own progress. **This line used to say
"nothing past Phase 1 is done yet" — stale since well before this update;
Phases 2–3 are done and Phase 4 (compute, semaphores, tiler heap) works
end to end on real hardware.** See "Where this actually is" below Phase 4
for the honest current line, and Phase 9 for what has already gone
upstream. Phases are ordered by dependency, not by how interesting they
are; resist the urge to jump to Phase 3 — though at this point the risk
runs the other way: check the code before assuming an item marked open
still is. Three were found stale in one afternoon (2026-08-01): the tiler
heap, the kbase queue integration decision, and `vm_bind`.

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
      **Synced forward to `43ec7c6b` (2026-08-06), was pinned `c439d52c`
      (2026-07-29 — 331 commits behind).** Fast-forward pull, no conflicts:
      checked first and confirmed neither `src/panfrost/lib/kmod/` (where
      the local kbase dispatch patch lives) nor `src/vulkan/runtime/
      vk_meta_clear.c` (the `vkCmdClearColorImage` bug from the
      `image_clearing` root-cause work above) were touched upstream in that
      range, so the local patch reapplied cleanly and the clear bug is
      still unaddressed upstream as of this commit. `src/panfrost/vulkan/`
      did pick up unrelated churn (a new `panvk_nir_lower_cooperative_
      matrix.c`, extension-support changes in `panvk_physical_device.c`).
      Also deleted a stray `pan_kmod.c.orig` sitting untracked in the clone
      — confirmed byte-identical to the pre-kbase-patch upstream file, a
      leftover backup from whenever the dispatch patch was first applied,
      not needed now that the patch and the pristine file are both
      accounted for.
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
- [x] Give PanVK a non-DRM `vk_sync` implementation. **DONE — physical
      device creation now succeeds on kbase.** `src/mesa/panvk_kbase_sync.c`
      implements a `vk_sync_type` over a page of `BASE_MEM_CSF_EVENT`
      memory: 256 slots of 16 bytes (value word + error word), one per
      sync, allocated through the new
      `pan_kmod_kbase_alloc_event_mem()`. The type itself owns the pool, so
      a `vk_sync` reaches it by `container_of()` on its own `->type` and
      nothing has to be added to `panvk_device`. Wired in by
      `src/mesa/patch-panvk-kbase-sync.py`, which routes kbase devices past
      `vk_drm_syncobj_get_type()` and marks them with a new `is_kbase` flag.
      Verified on the Poco X8 Pro via `tests/driver_enum_probe`:
      `vkEnumeratePhysicalDevices -> 0, count=1`, `Mali-G720 MC8`,
      apiVersion 1.4.354. The `-3 VK_ERROR_INITIALIZATION_FAILED` recorded
      above is gone.
      **Scope, honestly:** CPU signal/reset/wait/get_value are implemented;
      `VK_SYNC_FEATURE_GPU_WAIT` is deliberately *not* advertised, because
      nothing submits GPU work that would signal a slot yet. The CPU wait
      therefore polls rather than blocking on the kbase fd's notification —
      the notification mechanism is proven (see Phase 4) but has no
      producer until submission lands. Note a future blocking
      implementation needs a single owner of the fd's event stream, since
      `read()` consumes a notification.
      **Not yet exercised through the Vulkan API.** `tests/driver_sync_probe`
      drives a timeline semaphore and a binary fence through the real
      entrypoints, but cannot run yet because `vkCreateDevice` still fails —
      see the `vm_create` item below. The sync type is registered and its
      event page is allocated during physical device creation; the
      signal/wait paths have not run on hardware.
      **Bug found and fixed along the way:** `vkCreateDevice` calls
      `pan_kmod_dev_create(os_dupfd_cloexec(phys_dev->kmod.dev->fd))`
      (`panvk_vX_device.c:395`). A `dup()` shares the open file
      description, so `KBASE_IOCTL_VERSION_CHECK` returned `-EPERM` under
      the once-per-fd rule, `pan_kmod_fd_is_kbase()` concluded "not kbase",
      and the whole thing fell through to `drmGetVersion()`. Now `EPERM` is
      treated as a *positive* identification ("already greeted"), reporting
      version 0.0, which `kbase_kmod_dev_create()` recognises as
      "already set up" and so skips the equally once-per-context
      `SET_FLAGS`. Confirmed by logcat: device creation now gets past the
      dup and reaches `vm_create`.
      Original description of the blocker follows.
      `get_device_sync_types()` calls
      `vk_drm_syncobj_get_type(dev->fd)` unconditionally, so physical
      device creation cannot succeed on a misc device no matter what the
      backend does. Needs a `vk_sync` type backed by whatever kbase
      offers — which loops back to the unsolved completion-mechanism
      question in `docs/kbase-notes.md`.
      **Mechanism found in Panfork and now demonstrated on-device.** There
      is no kbase fence object to translate — you build one over
      GPU-visible event memory allocated with `BASE_MEM_CSF_EVENT` (a flag
      this repo had never set), signalled by the command stream itself.
      `tests/event_slot_probe` does the whole loop, 8/8 reproducible, no
      root: alloc + seed a slot, encode `MOVE64`/`MOVE64`/`SYNC_SET64`
      (system scope) with Mesa's `cs_builder.h`, `KICK`, and observe
      **both** the slot going `1 -> 2` and a real
      `base_csf_notification` type 0 (`BASE_CSF_NOTIFICATION_EVENT`)
      arriving on `poll()`. 2-4ms end to end. So `vk_sync` can block
      rather than spin. See "Finding 2" in `docs/kbase-notes.md` for the
      encoding details and the flags kbase adds
      (`CACHED_CPU`/`COHERENT_SYSTEM`).
      What remains for this checkbox is writing the actual `vk_sync` type
      against that mechanism and wiring it into
      `get_device_sync_types()` in place of
      `vk_drm_syncobj_get_type(dev->fd)` — the mechanism question is
      closed, the Mesa-side implementation is not.
- [x] **VM ops implemented as a single implicit VM — and that model is now
      measured to be insufficient.** `vm_create`/`vm_destroy`/`vm_bind` are
      no longer stubs: `vm_create` returns a bookkeeping object with
      handle 0 (what `pan_kmod.h` documents for KMDs with one VM per
      context), `vm_bind` handles `MAP`/`UNMAP`/`SYNC_ONLY`, and
      `bo_get_mmap_offset` works too.
      `bo_get_mmap_offset` was settled empirically by
      `tests/remap_probe`: re-using the `MEM_ALLOC` cookie fails `EINVAL`
      (kbase consumes it on first mmap), but the **resolved SAME_VA
      address works as an mmap offset** and aliases the same pages —
      confirmed by writing a sentinel through the first mapping and
      reading it back through the second. So the backend returns that.
      **The finding that matters: option (a) does not work.** The theory
      was that a no-op `vm_bind` is harmless until something submits GPU
      work. It is not. PanVK dereferences addresses from its own
      `util_vma_heap` during `vkCreateDevice`'s mempool setup, so
      returning success turned a clean error into a **segfault inside
      device creation**. The divergence is not marginal — logcat showed
      `requested 0xfffff000, BO is at 0x7ca2928000`.
      `vm_bind` therefore now **fails** on a caller-chosen VA it cannot
      honour, so `vkCreateDevice` returns an error instead of crashing.
      That is the honest state.
- [x] **Real VA management — done in the backend.** `bo_alloc` no longer
      uses `BASE_MEM_SAME_VA`. It allocates a GPU VA from a device-level
      `util_vma_heap` over the FIXED_VA zone, then `MEM_ALLOC_EX` +
      `BASE_MEM_FIXED` places the allocation there; `bo_free` does a real
      `MEM_FREE` and returns the VA to the heap; `bo_get_mmap_offset`
      returns the GPU VA, so `pan_kmod_bo_mmap()` produces a CPU mapping
      at an unrelated address. The zone is located at device-init by a
      one-page `BASE_MEM_FIXED` probe (never `FIXABLE`, which would poison
      the context — see `docs/kbase-notes.md`), overridable with
      `PANVK_KBASE_FIXED_VA_BASE`.
      **The key move is in `vm_create`: it forces
      `PAN_KMOD_VM_FLAG_AUTO_VA` on.** PanVK's `util_vma_heap` allocates
      from a range kbase rejects (it asked for `0xfffff000`; the zone is at
      `0x800200000000`). Rather than rewrite PanVK's VA setup, use the
      inversion `pan_kmod` already supports: with AUTO_VA,
      `panvk_priv_bo.c:62` leaves `op.va.start = PAN_KMOD_VM_MAP_AUTO_VA`
      and adopts whatever `vm_bind` writes back. So the backend picks from
      the zone the kernel accepts and PanVK follows. Same path panfrost
      (arch < 10) already uses, so it is a supported configuration.
      Confirmed working: BOs are allocated at real fixed addresses and
      `vm_bind` reports them with no mismatch warnings, and
      `driver_enum_probe` is unaffected (`count=1`, Mali-G720 MC8).
      Standalone probes were deliberately left on SAME_VA — they are the
      hardware evidence for Phases 3-4 and all still pass.
- [x] **The `generate_tiler_oom_handler` segfault — found and fixed. It
      was not a memory bug.** `panthor_kmod_get_csif_props()` does
      `container_of(dev, struct panthor_kmod_dev, base)` with no check of
      which backend the device belongs to, so on a kbase device it read
      whatever memory followed `struct kbase_kmod_dev`. The garbage landed
      in `cs_builder_conf.nr_registers` via `csif_info->cs_reg_count`, and
      `cs_builder` then wrote off the end of its buffer.
      The short-mapping theory (fault address ~32KB above a mapping) was
      wrong and is disproved by `tests/fixed_va_probe` section 7, which
      allocates 2-, 8- and 8-page fixed regions, maps them and writes
      every page successfully.
      PanVK reads that accessor in six places, so the fix is one site:
      `src/mesa/patch-panthor-csif-dispatch.py` makes
      `panthor_kmod_get_csif_props()` dispatch to
      `pan_kmod_kbase_get_csif_props()` when `dev->ops == &kbase_kmod_ops`,
      and all six callers get correct values unchanged. The backend fills
      a real `drm_panthor_csif_info` from
      `KBASE_IOCTL_CS_GET_GLB_IFACE` — confirmed on-device as
      `CSF iface v3.6.0, 8 CSG slots x 8 streams`, matching what
      `tests/glb_iface_probe` reported independently. Register counts are
      not exposed by that ioctl, so they use the architectural values
      (96 registers, 4 kernel-reserved) that `tests/cs_encode_probe` and
      `tests/live_kick_probe` already validated by executing real command
      streams on this GPU.
- [x] **`VK_ERROR_NOT_PERMITTED_KHR` from `vkCreateDevice` — fixed.** With
      the segfault gone, device creation failed `-1000174001` because
      `props.allowed_group_priorities_mask` was never set by the kbase
      backend. A zero mask rejects every priority including the default
      MEDIUM (`panvk_vX_device.c:239`), so no device could ever be
      created. Now filled from `KBASE_IOCTL_CONTEXT_PRIORITY_CHECK`, which
      clamps a requested priority to what the context may use — a priority
      is allowed iff it survives the round-trip. The vendor blob calls the
      same ioctl for the same reason (see the `libGLES_mali.so` survey in
      `docs/kbase-notes.md`).
- [x] **`vkCreateDevice` SUCCEEDS on kbase.** A sibling kbase GPU queue
      (`src/mesa/panvk_vX_kbase_queue.c`, approach (a) below) implements
      `create_kbase_queue`/`destroy_kbase_queue`/`kbase_queue_submit`/
      `kbase_queue_check_status`; `patch-panvk-kbase-queue.py` declares
      them and makes `panvk_vX_device.c` pick between the panthor and
      kbase versions on `is_kbase`, guarded `#if PAN_ARCH >= 10` since
      arch < 10 is JM and has no CSF queue at all.
      Creation does tiler heap → queue group → one bound CS ring buffer
      per subqueue, all via the ioctl sequences
      `tests/queue_group`/`tests/live_kick_probe` proved on hardware.
      **Precondition found on the way:** `CS_TILER_HEAP_INIT` fails
      `ENOMEM` unless `MEM_JIT_INIT` ran first — a tiler heap's chunks are
      JIT-backed. The backend now does `MEM_JIT_INIT`/`MEM_EXEC_INIT` at
      device-create (skipped for the dup'd fd, since both are one-shot per
      context). `live_kick_probe` only ever got a working heap because it
      replicated the vendor blob's setup; the Mesa path had not.
      **This finally validates the vk_sync through the real Vulkan API.**
      `tests/driver_sync_probe`: `vkCreateFence(SIGNALED)` reads signalled,
      `vkResetFences` then reads unsignalled, `vkDestroyFence` clean — all
      backed by 64-bit slots in `BASE_MEM_CSF_EVENT` memory.
      **Semaphores correctly refuse to be created** (`-1000072003`):
      `vk_semaphore.c:99` requires `VK_SYNC_FEATURE_GPU_WAIT`, which
      `panvk_kbase_sync` deliberately does not advertise while nothing can
      signal a slot from the GPU. That is the intended behaviour —
      advertising it would hand out semaphores that could never complete.
      *(Since fixed: the GPU does signal slots now, the feature is
      advertised, and semaphores work — see the Phase 4 fence-translation
      item. This line describes the state at the time, not a live
      limitation.)*
      `vkDeviceWaitIdle` returns `-8` from the submit stub, as designed.
- [x] **The kick/observe primitives are in the backend.** They existed only
      inside `tests/live_kick_probe` and `tests/event_slot_probe`, so
      nothing the driver links could ring a queue at all.
      `pan_kmod_kbase.{c,h}` now has `pan_kmod_kbase_queue_kick()`
      (publishes `CS_INSERT`, barriers, `CS_QUEUE_KICK`),
      `_queue_extract()` / `_queue_active()` (loads out of the user-IO
      output page, no ioctl), and `pan_kmod_kbase_read_event()`
      (`poll()` + `read()` of a `base_csf_notification`, decoded so callers
      need no kbase headers — this is the only way a GPU-side fault is
      reported at all, since a dead group just stops advancing
      `CS_EXTRACT`).
      Deliberately thin: ring management — where to write, when it wraps,
      how many submissions may be in flight — stays with the caller, which
      is where the command stream is built. `insert` is a monotonic byte
      *count*, not a ring offset; hardware takes it modulo the ring size.
      `csf_user_regs.h` is now copied into the Mesa tree next to the
      backend by `mesa-backend-sync` rather than duplicated, so the
      measured `[doorbell][input][output]` page order has one definition.
      Verified: compiles `-Wall`-clean against both vendored header sets
      (r44p0 and r49p1), links into `libpankmod_lib.a` through Mesa's real
      meson build, and cross-builds into the aarch64 Android driver. The
      four symbols are absent from the linked `.so` because nothing calls
      them yet and visibility is hidden — expected until the item below.
      **`ONE OWNER ONLY` on `read_event()`**: `read()` consumes a
      notification, so two threads polling the fd steal each other's
      wakeups. Anything built on it needs a single reader that dispatches.
      Also fixed on the way: `make mesa-backend-check` had been failing in
      `util/u_endian.h` before reaching backend code at all — it stands in
      for meson's config header, and was missing `-DHAVE_ENDIAN_H`.
- [x] **The GPU signals a `vk_sync` through `vkQueueSubmit`.** An empty
      submit — no command buffers — builds a stream whose entire content is
      one `SYNC_SET64` per signalled sync at system scope, publishes it to
      the compute subqueue's ring and kicks. `tests/driver_sync_probe` now
      creates a fence, passes it to `vkQueueSubmit`, and gets it back
      signalled: **5/5 runs, all checks passed**, three submits per run.
      Nothing on the CPU side of that path writes the slot, so a signalled
      fence was signalled by the GPU. `vkDeviceWaitIdle` returns 0.
      No regressions: `driver_enum_probe`, `first_test`, `queue_group`,
      `live_kick_probe`, `event_slot_probe` and `fixed_va_probe` all still
      pass.
      **A kick only lands on an idle CS — measured, and it cost a debugging
      cycle.** Kicking three times in a row while logging state:
      `insert=24 extract=0 CS_ACTIVE=0` ran; `insert=48 extract=24
      CS_ACTIVE=1` **did not run**; `insert=72 extract=48 CS_ACTIVE=0` ran.
      The failing kick is exactly the one issued while `CS_ACTIVE` was
      still 1, which lingers ~30-40ms after a stream ends. The bytes are
      not lost — they sit in the ring until a later kick flushes them, so
      the symptom is a submit that never completes rather than one that
      errors, and a 200ms delay before every kick made 4/4 run within 10ms.
      `pan_kmod_kbase_queue_wait_idle()` is the fix and the submit path
      calls it before every kick.
      **Ruled out, do not retry:** ringing the user-IO doorbell page (page
      0, offset 0, value 1) as Panfork's `kbase_cs_submit()` would if its
      doorbell branch were not hardcoded off. Tried unconditionally; the
      failing kick still did not run. That page also reads back whatever
      was last written to it, so on this device it is ordinary memory, not
      an MMIO doorbell — which is presumably why Panfork disabled it.
      **SUPERSEDED 2026-08-02, and the "revisit if kernel-side visibility
      becomes available" note was looking in the wrong place.** No kernel
      visibility was needed. The observation above is correct — a kick
      issued while `CS_ACTIVE` is set does not run — but the fix it led to
      (wait for idle, then kick) was answering the wrong question. When
      `CS_ACTIVE` is set the right move is *not to kick at all*: the
      still-resident CS picks up the published `CS_INSERT` by itself, the
      same way the `extract < old_insert` case already relied on. So the
      wait was paying 30-40ms for a state transition that had nothing to do
      with whether the work would run, and submissions were being
      serialised for no reason.
      `PANVK_KBASE_KICK_MODE=defer` is the default now; `auto` restores
      this behaviour. 41.5 -> 64.4 fps in a present loop, 30/30 regression.
      See "Where this actually is" below and `docs/kbase-notes.md`.
- [x] **The compute subqueue's GPU context is initialised.** Queue creation
      now calls `panvk_per_arch(init_gpu_queue)` — panthor's own
      `init_subqueue()` path — which allocates the shared syncobj array and
      a `panvk_cs_subqueue_context`, then builds and submits an init stream
      that loads the context register and initialises the scoreboard slots.
      **Shared, not duplicated.** Unlike the queue lifecycle, which
      diverges at every ioctl and got its own file, this code converges
      with panthor almost entirely — it is pool allocation and CS building,
      none of it driver-specific. It diverges at exactly three points, and
      `patch-panvk-kbase-subqueue-init.py` patches only those: where the
      init stream is built (panthor carves it out of the tiler heap's
      geometry buffer; kbase has no heap descriptor, so it uses a dedicated
      allocation), how it is submitted and waited on, and `init_queue()`'s
      panthor-only steps (render descriptor ringbuf and utrace, both of
      which need a DRM syncobj). That avoids ~260 lines of duplicated
      upstream logic that would immediately start drifting.
      Confirmed on hardware: a 48-byte init stream at `0x8003fffbd000` is
      published to subqueue 2 and consumed by the GPU during
      `vkCreateDevice`. `kbase_submit_and_wait()` blocks on `CS_EXTRACT`
      reaching the end of the stream and fails device creation if it does
      not within 2s — so `vkCreateDevice` returning 0 *is* the proof the
      stream ran. `driver_sync_probe` still passes all checks.
- [x] **`init_tiler()` is shared too.** The tiler heap descriptor, its
      geometry buffer and the scratch FBD the tiler-OOM handler writes into
      are all allocated by panthor's own `init_tiler()`, now exported as
      `panvk_per_arch(init_gpu_tiler)`. Only the heap-create ioctl differs,
      through `panvk_per_arch(kbase_create_tiler_heap)` —
      `CS_TILER_HEAP_INIT` needs no VM id and hands back both addresses
      directly, where panthor's returns a handle as well, so
      `context.handle` is unused on kbase and teardown goes by address.
      **This also fixed a latent mismatch**: the kbase path used to create
      its heap with local constants while the shared code writes the
      `TILER_HEAP` descriptor from `phys_dev->csf.tiler`, so the descriptor
      could have described a heap with a different chunk size than the one
      kbase actually allocated. Geometry now comes from one place.
      The `kbase_init_cs` scratch allocation added in the previous pass is
      gone: with a real tiler heap descriptor the init stream is built in
      the geometry buffer, exactly like panthor, removing a divergence
      point rather than adding one.
      Verified on hardware: `vkCreateDevice` still returns 0 and
      `driver_sync_probe` still passes every check.
- [x] **The render subqueues' *contexts* — done, and the ringbuf turned out
      not to gate them.** `init_gpu_queue()` now initialises all three
      subqueues on kbase. The belief that it could not was wrong, and
      disassembling a rejected stream with `PANVK_KBASE_DUMP=1` is what
      showed it: what VERTEX_TILER and FRAGMENT actually dereference for an
      ordinary command buffer is the subqueue context register, for
      `syncobjs` and `last_error`, in the epilogue `vkEndCommandBuffer`
      appends to *every* subqueue whether the application touched it or not.
      Leaving that register at 0 is what made those streams unrunnable — a
      GPU read of address 0. Only the `render.desc_ringbuf` field of the
      context needs the ringbuf; it is left zeroed on purpose, so anything
      that actually draws null-derefs it rather than running against a
      plausible wrong address. **Rendering is still blocked on the ringbuf;
      compute never was.** Original note follows.
- [ ] **(superseded, kept for the reasoning) The render subqueues, blocked on
      BO aliasing.** `init_gpu_queue()` used to loop over
      `PANVK_SUBQUEUE_COMPUTE` only. What stops the other two is
      `init_render_desc_ringbuf()`, and the reason is more interesting than
      it first looked: its syncobj is a `panvk_cs_sync32` in device memory,
      not a DRM syncobj, so that part is fine — but it maps one BO at *two*
      adjacent GPU VAs so a read running off the end of the ring wraps into
      the copy. `kbase_kmod_vm_bind()` cannot do that at all: an allocation
      lives where `MEM_ALLOC_EX` put it, so both `MAP` ops fail the
      caller-chosen-VA check.
      **`tests/alias_probe` settles whether kbase can express it: yes.**
      `KBASE_IOCTL_MEM_ALIAS` (nr 21) aliases one 4-page allocation twice
      with `stride = 4` pages and reports `va_pages = 8`, the full 2x span.
      Panfork never calls it, so there was no prior art.
      **The non-obvious part**: the handle is the *real GPU VA*, not the
      `gpu_va` the allocation reported — under `SAME_VA` that value is an
      mmap cookie and the real address is the CPU pointer. The cookie fails
      `ENOMEM`, which reads like a resource limit but means "no such
      allocation at that address".
      **But the returned value is a cookie, not an address, and that is a
      real obstacle.** `out.flags` is `0x400d` =
      `NEED_MMAP | GPU_WR | GPU_RD | CPU_RD` — `SAME_VA` stripped by
      `kbase_mem_alias()`, `BASE_MEM_NEED_MMAP` set. The region has no GPU
      mapping until userspace `mmap()`s it, and `kbase_context_mmap()` then
      refuses `nr_pages > stride` (`EINVAL`). **So a single mapping can
      never span both windows** — which is exactly what the ringbuf needs.
      `MEM_ALIAS` composes the region and its entries do share
      `alloc->pages`, but the route to a usable 2x GPU VA range is not
      established.
      **This cost two device reboots.** `tests/alias_cs_probe` pointed a
      command stream at `out.gpu_va + stride` on the assumption it was an
      address; writing to unmapped GPU memory faulted and wedged the kbase
      context past `kill -9`, needing a reboot each time. The GPU survives
      (`live_kick_probe` still passes 3/3 afterwards). That probe now
      refuses to run without `--i-know-it-hangs`, and `alias_probe` reports
      `out.flags` so the cookie case is visible without touching the GPU.
      **Both remaining variants were tried, and the answer is negative.**
      `stride` = full span returns `va_pages = 16`, still `NEED_MMAP`. A
      `BASE_MEM_FIXABLE` source — which does itself land at a real address,
      `0x800200000000` in the FIXED_VA zone — aliases fine, but the alias
      is still `NEED_MMAP`. The conclusion follows from the kernel's own
      arithmetic rather than any single error: `nr_pages == va_pages`
      requires `va_pages > stride`, rejected `EINVAL`; `nr_pages <= stride`
      covers one window. Since the GPU address is assigned at mmap time,
      **the GPU can only ever address one window**, so the wraparound is
      not expressible via `MEM_ALIAS` here. The aliasing itself is real —
      entries share `alloc->pages` and the region is composed as asked — it
      is the addressability that fails.
- [ ] **Next: bounds-check the ring instead of relying on the mapping.**
      With aliasing ruled out, `init_render_desc_ringbuf()` and whatever
      consumes `render.desc_ringbuf` have to stop assuming a read running
      off the end wraps into a second mapping, and handle the wrap
      explicitly in the command stream. That is a change to **shared PanVK
      code**, not to the kbase backend — a different kind of change from
      everything in Phase 4 so far, and the first that would alter
      behaviour for panthor too unless it is conditioned on `is_kbase`.
      Worth thinking about before starting, and worth raising upstream
      (Phase 9) since it touches code panthor relies on.
      Until it lands, `init_gpu_queue()` stays compute-only and
      `kbase_queue_submit()` keeps refusing submits carrying command
      buffers.
      Still not done either: **GPU-side waits**. `vk_submit->waits` are
      satisfied on the CPU before anything is published, so
      `VK_SYNC_FEATURE_GPU_WAIT` stays unadvertised and semaphores still
      cannot be created. Signalling from the GPU works; waiting on the GPU
      needs `SYNC_WAIT64` in the stream. (The earlier note here said
      submission working would let semaphores work — that was imprecise:
      signal and wait are separate features and only signal is done.)
      *(Superseded: semaphores work now. Advertising the feature turned out
      not to require `SYNC_WAIT64` at all — a CPU block satisfies its
      contract — and `SYNC_WAIT64` is currently unsafe against the
      kick-on-idle serialisation. See the fence-translation item.)*
      The CPU wait in `panvk_kbase_sync.c` should also switch from polling
      to blocking on the kbase fd's notification via
      `pan_kmod_kbase_read_event()` (noting `read()` consumes one, so it
      needs a single owner of the event stream).
- [x] ~~`vkCreateDevice` fails `-3` at the GPU queue~~ — resolved above.
      Kept for the scope analysis, which is still accurate:
      `panvk_vX_gpu_queue.c:685` issues `DRM_IOCTL_PANTHOR_GROUP_CREATE`
      on the kbase fd. Not a new bug: the whole GPU queue is
      panthor-specific.
      **Scope, measured rather than guessed.** It is not enough to swap
      the group-create ioctl. `create_gpu_queue()` (`:1401`) does, in
      order: `drmSyncobjCreate`, `init_tiler` →
      `DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE`, `create_group` →
      `_GROUP_CREATE`, then `init_queue` → `init_subqueue` (`:363`) **per
      subqueue**, and each of those *builds a real init command stream,
      submits it with `DRM_IOCTL_PANTHOR_GROUP_SUBMIT`, and blocks on
      `drmSyncobjWait` for it to complete* (`:556-568`). So making
      `vkCreateDevice` succeed on kbase requires a working
      submit-and-wait path, not just object creation — the whole of Phase
      4, in a 1511-line file with ~13 panthor/libdrm call sites.
      **Decided and built — this entry was stale.** Re-checked
      2026-08-01: the three integration options below were weighed while
      `pan_kmod_kbase_group_create`/`_destroy` and `_tiler_heap_create`/
      `_destroy` sat uncalled, which was true when this was written and
      has not been true for a while. Option (a) was chosen —
      `src/mesa/panvk_vX_kbase_queue.c`, 1294 lines, 7
      `panvk_per_arch(...)` entry points including
      `create_kbase_queue`/`kbase_create_tiler_heap` — and
      `patch-panvk-kbase-subqueue-init.py` dispatches to it from
      `panvk_vX_gpu_queue.c` on `is_kbase` at every site that mattered
      (group create, tiler init, per-subqueue init, teardown). Kept below
      for the reasoning, since (b) and (c) are still the right shape of
      argument for *future* integration points — the render ringbuf fix
      and the dma-buf import hook are exactly the "needs Panfrost
      agreement" case (c) describes.
- [ ] ~~Real VA management is required~~ — superseded by the two items
      above. Kept for the reasoning: kbase supports it: the vendored headers
      define `BASE_MEM_FIXED` (`csf/mali_base_csf_kernel.h:34`) and
      `BASE_MEM_FIXABLE` (`:58`), which is the mechanism for allocating at
      a caller-chosen GPU VA instead of letting the kernel pick via
      `BASE_MEM_SAME_VA`.
      Shape of the work: `bo_alloc` stops using SAME_VA and defers
      placement; `vm_bind` does the real mapping at `op->va.start` using
      `BASE_MEM_FIXED`; everything that currently treats the CPU pointer
      as the GPU VA has to stop (this backend, and the assumption is baked
      into `tests/` too — see `docs/kbase-notes.md`'s SAME_VA section).
      **Probed first, and it works** — `tests/fixed_va_probe`, 3/3
      reproducible. `MEM_ALLOC_EX` + `BASE_MEM_FIXED` honours a requested
      GPU VA exactly, two adjacent allocations land where asked, and the
      allocation is CPU-mappable at an unrelated CPU address (which is the
      point: GPU VA becomes ours to choose). Full writeup in
      `docs/kbase-notes.md`. Three constraints it turned up, all of which
      the implementation has to respect:
      (1) there is a **FIXED_VA zone at `0x800200000000`** on this device
      and requests outside it fail `ENOMEM` — including `0xfffff000`,
      which is exactly what PanVK's `util_vma_heap` asked for, so PanVK's
      VA range must be constrained to the zone; the zone's size is still
      unmeasured;
      (2) `BASE_MEM_FIXED` and `BASE_MEM_FIXABLE` are **mutually exclusive
      per context** — one `FIXABLE` allocation makes every later `FIXED`
      request fail `EINVAL`, so the backend must commit to one mode;
      (3) `ENOMEM` means "address unavailable" (outside the zone, or
      already allocated) while `EINVAL` means "wrong mode" — do not read
      `EINVAL` as unsupported.
- [x] / [ ] The three ops left stubbed when this phase was written have
      each been resolved since — this entry was stale, duplicating (and
      predating) Phase 3's accurate version below. `vm_create` /
      `vm_destroy` / `vm_bind` are **done** — see `src/mesa/README.md` for
      the `AUTO_VA` design. `bo_import` / `bo_export` are **scoped, not
      done** — import needs a shared-code change, export is not possible on
      kbase at all — see Phase 3 and `docs/upstream-import-question.md`.
      `bo_wait` remains genuinely blocked on the unsolved fence mechanism.

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
- [x] **dma-buf import — DONE, verified end to end on hardware**
      (2026-08-02). The reasoning below was right about the obstacle and is
      kept as history; what changed is that the shared-code change was
      written rather than only scoped.
      `src/mesa/patch-pan-kmod-import-fd.py` adds an optional
      `ops->bo_import_fd(dev, fd, size)` to `pan_kmod`, dispatched *before*
      `drmPrimeFDToHandle()`. Backends that do not set it (panthor,
      panfrost) are untouched. `kbase_kmod_bo_import_fd()` then runs
      `KBASE_IOCTL_MEM_IMPORT` with `BASE_MEM_IMPORT_TYPE_UMM`.
      Measured properties the implementation had to respect, none of them
      guessable: `in.phandle` is a pointer *to* the fd; `out.gpu_va` is a
      `NEED_MMAP` cookie that `mmap()` resolves; the cookie is single-use so
      the mapping must be kept and `bo_get_mmap_offset()` must refuse;
      `munmap()` is the free and `MEM_FREE` returns `EINVAL`; and the
      imported VA lands outside the `util_vma_heap` range (checked at
      runtime, not assumed).
      **Proven, not merely wired:** `tests/driver_dmabuf_probe` has the GPU
      write into imported memory and reads it back through an *independent*
      mmap of the dma-buf — a `vkMapMemory` readback would be satisfied by a
      driver that imported nothing. 50/50 rounds clean on a dma-heap buffer,
      and **a real AHardwareBuffer works too**, which is the actual WSI case.
      On the cache-sync trap that cost a debugging cycle, and the three
      presentation blockers still ahead of this, see `docs/kbase-notes.md`.
- [ ] **(superseded, kept for the reasoning) dma-buf import — needs a
      shared-code change, like the ringbuf.**
      Scoped 2026-08-01; it is not backend-local work, which is what it
      looked like. kbase can import: `KBASE_IOCTL_MEM_IMPORT` with
      `BASE_MEM_IMPORT_TYPE_UMM` takes a dma-buf fd, and Panfork's
      `kbase_import_dmabuf()` shows the whole sequence working on real
      hardware. The obstacle is above this backend — `pan_kmod_bo_import()`
      (`lib/kmod/pan_kmod.c:183`) calls `drmPrimeFDToHandle(dev->fd, ...)`
      to turn the fd into a GEM handle *before* dispatching to
      `ops->bo_import`, and on a misc device that fails, so
      `kbase_kmod_bo_import()` is never reached. Filling in the stub
      therefore accomplishes nothing on its own. What it needs is an
      fd-taking entry point that dispatches to the backend before any DRM
      call — a second upstream question, and a natural companion to the
      ringbuf one.
- [ ] ~~dma-buf export~~ — **not expressible on kbase.** Two independent
      reasons. `pan_kmod_bo_export()` is a `static inline` in
      `pan_kmod.h:703` that calls `drmPrimeHandleToFD()` itself; the
      backend's `bo_export` is only an optional post-export hook, so there
      is nothing to override. More fundamentally, kbase has no export
      mechanism at all — no PRIME, no dmabuf-out ioctl anywhere in the
      r49p1 UAPI. Unlike the ringbuf, there is no workaround to negotiate,
      because there is no primitive to build one from. Anything needing to
      hand a PanVK allocation to another process or device is out of scope
      on this driver.
- [x] **Advertise import, keep export false — done** (2026-08-02).
      `patch-panvk-kbase-external-memory.py` now splits the single predicate
      into `panvk_supports_external_import(phys_dev, handle_type)` and
      `panvk_supports_dma_buf_export(phys_dev)`, applied at three call sites
      rather than two — the third being `exportFromImportedHandleTypes`,
      which claimed "you can re-export what you imported" and is false here.
      Import is advertised for `DMA_BUF` only, not `OPAQUE_FD`: the only way
      to obtain a PanVK opaque fd is to export one, so advertising it would
      be a promise with no producer.
      `tests/driver_extmem_probe` was updated in the same change and is the
      reason this is trustworthy — it had been asserting the *old* contract
      (no feature bits at all) and so failed on success the moment import
      started working. A probe pinned to a stale contract is worse than no
      probe; its header now says so.
- [ ] **(superseded) Consequence, and the actionable part: stop advertising
      both.**
      `panvk_physical_device.c:1427` unconditionally reports
      `OPAQUE_FD | DMA_BUF` with `EXPORTABLE | IMPORTABLE`. On kbase that
      is untrue in both directions today, so an application that believes
      it gets `VK_ERROR_OUT_OF_DEVICE_MEMORY` out of `vkGetMemoryFdKHR()`
      rather than a clean "unsupported" at query time. Gating that on
      `is_kbase` is backend-local, needs nothing from upstream, and is
      correctness rather than a feature. Worth doing before WSI, since
      Phase 6 is where something will actually ask.
- [x] **Tiler heap / JIT growable memory — implemented, not yet
      exercised.** This entry was stale; re-checked 2026-08-01. The
      expectation that PanVK's growth logic would need adapting turned out
      to be wrong, and only heap *creation* differed.
      `pan_kmod_kbase_tiler_heap_create()` / `_destroy()` wrap
      `CS_TILER_HEAP_INIT` / `_TERM`, `MEM_JIT_INIT` is done at device
      setup (without it `CS_TILER_HEAP_INIT` fails `ENOMEM`, since heap
      chunks are JIT allocations), and `patch-panvk-kbase-subqueue-init.py`
      routes panthor's create/destroy through them — setting
      `context.dev_addr` for `cs_heap_set()` and `first_chunk_va` into the
      `TILER_HEAP` descriptor, with teardown by address since kbase has no
      heap handle.
      **Growth needs nothing kbase-specific.** It is not a kernel callback
      on either driver: PanVK installs a command-stream exception handler
      (`cs_set_exception_handler(MALI_CS_EXCEPTION_TYPE_TILER_OOM)`,
      `panvk_vX_cmd_draw.c:3916`) and the firmware runs it against the heap
      context. Both drivers supply that context the same way; only its
      creation differed, and that is done.
      Untested end to end, because the OOM handler is installed during a
      render pass and rendering is blocked on the ringbuf. So this is
      "implemented and wired", not "known to work".

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
      — `BASEP_QUEUE_NR_MMAP_USER_PAGES`, ordered
      HW-doorbell/input/output, measured — see `utils/csf_user_regs.h`)
      and confirmed mapping successfully
      on-device. Full teardown (`CS_QUEUE_TERMINATE` →
      `CS_QUEUE_GROUP_TERMINATE` → `kbase_bo_free`) also confirmed clean,
      no failed ioctls. Still doesn't prove GPU execution completed —
      just that the whole submit/bind/kick/teardown lifecycle round-trips
      without kernel-side rejection.
- [x] Confirm the completion/fence signaling mechanism kbase exposes for
      a submitted queue.
      **ANSWERED — it is GPU-visible event memory, not a fence.** Allocate
      with `BASE_MEM_CSF_EVENT`, seed a slot, have the command stream
      write it with `SYNC_SET64` at system scope. Userspace either polls
      the slot or blocks on `poll()` for a `base_csf_notification`. Both
      confirmed working on-device by `tests/event_slot_probe`, 8/8 runs,
      no root, 2-4ms end to end. The rest of this item is the trail of
      dead ends that got there — kept because two of them are things not
      to retry. See "Finding 2" in `docs/kbase-notes.md` for the working
      version.
      Original note: r49p1 (MediaTek fork) adds
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
      to the CS size a few ms later, unwritten by userspace. Corroborated by
      page 0 persisting across processes while pages 1 and 2 come up
      freshly zeroed.
      After the two-line offset fix and nothing else,
      `live_kick_probe` reports **3 of 3 configs actually executed on the
      GPU** (nr 58, `_1_6`/nr 42, and compute-only), `CS_EXTRACT`
      advancing to 8 in each.
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
- [x] **Map VkQueueSubmit onto kbase command-stream submission — done for
      compute, confirmed on hardware.** One ring stream per subqueue, each
      CALLing that subqueue's command-buffer streams where they lie rather
      than copying them into the ring: a cs_builder stream that outgrew its
      first chunk links chunk to chunk by absolute address, so a relocated
      copy would jump back to the original. Panthor's kernel CALLs
      stream_addr/stream_size for the same reason; this does it in userspace
      because on kbase there is no kernel in the submit path at all.
      That absence is also why the cache flush panthor's kernel emits ahead
      of the CALL has to be in the stream here, and why it invalidates rather
      than only cleaning: command-buffer and descriptor memory is recycled
      through the command pool, so the GPU can hold valid lines for an
      address the CPU has since rewritten. Flush id 0 — always flush;
      panthor's `latest_flush` skip optimisation reads a register page kbase
      does not expose the same way.
      Proven by `tests/driver_compute_probe` and
      `tests/driver_pipeline_probe`: a command buffer runs, a
      `vkCmdFillBuffer` shader runs and its pattern reads back, a compute
      pipeline built from application SPIR-V dispatches and writes
      `i * multiplier` to a storage buffer through a descriptor set and a
      push constant, and 2000 back-to-back submits wrap the 64KB ring
      repeatedly without failure.
      **Not done: rendering.** A stream requesting the tiler, IDVS or
      fragment endpoints is refused — on the requested resources rather than
      on which subqueue it landed on, because the latter cannot tell real
      render work from the epilogue above.
      Original note follows.
- [ ] **(superseded) Map VkQueueSubmit onto kbase atom/command-stream
      submission.** Real
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
- [x] **Fence translation — done.** `panvk_kbase_sync` is a vk_sync type
      backed by a 64-bit slot in `BASE_MEM_CSF_EVENT` memory, signalled from
      the command stream by `SYNC_SET64` at system scope. No DRM syncobj is
      involved, which is the point: the original note below correctly
      identified that the mechanism PanVK assumes does not exist here.
      **Waits and semaphores — also done, and not the way this line
      predicted.** `VK_SYNC_FEATURE_GPU_WAIT` is advertised, so binary and
      timeline semaphores create and work; `vk_submit->waits` are honoured by
      blocking the submitting thread before anything is published. The note
      here used to say this needed `SYNC_WAIT64` in the stream. It does not,
      and `SYNC_WAIT64` is in fact currently *unsafe*: a stream parked in one
      holds `CS_ACTIVE`, which the kick-on-idle serialisation would then
      violate. Advertising the feature commits only to "a submission does not
      begin until its waits are satisfied", which a CPU block satisfies.
      Two things came with it, both in `docs/kbase-notes.md`:
      `VK_SYNC_FEATURE_WAIT_PENDING` is not optional alongside `GPU_WAIT`,
      and `vk_sync_type::move` must exist and must swap slots rather than
      copy their contents. Verified by `tests/driver_semaphore_probe`.
      Original note follows.
- [ ] **(superseded) Build the fence-translation shim between kbase's
      completion
      mechanism and whatever PanVK's sync code expects to wait/signal on.
      Confirmed harder than "translate the ioctls": the same file signals
      completion via libdrm `drmSyncobj*` calls on `dev->drm_fd`, which
      only work on an actual DRM fd — kbase's `/dev/mali0` is a misc
      device, so there's no DRM fd to hang a syncobj off of. This isn't a
      wrong-ioctl-number problem, it's "the mechanism PanVK's sync code
      assumes doesn't exist on this kernel driver at all."
- [ ] Budget the most time here. This was the long pole for kgsl too.

## Where this actually is (2026-08-02)

**Short version.** Compute and rendering work and are pixel-exact. dma-buf
import works, including a real AHardwareBuffer — the memory path a swapchain
image takes. Sync-fd semaphores work, so both halves of the Android
acquire/release handshake exist. The driver loads rootlessly through a
custom linker namespace. A present-shaped frame costs ~15ms (64 fps) at
720p with trivial GPU work.

**What has never run: an actual swapchain.** Every piece of the presentation
handshake is proven individually, but only from a shell binary. Driving
`vkAcquireImageANDROID`/`vkQueueSignalReleaseImageANDROID` against a real
`ANativeWindow` needs an APK, which is a different kind of work from
anything in `src/tests/` and is the single largest remaining unknown.

**Verification.** `make regress` runs 30 probes across three tiers
(`tools/run-probes.sh`); all pass. CTS: `api.smoke` 6/6, `api.command_buffers`
130/131 with one known failure, and sampled sweeps of `copy_and_blit` and
`image_clearing` — see `docs/kbase-notes.md`.

The rest of this section is the 2026-07-31 write-up, still accurate except
where noted inline.

Working end to end on the Poco X8 Pro (Mali-G720 MC8, kbase r49p1):
`vkCreateDevice` → command buffer → compute pipeline from application
SPIR-V → `vkCmdDispatch` → GPU-signalled fence → correct results read back.
Stable across 2000 back-to-back submits.

Semaphores work too, as of the same day. `vkCreateSemaphore` used to fail
outright — the runtime rejects every sync type without
`VK_SYNC_FEATURE_GPU_WAIT`, so no application that orders work could get past
device setup, and compute only survived without it because fences do not need
it. Binary and timeline semaphores now create, a binary semaphore chains two
submits, and a timeline reaches exactly the value a submit asked for
(`tests/driver_semaphore_probe`, 5/5).

The wait itself is still on the CPU: the submitting thread blocks until the
slot reaches its value, rather than a `SYNC_WAIT64` blocking the GPU. That
satisfies the feature's contract and is deliberately as far as it goes — see
`docs/kbase-notes.md`.

**Submission no longer kicks unconditionally**, and as of 2026-08-02 it no
longer waits either. A CS that is still executing re-reads `CS_INSERT` on
its own, so work appended to a busy queue runs with no kick at all; only a
queue that has caught up needs one.

The paragraph that used to sit here said a submit arriving at an idle GPU
costs ~12ms because the kick must wait for `CS_ACTIVE` to clear, and called
that "wake-up latency no submit-path change removes". **That was wrong.** A
submit-path change removed it. The wait was never necessary: when
`CS_ACTIVE` is set the right move is not to wait for it to clear and then
kick, but *not to kick at all* — the still-resident CS picks up the
published `CS_INSERT` by itself. So the CS stops needing a kick well before
it stops reporting active, and the wait was paying 30-40ms for an
irrelevant state transition. `PANVK_KBASE_KICK_MODE=defer` is now the
default; `auto` restores the old behaviour without a rebuild.

Measured by `tests/driver_present_loop_probe` (1280x720, 120 frames):
frame time 27.97ms -> 15.33ms median, of which the submit itself went
26.54ms -> 0.19ms; 41.5 -> 64.4 fps. On a different workload, 200
fence-waited compute submits went 4769ms -> 3063ms. All nine render probes
still pass pixel-exact, which is the check that matters — a lost stream
would report *better* numbers.

Still true from the original measurement: `CS_EXTRACT` is a completion
signal on this device rather than a progress one, so ring occupancy is only
knowable at stream granularity. And `PANVK_KBASE_KICK_MODE=nowait` — kicking
*while* active rather than deferring — still loses the stream, so the
original observation was correct; only the conclusion drawn from it was not.

**Compute works. Rendering works too, for command buffers that avoid
`VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT` — this line was wrong until
2026-08-01.** The render descriptor ringbuf was believed to block all
rendering; re-checking that against Mesa's actual source found the real
hazard was narrower — every touch of `render.desc_ringbuf` in
`panvk_vX_cmd_draw.c` is conditional on `simul_use` — and this repo's own
submit-time gate was refusing render work more conservatively than the
hardware needed. Loosened and verified on-device: `tests/render_clear_probe`
(a render pass, `LOAD_OP_CLEAR`/`STORE_OP_STORE`, deliberately no draw
calls) ran to completion twice, reproducibly, with a byte-correct readback
and the device fully healthy afterward. Full writeup, including why the old
conclusion was reasonable when it was written and what stayed untested:
`docs/kbase-notes.md`'s "Non-simul_use rendering works" section.

**What is still actually true:** `init_render_desc_ringbuf()` maps one BO at
two adjacent GPU VAs, and kbase genuinely cannot express that (see the
`MEM_ALIAS` section in `docs/kbase-notes.md`) — so `SIMULTANEOUS_USE`
rendering is still blocked, and the fix for that is still a change to
*shared* PanVK code, which is why `docs/upstream-ringbuf-question.md`
exists and is still worth its answer. And **no actual draw call has been
tested** — `render_clear_probe` deliberately recorded zero draws, to
isolate the render-pass-entry hazard just retired from IDVS/rasterization
correctness, which is genuinely new, untested territory of its own.

**That question is now sent** (2026-08-01), as a Mesa GitLab issue. It was
re-checked against the tree it describes before going out, which changed it
twice: `init_render_desc_ringbuf()` had moved to `csf/panvk_vX_gpu_queue.c`,
and the draft asked whether the double mapping had a reason when the code
answers that directly — the `size * 2` alignment keeps the window inside one
4 GB span so `move_ptr()` can wrap `ptr_lo` with 32-bit adds. It also turned
up the strongest argument for the ask: tracing mode already runs this with a
single mapping and `wrap_around = false`, so the request is to generalise an
existing mode rather than add a kbase special case. See
`docs/upstream-ringbuf-question.md` for both the sent text and that
re-check.

**The unblocked queue is now essentially empty, and that is the honest
status.** When the question was sent, the plan was to carry on with dma-buf
and the tiler heap while it sat. Scoping both on 2026-08-01 dissolved that
plan rather than advancing it:

- **dma-buf import** is not backend-local — `pan_kmod_bo_import()` calls
  `drmPrimeFDToHandle()` before dispatching, so the hook is unreachable.
  Needs a shared-code change, i.e. a second upstream question.
- **dma-buf export** is impossible — kbase has no export path in its UAPI
  at all, so there is nothing to build on.
- **The tiler heap was already done**, and its growth path needs nothing
  kbase-specific. The roadmap entry was simply stale.

What came out of that scoping is real but small: the driver no longer
advertises dma-buf sharing it cannot do
(`patch-panvk-kbase-external-memory.py`), which is a correctness fix, and
the `BELONGS-UPSTREAM` tags now mark every place this port stands in for
work that belongs elsewhere.

**This section previously said the project was genuinely gated on the
ringbuf answer. That is no longer true** — see the correction above the
Phase 4 status. What actually happened while looking for unblocked parallel
work: the search itself found that Phase 5 doesn't need the ringbuf answer
at all, only a scoped local fix. Three narrower things were also worth
doing, and all three are done:

- **Rebuilt and verified the external-memory gate** — compiles against the
  Android cross toolchain, links, the driver still enumerates, and
  `tests/driver_extmem_probe` confirms on-device that both handle types now
  report `externalMemoryFeatures = 0x0`.
  The whole working feature set was then re-run against that same build, so
  the gate is known not to have cost anything: `driver_enum_probe`
  enumerates, `driver_compute_probe --fill` 0 failures,
  `driver_pipeline_probe` 0 failures (all 1024 elements correct),
  `driver_semaphore_probe` 0 failures (binary chain and a timeline the GPU
  drove to 42). That is the first full regression pass recorded against
  Mesa 26.3.0-devel rather than against whatever the previous build was, so
  it doubles as the baseline for the next rebase.
- **`vm_create`/`vm_bind`** — no decision was outstanding. It had already
  been made and implemented (force `PAN_KMOD_VM_FLAG_AUTO_VA`, backend picks
  the address from the zone kbase accepts, PanVK adopts it); only the docs
  still called it open. `src/mesa/README.md` now records the reasoning.
- **The follow-up upstream question is drafted** —
  `docs/upstream-import-question.md`, on giving `pan_kmod_ops` an fd-taking
  import hook tried before `drmPrimeFDToHandle()`. Small enough to go in the
  same conversation as the ringbuf one.

A pattern worth noting for whoever reads this next: three separate items
here turned out to be already done or already decided, and only the docs
were stale. Before starting anything this file lists as open, check the code
first.

Tools worth knowing about before touching any of this:

- `PANVK_KBASE_DUMP=1` prints every stream, with opcode names, before it is
  kicked — and every stream this driver refuses. It is the only pre-flight
  disassembly available here (`PANVK_DEBUG=trace` cannot run on kbase; see
  `docs/kbase-notes.md`), and it has already overturned one wrong belief.
- Mesa logs go to logcat under the `MESA` tag, not to a probe's stdout:
  `adb logcat -d -s MESA`.
- The `patch-panvk-kbase-*.py` scripts are run **by hand**, not by
  `wsl-build*.sh`, and self-skip when already applied. Changing one means
  restoring its target files in `/opt/mesa-src` and re-running *all* of
  them, since they share targets.
- Release builds define `NDEBUG`, so every `assert()` in the Vulkan runtime
  is gone. Three separate crashes here have been a runtime precondition
  asserted upstream and then dereferenced anyway — `assert(shader)` before
  `precomp_cache_get()`, and `assert(type->move)` before calling it. **A
  `SIGSEGV` at `pc 0` means an optional-looking function pointer was not
  optional**; read the caller frame before anything else.
  `llvm-symbolizer --obj=<unstripped .so> --functions=linkage` on the raw
  offsets from `adb logcat -b crash` names it in one step, which the device
  cannot do for `/data/local/tmp` libraries itself.

## Phase 5 — Headless triangle
- [x] **Entering and leaving a render pass — done, 2026-08-01.**
      `tests/render_clear_probe`: a render pass with `LOAD_OP_CLEAR` /
      `STORE_OP_STORE`, no draw calls, no `SIMULTANEOUS_USE`. Ran clean
      twice on the Poco X8 Pro, byte-correct readback, device healthy
      after. This was believed blocked on the ringbuf; it was not — see
      `docs/kbase-notes.md`'s "Non-simul_use rendering works". Recorded
      here rather than only in "Where this actually is" because it is real
      Phase 5 progress, not a Phase 4 status update.
- [x] **A real triangle — done, 2026-08-01, same session.**
      `tests/render_triangle_probe`: vertex shader (no vertex buffers,
      positions indexed by `gl_VertexIndex`) through this driver's real
      compiler on a graphics stage for the first time, `vkCmdDraw`, IDVS,
      tiling, a fragment shader (hardcoded colour, no descriptor sets).
      16x16 target, partial-coverage triangle so the result can't pass by
      accident. Ran clean twice on the Poco X8 Pro: 190 clear-colour + 66
      triangle-colour + **0 other** pixels, device fully healthy after
      (compute and clear-only probes both re-ran clean). First triangle
      this project has ever rendered. Full writeup:
      `docs/kbase-notes.md`'s "A real triangle renders correctly on
      kbase".
      **Still not exercised:** descriptor sets, push constants, textures,
      depth/stencil, multiple draws per render pass — each its own
      first-time unknown, not implied by this result.
- [x] **Real vertex buffers — done, same session.**
      `tests/render_vbo_probe`: same triangle as above, one variable
      changed — positions fetched from a bound `VkBuffer` via
      `vkCmdBindVertexBuffers` instead of hardcoded in the shader. Result:
      190/66/0, an **exact match** with the hardcoded version, which the
      probe checks and prints explicitly as a cross-check against a
      subtly-wrong fetch. Device healthy after. Third hardware-risk probe
      in a row clean on the first attempt.
- [x] **Push constants, graphics stage — done, same session.**
      `tests/render_push_probe`: same triangle again, one variable changed
      — the fragment colour comes from `vkCmdPushConstants` instead of
      being hardcoded. Checked against the *specific pushed value*
      (`66cc33ff`), not a separately-hardcoded expectation, so a shader
      silently falling back to a stale value would show up as a mismatch.
      190/66/0 again, pushed colour exact. Fourth hardware-risk probe in a
      row clean on the first attempt.
- [x] **Descriptor sets — done, same session.** `tests/render_ubo_probe`:
      same triangle, one variable changed — the fragment colour comes from
      a uniform buffer through a real, full `VkDescriptorSetLayout` /
      `VkDescriptorPool` / `vkCmdBindDescriptorSets` path instead of a push
      constant. Checked against the specific UBO value (`3399ccff`), a
      third distinct colour from every earlier probe. 190/66/0 again.
      Fifth hardware-risk probe in a row clean on the first attempt — the
      last basic plumbing mechanism a graphics pipeline needs, proven.
      What's left (textures, depth/stencil, multi-draw) is variation on
      this and the earlier probes, not a new mechanism.
- [x] **Textures — done, same session, but not clean on the first try.**
      `tests/render_texture_probe`: a real 1x1 texture through a combined
      image sampler. First attempt genuinely failed — no hang, no fault,
      but the sampled colour read back as `00000000` instead of the
      uploaded texel. Fixed by inserting an intermediate
      `TRANSFER_SRC_OPTIMAL` stage between upload and the shader-read
      transition, which the Vulkan spec does not obviously require.
      Reproduced clean twice after the fix: 190/66/0, exact match, correct
      colour. **One part is still unexplained:** a diagnostic added
      alongside the fix — reading the texture straight back via
      `vkCmdCopyImageToBuffer` before it's ever sampled — reads `00000000`
      on every run, even though the shader samples the correct colour
      moments later in the same command buffer. Not chased to a root
      cause; see `docs/kbase-notes.md`'s full account, including the two
      live hypotheses. **First probe this session not clean on the first
      attempt** — worth remembering that the five-clean streak before it
      was real progress, not proof the next thing will also just work.
- [x] **Depth test and depth write — done, same session, clean on the
      first attempt.** `tests/render_depth_probe`: a real `D32_SFLOAT`
      depth attachment, depth test/write enabled — the Z-test unit, no
      prior art in this repo at all. Checked more strictly than any
      earlier probe: both the colour output *and* the depth buffer itself,
      read back independently. Both matched exactly, both runs — 190
      far-depth + 66 near-depth + 0 other, the same split colour rendering
      has produced all session, now confirmed by a second, independent
      hardware path. No repeat of the texture probe's failure — applied
      that lesson going in by using the `TRANSFER_SRC_OPTIMAL`
      intermediate stage from the start rather than assuming the direct
      transition would work.
      **Seven hardware-risk probes this session: six clean on first
      attempt, one fixed with a documented, partially-understood change.**
      What remains for a conformance-shaped shader: multiple draws in one
      render pass. Everything past that is CTS scale (Phase 7), not
      basic-plumbing scale.
- [x] **Multiple draws in one render pass — done, same session, clean on
      the first attempt. The last item on this list.**
      `tests/render_multidraw_probe`: two non-overlapping triangles, one
      render pass, one bound vertex buffer and pipeline, a different
      push-constant colour before each `vkCmdDraw`. Reproduced twice:
      184 clear + 66 triangle-A (the exact baseline every probe on this
      geometry has produced, unaffected by a second draw following it) +
      6 triangle-B (present, correctly coloured, non-overlapping) + 0
      other (draw 2's state change did not corrupt draw 1's already-shaded
      pixels).
      **Eight hardware-risk probes run this session: seven clean on first
      attempt, one (texture sampling) fixed with a documented,
      partially-understood workaround.** Every basic Vulkan plumbing
      mechanism a real, conformance-shaped shader needs is now proven:
      render-pass entry, a full draw, vertex fetch, push constants,
      descriptor sets, texture sampling, depth test/write, multiple draws
      per pass. What remains is CTS scale (Phase 7) — dEQP-VK, real
      applications, extensions — not basic-plumbing scale.
      `SIMULTANEOUS_USE` rendering is the one thing still genuinely
      blocked, on the ringbuf and the unanswered upstream question.
- [x] Render to a buffer, dump to PNG, diff pixels. No WSI, no display.
      Substance done by the two probes above (render to a buffer, diff
      pixels programmatically); no PNG dump exists, since the in-process
      byte comparison already proves correctness without needing a human
      to look at an image. Add a PNG dump only if visual inspection
      becomes useful for a harder case — not needed for what's been
      tested so far.
- [x] **MSAA — done, clean on the first attempt** (2026-08-02).
      `tests/render_msaa_probe`: same triangle as `render_vbo_probe`, but
      the colour attachment is 4x multisampled with `storeOp = NONE` and
      resolves (`VK_RESOLVE_MODE_AVERAGE_BIT`) to a second, single-sample
      image that gets read back — the exact in-tile-memory resolve path
      PanVK's rewritten framebuffer abstraction added (`pan_fb_resolves`,
      per Collabora's write-up of MR mesa/mesa!39759; confirmed present in
      the vendored Mesa clone, `third_party/MESA-KMOD` pinned `c439d52c`).
      Every render probe before this one is single-sample, so nothing had
      touched this path before.
      Correctness check is geometry-aware rather than reusing the old
      exact 190/66/0 split: real antialiasing means edge pixels should
      resolve to a genuine AVERAGE blend, not snap to clear or triangle
      colour, so the probe classifies every pixel into clear-exact /
      triangle-exact / valid-blend (each channel between the two reference
      colours, alpha exactly 0xff) / corrupt, and treats `blend_px > 0` as
      the actual positive signal — a resolve that silently no-opped would
      reproduce the old 190/66/0 split with zero blended pixels, which
      would have failed this check.
      Result on the Poco X8 Pro (Mali-G720, r49p1): `152 clear-colour, 66
      triangle-colour, 38 blended-edge, 0 corrupt (of 256 total)` — every
      check passed, 0 failures. Device confirmed healthy afterward:
      `render_vbo_probe` re-run immediately after still gives the exact
      190/66/0 baseline. `framebufferColorSampleCounts` offered 4x, so
      that's what ran (probe falls back to 2x, then fails cleanly with a
      clear message if neither is offered).
- [ ] **Re-test the unexplained texture-probe anomaly against AFBC.** The
      same Mesa clone snapshot already has AFBC as PanVK's default
      compression (`PANVK_DEBUG_NO_AFBC` / `noafbc` in
      `panvk_instance.c`), which changes image memory layout — exactly what
      `render_texture_probe` touches. That probe's still-unexplained
      finding (`vkCmdCopyImageToBuffer` reading `00000000` moments before
      the shader samples the correct colour, `docs/kbase-notes.md`) was
      never re-run with `PANVK_DEBUG=noafbc` to see if disabling AFBC
      changes or removes it — a five-minute check that was not available
      as a hypothesis when the anomaly was first found.

## Phase 6 — WSI and Android driver packaging
- [x] **The Android external-memory path works: an AHardwareBuffer imports
      as `VkDeviceMemory`** (2026-08-02). That is the same memory path a
      swapchain image takes, so it is the substantive half of "gralloc
      support" rather than a preliminary.
      **Two beliefs recorded here were wrong and are worth not repeating.**
      First, `-Dandroid-stub=true` was thought to prevent gralloc from
      initialising; it only affects *link* time, the real `libhardware.so`
      resolves on device, and `u_gralloc_fallback_create()` never returns
      NULL anyway - so `VK_ANDROID_native_buffer` (rev 8) and
      `VK_ANDROID_external_memory_android_hardware_buffer` (rev 5) had been
      advertised all along. Second, the actual blocker was the
      `handle->data[0]` convention, which does not hold on this device, in
      **three** places - and two of them are in Mesa's shared Vulkan runtime
      (`vk_android.c`), not in PanVK, so they would affect any Mesa Vulkan
      driver on a gralloc that orders its handle differently.
      Fixed by `src/mesa/patch-panvk-android-gralloc-fd.py`; verified by
      `tests/driver_android_wsi_probe`. Full writeup in
      `docs/kbase-notes.md`.
- [x] **Sync-fd semaphores — done** (2026-08-02). `panvk_kbase_sync.c`
      implements `import_sync_file`/`export_sync_file`, so
      `vkAcquireImageANDROID` and `vkQueueSignalReleaseImageANDROID` both
      have what they need. No patch script was needed: the runtime derives
      `SYNC_FD` support from the ops being present.
      Import is `poll()` on the fd (`-1` means already-signalled, which is
      the common case); export waits and returns `-1`, the spec's "already
      signalled". A real exportable fence is **not possible** here —
      `KBASE_IOCTL_STREAM_CREATE` yields a timeline userspace cannot drive
      (`SW_SYNC_IOC_CREATE_FENCE` → `ENOTTY`), measured by
      `tests/sync_fd_probe`. Cost: a CPU block per present.
- [x] **Rootless ICD loading — solved** (2026-08-02). libadrenotools' "no
      Mali support" is about that library, not the platform: its
      Adreno-specific parts are file-redirect hooks and bcenabler, neither
      of which a Mesa driver needs. The namespace mechanism itself works
      unchanged — `tests/driver_namespace_probe` loads this driver through
      a custom linker namespace and enumerates Mali-G720 MC8.
      Needed because `libdrm.so`/`libhardware.so` are not Android public
      libraries. Recipe (both halves non-obvious) in `docs/kbase-notes.md`;
      `tools/package-driver.sh` produces the Adrenotools-convention zip.
      **Caveat:** demonstrated from a shell process, not from inside an app.
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
- [x] **CTS integration path proven** (2026-08-01): standard Vulkan loader
      ABI vs. this driver's Android hwvulkan HAL ABI mismatch solved with a
      purpose-built ICD shim (`src/tests/icd_shim/`), not by installing the
      driver as the system HAL. `deqp-vk` built for Android from a fresh
      VK-GL-CTS clone (`third_party/VK-GL-CTS/`) and run through the shim
      on the Poco X8 Pro via `--deqp-vk-library-path`. `dEQP-VK.info.*`
      (19 cases, no rendering/dispatch): 15 pass, 2 genuine spec-fails
      worth tracking (`VK_EXT_hdr_metadata`/`VK_EXT_headless_surface`
      dependency gaps), 1 correct `NotSupported`, 1 crash in CTS's own
      Android-EXE platform layer (`dEQP-VK.info.platform`, root-caused to
      a null `ANativeActivity*` upstream in `tcuAndroidPlatform.cpp`, not
      a driver defect). Device confirmed healthy after. See
      `docs/kbase-notes.md` for the full writeup.
- [x] **First external rendering test passes** (2026-08-01):
      `dEQP-VK.api.smoke.*` (CTS's own purpose-built first-thing-to-run
      group), 6/6 pass, 4 of them real triangle renders through CTS's own
      pipeline — independent confirmation of this driver's rendering path
      beyond this repo's own probes. `dEQP-VK.api.*` as a whole is
      267,166 cases (confirmed by dumping the case tree first, not
      discovered by running it blind) — too large for one unattended run.
- [x] **Real finding: a hard ~307-case ceiling on cumulative
      VkInstance/VkDevice creation** (2026-08-01), found running
      `dEQP-VK.api.object_management.*` (457 cases). Most cases in this
      group create their own throwaway `VkInstance`+`VkDevice` pair; after
      exactly 307 such cases succeed in one `deqp-vk` process, every
      subsequent `VkDevice` creation fails with
      `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Confirmed twice, landing on the
      identical 307-case count both times regardless of which specific
      case was next — points to a fixed-size resource table/slot count
      being exhausted, not a variable-rate leak. Device/GPU confirmed
      unaffected (fresh probe processes stay clean).
- [x] **Root-cause dig, first pass** (2026-08-01): built
      `tests/device_churn_probe` and ruled out, each tested well past 307
      iterations with zero failures: a plain fd leak (mali0 fd count
      stayed flat), bare instance/device create-destroy churn (500x),
      device+queue+command-pool churn (400x), real buffer+memory
      alloc/free churn (400x), and 8-thread concurrent device creation
      (400x, matching `multithreaded_per_thread_device`). Leading
      remaining candidate: **many simultaneously-live objects on one
      device** (what `max_concurrent.*`/`multiple_*` actually stress),
      architecturally different from every pattern ruled out so far —
      not yet tested. Web research confirms this is unexplored territory:
      no upstream Mesa kbase backend exists to compare against (this
      repo's `pan_kmod_kbase.c` is the first), the Poco X8 Pro's kernel
      source (which would show the real GPL kbase driver for this device)
      is not yet published by Xiaomi, and the one other kbase-based
      community project found (Panfork-derived, OpenGL/Gallium only) has
      no record of this either.
- [x] **Root-cause dig, second pass** (2026-08-01): tested the "many
      simultaneously-live objects" candidate directly — 250 devices ×
      500 simultaneous buffers (125,000 cumulative allocations) and 350
      devices × 20 simultaneous compute pipelines (7,000 cumulative) both
      came up clean. A 1,000-pipelines-on-one-device test also passed but
      completed suspiciously fast (0.04s), likely an internal pipeline
      cache deduplicating identical compiles rather than a genuine
      independent-`EXEC_VA`-allocation stress test — flagged as
      inconclusive, not counted as ruling anything out. **Every
      synthetic reproduction attempt across both passes has come up
      clean.** Changing approach: the recommended next step is
      instrumenting the real driver (`src/mesa/pan_kmod_kbase.c`) with
      temporary counters/logging and re-running the actual failing
      `object_management` caselist against that build, rather than
      continuing to guess the reproduction shape with hand-written
      probes. See `docs/kbase-notes.md` for the full writeup.
- [x] **Root-cause dig, third pass: found the exact failure mechanism**
      (2026-08-01). Instrumented the real driver
      (`PANVK_KBASE_DEBUG_COUNTERS=1`, kept in the tree, silent by
      default) and re-ran the failing caselist. Result: **device creation
      itself succeeds** (`dev_create #792 SUCCESS`); the failure is a
      genuine kernel `ENOMEM` from `KBASE_IOCTL_MEM_ALLOC_EX` on the
      *first buffer allocation after* that successful creation, with only
      3 devices live — far below the 18-device peak this same run reaches
      cleanly elsewhere, ruling out live-device count as the trigger. Also
      directly falsified the "any pattern eventually exhausts something
      after enough cumulative creations" theory: 1000 sequential
      iterations in one process (2000 cumulative `dev_create` calls) ran
      clean, well past the real failure's cumulative count of 792. The
      failure lands immediately after the `multithreaded_*` test groups,
      on `private_data`'s first case — a `VK_EXT_private_data` device
      with real slot-request `pNext` chain, a configuration nothing
      tested so far has touched. Two narrower next steps identified:
      probe `multithreaded_shared_resources`'s actual shape (threads
      sharing one device, not each owning its own), and probe
      `VK_EXT_private_data` with real slot requests directly. See
      `docs/kbase-notes.md` for the full log excerpt and reasoning.
- [x] **Root-cause dig, fourth pass: both candidates also ruled out**
      (2026-08-01). Replicated `VK_EXT_private_data` device creation
      exactly (checked the CTS source first: the actual failing case uses
      zero requested slots, not the heaviest config) — 1000 iterations in
      one process, clean, byte-identical BO pattern to the plain
      baseline. Replicated `multithreaded_shared_resources`'s actual
      shape — N threads concurrently creating/destroying buffers on *one
      shared* device with CTS's own barrier-sync pattern, then checking
      whether a fresh unrelated device still works afterward — 60 rounds
      × 8 threads × 200 iterations (96,000 concurrent cycles), clean.
      **Four full passes now with no synthetic reproduction.** Remaining
      open hypotheses: the failure needs the exact cumulative sequence of
      every preceding CTS group (not any single isolated pattern), or —
      not yet checked — genuine Android-level system memory pressure
      unrelated to this driver, indistinguishable from a driver leak
      without sampling `/proc/meminfo` during a real run. Recommendation:
      next attempt should instrument and re-run the real caselist with
      memory sampling added, rather than inventing a fifth synthetic
      pattern; otherwise treat this as a well-characterized, deliberately
      set-aside finding and prioritize broader CTS coverage. See
      `docs/kbase-notes.md` for full reasoning.
- [x] **Root-cause dig, fifth pass: memory-sampled the real run — not
      system memory pressure either** (2026-08-01). Live `/proc/meminfo`
      sampling (`CmaFree` especially — the Contiguous Memory Allocator
      pool Mali/kbase GPU allocations typically draw from on this
      MediaTek SoC) during the real failing caselist run. Found something
      real: `CmaFree` crashed from ~105,000 kB to **236 kB** around case
      59-79 (during `max_concurrent.*`) — a genuine severe crunch — but
      it fully recovered and stayed stable (~95,000-116,000 kB) for over
      200 cases, including a sample at **case 305**, two cases before the
      abort point, where every memory indicator (`MemFree`,
      `MemAvailable`, `CmaFree`, `Cached`, `SwapFree`, process `VmRSS`)
      was completely ordinary — no trend, no decline. This rules out
      system-wide and CMA-specific memory pressure as the direct cause.
      No `dmesg`/kernel-log access without root, no accessible kbase
      debugfs on this device, and the Poco X8 Pro's kernel source still
      isn't published — so whatever is exhausted is invisible to every
      userspace vantage point available this session. **Five full passes
      have converged on a precise, well-evidenced characterization
      (kernel-level `ENOMEM`, unrelated to live-device count or system
      memory, following a severe-but-resolved CMA crunch) without a
      definitive root cause reachable from userspace.**
- [x] **Closed via open-source prior art, not root access** (2026-08-01).
      Root/kernel-log access deliberately not pursued (explicit
      instruction). Searched instead for whether kbase's own public
      source explains this symptom shape — it does: kbase's kernel-side
      memory pool (`kbase_mem_pool`) is backed by a Linux shrinker that
      reclaims pages under memory pressure (matching the observed CMA
      crunch/recovery), and `kbase_mem_pool_grow()` regrowing that pool
      afterward is documented to be able to fail with `ENOMEM`
      independently of `/proc/meminfo`-visible metrics — exactly the
      mismatch this dig measured. Also a known **category** of kbase
      issues, not a one-off: two GitHub Security Lab advisories
      ([GHSL-2022-127](https://securitylab.github.com/advisories/GHSL-2022-127_Arm_Mali/),
      [GHSL-2023-005](https://securitylab.github.com/advisories/GHSL-2023-005_Android/))
      are rooted in the same pool/shrinker/eviction-list subsystem.
      **Conclusion: very likely downstream kbase kernel-driver behavior,
      not a bug in this repo's `pan_kmod_kbase.c`/
      `panvk_vX_kbase_queue.c`** — any userspace driver on this kernel
      would hit the same pool-regrowth failure mode after the same kind
      of memory-pressure burst. Known workaround (excluding the specific
      leaves that land on this ceiling) already in use; doesn't block
      broader CTS work. See `docs/kbase-notes.md` for full reasoning and
      sources.
- [x] **`dEQP-VK.api.*` widened: 2194/2194 non-huge-sweep cases run**
      (2026-08-01) — everything except `copy_and_blit`/`image_clearing`
      (huge sweeps, deferred), `info` (deferred), `buffer`/
      `ds_color_copy`/`buffer_view`/`image_compression_control` (medium
      sweeps, deferred), and `object_management` (already covered).
      **1258 passed, 931 correctly `NotSupported`, 5 genuine failures.**
      Rebooted the device mid-pass (normal power cycle, not root) after
      the resource ceiling started recurring much earlier than the
      original 792-case finding — the reboot gave a genuinely useful
      negative result: `device_init.create_device_global_priority.basic`
      failed identically before and after, ruling out session-cumulative
      wear for *that* case and pointing instead at a real, likely-fixable
      bug in this driver's acknowledged-incomplete global-priority
      mapping. Two real `SIGSEGV` crashes found (device confirmed healthy
      after both): `create_instance_device_intentional_alloc_fail` (a
      simulated host-allocation failure isn't handled gracefully) and
      `null_handle.destroy_device` (`vkDestroyDevice(VK_NULL_HANDLE)`
      should be a spec-legal no-op). One CTS-side bug, not this driver's:
      the whole `external.memory.android_hardware_buffer` subtree hits a
      CTS assertion that doesn't hold on this device's SDK version (36).
      `command_buffers` (131 cases, real GPU work per case → slow, not
      risky) deferred but partially run, surfacing two genuine
      rendering-correctness failures both specific to **drawing from
      secondary command buffers** — a shape no render probe this session
      has tested. Five genuine failures in the completed run itself:
      layer-name-abuse validation not enforced, wrong reported
      `conformanceVersion`, `extension_duplicates.device.*` failing with
      `VK_ERROR_OUT_OF_DEVICE_MEMORY` (not yet distinguished from the
      resource-ceiling pattern vs. a real duplicate-extension-handling
      bug), and `version_check.unavailable_entry_points`. See
      `docs/kbase-notes.md` for full classification and the exact
      exclusion list.
- [x] **Fixed: `vkDestroyDevice(VK_NULL_HANDLE)` segfault** (2026-08-01).
      Symbolized the crash tombstone against the unstripped local build
      with the NDK's own `llvm-addr2line` (no root needed) — pinpointed
      to `panvk_DestroyDevice()` (`panvk_physical_device.c`, plain
      upstream PanVK code, not kbase-specific) dereferencing the device
      handle before checking for `VK_NULL_HANDLE`, which every
      `vkDestroy*` command must accept as a no-op. Fixed via
      `src/mesa/patch-panvk-null-device-destroy.py`, a new hand-run
      idempotent patch following the existing `patch-panvk-kbase-*.py`
      convention but deliberately not named `kbase` — this bug/fix is
      shared PanVK code, a real candidate for upstreaming once verified
      against actual Mesa. Rebuilt, deployed, verified:
      `dEQP-VK.api.null_handle.destroy_device` passes, the full
      `null_handle.*` group is 23/24 (1 correctly `NotSupported`, no
      regressions). Second crash
      (`create_instance_device_intentional_alloc_fail`) not yet
      triaged — deeper dig, tracked separately.
- [x] **Fixed: `create_instance_device_intentional_alloc_fail` crash**
      (2026-08-01). Root cause: `create_kbase_kmod_dev()`
      (`panvk_physical_device.c`, generated by this repo's own
      `patch-panvk-kbase-enumeration.py`, not upstream PanVK code) couldn't
      distinguish "this fd genuinely isn't kbase" from "this fd is kbase
      but device creation failed for a real reason (host allocation, ioctl
      failure)" — both collapsed to `VK_ERROR_INCOMPATIBLE_DRIVER`, which
      Mesa's `vk_instance.c` treats as "try the next enumeration method,"
      silently turning a genuine allocation failure (exactly what this CTS
      test injects) into `vkEnumeratePhysicalDevices` succeeding with zero
      devices — which then crashed the test on its first physical-device
      access. Fixed by calling `pan_kmod_fd_is_kbase()` and
      `kbase_kmod_ops.dev_create()` directly instead of through the generic
      dispatcher (same calls, relocated to the caller so the two failure
      reasons can be told apart and reported correctly) — still exactly one
      `KBASE_IOCTL_VERSION_CHECK` call, respecting the documented
      once-per-fd constraint. Rebuilt, deployed, verified: the target test
      passes (genuinely exercising the alloc-fail retry loop, not a trivial
      pass), the full `device_init.*` group runs to completion with no
      abort (227/236 passed, 8 correctly `NotSupported`, 1 pre-existing
      unrelated failure, no regressions), device stays healthy throughout.
      **Both `SIGSEGV` crashes found by this session's CTS widening are now
      fixed and verified on hardware.** See `docs/kbase-notes.md` for full
      reasoning.
- [x] **Triaged the remaining four CTS findings** (2026-08-01). Three are
      not code bugs: `driver_properties.conformance_version` (an honest
      `{0,0,0,0}` for a non-conformance-tested architecture, not something
      to fake); `extension_duplicates.device.*` (confirmed recurrence of
      the closed resource-ceiling finding, not a real dedup bug — excluded
      like `object_management`'s leaves); `create_instance_layer_name_abuse`
      (confirmed architectural — layer validation is the loader's job, and
      this project's CTS runs deliberately bypass the loader via the ICD
      shim). The fourth — the two secondary-command-buffer rendering
      failures — sharpened into a real, specific lead via a short staged
      CTS follow-up (not code reading): `record_many_draws_secondary_2`
      **fails 100% of the time in isolation, but passes if literally any
      other secondary-buffer draw ran first in the same process** — a
      deterministic cold-start/lazy-init bug, not flakiness or a volume
      issue. `many_indirect_draws_on_secondary` may be the same bug landing
      on the alphabetically-first case or a distinct one — testing that
      needs a small dedicated probe (`deqp-vk`'s fixed alphabetical case
      order makes it untestable via CTS case selection alone, confirmed by
      trying both `--deqp-case` lists and ordered `--deqp-caselist-file`).
      See `docs/kbase-notes.md` for the full reasoning, evidence, and next
      step.
- [x] **Built `tests/render_secondary_warmup_probe`: clean negative result**
      (2026-08-01). Tested on hardware whether `many_indirect_draws_on_secondary`
      shares `record_many_draws_secondary_2`'s cold-start bug: one indirect
      draw from a secondary command buffer, as the first secondary op in
      the process, then a warm-up draw, then the same indirect draw again.
      **Both rounds passed cleanly, first attempt** — ruling out the simple
      "same bug, different case" hypothesis. Whatever breaks CTS's own test
      needs something this probe didn't replicate: draw volume (CTS issues
      4096 indirect draws, this probe issued 1), point-list topology
      (vs. triangle list), or target size (64×64 vs. 16×16). Device
      confirmed healthy after.
- [x] **Draw-call count also ruled out** (2026-08-01). Added an
      `indirect-repeat-count` argument and re-ran with CTS's exact value —
      4096 separate `vkCmdDrawIndirect` calls recorded into one secondary
      buffer (matching CTS's actual shape: many separate calls, not one
      call with a high `drawCount`). **Both cold and warm rounds passed
      cleanly again.** Two independent, exact-value negative results now:
      neither a single indirect draw nor 4096 of them (CTS's real count)
      reproduces the failure. The two remaining candidates — point-list
      topology and 64×64 target size — are very likely entangled in CTS's
      test (4096 draws = 64×64 pixels, suggesting one point per pixel, a
      full-coverage test, not draws piled on one spot). Testing that
      properly means reproducing per-draw varying pixel-targeted
      positions — substantially closer to reimplementing CTS's own test
      than the two cheap isolations already done. Stopped here
      deliberately rather than escalating further without a check-in. See
      `docs/kbase-notes.md` for full detail.
- [ ] dEQP-VK in stages: smoke → rendering → sync → compute → multisample
      → extensions. Keep an xfail list. Land fixes in small batches.
      `dEQP-VK.info.platform` excluded (known CTS-Android-EXE gap, not a
      driver issue); `object_management`'s `*.device`/`*.device_group`
      leaves excluded pending the resource-ceiling investigation above.
      Next: the deferred medium/huge sweeps, `command_buffers` to
      completion, and `dEQP-VK.query_pool.*` per the standing plan.
      **Reprioritize `copy_and_blit`/`image_clearing` above `multisample`
      when picking the next deferred huge sweep** (checked 2026-08-02): the
      vendored Mesa clone already ships AFBC-by-default and the new
      `pan_fb_*` framebuffer/resolve abstraction (see Phase 5's new MSAA
      and AFBC items above), and `copy_and_blit`/`image_clearing` are
      exactly the CTS groups that exercise image-copy + compressed-layout
      interactions together — the same combination behind the still-open
      texture-probe anomaly.
      **`image_clearing` root cause narrowed further (2026-08-06):**
      `tests/render_clear_probe --linear`/`--general`/`--no-ca-usage`
      reproduce, via a real render-pass clear, every condition used above to
      explain the `vkCmdClearColorImage` failures (non-AFBC tiling, the
      `GENERAL` layout `vk_meta` actually uses, and creating the image
      without `COLOR_ATTACHMENT_BIT` the way an app calling that entry
      point would). All three pass, pixel-exact, on the Poco X8 Pro. So
      "the non-AFBC clear path is broken" is not quite right either — the
      uncompressed path works fine for render-pass clears. The bug is
      narrower than that: it's inside `vk_meta_clear_color_image` (or the
      internal render pass it builds) specifically, not in AFBC/layout/
      tiling handling in general. Still upstream, still not this port —
      see `docs/kbase-notes.md`.
      **When the `extensions` stage is eventually reached, expect a known
      gap list, not driver bugs to chase.** Cross-checked against GitLab
      work item `panfrost/mesa#125` ("features the Mali DDK implements but
      PanVK does not"), 49/76 done as of 2026-07-15: `dEQP-VK.*` cases for
      `VK_EXT_transform_feedback`, `VK_KHR_ray_query`/`ray_tracing_pipeline`,
      `VK_EXT_fragment_density_map`/`_map2`, `VK_EXT_subpass_merge_feedback`,
      `VK_EXT_primitives_generated_query`, `VK_EXT_image_compression_control`,
      `geometryShader`, and `tessellationShader` should correctly report
      `NotSupported` on this driver — none of them are gated by this
      repo's target hardware (Mali-G720, v10-class clears every v9+/v10+
      gate seen in that ticket), they are simply not implemented upstream
      yet.
      **`VK_EXT_transform_feedback` (no GS/tessellation) is real and scoped,
      unlike `geometryShader`/`tessellationShader` above** — investigated,
      implemented, and tested on real hardware (2026-08-06):
      `src/mesa/patch-panvk-xfb-phase1.py` + `src/mesa/panvk_vX_cmd_xfb.c`.
      Full writeup and architecture in `docs/kbase-notes.md`; short version:
      a second, monolithic VS variant (`no_idvs=true`,
      `nir_lower_xfb_to_stores` applied) gets compiled alongside the normal
      render VS and launched as a plain compute job on
      `PANVK_SUBQUEUE_COMPUTE`, reusing the render VS's
      `vs_desc_state->res_table` for hardware attribute fetch — mirroring
      Panfrost GL's `GENX(csf_launch_xfb)` as closely as PanVK's
      multi-subqueue CSF architecture allows.
      **Compiles clean** across v6/v7/v10/v12/v13/v14; the patch script
      applies cleanly and idempotently to both `/opt/mesa-src` and
      `third_party/MESA-KMOD` despite the two trees being different
      commits (verified byte-identical output).
      **Tested on the real Poco X8 Pro (Mali-G720) — found and fixed two
      confirmed bugs**: (1) `bifrost_postprocess_nir()` unconditionally
      requires a non-NULL `varying_layout` for any `MESA_SHADER_VERTEX`
      compile (its `assert()` compiles out in release builds and it then
      `memcpy()`s from NULL — a real tombstone, not a guess); (2) the
      dispatch populated the CSF "slot 1" registers
      (`MALI_COMPUTE_SR_*_1`) but launched with slot-0 resource selection
      copied from GL's plain-offset example, so the hardware read
      uninitialized registers. Both fixed. **Still not a working feature**:
      after both fixes, the dispatch cleanly returns `VK_ERROR_DEVICE_LOST`
      — a real GPU-level fault, but not a hang (confirmed via
      `driver_compute_probe --fill` immediately after every single
      attempt: 0 failures, no wedge, no reboot needed, every time).
      **Cross-subqueue sync investigated (2026-08-06, continued) — found
      and fixed a real structural bug, but it wasn't the whole answer.**
      `flush_tiling()` (`csf/panvk_vX_cmd_draw.c`) — the only thing that
      signals `PANVK_SUBQUEUE_VERTEX_TILER`'s syncobj and gives
      `PANVK_SUBQUEUE_COMPUTE` something valid to wait on — runs once per
      render pass, from `CmdEndRendering`, never per draw. The dispatch
      originally fired immediately inside `CmdDraw`, before `flush_tiling()`
      had ever run, so any wait it inserted would target a stale or
      nonexistent sync point. Direct precedent for deferring found
      already in PanVK: `panvk_cmd_end_occlusion_query()` explicitly waits
      for `EndRendering` for the same reason. **Fixed** with a
      queue-and-flush restructure: `CmdDraw` now queues pending draws
      (`xfb.pending_draws[]`), and
      `panvk_per_arch(cmd_flush_pending_xfb_captures)()`, called from
      `CmdEndRendering` right after `flush_tiling()`, replays them with an
      explicit cross-subqueue wait against the just-incremented sync
      point (mirroring `emit_barrier_insert_waits()`'s exact primitives).
      **Still faults identically after this fix** —
      `VK_ERROR_DEVICE_LOST`, device stays fully healthy every time.
      **Root-caused and fixed (2026-08-06, continued): the XFB variant's
      SPD declared `MALI_SHADER_STAGE_VERTEX`** (built by
      `panvk_shader_upload()`, which branches purely on
      `shader->info.stage` — still `MESA_SHADER_VERTEX` for this variant,
      required for `bifrost_postprocess_nir()`'s VS-specific lowering to
      run at all), **while being launched via `cs_run_compute`, a
      `COMPUTE`-shaped job.** Found via systematic bisection (SRT forced
      to 0 — still faulted; skipping `cs_run_compute()` entirely — fence
      succeeded, localizing the fault to execution itself; confirmed
      compute/render interleaving isn't inherently broken by finding
      `update_prims_generated_query()` already doing exactly that in
      shipping code; skipping `nir_lower_xfb_to_stores` — still
      faulted, ruling out the XFB write mechanism). Building a plain
      `SHADER_PROGRAM` descriptor that explicitly declares
      `MALI_SHADER_STAGE_COMPUTE` — pointing at the same compiled binary,
      no shader-side change — **fixed it**: `vkWaitForFences -> 0`,
      confirmed on the real Poco X8 Pro, render unaffected. This is now
      the permanent implementation. **The crash is solved.**
      **Vertex/instance-ID fix**: `nir_load_raw_vertex_id`/
      `load_vertex_id`/`load_instance_id` all compile to hardware-preloaded
      registers that firmware only populates correctly for real
      VERTEX/IDVS jobs; a `COMPUTE`-declared job gets something else
      preloaded into that slot. Fixed with a new NIR pass,
      `panvk_lower_xfb_compute_dispatch_ids`, replacing all three with
      `workgroup_id`-derived values (the capture dispatch fixes
      `WG_SIZE=1x1x1`, so `workgroup_id.x`/`.y` directly equal the
      vertex/instance index).
      **ROOT CAUSE FOUND AND FIXED — the capture now works end to end
      (2026-08-06).** Every fix above still left the XFB buffer reading
      back as untouched poison, because the shader had never executed at
      all: `panvk_shader_upload()` — the only thing that uploads a
      variant's binary and sets `code_mem` — is driven by
      `panvk_shader_foreach_variant()`, which walks `variants[]`, and
      `xfb_variant` is deliberately *not* in that array. Its code was
      never uploaded, so `panvk_shader_variant_get_dev_addr()` returned 0
      and the dispatch's `SHADER_PROGRAM` descriptor pointed
      `cfg.binary` at device address 0 — the GPU was dispatching a shader
      with no code, which writes nothing and never faults. Fixed by
      uploading the XFB variant's binary explicitly. Found by checking
      each link in the chain rather than the shader logic:
      `BIFROST_MESA_DEBUG=shaders` showed the compiled shader was correct
      (full `STORE.i128`, address from FAU `u0`); host instrumentation
      showed `FAU[0]` held exactly the XFB buffer address; CS-level marker
      stores bracketing `RUN_COMPUTE` both landed, proving the command
      stream ran — leaving only `cfg.binary`, which printed as 0.
      **Verified on the real Poco X8 Pro**: `tests/render_xfb_probe`
      captures all 3 vertices exactly, 0 failures, render unaffected
      (190/66/0), device healthy afterwards.
      **Phase 2, step 1 — indexed draws now capture (2026-08-06, working).**
      `vkCmdDrawIndexed` was the biggest gap for real applications. The
      XFB store slot and the attribute-fetch index differ for an indexed
      draw, and they conveniently already arrive as two distinct NIR
      intrinsics: `raw_vertex_id` (store slot, from
      `nir_lower_xfb_to_stores`) stays sequential, while `vertex_id`
      (attribute fetch, from `panvk_lower_load_vs_input`) became
      index-buffer aware via a new `panvk_lower_xfb_vertex_id()` pass.
      `vertexOffset` needs no shader work — `GLOBAL_ATTRIBUTE_OFFSET`
      already applies exactly that bias in hardware. One variant serves
      both draw kinds, so the index fetch (and the index width) is a real
      runtime branch on a new `xfb.index_buffer` sysval rather than a
      `bcsel`, which would wrongly evaluate the load when there is no
      index buffer. Tested by `render_xfb_probe --indexed`, which draws
      through indices `{2,0,1}` — a cyclic rotation, so the render result
      must stay byte-identical (190/66/0) while the capture order
      permutes; capturing the *unpermuted* order is precisely the
      signature of a shader that ignored the index buffer, and the probe
      names that failure mode explicitly. Both modes pass on the real
      Poco X8 Pro, 0 failures, device healthy.
      **Out-of-bounds capture writes — found and fixed (2026-08-06).** A
      GPU memory-safety bug, found while scoping phase-2 work rather than
      by a failing test: nothing checked the bound XFB buffer size, so a
      draw larger than its capture buffer wrote past the end of it (a
      48-byte buffer with a 100-vertex draw wrote ~1600 bytes). Phase 1's
      plan had called for host-side bounds checking; it was never
      implemented. Fixed by clamping the dispatch grid to the tightest
      capacity across every buffer the shader writes. Deliberately
      conservative in one direction: a trailing *partial* instance is
      dropped rather than split, since keeping `vertex_count` intact keeps
      every surviving slot at the address it would otherwise have had.
      Tested by `render_xfb_probe --overflow`, which binds 32 bytes of a
      poisoned 48-byte buffer while drawing 3 vertices and checks the tail
      is untouched — verified to genuinely detect the bug (fails against
      the pre-clamp driver, passes after). All four probe modes green.
      The same capacity computation is what
      `VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT` needs, since
      generated-vs-written is exactly the unclamped-vs-clamped count.
      **Counter-buffer resume re-scoped**: initially grouped with queries
      as cheap host-side work, but a resumed offset lives in GPU memory
      and so cannot be baked into the push-uniform buffer at record time.
      Once the starting offset is GPU-resident, the bounds clamp and the
      End counter writeback stop being host-computable too and all three
      must move into the command stream together — structurally the same
      work as indirect draws, not a cheap addition. XFB queries remain
      genuinely cheap here, since their counts are host-known.
      **`VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT` implemented
      (2026-08-06, working).** Two uint64 reports — primitives written and
      primitives generated — mirroring the existing
      `PRIMITIVES_GENERATED_EXT` CSF pattern; upstream had even left a
      `/* TODO: transform feedback */` on the already-present
      `CmdBegin/EndQueryIndexedEXT`. `transformFeedbackQueries` is now
      `true`. This pulled in three related fixes: a **topology
      restriction** (the capture emits one vertex per *input* vertex,
      which only matches XFB semantics for LIST topologies — a triangle
      strip needs 3·(N−2) captured vertices with duplication, so strips
      and fans are now rejected rather than silently producing wrong
      data); the **bounds clamp becoming primitive-granular** (XFB drops
      whole primitives rather than truncating them, so a partially-fitting
      triangle now captures nothing, which also keeps "written" a whole
      number of real primitives); and **deferred query availability**
      (counters are only accumulated at `CmdEndRendering`, so a query
      ended before the render pass hands its availability write to the
      flush). The probe caught a genuine bug during bring-up: the
      zero-capture path early-returned before accounting, reporting
      `generated = 0` when a dropped primitive was still generated.
      Tested by `render_xfb_probe --query` (composable with `--indexed` /
      `--overflow`); the overflow case is the only one where the two
      counters legitimately disagree (0 written, 1 generated). All eight
      probe-mode combinations pass, device healthy.
      **Capture dispatch flattened to a 1D grid (2026-08-06)** —
      groundwork for GPU-resident counts. The grid was
      (`vertex_count`, `instance_count`); clamping *that* against a value
      living in GPU memory (what counter-buffer resume and indirect draws
      both need) means splitting the clamp across two axes and dropping
      whole instances rather than a partial one. Now `JOB_SIZE_X =
      vertex_count * instance_count`, `workgroup_id.x` is the linear
      capture slot, and the shader recovers `instance = slot /
      num_vertices`, `vertex = slot - instance * num_vertices` — which
      reconstructs the XFB store slot exactly, since
      `nir_lower_xfb_to_stores` computes `instance_id * num_vertices +
      raw_vertex_id`. `num_vertices` stays the *unclamped* per-instance
      count so the decomposition survives clamping. The host clamp
      collapses to one `MIN2` plus primitive alignment, a partial instance
      is now captured rather than dropped, and the two lowering passes
      merged into one. Required adding `render_xfb_probe --instanced`:
      every previous mode drew `instanceCount == 1`, where the
      decomposition is degenerate (`instance` always 0, `vertex == slot`),
      so a completely broken one would still have passed all eight modes.
      All twelve mode combinations now pass, device healthy.
      **CS arithmetic is arch-gated — measured, after two wrong guesses.**
      `cs_udiv32`, `cs_umul64`, `cs_add32`, `cs_sub32`, `cs_add64` and
      `cs_lshift_imm32` all exist in `cs_builder.h` but sit inside a
      `#if PAN_ARCH >= 13` block (lines 1844-2096). On this device
      (Mali-G720, PAN_ARCH 10) the command stream has only
      `cs_add_imm32`/`cs_add_imm64` (immediate operand), `cs_umin32`,
      `cs_and32` and load/store/move — **no register-register add,
      subtract, multiply or divide**. A capacity clamp written against
      those ops builds for v13/v14 and fails outright on v10/v12, which is
      how this was found; that attempt was reverted rather than left
      half-applied. The design that fits the constraint does the
      arithmetic in the capture shader (where it is cheap) and leaves the
      command stream only *copying* values: a GPU scratch write position
      per buffer, a new `xfb.slot_offset[i]` sysval copied into the
      already-uploaded push uniforms with a plain load32/store32
      (precedent in `panvk_vX_cmd_dispatch.c`'s indirect path), the shader
      computing `base + (slot_offset + slot) * stride` and skipping stores
      that would not fit, and `cs_add_imm32` advancing by the host-known
      generated count.
      **GPU-resident capture offsets landed (2026-08-06).** Prior art
      settled the open question with *neither* of the options above: Asahi
      `hk` uses a single-invocation setup kernel that computes the clamp
      in closed form and updates counters with plain non-atomic `+=`
      (`src/poly/cl/geometry.cl`), needing no atomics and no
      per-invocation checks. PanVK now has `panlib_xfb_setup` in
      `libpan/draw_helper.cl` doing the same — clamping to the tightest
      remaining capacity, writing the slot count where the CS loads it
      into `JOB_SIZE_X`, resolving `base + offset*stride` into the
      push-uniform slot the capture shader reads, and owning the query
      counters and the write-position advance. `draw_helper.cl` was
      already in `libpan/meson.build`, so no build plumbing was needed.
      **The bug that made it look impossible**: `PANVK_CSF_BARRIER_WAIT`
      is `cs_wait_slot()` and `SYNC` bumps a syncobj — both pure
      synchronisation, neither flushes caches — so a value the kernel had
      just computed was still in L2 when the command stream loaded it. An
      explicit `cs_flush_caches(CLEAN)` plus a wait is required whenever
      the CS consumes what a kernel just wrote, and there was no prior
      instance of that pattern in PanVK to copy. A second, self-inflicted
      bug is worth remembering: `offsets_gpu` was released in
      `CmdEndTransformFeedbackEXT`, but End runs *before* `CmdEndRendering`
      where the captures are actually dispatched — the same shape as the
      deferred query-availability bug, and a trap for any future End-time
      cleanup. All twelve probe modes pass, device healthy.
      **Counter-buffer resume landed (2026-08-06).**
      `vkCmdBeginTransformFeedbackEXT` with `pCounterBuffers` now resumes
      from the byte offset the counter buffer holds, and the final
      position is written back at End. One design change made it easy:
      the write position is tracked in **bytes** rather than capture
      slots, because that is the unit a counter buffer uses — in slots,
      seeding would need a divide and writeback a multiply, neither of
      which the command stream can do on arch 10. In bytes both are plain
      32-bit copies and the only division lives in the kernel, which
      divides for free; it also simplifies the address resolve to
      `base + offset`. End again cannot do the writeback itself (it runs
      before `CmdEndRendering`), so it records the targets and the flush
      emits the copies — the *third* instance of that same deferral in
      this feature, now the expected shape for anything End touches.
      Tested by `render_xfb_probe --resume`, which seeds the counter
      buffer with 48 bytes and checks the first 48 bytes stay untouched,
      the triangle lands at offset 48, and the counter reads back 96.
      All fifteen probe modes pass, device healthy.
      **Indirect draws landed (2026-08-06)** — `vkCmdDrawIndirect`,
      non-indexed, `drawCount == 1`. With the counts in GPU memory three
      things stop being host-known, and the setup kernel was already the
      right home for all of them: the clamp and query counters (the kernel
      reads `vertexCount`/`instanceCount` from the buffer — both
      `VkDrawIndirectCommand` and `VkDrawIndexedIndirectCommand` start
      count-then-instance-count, so the indexed variant will need no
      change there), `num_vertices` (which the shader needs to split its
      linear slot, patched into the push uniforms just like
      `xfb.buffer_addrs[]`), and `firstVertex` (loaded straight into
      `GLOBAL_ATTRIBUTE_OFFSET` from the command — needing no cache flush,
      since it comes from an application buffer with no producer in our
      command stream). The awkward part is TLS sizing, which happens on
      the host before the kernel runs: with no host count it sizes for the
      largest capture the bound buffers could hold, which is by
      construction the most the clamp can let through. Tested by
      `--indirect` and especially `--indirect --instanced`, where a broken
      `num_vertices` patch would make instance 1 read past the vertex
      buffer. All twenty probe modes pass, device healthy.
      **Indexed indirect landed (2026-08-06)** — `vkCmdDrawIndexedIndirect`,
      and as cheap as predicted: one more push-uniform patch in a kernel
      that already did two. The counts needed no work at all (both command
      structs share their first two fields). `firstIndex` (word 2) does:
      the queued draw carries the **unbiased** index-buffer base and the
      kernel writes `base + firstIndex * index_size` into the
      `xfb.index_buffer` sysval, whereas a *direct* indexed draw still
      carries the pre-biased address since the host knows `firstIndex`
      there. `vertexOffset` sits at word 3 where the non-indexed command
      has `firstVertex` at word 2, so `GLOBAL_ATTRIBUTE_OFFSET` loads from
      12 rather than 8. The test is self-checking: the `{2,0,1}` indices
      permute the vertices, so a mis-applied bias (or an ignored index
      buffer) shows up as the *unpermuted* order the probe already names
      as a distinct failure. All twenty-four probe modes pass, device
      healthy.
      **Patch script verified to reproduce the tested tree (2026-08-07).**
      With the script now patching eleven files across
      `src/panfrost/vulkan/` and `src/panfrost/libpan/`, "applies cleanly"
      stopped being safe to assume. The blocker was pin drift:
      `third_party/MESA-KMOD` sits at `43ec7c6b` while the work was
      developed and verified against `7296f9af84cd`, and upstream
      refactored `panvk_vX_shader.c` in between (one of the script's
      anchors no longer exists there). Resolved without moving either pin,
      by verifying against a throwaway `git worktree` of MESA-KMOD checked
      out at `7296f9af84cd` — leaving the main checkout and its synced
      kbase backend untouched. Result: applies cleanly, is idempotent, and
      every file the script owns comes out byte-identical to the
      hardware-verified tree (20/20 checks); the rebuilt driver passes all
      24 probe modes. **The divergence this caught**: `/opt/mesa-src`
      carried a `/* TEMP: enabled for hardware verification */` edit
      advertising the extension, while the tracked script emitted `false`
      — so every recorded hardware result was obtained with it on, and the
      script as tracked produced a driver the probe could not test at all
      (`vkCreateDevice` rejects an unadvertised extension, so the entry
      points cannot even be resolved). Now a deliberate, documented
      toggle: `PANVK_XFB_ADVERTISE=1 patch-panvk-xfb-phase1.py <dir>`.
      **Multi-draw indirect landed (2026-08-07)** — and needed *no kernel
      change at all*. Each command is an independent draw and
      `panlib_xfb_setup()` already advances the GPU-resident write
      position once per capture, so the driver just queues N pending
      captures at record time, one per command at `base + i * stride`;
      they run in order on the compute subqueue, each picking up where the
      last left off. Both indirect entry points now share one
      `xfb_queue_indirect_captures()` helper. Tested by `--multidraw`,
      which requires the triangle to appear twice at offsets 0 and 48 —
      had the position not advanced, the second capture would land on the
      first and the buffer's second half would stay poison. Two *probe*
      expectations were wrong before the driver was: `--multidraw --query`
      reports 2 primitives (two commands, two triangles; the query is
      scoped to the render pass), and `--multidraw --instanced --query`
      reports 2 written against 4 generated because the six-vertex buffer
      is filled entirely by command 0 — the bounds clamp working *across*
      a multi-draw. The probe now derives "written" from buffer capacity
      rather than assuming everything fits. All 29 probe modes pass.
      `.EXT_transform_feedback` is nonetheless still left `false` by
      default: still unsupported and asserted on are strip/fan topologies,
      primitive restart with XFB active and
      `vkCmdDrawIndirectByteCountEXT` — so advertising it would turn
      "unsupported" into "assert/abort". Flipping it on stays a
      deliberate follow-up.
      Full geometry-shader/
      tessellation-shader emulation remains explicitly out of scope — see
      `docs/kbase-notes.md` for why (Asahi's `hk` driver is the only prior
      art, ~11,000+ lines, multi-month even reused).

## Phase 8 — Real-app validation
- [ ] apitrace/gfxreconstruct captures of actual apps/games once CTS is
      mostly green. CTS will not catch everything real apps hit.

## Phase 9 — Upstream conversation
- [x] **First concrete question sent** (2026-08-01, Mesa GitLab issue):
      `docs/upstream-ringbuf-question.md`. The render descriptor ringbuf's
      double mapping cannot be expressed on kbase, and the fallback
      (tail-padding instead of relying on the mapping to wrap) changes
      shared PanVK code that panthor also runs — so it is the first change
      here that genuinely needs agreement rather than a patch. The doc
      holds the chat form, the issue form, the measurements, and the
      kernel-source citations. **Awaiting a reply; do not write the
      tail-padding change until there is one** — the whole point of asking
      was to avoid guessing at shared-code behaviour that cannot be tested
      on panthor from here.
      **Context check, 2026-08-02:** `init_render_desc_ringbuf()` and the
      ringbuf macros this question is about live in
      `src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c` — the same file that
      now pulls `pan_fb_layout`/`pan_fb_load`/`pan_fb_store`/
      `pan_fb_desc_info` from PanVK's recently-rewritten framebuffer
      abstraction (Collabora's write-up of MR mesa/mesa!39759). Not the
      same code path as the ringbuf's double-mapping issue, but close
      enough in the same file that it is worth re-reading the current tree
      before acting on any reply, in case the surrounding render-pass code
      shifted shape since the question was drafted.
- [x] A first upstream contribution landed alongside it, deliberately
      small: `Joshua-Micheletti/PanVK2KBase#2`, making `kbase_bo_create()`
      treat the CPU pointer as the GPU address only under `SAME_VA`.
      Measured on-device, corroborated by the kernel source and by
      Panfork doing the identical thing. Not PanVK, but it establishes the
      pattern for how these should be argued: evidence attached, claim
      narrow.
- [ ] **The "BEFORE you're deep into Phase 4" condition is moot — that
      already happened, the other way round.** Re-checked 2026-08-01: two
      concrete technical questions went out instead of a general
      project heads-up (the ringbuf, `docs/upstream-ringbuf-question.md`;
      the import hook, `docs/upstream-import-question.md`), after Phase 4
      compute was already working. That was not a considered choice to
      skip this step — it is what "ask about the specific thing you're
      stuck on" naturally produced. Still outstanding: a broader "here is
      what this project is and here is what's blocking it" framing, which
      neither question makes on its own and which is the thing that would
      actually answer "is kbase wanted upstream at all" rather than
      settling one design question at a time.
      **That framing is now drafted:** `docs/upstream-project-status-
      question.md`. Sent after the other two, deliberately — it reads as
      a status report with two live questions attached rather than a cold
      introduction.
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
