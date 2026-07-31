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
| `pan_kmod_kbase.h` | Declares `kbase_kmod_ops` and `pan_kmod_fd_is_kbase()`. Mirrors `panthor_kmod.h`. Not optional — Mesa builds with `-Werror=missing-prototypes`. |
| `pan_kmod.c.kbase.patch` | The dispatch change: `pan_kmod_dev_create()` must probe for kbase *before* calling `drmGetVersion()`, which fails on a misc device. Kept as a readable patch rather than auto-applied, since upstream `pan_kmod.c` moves. |
| `meson.build.kbase.patch` | The `meson.build` hunk: adds the source file, the kbase UAPI include path, `-DMALI_USE_CSF=1`, and the kconfig shim. |
| `patch-panvk-kbase-*.py` | Hand-run, idempotent patches to PanVK itself (not to `pan_kmod`). Scripts rather than diffs because upstream moves. `enumeration` finds the device, `queue` / `subqueue-init` / `sync` bring up submission, `external-memory` stops the driver claiming dma-buf sharing it cannot do. They share target files, so changing one means restoring its targets in `/opt/mesa-src` and re-running **all** of them. |
| `wsl-install-deps.sh` | Installs the Linux toolchain needed to build Mesa's panfrost targets. |
| `wsl-build.sh` | Syncs the backend in, applies both patches, configures and builds `libpankmod_lib`. |
| `android-aarch64.cross` | Meson cross-file for the eventual Android build. |

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
- `bo_import` — dma-buf in. kbase can do it (`MEM_IMPORT` /
  `BASE_MEM_IMPORT_TYPE_UMM`), but the common `pan_kmod_bo_import()` calls
  `drmPrimeFDToHandle()` on `dev->fd` before dispatching to the backend, so
  this hook is never reached. Needs a change above the backend.
- `bo_export` — dma-buf out. Not "not yet": kbase has no export path at
  all, and `pan_kmod_bo_export()` is a `static inline` that calls
  `drmPrimeHandleToFD()` itself. See ROADMAP Phase 3.
- `vm_create` / `vm_destroy` / `vm_bind` — kbase has no explicit VM
  object; a context owns one address space and allocations are mapped at
  `MEM_ALLOC` time, with no separate bind step. Mapping pan_kmod's
  explicit-VM model onto that needs a design decision, not a guess.

## `BELONGS-UPSTREAM`: work done here that shouldn't live here

Some of what this port does is standing in for something PanVK, `pan_kmod`
or the kernel ought to provide. Left unmarked, those stop-gaps quietly
become permanent — so each one carries a tag naming who should own it:

```
BELONGS-UPSTREAM(panvk):    should live in the Vulkan driver
BELONGS-UPSTREAM(pan_kmod): should live in the shared kmod layer
BELONGS-UPSTREAM(kernel):   needs something kbase does not expose
```

Find them all with:

```sh
grep -rn "BELONGS-UPSTREAM" src/
```

Each tag says what upstream should provide, and why it is done here
instead. When upstream grows the real thing, the tag is the delete list.

| where | owner | what |
|---|---|---|
| `pan_kmod_kbase.c` `bo_import` | `pan_kmod` | an fd-taking import entry point that dispatches to the backend before any DRM call |
| `pan_kmod_kbase.c` `bo_export` | `kernel` | kbase has no dma-buf export; nothing to build on |
| `panvk_vX_kbase_queue.c` render ringbuf | `panvk` | a ring discipline that does not need one BO at two adjacent VAs — asked upstream, see `docs/upstream-ringbuf-question.md` |
| external-memory capability gating | `panvk` | `panvk_physical_device.c` advertises dma-buf import/export unconditionally; it should ask the backend |


## Building

The backend **builds as part of Mesa**, verified on Linux. Windows can't do
it (see "Why Linux" below), so the workflow is WSL:

```bash
# in WSL, as root (wsl -u root - no sudo/password needed)
bash src/mesa/wsl-install-deps.sh    # toolchain + LLVM + libclc
bash src/mesa/wsl-build.sh           # sync, patch, configure, build
```

