# kbase research notes

## Header provenance — already real, keep it that way

`third_party/kbase-uapi-r44p0/` is a **real** vendored UAPI header, not a
stub — see that directory's `README.md` for exact source/commit. It's
pinned to UK interface 44.10 for a Mali-G615-MC2 target. If you retarget
this repo to a different device/kernel, re-vendor the matching header
rather than hand-editing this one — wrong ioctl numbers/struct layouts
against a real kernel can silently corrupt memory or hang the GPU instead
of just failing loudly.

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
- [ ] Confirm the decoded GPU ID / model from `parse_gpu_props.h` against
      what the vendor blob driver reports for the same device — the
      ioctl round-tripping successfully doesn't by itself prove the
      decode table (`gpu_model()`'s arch/product table) is being read
      correctly for this specific chip.

## Where to ask

The `#panfrost` channel (Matrix, bridged to OFTC IRC) is where Panfrost/
PanVK/Panthor upstream discussion happens. Worth lurking before you start
and posting once Phase 1 (standalone probe — already working here) is
solid — see Phase 9 in `ROADMAP.md` for why raising it early matters.
