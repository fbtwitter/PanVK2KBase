# `pan_kmod_kbase` — Mesa backend source

Phase 2 of `ROADMAP.md`: a third `pan_kmod` backend alongside `panfrost`
(DRM, JM) and `panthor` (DRM, CSF), targeting Arm's proprietary kbase
kernel driver.

## Why the source lives here and not in the Mesa tree

The Mesa checkout (`third_party/MESA-KMOD/`) is **gitignored** — it's a
~600MB clone that's local to whichever machine cloned it (see
`docs/mesa-cs-builder.md`). Anything written directly into it is not
tracked by this repo and would be lost on a fresh clone.

So the backend is developed here, under version control, and *synced into*
the Mesa tree:

```
make mesa-backend-sync     # copy backend in + show the dispatch patch
```

That copies `pan_kmod_kbase.c` into
`third_party/MESA-KMOD/src/panfrost/lib/kmod/` and reports what still has
to be applied by hand.

## Files

| File | Purpose |
|---|---|
| `pan_kmod_kbase.c` | The backend itself — implements `struct pan_kmod_ops`. |
| `pan_kmod.c.kbase.patch` | The dispatch change: `pan_kmod_dev_create()` must probe for kbase *before* calling `drmGetVersion()`, which fails on a misc device. Kept as a readable patch rather than auto-applied, since upstream `pan_kmod.c` moves. |
| `meson.build.kbase` | The `meson.build` hunk: adds the source file and the kbase UAPI include path. |

## The two structural problems this backend runs into

Both are documented at length in `docs/architecture.md`; briefly:

1. **kbase is not a DRM driver.** `pan_kmod_dev_create()` dispatches on the
   DRM driver name from `drmGetVersion()`. That call fails outright on
   `/dev/mali0`. Hence the dispatch patch. This mirrors how Turnip
   special-cases kgsl.

2. **`pan_kmod_ops` has no submission or sync entry.** Command-stream
   submission and fencing live in the Vulkan driver
   (`src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`), which calls
   `DRM_IOCTL_PANTHOR_*` and libdrm `drmSyncobj*` directly on a DRM fd.
   So finishing this backend gets device probe and BO/VM management
   working — it does **not** get submission working. That's separate,
   larger work, and is additionally blocked on the unsolved
   group-scheduling problem in `docs/kbase-notes.md`.

## Implementation status

Backed by ioctl sequences verified on real hardware (Poco X8 Pro,
Mali-G720, kbase r49p1) by this repo's standalone probes:

- `dev_create` / `dev_destroy` — `VERSION_CHECK` → `SET_FLAGS` →
  `GET_GPUPROPS`, decoding the property blob into `pan_kmod_dev_props`.
- `bo_alloc` / `bo_free` — `MEM_ALLOC` + `mmap`, and `munmap`-only free
  (for SAME_VA regions `munmap` *is* the free; a follow-up `MEM_FREE`
  returns `EINVAL`).

Deliberately **not** implemented, and returning explicit errors rather
than plausible-looking fakes:

- `bo_wait` — needs a working fence mechanism; none is known to work on
  kbase (see `docs/kbase-notes.md`).
- `bo_import` / `bo_export` — dma-buf. The common `pan_kmod_bo_import()`
  goes through `drmPrimeFDToHandle()` on `dev->fd`, so this needs changes
  above the backend too. Phase 3.
- `vm_create` / `vm_destroy` / `vm_bind` — kbase has no explicit VM
  object; a context owns one address space and allocations are mapped at
  `MEM_ALLOC` time, with no separate bind step. Mapping pan_kmod's
  explicit-VM model onto that needs a design decision, not a guess.

## Building

```
make mesa-backend-sync     # copy the backend into the Mesa tree
make mesa-backend-check    # syntax-check it against real headers
```

`mesa-backend-check` compiles the backend with `-fsyntax-only` against the
**real** `pan_kmod.h` / `pan_kmod_backend.h` from the Mesa checkout and the
**real** vendored kbase UAPI headers. Verified clean against both
`kbase-uapi-r49p1` and `kbase-uapi-r44p0`.

Two honest caveats about what that does and doesn't prove:

- It uses a minimal libdrm stub (`syntax-check-stubs/xf86drm.h`), because
  `pan_kmod.h` includes `<xf86drm.h>` unconditionally and libdrm is a meson
  wrap that a shallow Mesa clone doesn't fetch. The stub supplies only the
  two symbols `pan_kmod.h` references. A real build links real libdrm and
  never sees it.
- It is a syntax/type check, **not** a full Mesa build or link. Building for
  real needs a complete Mesa meson configure (Android cross-file, NDK
  toolchain), which is its own piece of setup and hasn't been done.

So: this compiles against the real interfaces, and the ioctl sequences in it
are ones verified on hardware — but it has never been linked into a running
Mesa, let alone exercised.