`wsl-build.sh` clones nothing — point `MESA` at a checkout, or clone into the
WSL *native* filesystem first (building on `/mnt/c` goes through the 9p
bridge and is dramatically slower for a tree this size).

There is also a lighter-weight check that doesn't need a Mesa build at all:

```
make mesa-libdrm && make mesa-backend-sync && make mesa-backend-check
```

which cross-compiles just the backend for `aarch64-linux-android26` and
verifies its symbols.

### Verified

**The backend is built by Mesa's own build system into the real library
target.** Mesa 26.3.0-devel (`7296f9a`), native x86_64 Linux, meson 1.11.2,
GCC 13.3, LLVM 18.1.3:

```
[1/3] Compiling C object src/panfrost/lib/kmod/libpankmod_lib.a.p/pan_kmod.c.o
[2/3] Compiling C object src/panfrost/lib/kmod/libpankmod_lib.a.p/pan_kmod_kbase.c.o
[3/3] Linking static target src/panfrost/lib/kmod/libpankmod_lib.a

--- archive members ---
libpankmod_lib.a.p/pan_kmod.c.o
libpankmod_lib.a.p/panfrost_kmod.c.o
libpankmod_lib.a.p/panthor_kmod.c.o
libpankmod_lib.a.p/pan_kmod_kbase.c.o     <-- ours, alongside the upstream three

--- kbase symbols ---
0000000000000000 D kbase_kmod_ops
0000000000000000 T pan_kmod_fd_is_kbase
```

So: it compiles under Mesa's full warning set (including
`-Werror=missing-prototypes`, `-Werror=incompatible-pointer-types`,
`-Werror=int-conversion`), links into `libpankmod_lib.a`, and the patched
`pan_kmod.c` resolves against it.

Separately, the backend also cross-compiles clean for
`aarch64-linux-android26` against both `kbase-uapi-r49p1` and
`kbase-uapi-r44p0`.

### Android cross-build — done

A full PanVK Android driver now builds with the kbase backend in it:

```bash
bash src/mesa/wsl-install-deps.sh      # toolchain, LLVM, libclc
bash src/mesa/wsl-fetch-ndk.sh         # Linux NDK r27c -> /opt/android-ndk
bash src/mesa/wsl-build-host-tools.sh  # mesa_clc + panfrost_compile
bash src/mesa/wsl-build-android.sh     # the driver itself
```

The host-tools step is not optional: a cross build can't run the aarch64
binaries it produces, so Mesa's shader compilers must exist as native
binaries for `-Dmesa-clc=system -Dprecomp-compiler=system` to consume.

Result — `libvulkan_panfrost.so`, 18.8 MB:

```
ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV), dynamically linked
NEEDED: libdrm.so libhardware.so liblog.so libnativewindow.so libsync.so
        libm.so libz.so libdl.so libc.so
kbase symbols: kbase_kmod_dev_create, kbase_kmod_bo_alloc, kbase_kmod_ops,
               pan_kmod_fd_is_kbase, ... (all present)
```

**It loads on the real device.** `make driver_load_probe` builds a probe
that `dlopen()`s the driver from `/data/local/tmp` — no `/vendor` changes,
no root, no risk to the running graphics stack. On the Poco X8 Pro:

```
OK: loaded, all NEEDED dependencies resolved
OK: HMI (Android HAL module entrypoint) present
  tag  = 0x48574d54 (HARDWARE_MODULE_TAG)
  id   = vulkan
  name = Mesa 3D Vulkan HAL
```

So every `NEEDED` dependency resolves on a stock device, and it exposes a
well-formed Android hwvulkan HAL module — the same interface the device's
own `vulkan.mali.so` uses (`ro.hardware.vulkan=mali`).

### Enumeration — done, and the backend now really runs

`patch-panvk-kbase-enumeration.py` adds a `physical_devices.enumerate` hook
to PanVK that opens `/dev/mali0` directly. Mesa's `vk_instance` calls that
hook *before* DRM enumeration and falls through to DRM if it returns
`VK_ERROR_INCOMPATIBLE_DRIVER` — so one binary still works on
panfrost/panthor hardware. (Turnip picks kgsl-or-DRM at build time; this is
strictly more flexible.)

