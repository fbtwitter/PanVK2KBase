# Architecture notes

**Status (2026-08-01): historical.** Everything below was written as
pre-work analysis, before any of it existed — the "Action item" at the
end of the first section literally says "before writing a
`pan_kmod_kbase.c` backend, go read...". That backend has existed for a
long time now, and every problem this document treats as open is solved:

- **Device probe** — `pan_kmod_kbase.c` + the enumeration patch
  (`src/mesa/patch-panvk-kbase-enumeration.py`).
- **BO/VM management** — `pan_kmod_kbase.c`, with the SAME_VA/cookie and
  FIXED_VA design this doc flags as needing "real design decisions" both
  resolved and documented in `docs/kbase-notes.md`.
- **Submission + sync, called out below as "the hardest part... where to
  expect the most iteration"** — correctly predicted. The kbase-specific
  sibling to `panvk_vX_gpu_queue.c` this document says will be needed is
  `src/mesa/panvk_vX_kbase_queue.c`, 1294 lines. Compute runs end to end
  on real hardware (application SPIR-V, `vkCmdDispatch`, GPU-signalled
  fences, 2000 back-to-back submits with no failures), binary and
  timeline semaphores work, and as of today, rendering does too — a real
  triangle, vertex buffers, push constants, descriptor sets, texture
  sampling, depth test/write, multiple draws per pass, each proven with
  its own hardware probe in `src/tests/render_*_probe/`.

