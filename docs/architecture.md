# Architecture notes

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
vtable every backend implements — device open/close, BO alloc/free/mmap/
import/export, VM management, and submission. **Do not trust the exact
field names below** — verify against the actual header in the Mesa
checkout you're building against, since it evolves:

- device probe / properties query
- BO create / free / mmap / import (dma-buf) / export (dma-buf)
- VM (address space) create / map / unmap
- job/command-stream submission
- sync object wait / signal

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
   tiler-heap growth model. Needs adapting, not just wrapping.
3. **Submission + sync** — the hardest part. kbase's atom/job-chain
   submission and completion signaling don't map cleanly onto what
   PanVK's sync code (built around DRM sync objects / timelines) expects.
   This is where to expect the most iteration.

## JM vs CSF

Pick one and stay there for your first working version:

- **JM (Job Manager)** — older Bifrost/Midgard. Simpler submission model,
  but you'd be pairing it with PanVK code paths that are already the
  least mature.
- **CSF (Command Stream Frontend)** — Valhall v10+. `first_test.c`
  already builds with `-DMALI_USE_CSF=1` (see the root `makefile`), so
  this repo has implicitly committed to CSF — keep that consistent going
  into Phase 2 rather than trying to support both from day one.