**Confirmed on-device** with `make driver_enum_probe`, which drives the HAL
directly and calls `vkEnumeratePhysicalDevices`:

```
kbase: dev_create entered, uk 1.30
kbase: SET_FLAGS ok
kbase: props ok, gpu_id=0xc8700010 variant=0x4
Found compatible kbase device '/dev/mali0'.
```

That `gpu_id` matches exactly what `tests/first_test` reads from the
hardware. So `open("/dev/mali0")` → `VERSION_CHECK` → `SET_FLAGS` →
`GET_GPUPROPS` → decode into `pan_kmod_dev_props` all execute inside
`pan_kmod_kbase.c`. **The backend is live code now, not a stub.**

#### The once-per-fd VERSION_CHECK rule

Getting there required a non-obvious fix. `KBASE_IOCTL_VERSION_CHECK` may
be issued **exactly once per fd** — a second call returns `-EPERM`, even
though the handshake it performed remains in effect (`SET_FLAGS` afterwards
still succeeds). `tests/double_handshake_probe/` demonstrates this.

That broke the path twice, in different places:

1. `kbase_kmod_dev_create()` originally re-ran the handshake. Fixed by
   taking the UK version from `drv_info`, which is exactly why
   `pan_kmod_fd_is_kbase()` reports it through out-params.
2. More subtly, the first version of the PanVK patch *also* probed with
   `pan_kmod_fd_is_kbase()` before calling `pan_kmod_dev_create()` — making
   the dispatch's own probe the second call. It returned `false`, so the
   dispatch concluded this wasn't kbase and fell through to
   `drmGetVersion()`, which fails on a misc device. The entire kbase path
   silently never ran, with no error logged anywhere.

Lesson worth keeping: only one place may probe, and it must be the
dispatch.

### Not verified

- **The driver still cannot drive the GPU.** Enumeration now reaches the
  backend, but device creation fails immediately after, at exactly the
  point `docs/architecture.md` predicted:

  ```c
  device->drm_syncobj_type = vk_drm_syncobj_get_type(device->kmod.dev->fd);
  if (!device->drm_syncobj_type.features)
     return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED, ...);
  ```

  `vk_drm_syncobj_get_type()` needs a real DRM fd; `/dev/mali0` is a misc
  device, so it fails and `vkEnumeratePhysicalDevices` returns `-3`
  (`VK_ERROR_INITIALIZATION_FAILED`). PanVK's sync model is DRM-syncobj
  based from the ground up, so this needs a `vk_sync` implementation backed
  by whatever kbase offers — and *that* is still blocked on the unsolved
  "queue group never gets scheduled" problem in `docs/kbase-notes.md`.
- **Submission is untouched.** `panvk_vX_gpu_queue.c` still issues
  `DRM_IOCTL_PANTHOR_*` directly.
- **BO/VM ops still never execute.** Initialisation fails before any
  allocation happens.
- **Not installed as the system driver.** Replacing
  `/vendor/lib64/hw/vulkan.mali.so` needs a writable `/vendor` (root) and
  would break the device's graphics if the driver misbehaves. Deliberately
  not attempted.

### Why Linux (and not Windows)

Any panfrost target pulls in CLC, which requires LLVM:

```
meson.build:976: ERROR: Feature llvm cannot be disabled: CLC requires LLVM
```

`with_driver_using_cl` covers *both* `with_gallium_panfrost` and
`with_panfrost_vk`, so no panfrost configuration avoids it. The documented
cross-build escape hatch `-Dmesa-clc=system` only relocates the problem — it
then wants a prebuilt native `mesa_clc`:

```
meson.build:965: ERROR: Program 'mesa_clc' not found or not executable
```

On Linux this is just `apt install llvm-dev libclc-18-dev` (note: the libclc
version must match the LLVM version — `libclc-18-dev` for LLVM 18). On
Windows it's a substantial LLVM build, and Mesa cross-building from a
Windows host is an off-the-beaten-path setup its own docs don't cover.

Ubuntu 24.04 caveat: its meson is 1.3.2, older than Mesa's `>= 1.4.0`
requirement, so `wsl-install-deps.sh` puts a newer meson in a venv at
`/opt/mesa-venv` rather than fighting the system package.