Kept below for the reasoning — the seam diagram, the Turnip/kgsl
precedent, and the JM-vs-CSF call are all still accurate background - but
treat every "needs adapting" / "still unsolved" / "action item" past this
point as describing the state before this backend was built, not the
state now. For the current, accurate picture: `ROADMAP.md` (Phase 4's
"Where this actually is", Phase 5's checkboxes) and `docs/kbase-notes.md`.

## The core mismatch: kbase is not a DRM driver

Every Mesa Vulkan driver's physical-device enumeration path assumes it's
looking for a DRM device — a `/dev/dri/renderD*` (or `/dev/dri/card*`)
node, opened via libdrm, with a major/minor pair the loader can use to
identify and dedupe devices. `pan_kmod`'s existing `panfrost` and
`panthor` backends both live comfortably inside that assumption, because
both are real DRM drivers.

kbase is a **misc character device** — `/dev/mali0` on the target this
repo has been testing against — with its own private ioctl surface. It is
not enumerated through the DRM subsystem at all. This means a "kbase
backend for pan_kmod" is not simply "write a third `pan_kmod_ops`
implementation" — the physical-device enumeration layer above `pan_kmod`
needs to learn about a non-DRM device path in the first place.

`tests/first_test/first_test.c` already confirms the misc-device side
works end to end: open `/dev/mali0`, `KBASE_IOCTL_VERSION_CHECK`,
`KBASE_IOCTL_SET_FLAGS`, `KBASE_IOCTL_GET_GPUPROPS`, decode. None of that
touches Mesa — it's the standalone building block Phase 2 below needs to
wire in.

### The precedent: Turnip + kgsl

This is not a new problem. Qualcomm's downstream `kgsl` driver
(`/dev/kgsl-3d0`) is also not a DRM node, and Turnip needed to run on
phones that only ship `kgsl`, not the upstream `msm` DRM driver. Turnip's
solution was to special-case device probing in its
instance/physical-device creation code so it looks for `kgsl-3d0`
alongside the usual DRM enumeration, then dispatches into a `kgsl`-backed
winsys (`tu_knl_kgsl.cc`) instead of the `msm` DRM winsys.

**Action item:** before writing a `pan_kmod_kbase.c` backend, go read how
Turnip's device-creation code decides "DRM path vs kgsl path" and where
in the call stack that decision happens. PanVK's physical-device creation
will need the same kind of branch for "DRM path (panfrost/panthor) vs
kbase misc-device path." Expect this to touch code outside `pan_kmod`
itself, not just inside it.

## Where the seam actually is

```
                     ┌─────────────────────────┐
                     │  PanVK (Vulkan driver)   │
                     └────────────┬─────────────┘
                                  │
                     ┌────────────▼─────────────┐
                     │   pan_kmod (kernel        │
                     │   abstraction layer)      │
                     └──┬──────────┬──────────┬──┘
                        │          │          │
                  ┌─────▼───┐ ┌───▼────┐ ┌───▼────────┐
                  │panfrost │ │panthor │ │ kbase (NEW) │
                  │  (DRM,  │ │ (DRM,  │ │ (misc dev,  │
                  │   JM)   │ │  CSF)  │ │  JM or CSF) │
                  └─────────┘ └────────┘ └────────────┘
```

`pan_kmod_ops` (in upstream Mesa: `src/panfrost/lib/pan_kmod.h`) is the
vtable every backend implements — device open/close, BO alloc/free/
import/export, and VM management. **Do not trust the exact field names
below** — verify against the actual header in the Mesa checkout you're
building against, since it evolves:

- device probe / properties query
- BO alloc / free / import (dma-buf) / export (dma-buf) /
  get-mmap-offset / wait / evictable
- VM create / destroy / bind (map/unmap) / query state
- misc: query GPU timestamp, BO labeling, perf-counter session

**Correction, checked against a real Mesa checkout (`git clone
--depth 1 https://gitlab.freedesktop.org/mesa/mesa.git`, cloned into
the gitignored `third_party/MESA-KMOD/`, `src/panfrost/lib/kmod/`):**
`pan_kmod_ops` has **no submission or sync entry at all** — the
"submission" and "sync object wait/signal" bullets from earlier
drafts of this doc were wrong. Command-stream submission and fence
handling for CSF are hardcoded directly into the Vulkan driver, not
routed through `pan_kmod_ops`:

- `src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c` calls
  `DRM_IOCTL_PANTHOR_GROUP_CREATE` / `_SUBMIT` / `_DESTROY` /
  `_GET_STATE` and `DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE` / `_DESTROY`
  directly on `dev->drm_fd` via `pan_kmod_ioctl()` — panthor-specific
  ioctls, not anything a `pan_kmod_kbase` backend can intercept just by
  implementing the vtable.
- The same file signals/waits on completion via libdrm's generic DRM
  syncobj calls (`drmSyncobjCreate`/`Wait`/`Reset`/`Transfer`/
  `TimelineWait`), also on `dev->drm_fd`. These are DRM-subsystem
  ioctls, not panthor-specific, but they require the fd to actually be
  a DRM fd — and kbase's `/dev/mali0` is a misc device (see above), so
  none of them work against it even in principle, not just as a matter
  of "wrong ioctl numbers."

**Practical consequence for `ROADMAP.md`:** a `pan_kmod_kbase.c`
backend (Phase 2) covers device probe + BO/VM management, but Phase 4
(submission + sync) needs a kbase-specific sibling to
`panvk_vX_gpu_queue.c` itself, not just a `pan_kmod_ops`
implementation — a materially bigger scope than "translate the ioctls
kmod calls." The CS group-create/register/bind/kick sequence this
repo's `tests/queue_group/queue_group.c` already exercises
successfully on-device (see `docs/kbase-notes.md`) is the raw material
for that sibling file's submission half; the sync half is still
unsolved (no DRM fd to hang a syncobj off of).

For kbase, several of these need real design decisions, not just ioctl
translation:

1. **Device probe** — solved by the enumeration fix above, then a
   straightforward "open + query GPU ID/props via kbase ioctl" call. This
   half is done at the standalone-probe level in
   `tests/first_test/first_test.c` + `utils/parse_gpu_props.h` — decodes
   GPU ID, shader core mask popcount, L2 slice popcount, and max clock
   from the raw `KBASE_IOCTL_GET_GPUPROPS` blob. What's left is wiring
   this into `pan_kmod_dev_props` once a `pan_kmod_kbase.c` backend
   exists (Phase 2 of `ROADMAP.md`).
2. **BO/VM management** — kbase has its own memory-region and JIT/
   tiler-heap growth model. Needs adapting, not just wrapping. BO
   create/free against real hardware is already prototyped (not through
   `pan_kmod_ops` yet) in `utils/memory.h` — see `docs/kbase-notes.md`
   for the SAME_VA free semantics found there.
3. **Submission + sync** — the hardest part, and bigger than originally
   scoped here (see the correction above): it's not a `pan_kmod_ops`
   translation, it's a kbase-specific fork of the Vulkan driver's own
   CSF queue file. kbase's atom/command-stream submission and completion
   signaling don't map onto DRM syncobjs at all, since kbase isn't a DRM
   device. This is where to expect the most iteration.

## JM vs CSF

Pick one and stay there for your first working version:

- **JM (Job Manager)** — older Bifrost/Midgard. Simpler submission model,
  but you'd be pairing it with PanVK code paths that are already the
  least mature.
- **CSF (Command Stream Frontend)** — Valhall v10+. `first_test.c`
  already builds with `-DMALI_USE_CSF=1` (see the root `makefile`), so
  this repo has implicitly committed to CSF — keep that consistent going
  into Phase 2 rather than trying to support both from day one.
