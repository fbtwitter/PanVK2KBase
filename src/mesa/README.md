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
make mesa-libdrm           # once: fetch real libdrm via Mesa's own wrap
make mesa-backend-sync     # copy the backend into the Mesa tree
make mesa-backend-check    # compile it for aarch64-android + check symbols
```

### What has actually been verified

- **Compiles to a real object file** (`-c`, not `-fsyntax-only`) for
  `aarch64-linux-android26`, against the **real** `pan_kmod.h` /
  `pan_kmod_backend.h` from the Mesa checkout, the **real** vendored kbase
  UAPI, and **real libdrm 2.4.133** (fetched via Mesa's own pinned wrap —
  the stub in `syntax-check-stubs/` is only a fallback when libdrm hasn't
  been downloaded). Clean, no warnings, against both `kbase-uapi-r49p1` and
  `kbase-uapi-r44p0`.
- **Integrates with the dispatch patch at link level.** With
  `pan_kmod.c.kbase.patch` applied, `pan_kmod.c` also compiles clean, and
  its two undefined kbase symbols (`kbase_kmod_ops`,
  `pan_kmod_fd_is_kbase`) resolve exactly against the ones
  `pan_kmod_kbase.c` defines — checked with `llvm-nm`.

### What has *not* been verified

**A full Mesa build has not been done, and is blocked on a real
dependency.** Modern Mesa requires LLVM to build any panfrost target:

```
meson.build:976: ERROR: Feature llvm cannot be disabled: CLC requires LLVM
```

`with_driver_using_cl` includes *both* `with_gallium_panfrost` and
`with_panfrost_vk`, so there's no panfrost configuration that avoids CLC.
The documented cross-build escape hatch, `-Dmesa-clc=system`, only moves the
problem — it then requires a prebuilt native `mesa_clc`:

```
meson.build:965: ERROR: Program 'mesa_clc' not found or not executable
```

Building `mesa_clc` natively needs LLVM + Clang **development libraries on
the build machine**. On this Windows host that's a substantial install, and
Mesa cross-building from a Windows host to Android is an unusual path
(Mesa's own docs assume a Linux host). A Linux build machine would make this
straightforward.

So the honest status: the backend compiles and links correctly against real
Mesa interfaces, and the ioctl sequences in it were verified on real
hardware by this repo's probes — but it has **never been built as part of
Mesa, loaded, or executed**.

An Android meson cross-file is checked in at `android-aarch64.cross` (meson
accepts it and finds the NDK toolchain — configure gets as far as the LLVM
error above, so the cross-file itself is good).
