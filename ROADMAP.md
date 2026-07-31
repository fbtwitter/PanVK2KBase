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
      **Cost: submissions are serialised**, which defeats much of the point
      of a ring buffer. Correct but slow, and the honest option while the
      real wake mechanism for an onslot idle CS is unknown. Revisit if
      kernel-side visibility ever becomes available — the answer is
      presumably in how `kbase_csf_queue_kick()` decides whether to ring
      the hardware doorbell for a group that is already onslot.
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
- [ ] **The render subqueues, blocked on BO aliasing — now unblocked in
      principle.** `init_gpu_queue()` still loops over
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
      **Groundwork landed:** `pan_kmod_kbase_group_create`/`_destroy` and
      `_tiler_heap_create`/`_destroy` are now in the backend
      (`src/mesa/pan_kmod_kbase.{c,h}`), wrapping the exact ioctl
      sequences `tests/queue_group` and `tests/live_kick_probe` already
      run on hardware. They compile into the driver but **nothing calls
      them yet**, so they are unproven in this form.
      **Three ways to integrate, and the choice matters:**
      (a) a sibling `panvk_vX_kbase_queue.c` selected at queue-creation
      time — cleanest to read and closest to how a `tu_knl_kgsl.cc`-style
      backend is structured, but duplicates a lot of non-ioctl logic that
      would then drift from upstream;
      (b) patch the ~13 call sites in `panvk_vX_gpu_queue.c` to dispatch
      on `dev->ops == &kbase_kmod_ops` — smallest diff, keeps one copy of
      the logic, but the patch script becomes large and fragile against
      upstream movement;
      (c) push submission into `pan_kmod_ops` as new vtable entries so
      panthor and kbase are peers — the only option with an upstreaming
      story (Phase 9), and the only one that needs agreement from
      Panfrost maintainers before it is worth writing.
      All the kbase-side primitives are proven; this is an integration
      decision, not a hardware unknown.
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
