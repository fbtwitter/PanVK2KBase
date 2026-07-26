# kbase research notes

## Header provenance — already real, keep it that way

`third_party/kbase-uapi-r44p0/` is a **real** vendored UAPI header, not a
stub — see that directory's `README.md` for exact source/commit. It's
pinned to CSF UK interface **1.20** (`BASE_UK_VERSION_MAJOR`/`_MINOR` in
`csf/mali_kbase_csf_ioctl.h`) for a Mali-G615-MC2 target — earlier
versions of this doc said "UK interface 44.10", which was wrong: that
number conflated the r44p0 driver *release name* with the CSF UK
*interface* version, which are two different numbers that don't move
together (see below for how this was caught). If you retarget this repo
to a different device/kernel, re-vendor the matching header rather than
hand-editing this one — wrong ioctl numbers/struct layouts against a real
kernel can silently corrupt memory or hang the GPU instead of just
failing loudly.

A second target, `third_party/kbase-uapi-r49p1/`, is also vendored — see
below for provenance and why it exists.

- Find the kbase kernel driver source that matches what's actually
  running on your target device (check kernel version + vendor + Mali
  driver version reported in `dmesg` or `/sys/kernel/debug` if available
  as a starting point).
- The UAPI header is typically named something like
  `mali_kbase_ioctl.h` inside the vendor kernel's
  `drivers/gpu/arm/midgard/` (JM) or `drivers/gpu/arm/bifrost/` or
  `.../valhall` (CSF) tree, depending on vendor/version.
- If your device's kernel source isn't published (common on Android
  vendor kernels, depending on GPL compliance practices), you may need to
  request it from the OEM/SoC vendor, or find a close enough kernel
  version from a sibling device using the same SoC family — but verify
  ioctl numbers match rather than assuming.

## JM vs CSF, concretely

Check which one you have before assuming:
- JM kbase: older API, "job slots", associated with Midgard/Bifrost GPUs.
- CSF kbase: "command stream group" model, associated with Valhall v10+.

This repo builds with `MALIFLAGS := -DMALI_USE_CSF=1` (root `makefile`),
so it's already committed to CSF. These are different enough that code
written against one will not compile-swap to the other — if you ever
need JM, that's effectively a separate branch, not a flag flip.

## Verified so far (via `tests/first_test/first_test.c`)

- [x] `KBASE_IOCTL_VERSION_CHECK` round-trips.
- [x] `KBASE_IOCTL_SET_FLAGS` round-trips.
- [x] `KBASE_IOCTL_GET_GPUPROPS` two-step (size probe, then buffer
      fetch) round-trips and decodes to sane-looking values via
      `parse_gpuprops()` in `utils/parse_gpu_props.h`.

## Things worth checking early, not late

- [ ] Does your kernel's kbase expose the ioctls you'll need for
      dma-buf import/export, or only its own private memory model? This
      affects how early you can get WSI working later.
- [ ] What's the actual completion/fence signaling mechanism kbase
      exposes for a submitted atom? (poll on an fd? a separate "wait"
      ioctl? something else?) This directly drives Phase 4 of the
      roadmap and is worth confirming before you design your sync shim.
- [ ] Does your device's kbase build support explicit unmapping /
      partial VM operations, or only whole-region operations? Affects
      how closely you can mirror panthor's VM semantics.
- [x] Confirm the decoded GPU ID / model from `parse_gpu_props.h` against
      what the vendor blob driver reports for the same device — done for
      the Poco X8 Pro, see below. `Mali-TTIX`/G720 decode confirmed
      against the vendor userspace driver blob's own strings, not just
      the ioctl round-tripping.

## Second device tested: Poco X8 Pro (mt6899 / Dimensity 8500 Ultra, Mali-G720)

Cross-compiled `first_test.c` with the Android NDK (`aarch64-linux-android26`
target, same `-DMALI_USE_CSF=1` flag as the makefile) and ran it via `adb
shell` on a Poco X8 Pro — no code changes. `/dev/mali0` was world-RW
(`crw-rw-rw-`), so this worked as the unprivileged `shell` user, no root
needed.

