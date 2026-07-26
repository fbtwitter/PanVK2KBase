# Roadmap

Checkboxes are for tracking your own progress — nothing past Phase 1 is
done yet. Phases are ordered by dependency, not by how interesting they
are; resist the urge to jump to Phase 3.

Adapted from a companion scaffold repo's roadmap to match this repo's
actual progress: this project already has a real vendored kbase UAPI
header and a working standalone probe, so Phase 0 and Phase 1 are further
along here than a from-scratch roadmap assumes. Track deviations from the
original recommendations inline below.

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
- [ ] Identify whether your kernel exposes JM (job-manager, older) or CSF
      (command-stream frontend, v10+) kbase ioctls. `first_test.c` builds
      with `-DMALI_USE_CSF=1` (see `makefile`), so this repo is already
      assuming CSF — confirm that assumption holds for the real device,
      not just the header.
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
- [ ] Clone Mesa.
- [ ] Add a `pan_kmod_kbase` backend as a third `pan_kmod` backend
      alongside `panfrost` and `panthor` (mirror their file shape in
      `src/panfrost/lib/`).
- [ ] Get device probe + enumeration working — this is where the
      DRM-node-vs-misc-device mismatch in `docs/architecture.md` has to
      actually be solved. Look at how Turnip's kgsl path is special-cased
      in physical-device enumeration and mirror that shape. The
      GPU-properties parsing already working in `utils/parse_gpu_props.h`
      is the raw-decode half of this; the other half is wiring that into
      `pan_kmod_dev_props`.

## Phase 3 — Memory management
- [ ] BO create/free through kbase's mem-alloc ioctls.
- [ ] mmap.
- [ ] dma-buf import/export.
- [ ] Tiler heap / JIT growable memory — PanVK's current growth logic
      assumes panthor/panfrost conventions; expect to adapt it.

## Phase 4 — Submission and sync (highest risk)
- [ ] Map VkQueueSubmit onto kbase atom/command-stream submission.
- [ ] Build the fence-translation shim between kbase's completion
      mechanism and whatever PanVK's sync code expects to wait/signal on.
- [ ] Budget the most time here. This was the long pole for kgsl too.

## Phase 5 — Headless triangle
- [ ] Render to a buffer, dump to PNG, diff pixels. No WSI, no display.

## Phase 6 — WSI
- [ ] Only after Phase 5 is solid. Android gralloc/ANativeWindow if
      targeting phones, or DRM/kmsro if targeting an embedded board still
      on kbase.

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