Result: full success. `VERSION_CHECK`, `SET_FLAGS`, and `GET_GPUPROPS` all
round-tripped, and `parse_gpuprops()` correctly decoded the GPU as
**Mali-TTIX** (Mali-G720's internal codename) — architecture 12.8, revision
7, 8 shader cores, 1 L2 slice, max clock 1508 MHz, GPU ID `0xc8700010`. This
is a real answer to the Phase 1 checklist item above ("confirm the decoded
GPU ID/model... for this specific chip") on a second, unrelated device — the
`gpu_model()` arch/product table isn't just decoding the one Mali-G615
target correctly by luck.

**Version mismatch, since resolved:** this device reported kbase UK
interface `major=1, minor=30` via `KBASE_IOCTL_VERSION_CHECK`, which didn't
match the `1.20` this repo's r44p0 headers are pinned to (see the doc-bug
note above — the header comparison here is also what caught that this
repo's docs previously said "44.10", which was simply wrong). Tracked the
real driver down: MediaTek's own `mali_avalon` kbase source tree
(`Mayuri-Chan/MTK_kernel_device_modules_6.6`, a vendored copy used by
several mt6899/Dimensity-8xxx LineageOS device trees) ships driver release
**r49p1**, whose `csf/mali_kbase_csf_ioctl.h` defines
`BASE_UK_VERSION_MAJOR=1`, `BASE_UK_VERSION_MINOR=30` — an exact match.
That header set is now vendored at `third_party/kbase-uapi-r49p1/` (see
its `README.md` for full provenance: repo, branch, commit, original path).
Same 18-file layout as `kbase-uapi-r44p0/`, so swapping targets is just
changing `KBASE_UAPI_DIR` in the root `makefile`.

Practical takeaway: r44p0/UK-1.20 and r49p1/UK-1.30 are close enough that
this repo's Phase 1 probe (version check, flags, two-step gpuprops
fetch+decode) round-trips fine against either. That is **not** evidence the
two are ABI-compatible in general — it only exercises a handful of fields.
Anything past Phase 1 (BO/VM management, submission) should target
whichever of the two vendored header sets actually matches the device you're
building for, not assume the probe succeeding means they're interchangeable.

**Vendor blob driver diff (closes the "confirm against vendor blob" item
above):** pulled the actual userspace Mali driver off the Poco X8 Pro via
`adb pull` — `/vendor/lib64/hw/mt6899/vulkan.mali.so` turned out to be a
thin loader stub (`mali_vk_vendor_stub`, ~130KB) that just dlopens the real
implementation; the actual GLES+Vulkan driver is
`/vendor/lib64/egl/mt6899/libGLES_mali.so` (~52MB). Extracted printable
strings from it (no root needed — both files are world-readable) and
found:

- A leaked build-path string —
  `vendor/mediatek/proprietary/hardware/gpu_mali/mali_avalon/r49p1/product/gles/src/program/mali_gles2_program_shader_api.c`
  — literally `mali_avalon/r49p1`. It's the only `rXXpY`-shaped string
  anywhere in the binary, so no ambiguity between candidate releases.
- `Mali-G720` present in the driver's supported-chip name table (alongside
  G610/G615/G625/G710/G715/G725/G620/G510/G310 — a shared multi-chip
  build).

This independently confirms both halves of the earlier version-tracking
work from the vendor's own driver, not just from kbase's `VERSION_CHECK`
ioctl: driver release **r49p1**, GPU **Mali-G720** (`Mali-TTIX`). Three
independent signals now agree (kbase UK version, this repo's own
`parse_gpuprops()` decode, and the vendor blob's own strings) — about as
solid as this gets without MediaTek's actual private source tree.

## Where to ask

The `#panfrost` channel (Matrix, bridged to OFTC IRC) is where Panfrost/
PanVK/Panthor upstream discussion happens. Worth lurking before you start
and posting once Phase 1 (standalone probe — already working here) is
solid — see Phase 9 in `ROADMAP.md` for why raising it early matters.
