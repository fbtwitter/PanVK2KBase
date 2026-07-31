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

**Writing code that targets "any vendored version", not just these
two:** the build isn't hardcoded to r44p0/r49p1 — `KBASE_VERSION` in the
root `makefile` accepts any `third_party/kbase-uapi-<name>/` directory
(`make list-kbase-versions` lists what's actually vendored). To keep
source files buildable against whichever version is selected:

- If an ioctl/struct/flag isn't guaranteed present in every vendored
  version (like `KBASE_IOCTL_INTERNAL_FENCE_WAIT`, r49p1-only), guard
  its use with `#ifdef SYMBOL_NAME` rather than assuming it exists —
  see `tests/fence_probe/fence_probe.c` for the pattern: the ioctl-only
  parts are compiled in when the header declares them, and skipped
  (with a clear runtime message) otherwise, so the *same source*
  compiles and runs correctly against r44p0, r49p1, or a future version
  that hasn't been vendored yet.
- For a field *added to an already-present struct* at a specific UK
  version bump (e.g. `cs_fault_report_enable` added at UK 1.22, see the
  changelog comments atop `csf/mali_kbase_csf_ioctl.h`), prefer `#if
  BASE_UK_VERSION_MINOR >= N` over an ioctl-name check — every vendored
  version defines `BASE_UK_VERSION_MAJOR`/`_MINOR` unconditionally, so
  this works even when there's no separate symbol to `#ifdef` on.
- What this can't automate: a vendor sometimes ships an *incomplete*
  drop (r49p1 was missing `mali_gpu_props.h` until this repo vendored it
  separately — see below). Adding a genuinely new version still needs a
  one-time check that all its `#include`s actually resolve within the
  vendored directory; the `#ifdef`/`#if BASE_UK_VERSION_MINOR` patterns
  above only handle *feature* differences between complete header sets,
  not missing files.

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

**JM vs CSF, confirmed for real (closes the ROADMAP Phase 0 item):**
`first_test.c` builds with `-DMALI_USE_CSF=1`, and this device backs that
assumption up three ways, not just "the probe didn't error":

- `/vendor/firmware/mali_csffw.bin` exists on-device (300KB). This file is
  CSF-architecture-specific — it's firmware uploaded to the GPU's own
  command-stream-frontend microcontroller at driver init. JM-generation
  kbase has no equivalent; there's nothing to upload.
- The two UK-version numbering schemes in the vendored r49p1 headers are
  worlds apart — CSF: `BASE_UK_VERSION_MAJOR=1` (`csf/mali_kbase_csf_ioctl.h`),
  JM: `BASE_UK_VERSION_MAJOR=11` (`jm/mali_kbase_jm_ioctl.h`). The device
  reported `major=1` — squarely CSF's scheme, not JM's.
- Mali-G720 is Valhall 5th-gen (the `TTIx`/`Mali-TTIX` family decoded
  earlier) — architecturally CSF-only regardless of driver config; Arm
  hasn't shipped a JM variant of any Gen5+ Mali design.

`dmesg` and `/proc/device-tree` were both inaccessible to the unprivileged
`shell` user on this device (empty output, not an error) — worth knowing
if you try to confirm this on another device and those paths are your
first instinct; the firmware-blob check doesn't need either.

## r44p0 vs r49p1: what actually changed for submission (Phase 4 prep)

`queue_group.c` (root repo, merged into this branch) exercises
`KBASE_IOCTL_CS_QUEUE_GROUP_CREATE`, `_QUEUE_REGISTER`, `_QUEUE_BIND`, and
`_QUEUE_KICK` against `third_party/kbase-uapi-r44p0`, but the confirmed
header for the actual tested device (Poco X8 Pro, see above) is r49p1.
Diffed the two header sets for exactly the structs/ioctls that code
touches, since "the probe round-trips" isn't evidence past Phase 1 (see
the "Practical takeaway" note above):

- `union kbase_ioctl_cs_queue_group_create` — the only struct that
  changed shape. r44p0 has `__u16 reserved`; r49p1 splits it into
  `__u8 reserved` + `__u8 cs_fault_report_enable` (new in UK 1.22, CS
  fault reporting). Same total size, same offset for every field
  `queue_group.c` actually sets (`tiler_mask`, `fragment_mask`,
  `compute_mask`, `cs_min`, `priority`, `*_max`) — the code
  zero-initializes with `= {0}` and never touches `reserved`, so this is
  a safe swap, not a silent ABI break.
- `_QUEUE_REGISTER`, `_QUEUE_BIND`, `_QUEUE_KICK` structs and ioctl
  numbers (37/39, register unnumbered but unchanged): byte-for-byte
  identical between the two header sets.
- `KBASE_IOCTL_MEM_ALLOC` and the `BASE_MEM_PROT_*`/`SAME_VA` flags
  `memory.h`/`flags_helper.h` use: unchanged.

Conclusion: rebuilding `queue_group`/`memory2` against r49p1 should be a
pure `KBASE_VERSION=r49p1` makefile-variable swap (now supported — see
root `makefile`), not a code change. Still needs an actual on-device run
to confirm — this diff only proves the header shapes match, not that the
ioctls behave identically on r49p1's kernel.

**New in r49p1, not in r44p0:** `KBASE_IOCTL_INTERNAL_FENCE_WAIT` (ioctl
80, gated behind `CONFIG_MALI_MTK_FENCE_DEBUG` — a MediaTek vendor
addition, consistent with r49p1's `mali_avalon`/MTK provenance noted
above). This waits on an "internal fence" given a `pid`/`queue` pointer
and a microsecond timeout. Directly relevant to the still-open "what's
the actual completion/fence signaling mechanism kbase exposes for a
submitted atom?" question below — worth checking whether this ioctl (or
its absence on non-MTK kbase forks) is the answer before designing
Phase 4's fence-translation shim around it.

## r49p1 build fixes, and the first on-device submission round-trip

Building `memory`/`memory2`/`queue_group` against r49p1 (NDK
`aarch64-linux-android26-clang`, `ndk;29.0.14206865`) surfaced two real
gaps in the vendored header set — both fixed without touching the
semantic content (ioctl numbers/struct layouts) of any vendored file:

- `mali_base_kernel.h` in r49p1 added `#include "mali_gpu_props.h"`
  (r44p0 doesn't have this include), but that file was never vendored.
  Pulled it from the same repo/branch/commit as the rest of
  `kbase-uapi-r49p1` (see that directory's `README.md`) — it's now a
  19th file there.
- `csf/mali_base_csf_kernel.h` guards MediaTek debug-dump-only fields
  with `IS_ENABLED(CONFIG_MALI_MTK_DEBUG_DUMP)`, assuming a real kernel
  build tree defines `IS_ENABLED` via `<linux/kconfig.h>`. A standalone
  probe has neither. Added `src/utils/kconfig_shim.h` (pulled in via
  `-include` in the root `makefile`, harmless for r44p0) that defines
  `IS_ENABLED(x)` as `0` — correct here since those fields are unused by
  any test in this repo and "not enabled" is the same state a normal
  (non-MTK-debug) kernel build would produce.

Separately, `src/utils/initialize.h` and `src/utils/memory.h` were
missing `<fcntl.h>`/`<string.h>` includes for `open()`/`O_RDWR` and
`memset()` respectively — worked before only because `first_test.c`
happened to include those headers first; broke under NDK clang (which
doesn't allow implicit function declarations) once `queue_group.c`/
`memory2.c` included them without that accidental ordering. Fixed by
adding the includes directly to the headers that use them.

With those fixed, ran all four probes on the Poco X8 Pro against
r49p1 (`/data/local/tmp`, unprivileged `shell` user, no root):

- `first_test`: unchanged from earlier r44p0 run — `VERSION_CHECK`
  reports `major=1 minor=30` (matches r49p1's pinned UK version exactly,
  vs. r44p0 which is off by one minor version from what the device
  actually reports).
- `memory`: `KBASE_IOCTL_MEM_ALLOC` + `mmap()` round-trip; decoded
  output flags include `SAME_VA` even though the input flags leave it
  commented out (kernel-added default — not yet understood, noted in
  `ROADMAP.md` Phase 3).
- `queue_group`: **first confirmed submission-chain round-trip.**
  `CS_QUEUE_GROUP_CREATE` → BO alloc → `CS_QUEUE_REGISTER` →
  `CS_QUEUE_BIND` → `CS_QUEUE_KICK` all returned `ret=0`, no error path
  taken. This is a real result, but a narrow one: it confirms the
  syscalls succeed, not that the GPU executed or completed anything —
  the probe never builds an actual command stream (just writes sentinel
  words) and never maps or reads the doorbell/ring-buffer region from
  `bind.out.mmap_handle` (left commented out in the source). Confirming
  real GPU-side completion is still open — see the fence-mechanism note
  above.

## SAME_VA free semantics and the gpu_va "cookie" (found via `kbase_bo_free`)

Added `kbase_bo_free()` (`utils/memory.h`) and wired it into
`memory2.c`/`queue_group.c`. Two on-device surprises worth recording
since they'd otherwise cause confusing failures later:

- **`munmap()` is the free, not `KBASE_IOCTL_MEM_FREE`.** Every
  allocation from `kbase_bo_create()` comes back with `SAME_VA` set in
  the decoded output flags, even though the input flags leave it
  commented out — this device's kbase defaults to it for this flag
  combination. For SAME_VA regions the GPU allocation is tied 1:1 to the
  CPU VMA: `munmap()` tears it down on `vm_close`, and a follow-up
  `MEM_FREE` call fails `EINVAL` because the region is already gone.
  Confirmed by trying both orders on-device. `kbase_bo_free()` now only
  calls `munmap()`.
- **The `gpu_va` alloc returns is a reusable cookie, not a stable
  address.** Allocating 4 buffers back-to-back in `memory2.c` (no frees
  in between) returned `gpu_va = 0x41000` for *all four* — only the
  `mmap()`'d CPU addresses differed. This is expected SAME_VA cookie
  behavior (the cookie is retired/becomes reusable once its `mmap()`
  resolves it to a real GPU VA), but it means `bo->gpu_va` cannot be used
  to distinguish or look up a specific live allocation across a run —
  only `bo->cpu` (the CPU-side pointer) is a unique handle post-mmap.

`queue_group.c`'s teardown order also had to be fixed for the same
reason: `queue_bo` is referenced by the queue registration (`REGISTER`'s
`buffer_gpu_addr`) and the group (`BIND`), so freeing it before
`CS_QUEUE_TERMINATE` → `CS_QUEUE_GROUP_TERMINATE` also failed `EINVAL`.
Correct order, confirmed clean on-device: unmap doorbell → terminate
queue → terminate group → free BO.

## KBASE_IOCTL_INTERNAL_FENCE_WAIT: reachable, but NOT the completion mechanism

`tests/fence_probe/fence_probe.c` calls r49p1's MediaTek-only
`KBASE_IOCTL_INTERNAL_FENCE_WAIT` (ioctl 80, documented as gated behind
`CONFIG_MALI_MTK_FENCE_DEBUG`) with an all-zero
`{pid, flags, time_in_microseconds=1000, queue}` struct, to check
whether the *running kernel* actually implements it (the header always
declares it regardless of kernel config).

Result on-device: `ret=0, errno=0` — success, not `ENOTTY`. This
**confirms the kernel does implement the ioctl** (the MTK fence-debug
code path is built into this device's kernel, not just declared in the
header). That's as far as this result can honestly be pushed, though:
succeeding on an all-zero `pid=0`/`queue=0` doesn't distinguish "this is
a real wait mechanism and there was trivially nothing to wait for" from
"this is a no-op/validation-only path for degenerate input." It does
NOT yet confirm this is a usable completion signal for Phase 4's
fence-translation shim.

**Follow-up done, and it's a negative result.** Extended
`fence_probe.c` to set up a real queue group + bound CS queue (same
shape as `queue_group.c`) and called `INTERNAL_FENCE_WAIT` with the
queue's real GPU VA, the calling process's real `pid`, a 2-second
`time_in_microseconds` timeout, and every documented flag combination
(`BASE_INTERNAL_FENCE_WAIT_IDLE_FLAG`, `_RESULT_FLAG`, `_DUMP_FLAG`) —
both before and after `CS_QUEUE_KICK`. Timed each call with
`clock_gettime()` to distinguish "actually blocked" from "returned
instantly."

Result: **every single variant returned in ~0.0ms**, `ret=0`/`errno=0`,
regardless of real vs. zeroed input, bound vs. unbound, kicked vs. not,
or which flag was set. A 2-second requested timeout that never once
measurably blocks is strong evidence this ioctl does not function as a
general-purpose "wait for this queue to reach some state" primitive —
at minimum not for the args this repo's ioctl sequence produces. Most
likely explanation given the name and `CONFIG_MALI_MTK_DEBUG_DUMP`/
`_FENCE_DEBUG` gating: this is a kernel-internal diagnostic hook (e.g.
for MTK's own driver-side fence-timeout dump tooling), not a
userspace-facing completion-wait API — "internal" in the struct/ioctl
name should have been a bigger hint from the start.

**Conclusion: don't build Phase 4's fence shim on this ioctl.** The
more standard mainline-kbase primitive to try next is `poll()`/`read()`
on the kbase device fd itself for CS event notifications, paired with
`KBASE_IOCTL_CS_EVENT_SIGNAL` (`csf/mali_kbase_csf_ioctl.h`, ioctl 44,
present in both r44p0 and r49p1 — not an MTK-only addition) and
`KBASE_IOCTL_CS_GET_GLB_IFACE` for the global command-stream interface.
This matches how mainline kbase CSF is actually documented to notify
userspace of queue/group events elsewhere (Arm's own kbase driver
design), unlike `INTERNAL_FENCE_WAIT` which is MTK-only and now shown
empirically not to block. Not yet tried against this device.

## poll()/read() on the kbase fd: mechanism untested, kick likely inert

> **RETIRED — this section's conclusion is superseded.** `poll()` on the
> kbase fd works fine and delivers a real `base_csf_notification`
> (type 0, `BASE_CSF_NOTIFICATION_EVENT`); see "Finding 2" near the end of
> this file. Two separate bugs made it look inert here: `CS_INSERT` was
> being written to the wrong page, and the command stream never signalled
> an event slot, so firmware had no reason to notify anyone. Kept for the
> method, not the conclusion.

`tests/event_probe/event_probe.c` follows up on the redirect above:
`poll(fd, POLLIN, timeout)` at three points (before any group/queue
setup, bound but not kicked, and after `CS_QUEUE_KICK`), reading a
`struct base_csf_notification` (`csf/mali_base_csf_kernel.h`) whenever
`poll()` reports readable. Also a real-world exercise of the
version-adaptive pattern: `BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FAULT` and
its `fault_queue` payload only exist in r49p1 (added after r44p0's UK
1.20), so that decode path is behind `#ifdef
BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FAULT` — confirmed building clean
against both header sets.

Result on-device: `poll()` **never** returned readable, not even 2
seconds after `KICK`, despite the queue buffer being filled with
`0xdeadbeef` (garbage that should read as an invalid instruction to any
real CS interpreter).

**Most likely explanation, not yet confirmed:** this repo's `KICK` never
touches the queue's mmap'd input/output pages (`queue_state`, from
`CS_QUEUE_BIND`'s `mmap_handle`) — only the raw ring-buffer BO. Real CSF
queues are expected to need the *insert* offset in that input page
updated to tell firmware how much of the ring buffer is valid pending
work; without that, `KICK` plausibly never registers as real work to
the firmware at all, so there's nothing to fault on and nothing to
notify. This is consistent with, not contradictory to, the caveat
already noted for `queue_group.c`: `ret=0` from `KICK` was always only
proof the *ioctl* succeeded, never proof the GPU executed anything.

**Where this investigation stops for now:** confirming poll()/read() as
a real mechanism needs an actual minimal CS instruction stream written
through the correct insert-pointer protocol - that requires CSF ISA
knowledge (instruction encoding for the command-stream frontend) this
repo hasn't built up yet, and arguably belongs to Phase 4's "map
VkQueueSubmit onto kbase command-stream submission" work directly
rather than a quick fence-mechanism probe. Neither confirmed nor ruled
out; unlike `INTERNAL_FENCE_WAIT`, there's no evidence against this
being the right mechanism, only an inconclusive "we haven't given the
firmware real work yet" result.

**CSF ISA docs located — path forward exists, not yet taken.** The
"this repo hasn't built up CSF ISA knowledge" gap above has a real
answer, already on disk: `third_party/MESA-KMOD/src/panfrost/genxml/`
(the Mesa clone, see `docs/architecture.md`) has the actual instruction
set as genxml — `v12.xml` matches this device's architecture (12.8,
Mali-G720/Mali-TTIX, confirmed in `docs/kbase-notes.md`'s device
section above). Real opcode table: 64-bit instructions, 8-bit opcode in
the top byte (`start="56"`), e.g. `MOVE48=1`, `MOVE32=2`, `WAIT=3`,
`FINISH_TILING=9`, `SYNC_WAIT32=39`, `SYNC_WAIT64=53`. Mesa's own
encoder, `genxml/cs_builder.h` (3131 lines), builds real instruction
streams from this table.

**Two ways to actually use this, with different risk profiles.** Chose
option 1:
1. **Link against `cs_builder.h` directly** — correct-by-construction
   encoding. Done: see `docs/mesa-cs-builder.md` for the full setup
   (Mesa clone, genxml codegen, the two util `.c` files and two
   platform macros actually needed, why `--gc-sections` matters).
   `tests/cs_encode_probe/cs_encode_probe.c` confirms it produces
   correct bytes offline (no device access) — a `MOVE32` instruction
   encodes to `0x0200000000001234` for immediate `0x1234`, `0x02`
   matching `v12.xml`'s opcode table exactly.
2. Hand-encode one minimal instruction word directly from `v12.xml`,
   no Mesa build dependency — not taken, in favor of option 1's
   correctness guarantee.

## Live KICK with a real instruction: submitted cleanly, never executed

`tests/live_kick_probe/live_kick_probe.c` does the real thing:
encodes a `MOVE32` with `cs_builder.h` straight into a bound queue's
ring buffer, writes `CS_INSERT` in the mmap'd user input page, and
calls `CS_QUEUE_KICK` for real. Run on-device (Poco X8 Pro, r49p1).

**Protocol details worth keeping** (all verified against the kernel
driver source, see `utils/csf_user_regs.h` for provenance):

- `CS_INSERT` is a **byte offset** into the ring buffer, not an address.
  Confirmed from the kernel's own diagnostic print, which reports ring
  buffer base, insert, and extract as three separate values with
  insert/extract starting at 0 while base is a real address.
- The ring buffer's GPU address is the **CPU pointer** (SAME_VA), *not*
  `bo->gpu_va` — that's the reusable cookie (`0x41000` for every
  allocation, see the SAME_VA section above). An early version of this
  probe passed the cookie to `cs_builder`'s `cs_buffer.gpu`; harmless
  for a single chunk that emits no link instructions, but wrong, and it
  would corrupt any multi-chunk stream.
- A single 64-bit store satisfies the kernel's "CS_INSERT should be
  accessed atomically" requirement on aarch64 (one `STR`).
- Userspace does **not** need to ring the hardware doorbell itself for
  an already-bound queue. `kbase_csf_queue_kick()` only flags the queue
  and wakes the scheduler kthread; the kernel rings the real MMIO
  doorbell later on our behalf.

**Result: the submission is accepted, but the GPU never runs it.**
`KICK` returns 0, and then `CS_EXTRACT` stays `0` (never advances to
the expected `8`) and `CS_ACTIVE` stays `0` for the full 2s wait. No
`poll()` notification either. **No GPU hang** — `first_test` and
`queue_group` both still run clean afterward, and the device needed no
reset. Ruled out: the CS interface index (`csi_index` 0 and 1 give
identical results).

**Where it actually stops, from the kernel source.** The CS is only
programmed and started when the scheduler places the group on a **CSG
slot** — that's `onslot_csg_add_new_queue()`, which writes `CS_BASE`/
`CS_SIZE`, does the `CS_REQ.STATE=START` handshake, and rings the
kernel doorbell. But in `kbase_csf_scheduler_queue_start()` that call
is guarded by `kbasep_csf_scheduler_group_is_on_slot_locked(group)`,
which is false for a freshly created group. The preceding
`scheduler_group_schedule()` returns 0 *unconditionally* — it merely
inserts the group into the runnable list and calls `scheduler_wakeup()`.
So a successful `KICK` guarantees nothing about execution; actual slot
assignment is deferred to a scheduler tick, and here it evidently never
completes. Note this also explains the earlier `event_probe.c` result:
if the group never reaches a slot, `queue->user_io_gpu_va` stays 0
(kernel comment: "only mapped when scheduler decides to put the queue
on slot at runtime"), so firmware never even sees these pages — which
is exactly consistent with `CS_ACTIVE` never leaving 0.

**Hypotheses tested and ruled out** (via the config sweep now built into
`live_kick_probe.c`, which runs several group shapes in one pass and
reports whether `CS_EXTRACT` ever advances):

- *CS interface index* — `csi_index` 0 and 1 behave identically.
- *Endpoint masks* — the first version passed `~0ULL` for tiler/fragment/
  compute masks. The kernel only validates `*_max <= hweight64(*_mask)`,
  then hands the mask to firmware as `CSG_ALLOW_*`, so asking for 64
  shader cores on an 8-core GPU looked like a plausible cause. It isn't:
  the real mask (`RAW_SHADER_PRESENT = 0x550055`), a compute-only group
  with no tiler/fragment endpoints, and the original `~0ULL` all fail
  identically. (`parse_gpu_props.h` gained `gpuprops_lookup()` /
  `kbase_get_shader_present()` to query the real mask rather than
  hardcoding it.)
- *Ring buffer validity* — 4096 bytes is exactly `CS_RING_BUFFER_MIN_SIZE`,
  is a power of two, and page-aligned; `CS_QUEUE_REGISTER` accepts it and
  the kernel's region checks (native type, not shrinkable, big enough)
  all pass.
- *Firmware capacity* — `tests/glb_iface_probe/glb_iface_probe.c` queries
  `KBASE_IOCTL_CS_GET_GLB_IFACE` (read-only, no submission). The firmware
  reports **glb_version 3.6.0, 8 CSG slots, 8 streams per slot (64
  total), 27520-byte suspend buffer per group, stream features
  `0x0007107f`**. So there is no shortage of slots or streams, and the
  interface version is far newer than anything kbase gates group
  scheduling on. `iface_has_enough_streams(cs_min=1)` clearly passes too.

### What the vendor blob does that this repo doesn't (RE findings)

Since kernel-side diagnostics are unavailable (below), the other angle is
the vendor userspace driver, which demonstrably *does* get groups
scheduled. Pulled `/vendor/lib64/egl/mt6899/libGLES_mali.so` (52MB,
world-readable, no root) and mapped its kbase ioctl usage. Method, for
reproducibility:

1. Auto-generate a dumper of every `KBASE_IOCTL_*` value from the
   vendored r49p1 headers, cross-compile it, and run it on-device — the
   ioctl numbers encode `sizeof(struct)`, so computing them by hand is
   error-prone.
2. `llvm-objdump -d` the blob and extract every `bl … <ioctl@plt>` call
   site with preceding context (`ioctl@LIBC` is an imported symbol, so
   all call sites are findable).
3. For each call site, walk backwards to pair the `mov w1, #lo` with its
   `movk w1, #hi, lsl #16` and reconstruct the 32-bit request. **Note:**
   the `movk` is scheduled several instructions after the `mov`, not
   adjacent — naive adjacent-line matching finds almost nothing, and
   the constants appear neither as raw literals in the file nor in
   literal pools.

Result: 50 of 56 call sites resolved, and they form a clean 1:1
ioctl-wrapper layer (one wrapper per ioctl, laid out in header order
around `0x1dba000`–`0x1dbc300`), so this is the blob's complete kbase
surface, not a sample.

**Called by the blob, never called by this repo's probes:**

- `KBASE_IOCTL_MEM_JIT_INIT` — JIT memory pool setup.
- `KBASE_IOCTL_MEM_EXEC_INIT` — executable-VA zone setup.
- `KBASE_IOCTL_CS_TILER_HEAP_INIT` (+ `_1_13` and `_TERM`) — confirms
  the tiler-heap hypothesis is live; the vendor driver always creates
  one.
- `KBASE_IOCTL_CONTEXT_PRIORITY_CHECK`, `KBASE_IOCTL_GET_CONTEXT_ID`,
  `KBASE_IOCTL_STREAM_CREATE` (fence stream), `KBASE_IOCTL_MEM_SYNC`,
  `KBASE_IOCTL_KCPU_QUEUE_CREATE`/`_DELETE`/`_ENQUEUE`.

**Notable version choice:** the blob's group-create wrapper uses
`KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6` (nr 42), *not* the modern
`CS_QUEUE_GROUP_CREATE` (nr 58) that this repo's probes use, and not
`_1_18` either. Both of those are entirely absent from the blob. Worth
testing whether the older, smaller group-create struct behaves
differently — this is a concrete, cheap experiment.

**Also confirmed:** `KBASE_IOCTL_INTERNAL_FENCE_WAIT` really is used by
the vendor driver (one call site), so it is a genuine MTK mechanism —
but that does not contradict the finding above that it never blocks for
the arguments this repo can construct.

**Experiment run, and it's another clean negative.** `live_kick_probe.c`
now replicates that setup: `MEM_JIT_INIT` + `MEM_EXEC_INIT` after
`SET_FLAGS`, then `CS_TILER_HEAP_INIT`, then group create via both nr 58
and nr 42. **Every setup ioctl succeeds** —

```
MEM_JIT_INIT   : OK (va_pages=16384)
MEM_EXEC_INIT  : OK (va_pages=65536)
TILER_HEAP_INIT: OK gpu_heap_va=0x7ffc000000 first_chunk_va=0x7ffc002000
```

— and the result is unchanged: `CS_EXTRACT=0`, `CS_ACTIVE=0`, no
notification, for all three group configurations. So the missing context
setup was *not* the blocker, and `CS_QUEUE_GROUP_CREATE_1_6` (nr 42)
behaves identically to nr 58 despite being what the vendor uses.

Worth noting the tiler heap lands at a real dedicated GPU VA
(`0x7ffc000000`), unlike the SAME_VA allocations whose GPU address is
just the CPU pointer — so the kernel is clearly willing to hand this
context real GPU VA space.

**Leading remaining hypothesis** (untestable without root): the group
never gets its MCU shared region bound. Before a group can go on a slot
the kernel must map its suspend buffers, ring buffer, and user-IO pages
into the MCU's own address space
(`kbase_csf_mcu_shared_group_bind_csg_reg()`,
`csf/mali_kbase_csf_mcu_shared_reg.c` — note `group->csg_reg` and
`group->csg_reg_bind_retries` are initialised at group creation). If
that binding fails, the group stays runnable-but-never-scheduled, with
no userspace-visible error — exactly the observed behaviour. The other
candidate is simply that the scheduler tick never selects this context.
Both are `dev_dbg`-only paths.

**Why this can't be chased further from userspace on this device.**
The failure path is `dev_dbg`-only and otherwise silent, so it needs
kernel-side visibility. Every avenue was checked explicitly on this
device (Poco X8 Pro, `ro.build.type=user`, `ro.debuggable=0`, SELinux
enforcing as `u:r:shell:s0`) — **all closed**, so don't re-tread these:

| Surface | Result |
|---|---|
| `dmesg` / `klogctl` | `Permission denied` (no `CAP_SYSLOG`) |
| `/proc/sys/kernel/dmesg_restrict` | `Permission denied` |
| `/dev/kmsg` | `Permission denied` |
| `/sys/kernel/debug/mali0/` | does not exist (debugfs not exposed) |
| tracefs `events/mali/` | dir lists, but `enable`/read `Permission denied` — **and** the registered tracepoints are memory/JIT only (`mali_mem_*`, `mali_jit_*`, `mali_mmu_page_fault_*`); there are no CSG-scheduling tracepoints, so this wouldn't answer the question even if writable |
| `/proc/mtk_mali/{logbuf_critical,logbuf_exception,logbuf_regular,fwlog}` | exist (MediaTek's own Mali log buffers — exactly what's needed) but all `Permission denied` |
| `logcat` | nothing Mali/kbase-related; kernel `dev_dbg` doesn't route here |
| `adb root` | `adbd cannot run as root in production builds` |

Note the Xiaomi developer-option "USB debugging (Security settings)"
does **not** help — it governs permission modification and input
simulation, not kernel access. Verified after enabling it: no change to
any row above. Candidate next steps, all needing more than
this probe: a rooted device or a userdebug build to read the driver's
own diagnostics; comparing against a trace of the vendor blob driver
doing a real submission (it clearly gets groups onto slots); or
checking whether group creation needs more setup than this probe does
(e.g. a tiler heap via `CS_TILER_HEAP_INIT`, or the `dvs_buf` field
left zero in `GROUP_CREATE`) before the scheduler will consider the
group schedulable.

## Prior art found: Panfork already runs Panfrost on kbase/CSF

Searched for existing work instead of continuing to derive this from
scratch, and there is a lot of it. **Panfork** (icecream95' Mesa fork) is
a Gallium Panfrost driver that runs on **kbase**, on CSF/Valhall v10
(G610/G710), built because panthor did not exist yet. Its kbase layer is
`src/panfrost/base/` (`pan_base.c`, `pan_vX_base.c`, `pan_base.h`) — a
custom UAPI abstraction sitting exactly where this repo's
`pan_kmod_kbase.c` sits. It gets CSG groups scheduled and executing on
real hardware, which is the thing this repo is blocked on.

The canonical GitLab repo (`gitlab.com/panfork/mesa`) has been **emptied**
— it is now a single-commit README saying "use upstream instead", since
panthor landed. Use a mirror. Cloned here as
`third_party/PANFORK` (gitignored, local/ephemeral, same treatment as
`third_party/MESA-KMOD`) from
`https://github.com/ROCKNIX/mesa-panfork`.

Other artifacts, not yet mined:

- `github.com/PojavLauncherTeam/panfork_offscreen_rootless` and
  `github.com/SolDev69/panfrost-gallium-mesa` — Panfork on **stock,
  unrooted Android on kbase**. Direct evidence the "user build, no root"
  constraint recorded above is surmountable. (Pojav repo archived
  2025-06-20.)
- `gitlab.com/icecream95/panloader` — includes a `pantrace` tool.
- `gitlab.com/icecream95/kbase-valhall` — kbase patched for `MALI_NO_MALI`,
  i.e. run the *blob userspace* against a fake kernel driver on a normal
  machine. This is how the G610 RE series was done: no Mali hardware and
  no root needed. A plausible replacement for the rooted-device
  requirement above.

### Finding 1: the user-IO page order contradicts `csf_user_regs.h`

Verbatim from `third_party/PANFORK/src/panfrost/base/pan_vX_base.c:1434`:

```c
#define CS_RING_DOORBELL(cs)        *((uint32_t *)(cs->user_io)) = 1
#define CS_READ_REGISTER(cs, r)     *((uint64_t *)(cs->user_io + 4096 * 2 + r))
#define CS_WRITE_REGISTER(cs, r, v) *((uint64_t *)(cs->user_io + 4096 + r)) = v
```

and `cs->user_io` is the raw mmap base, same as this repo's `queue_state`
(`pan_vX_base.c:1322` — `mmap(NULL, page_size * BASEP_QUEUE_NR_MMAP_USER_PAGES,
..., bind.out.mmap_handle)`).

So the two codebases disagree by exactly one page:

| page | Panfork (working on real HW) | this repo (`utils/csf_user_regs.h`, `live_kick_probe.c:302`) |
|---|---|---|
| 0 | HW doorbell | CS_USER_INPUT (writes `CS_INSERT` here) |
| 1 | CS_USER_INPUT (`CS_INSERT`) | CS_USER_OUTPUT (reads `CS_EXTRACT`/`CS_ACTIVE` here) |
| 2 | CS_USER_OUTPUT (`CS_EXTRACT`, `CS_ACTIVE`) | HW doorbell (never touched) |

**If Panfork is right, this repo writes `CS_INSERT` into the doorbell page
and polls `CS_EXTRACT`/`CS_ACTIVE` out of the input page** — which the
kernel zeroes at bind time and firmware never writes. That produces
`CS_EXTRACT=0`, `CS_ACTIVE=0`, forever, no hang, healthy device: an exact
match for the symptom recorded under "Live KICK with a real instruction"
above.

**Settled on-device by `tests/user_io_probe`: Panfork is right.** The
probe makes no assumption either way — it writes `CS_INSERT` at each
candidate page in turn, each with a fresh group/queue, and diffs the whole
12KB mapping. Result, 6/6 reproducible runs:

| CS_INSERT written at | what changed in the 12KB mapping |
|---|---|
| page 0 (this repo's layout) | nothing, anywhere. 0 words. |
| page 1 (Panfork's layout) | our own write, **plus page 2 + 0x00 advancing `0 -> 8` (= the CS size) a few ms later, which userspace never wrote** |

Two independent corroborations from the same probe:

- **Page 0 persists across processes; pages 1 and 2 do not.** A value
  written to page 0 + 0x00 is still there on the next run of the binary
  (fresh process, fresh context, fresh group), while pages 1 and 2 always
  come up freshly zeroed. That is what a shared HW doorbell page vs.
  per-queue I/O blocks zeroed by `init_user_io_pages()` look like.
- All three pages read back what is written to them, so none is a
  write-only MMIO aperture — the doorbell page is normal memory here.

So the order is `[doorbell][input][output]`. The uapi comment at
`csf/mali_base_csf_kernel.h:117` ("A pair of input/output pages **and** a
Hw doorbell page") describes the *contents*, not the order, and reading it
as an order was the mistake. Note Panfork hardcodes `4096` in these macros
while using `k->page_size` for the mmap, so it assumes 4K pages.

`utils/csf_user_regs.h` now carries `CSF_USER_DOORBELL_PAGE` /
`CSF_USER_INPUT_PAGE` / `CSF_USER_OUTPUT_PAGE` — use those instead of
hardcoding indices.

### Consequence: the GPU executes. The CSG-slot theory was wrong.

`tests/live_kick_probe` was writing `CS_INSERT` into the doorbell page and
polling `CS_EXTRACT`/`CS_ACTIVE` out of the input page — a page the kernel
zeroes at bind and firmware never writes. Every `CS_EXTRACT=0` /
`CS_ACTIVE=0` reading recorded in the "Live KICK" section above was a read
of a dead page.

With the two-line page-offset fix, and **nothing else changed**:

```
==== 3 of 3 configs actually executed on the GPU ====
  *** CS_EXTRACT advanced to 8 after ~50ms - GPU CONSUMED the instruction ***
```

(The "~50ms" there is `live_kick_probe`'s own 50ms poll interval, not a
latency measurement. Measured at 1ms granularity by
`tests/event_slot_probe`, the real figure is **2-4ms** from `KICK` to
`CS_EXTRACT` advancing. Don't quote the 50ms as a performance number.)

All three configs — nr 58 group create, `_1_6` (nr 42), and compute-only.
So the following are now **withdrawn**, not merely unproven:

- "The group never reaches a CSG slot" / `onslot_csg_add_new_queue()` /
  `scheduler_group_schedule()`. The group *is* scheduled and firmware *does*
  run the stream.
- The MCU-shared-region hypothesis (`kbase_csf_mcu_shared_group_bind_csg_reg()`)
  that was blocked on needing root. Nothing here needed root.
- The framing that `KICK` returning 0 proves nothing about execution — it
  turns out `KICK` was doing its job the whole time.

The kernel-side visibility that was blocked on root (`dmesg`, debugfs,
`/proc/mtk_mali/*`) was never needed. The bug was a one-page offset in
this repo's own userspace.

**Method note worth keeping.** The probe initially reported the *opposite*
answer, twice, because its 2s poll loop broke early: it tested
`value >= cs_size` on every non-insert page, and page 0's stale value from
a previous trial satisfied that immediately, so the snapshot diff ran at
~0ms — before firmware had responded. Comparing against the post-BIND
baseline instead of an absolute threshold fixed it. A negative result from
a polling probe is worth re-checking against its own exit condition before
being believed.

### Still open after this

`CS_ACTIVE` reads 0 even on runs where `CS_EXTRACT` advanced — consistent
with the stream having finished by the time it is sampled, but not
confirmed. No CSF notification arrives within 300ms of a consumed
instruction either, which is expected: a bare `MOVE32` signals nothing.
Getting a notification needs a CS that writes an event slot — which is
now done, see the next section.

### Finding 2: the completion mechanism, which is not a fence at all

Panfork does **not** use DRM syncobjs, and does not use
`KBASE_IOCTL_INTERNAL_FENCE_WAIT` (correctly buried as a dead end above).
It builds its own `kbase_syncobj` over **GPU-visible event memory**:

- `alloc_event_mem()` (`pan_vX_base.c:359`) allocates 2 pages with
  `BASE_MEM_CSF_EVENT` alongside the usual CPU/GPU RW + `SAME_VA` flags.
  That flag is the load-bearing part — this repo has never set it.
- Each bound CS gets a slot in that memory
  (`kbase_cs_bind()`, `pan_vX_base.c:1336`), seeded to `1` because it uses
  the CSF "Higher" wait condition, with the error word zeroed to avoid
  inheriting faults.
- The **command stream itself** signals completion by writing its event
  slot; userspace waits via `kbase_wait_for_event()` +
  `kbase_syncobj_update()` (`pan_vX_base.c:872`), with
  `kbase_read_event()` (`:964`) reading `struct base_csf_notification`
  off the kbase fd — the same read `tests/event_probe/` was already doing.

So the answer to both open sync items — Phase 2's non-DRM `vk_sync` and
Phase 4's "fence-translation shim" — is: implement `vk_sync` over
`BASE_MEM_CSF_EVENT` memory plus CS-emitted sync writes. There is no fence
object in kbase to translate; you build one.

Note this also means `live_kick_probe`'s single `MOVE32` could never
signal anything even if it executed. A real submission needs a sync
instruction targeting event memory.

#### Confirmed end-to-end on-device: `tests/event_slot_probe`

Both halves work, 8/8 reproducible runs, unprivileged `shell` user, no
root. The probe allocates event memory, seeds a slot, encodes a CS that
signals it, kicks, and watches both channels:

```
=== event memory (BASE_MEM_CSF_EVENT) ===
flags=0x8340f   CPU_RD CPU_WR GPU_RD GPU_WR SAME_VA CACHED_CPU COHERENT_SYSTEM
  event slot at gpu_va=0x7266e3e000 seeded: value=1 error=0

=== encoding the CS ===
  encoded 24 bytes (MOVE64 addr, MOVE64 val, SYNC_SET64 system)
    [0] 0x0100007266e3e000
    [1] 0x0102000000000002
    [2] 0x3400000200000000

=== channel 1: event memory ===
  CS_EXTRACT: advanced within 2-3ms (=24, cs_size=24)
  event slot: seeded 1, now 2 (error word 0)
  *** GPU SIGNALLED THE EVENT SLOT within 2-3ms ***

=== channel 2: base_csf_notification on the kbase fd ===
  *** NOTIFICATION: type=0 (EVENT) ***
```

Points worth keeping:

- **`BASE_MEM_CSF_EVENT` is accepted and changes the mapping.** Output
  flags come back `0x8340f` — the kernel adds `CACHED_CPU` and
  `COHERENT_SYSTEM` on top of what was asked for. That system coherence is
  presumably why the CPU sees firmware's write without any explicit cache
  maintenance; don't assume a plain BO would behave the same way.
- **`SYNC_SET64` with `MALI_CS_SYNC_SCOPE_SYSTEM` is the right
  instruction.** Address and value both have to be loaded into CS
  registers first (`cs_move64_to`) — `SYNC_SET64` takes register indices,
  not immediates. Panfork does the same with its `0x48`/`0x4a` pair
  (`pan_cmdstream.c:3094`). Encoded via Mesa's `cs_builder.h`, so this is
  not hand-rolled bytes.
- **`poll()` on the kbase fd now fires**, returning
  `base_csf_notification` type 0 (`BASE_CSF_NOTIFICATION_EVENT`). This
  retires the "poll()/read() on the kbase fd: mechanism untested" section
  above — the channel was always fine; nothing had ever given firmware a
  reason to notify. So a `vk_sync` can **block** rather than spin.
- **Latency is 2-4ms** from `KICK` to both the slot write and
  `CS_EXTRACT` advancing, measured at 1ms polling granularity.

What this does *not* yet cover: multiple slots in one event page (Panfork
packs them at `PAN_EVENT_SIZE` = 16 bytes, value word + error word, and
tracks a per-slot seqnum); the "Higher" wait condition and `SYNC_WAIT` for
GPU-side waits; and error propagation through the error word. Those are
implementation detail for the real `vk_sync`, not open questions about the
mechanism.

## Caller-chosen GPU VAs: BASE_MEM_FIXED works, in one zone, in one mode

This decides whether `pan_kmod`'s `vm_bind` contract is implementable on
kbase at all. `BASE_MEM_SAME_VA` lets the *kernel* pick an address, but
PanVK picks its own and then dereferences it during `vkCreateDevice`'s
mempool setup — so a backend that cannot place an allocation where the
caller asked cannot work. Answered by `tests/fixed_va_probe`, 3/3
reproducible on the Poco X8 Pro (r49p1), unprivileged, no root.

**It works.** `KBASE_IOCTL_MEM_ALLOC_EX` (nr 59, UK 1.9+) takes an
`in.fixed_address` that is honoured exactly when the allocation carries
`BASE_MEM_FIXED`:

```
FIXED @ 0x800200000000   -> gpu_va=0x800200000000   *** HONOURED exactly ***
FIXED @ 0x800200100000   -> gpu_va=0x800200100000   *** HONOURED exactly ***
FIXED @ 0x800210000000   -> gpu_va=0x800210000000   *** HONOURED exactly ***
FIXED @ 0xfffff000       -> FAILED (Out of memory)
```

and placement is genuinely under userspace control — two allocations
requested at `zone + 0x30000000` and `+ 4K` both landed exactly there.

Four things a real implementation has to respect:

- **There is a FIXED_VA zone, and it is not where PanVK allocates.** On
  this device it is at **`0x800200000000`**. Requests outside it fail
  `ENOMEM` — including `0xfffff000`, which is literally what PanVK's
  `util_vma_heap` asked `vm_bind` for. So PanVK's VA allocator has to be
  constrained to this zone (`pan_clamp_to_usable_va_range`, and the
  `util_vma_heap` setup in `panvk_vX_device.c`). The zone's *size* is not
  established here; allocations up to `zone + 0x30001000` were accepted.
- **`BASE_MEM_FIXED` and `BASE_MEM_FIXABLE` are mutually exclusive per
  context.** Once a `FIXABLE` allocation exists, every later `FIXED`
  request fails `EINVAL`, and vice versa. This cost real time: a first
  version of the probe located the zone with a `FIXABLE` allocation and
  then found every `FIXED` request rejected — the same address that had
  returned `ENOMEM` in a run without a preceding `FIXABLE` returned
  `EINVAL` with one. The mode, not the address, was the difference.
- **errno tells you which problem you have.** `ENOMEM` = address
  unavailable: outside the zone, or already allocated (re-requesting a
  page the probe itself had taken reproduces it). `EINVAL` = wrong mode,
  per the previous point. Do not read `EINVAL` as "unsupported".
- **The CPU mapping is separate, and that is the point.** `mmap()`ing the
  fd at the fixed `gpu_va` gives a working CPU view at an unrelated CPU
  address (`cpu=0x71730bc000` for `gpu_va=0x800200000000`), verified by a
  sentinel write/read. Under SAME_VA the two were the same number; here
  the GPU address is ours to choose and the CPU address is wherever it
  lands. Anything treating the CPU pointer as the GPU VA — this repo's
  backend and most of `tests/` — has to stop.

Consequence for the backend: `bo_alloc` should stop using SAME_VA, and
`vm_bind` should do the real mapping at `op->va.start` with
`MEM_ALLOC_EX` + `BASE_MEM_FIXED`. See ROADMAP.md Phase 2.

## A kick only lands on an idle CS (`CS_ACTIVE` must be 0)

Found while wiring `VkQueueSubmit`: the first submit on a queue worked and
every one after it silently did nothing. Not an error — `CS_QUEUE_KICK`
returned 0 every time, and the bytes were still in the ring; they just sat
there until some later kick flushed them. The symptom is a `vkWaitForFences`
that times out, not a failure you can catch at submit time.

Logging `CS_INSERT` / `CS_EXTRACT` / `CS_ACTIVE` at each kick, three in a
row on one queue (Mali-G720, r49p1, `tests/driver_sync_probe`):

```
kick 1: insert=24 extract=0  CS_ACTIVE=0  -> ran, extract reached 24
kick 2: insert=48 extract=24 CS_ACTIVE=1  -> DID NOT RUN
kick 3: insert=72 extract=48 CS_ACTIVE=0  -> ran, extract reached 72
```

The failing kick is exactly the one issued while `CS_ACTIVE` was still 1.
That flag lingers ~30-40ms after a stream finishes, which is well inside the
turnaround of a tight submit/wait loop — so in practice *every* submit after
the first landed in the bad window. Inserting a 200ms delay before each kick
made 4/4 submits run within 10ms of their kick; removing it made every
submit after the first time out. That delay was the only difference.

Reading of it: the kick handler flags the queue and wakes the scheduler,
which is what gets an *offslot* group scheduled. A CS that is already onslot
and has caught up (`extract == insert`) is not waiting on the scheduler, and
nothing in that path tells it to re-read `CS_INSERT`.

**Ruled out — do not retry: ringing the user-IO doorbell page.** Panfork's
`kbase_cs_submit()` (`pan_vX_base.c:1470`) has exactly this branch — read
`CS_ACTIVE`, and if set, write 1 to `user_io + 0` instead of calling the
ioctl — but its condition is hardcoded false so it always takes the ioctl
path. Tried here anyway, unconditionally: the failing kick still did not
run. That page also reads back whatever was last written to it (wrote 1,
read 1 at the next kick), so on this device it behaves as ordinary memory,
not as an MMIO doorbell register. Panfork disabling that branch looks
deliberate rather than accidental.

Fix in use: `pan_kmod_kbase_queue_wait_idle()`, called before every kick.
It works — 5/5 runs of `driver_sync_probe`, three submits each — but it
serialises submissions, which defeats much of the point of a ring buffer.
It is correct-but-slow, and it is the honest option while the real wake
mechanism for an onslot idle CS is unknown.

Worth revisiting if kernel-side visibility ever becomes available (it needs
root — see the debugfs/dmesg section above): the answer is presumably in how
`kbase_csf_queue_kick()` decides whether to ring the hardware doorbell for a
group that is already onslot.

## MEM_ALIAS works, but the handle is the GPU VA and not the cookie

PanVK's render descriptor ringbuf maps one BO at `dev_addr` and again at
`dev_addr + size`, so a descriptor read running off the end of the ring wraps
into the copy and the wraparound can be done with 32-bit arithmetic. Both the
VERTEX_TILER and FRAGMENT subqueue contexts point at it, so without it neither
can be initialised.

`kbase_kmod_vm_bind()` cannot express that at all: an allocation lives where
`MEM_ALLOC_EX` put it, so both `MAP` ops fail the caller-chosen-VA check.
`KBASE_IOCTL_MEM_ALIAS` (nr 21) is the mechanism that can — `stride` plus
`nents` entries, each naming an existing allocation with an offset and length,
so two entries naming the same allocation give the double mapping. Panfork
never calls it, so there was no prior art to copy.

`tests/alias_probe` settles it. Aliasing one 4-page allocation twice with
`stride = 4` pages succeeds and reports `va_pages = 8`, the full 2x span.

**The non-obvious part: which u64 is "the handle".** Passing the `gpu_va` the
allocation reported fails `ENOMEM`; passing the CPU pointer succeeds. That is
not a contradiction — under `BASE_MEM_SAME_VA` the reported `gpu_va` is an
mmap cookie and the *real* GPU address is the CPU pointer (see the SAME_VA
cookie section above). `MEM_ALIAS` wants the real address. `ENOMEM` here means
"no such allocation at that address", the same way it means "address
unavailable" for `MEM_ALLOC_EX`, and is easy to misread as a resource limit.

```
cookie handle, SAME_VA alias      -> Out of memory
real GPU VA handle, SAME_VA alias -> OK   (gpu_va=0x41000, va_pages=8)
```

**`out.gpu_va` is an mmap cookie, not an address.** `out.flags` comes back
`0x400d` = `NEED_MMAP | GPU_WR | GPU_RD | CPU_RD` — note `SAME_VA` is absent,
because `kbase_mem_alias()` strips it. `BASE_MEM_NEED_MMAP` means the region
has no mapping, GPU or CPU, until userspace `mmap()`s it. **Check this flag
before doing anything with the returned value.**

**This cost two device reboots.** `tests/alias_cs_probe` pointed a command
stream at `out.gpu_va + stride`, assuming it was an address because `SAME_VA`
had been stripped. Writing to an unmapped GPU address faulted, and the fault
wedged the kbase context past `kill -9` — the process sits in uninterruptible
`D` state and only a reboot clears it. The GPU itself survives: a fresh
context opens fine afterwards and `live_kick_probe` still passes 3/3. The
probe now refuses to run without `--i-know-it-hangs`.

Two `kbase_context_mmap()` constraints then box the whole approach in:

- `PROT_WRITE` is refused with `EPERM` ("VM flags inconsistent with region
  flags"), because `CPU_WR` is not in `kbase_mem_alias()`'s accepted mask.
- `nr_pages > stride` is refused with `EINVAL`, so **a single mapping can
  never span more than one window**.

That last constraint is the real obstacle. PanVK's ringbuf needs *one* VA
range covering both windows back to back — that is the entire point of the
double mapping — and a mapping capped at `stride` pages cannot produce one.
So `MEM_ALIAS` composes the region as asked, and its entries genuinely share
`alloc->pages` (confirmed in the kernel source, `kbase_mem_phy_alloc_get()`),
but the route from there to a usable 2x GPU VA range is not established.

**Both remaining variants were tried, and both fail the same way.** `stride`
set to the full span returns `va_pages = 16` and still `NEED_MMAP`. A
`BASE_MEM_FIXABLE` source — which does itself land at a real address,
`0x800200000000` in the FIXED_VA zone — aliases fine but the *alias* still
comes back `NEED_MMAP`.

So the conclusion is negative, and it follows from the kernel's own
arithmetic rather than from any one error code:

- `NEED_MMAP` means there is no GPU mapping until userspace `mmap()`s the
  cookie, and the GPU address is assigned at that point;
- `nr_pages == va_pages` requires `va_pages > stride`, which
  `kbase_context_mmap()` rejects `EINVAL`;
- `nr_pages <= stride` covers exactly one window.

**Therefore the GPU can only ever address one window, and the ringbuf's
wraparound is not expressible via `MEM_ALIAS` on this kernel.** The aliasing
itself is real — the entries share `alloc->pages`, and the region is composed
exactly as asked — it is the *addressability* that fails.

The fallback is to stop relying on the mapping to wrap and bounds-check the
ring in the command stream instead. That is a change to shared PanVK code
(`init_render_desc_ringbuf()` and whatever consumes `render.desc_ringbuf`),
not to the kbase backend.

**Two error codes worth not misreading**, both of which cost time here:
`ENOMEM` from `MEM_ALIAS` means "no allocation at that handle", not a
resource limit — passing a FIXABLE allocation's *CPU* pointer as the handle
produces it, because the SAME_VA rule that the CPU pointer is the GPU address
does not hold for FIXABLE. And `EPERM` from `mmap()` is the flags check
(`CPU_RD` absent from the alias), not a blanket refusal, so it masks the
`EINVAL` you would otherwise get from the page-count check.

**Method note that cost real time here:** `adb push` run from Git Bash has
its destination path mangled (`/data/local/tmp/x` becomes
`C:/Program Files/Git/data/local/tmp/x`), and it still reports "1 file
pushed". The device silently keeps running the old binary. Two of the
conclusions above were briefly wrong because of it. Push from PowerShell, or
verify with `md5sum` on both sides.

## Where to ask

The `#panfrost` channel (Matrix, bridged to OFTC IRC) is where Panfrost/
PanVK/Panthor upstream discussion happens. Worth lurking before you start
and posting once Phase 1 (standalone probe — already working here) is
solid — see Phase 9 in `ROADMAP.md` for why raising it early matters.

## Every command buffer touches all three subqueues, even a compute-only one

Found by `tests/driver_compute_probe --submit`, the first attempt to submit a
real command buffer. The command buffer recorded nothing but a
compute-to-compute `VkMemoryBarrier2`. `vkQueueSubmit` returned `-8`
(`VK_ERROR_FEATURE_NOT_PRESENT`) — this driver's own refusal to run work on a
subqueue with no GPU-side context.

The reason is `finish_cs()` in `csf/panvk_vX_cmd_buffer.c`, which
`vkEndCommandBuffer` calls in a loop over **every** subqueue, not only the
ones the application touched. So `cs_is_empty()` is false for all three after
recording anything at all, and "empty" is not a usable test for "carries no
work" once a command buffer has been ended.

That epilogue is not harmless on kbase. It does:

- `cs_wait_slots(all_mask)` — fine on an untouched CS, nothing is pending;
- a `last_error` check that does `cs_load64_to(sync_addr,
  cs_subqueue_ctx_reg(b), offsetof(..., syncobjs))`.

The second one dereferences the subqueue context register. `init_gpu_queue()`
only loads that register for COMPUTE here, so on VERTEX_TILER and FRAGMENT it
is still 0 — a GPU read of address 0, i.e. a page fault, i.e. the wedged
context that has previously needed a reboot. Refusing the submit is the
correct behaviour and it is what happened; the guard did its job.

### Why this is smaller than it looks

The epilogue needs exactly one thing from the context: `syncobjs`. And
`init_subqueue()` sets `.syncobjs` for every subqueue unconditionally, at the
top. Everything that needs the render descriptor ringbuf — `render.tiler_heap`,
`render.geom_buf`, `render.desc_ringbuf`, `tiler_oom_ctx.ir_scratch_fbd_ptr` —
is set further down, under the render-only branch.

So a **minimal context init** for VERTEX_TILER and FRAGMENT looks possible
without solving the ringbuf at all: allocate the context, populate `syncobjs`
and `iter_sb`, run the short init stream that loads
`cs_subqueue_ctx_reg`, and skip the render fields. The epilogue then has
everything it dereferences, and a compute-only command buffer can be
submitted with its two render streams published as the no-ops they are.

Not yet attempted. It does not unblock rendering — that still needs the
ringbuf, see the MEM_ALIAS section — it unblocks *compute*, which is
currently blocked on render subqueues that are only along for the ride.

### Confirmed by disassembly, not inference

`PANVK_KBASE_DUMP=1` (added in `panvk_vX_kbase_queue.c`) prints every stream
before it is kicked, and every stream this driver rejects. The rejected
VERTEX_TILER stream from the barrier-only command buffer above, 176 bytes:

```
 [ 0] 0x0242000000000000  MOVE32          flush id = 0
 [ 1] 0x2400420000000200  FLUSH_CACHE2    the barrier's own flush
 [ 2] 0x0300000000010000  WAIT
 [ 3] 0x0300000000030000  WAIT
 [ 4] 0x14427a0000030000  LOAD_MULTIPLE   <- from reg 0x7a = 122
 [ 5] 0x0300000000010000  WAIT
 [ 6] 0x1142420000000000  ADD_IMMEDIATE64
 [ 7] 0x0144000000000001  MOVE48
 [ 8] 0x3300424400000005  SYNC_ADD64      barrier signals the VT syncobj
 [ 9] 0x1174740000000001  ADD_IMMEDIATE64 <- reg 0x74 = 116, progress seqno
 [10] 0x1176760000000001  ADD_IMMEDIATE64 <- reg 0x76 = 118, progress seqno
 [11] 0x0300000000ff0000  WAIT            finish_cs's wait on all slots
 [12] 0x14427a0000030000  LOAD_MULTIPLE   <- from reg 0x7a = 122 again
 [13] 0x0300000000010000  WAIT
 [14] 0x1444420000010008  LOAD_MULTIPLE
 [15] 0x0300000000010000  WAIT
 [16] 0x1600440020000002  BRANCH
 [17] 0x15447a000001000c  STORE_MULTIPLE  -> reg 122 + 0xc, last_error
 [18] 0x0300000000010000  WAIT
 [19] 0x0242000000000000  MOVE32
 [20] 0x2400420000000011  FLUSH_CACHE2    end-of-cmdbuf clean
 [21] 0x0300000000010000  WAIT
```

Register 122 is `PANVK_CS_REG_SUBQUEUE_CTX_START`. `[4]`, `[12]` and `[17]`
go through it, and on VERTEX_TILER it is still 0 - so this stream would read
and write around address 0. That is the fault, seen rather than reasoned
about.

**And it is the whole extent of the problem.** Everything reached through
register 122 here is `syncobjs` and `last_error`, both plain fields of the
subqueue context. Registers 116 and 118 are progress seqnos, which live in
the register file and need no memory at all. There is no `RUN_IDVS`, no
tiler heap access, and nothing touching the descriptor ringbuf anywhere in
the stream. So loading the context register is sufficient to make this
stream safe - the ringbuf question does not gate it.

### Side finding: `PANVK_DEBUG=trace` cannot be used here

`vkCreateDevice` fails with `-2` (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) under
`PANVK_DEBUG=trace`, with nothing in logcat. Trace mode switches the
subqueue allocations to the non-cached pool and adds a per-subqueue
tracebuf. The tracebuf is the blocker, and it is not a small fix:
`init_subqueue_tracing()` reserves a caller-chosen VA with `panvk_as_alloc()`
and binds the BO into it with `pan_kmod_vm_bind()`, leaving a deliberate
guard page unmapped. kbase has no VM object to bind into — a region's GPU VA
is fixed when it is allocated — so this is the same class of gap as the
`MEM_ALIAS` one, not a missing flag.

Superseded in practice by `PANVK_KBASE_DUMP=1`, which prints instruction
words and opcode names straight from the submit path and depends on none of
that machinery. Less capable than `pandecode` — operands stay hex — but it
answers "is this the stream I meant to build", which is the question that
matters before a kick.

## Shader code needs the EXEC_VA zone, not the FIXED_VA one

Found by `tests/driver_compute_probe --fill`, which segfaulted during
*recording* - before any GPU work - with a null deref at fault address `0x8`.

The chain, from the symbolised backtrace (`llvm-symbolizer` against the
unstripped `.so`; frames were `cmd_dispatch_prepare_tls` <-
`dispatch_precomp` <- `CmdFillBuffer`):

1. `kbase_kmod_bo_alloc()` refused every BO carrying flags, because
   `supported_bo_flags` was 0.
2. PanVK's `dev->mempools.exec` asks for `PAN_KMOD_BO_FLAG_EXECUTABLE`, so
   `panvk_shader_upload()` failed for every shader.
3. `create_shader_from_binary()` returned failure, so
   `precomp_cache_get()` returned NULL.
4. `dispatch_precomp()` has `assert(shader)` - **compiled out under NDEBUG** -
   and passed NULL to `cmd_dispatch_prepare_tls()`, which read
   `cs->info.tls_size` off it.

So a missing allocator flag surfaced as a null deref three frames away. Worth
remembering as a shape: a release build turns "this returned NULL" into a
crash somewhere else entirely.

### The fix, and the wrong first attempt

Adding `BASE_MEM_PROT_GPU_EX` to the existing `MEM_ALLOC_EX` call was not
enough - it returned `ENOMEM`. kbase keeps executable memory in its own
**EXEC_VA** zone, the one `KBASE_IOCTL_MEM_EXEC_INIT` reserves, while
`BASE_MEM_FIXED` places an allocation in the **FIXED_VA** zone. The two are
mutually exclusive, and `ENOMEM` is what asking for both gets you.

Executable BOs therefore take a different path: plain `KBASE_IOCTL_MEM_ALLOC`
with `CPU_RD | CPU_WR | GPU_RD | GPU_EX`, no `BASE_MEM_FIXED`, and the kernel
picks the address. `BASE_MEM_SAME_VA` is absent, so the returned `gpu_va` is a
real GPU address rather than an mmap cookie, and doubles as the mmap offset
exactly as the fixed-address path's does. No `GPU_WR`: shader code is not
written by the GPU.

Two consequences worth keeping:

- `MEM_EXEC_INIT` finally has a purpose here. This backend called it because
  the vendor blob does (see the RE section above) and nothing had needed it
  since.
- `kbase_kmod_bo_free()` must **not** return an executable BO's address to the
  FIXED_VA heap - the kernel chose it, the heap never owned it, and inserting
  it would corrupt every later allocation. Guarded on the flag.

### A 4 GB constraint on shader BOs, from Panfork — not yet hit here

Flagging this because it is latent rather than observed, and because letting
the kernel pick the address means nothing here controls it. Panfork strips
the same two flags for executable BOs, for a reason it states outright
(`src/panfrost/base/pan_vX_base.c:626`):

```c
/* Using SAME_VA for executable BOs would make it too likely
 * for a blend shader to end up on the wrong side of a 4 GB
 * boundary. */
flags |= BASE_MEM_PROT_GPU_EX;
flags &= ~(BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);
```

Dropping `GPU_WR` and `SAME_VA` matches what this backend already does, and
was arrived at here independently for different reasons (`GPU_WR` because
shader code is not GPU-written; `SAME_VA` because the EXEC_VA zone does not
grant it). The part this backend does **not** account for is the 4 GB
boundary: on the old-API path Panfork additionally aligns shader BOs to
16 MB and over-allocates 4x to force it.

The underlying constraint is that some shader-referencing fields carry only
a low 32-bit offset, so a shader and what refers to it must sit in the same
4 GB span. Compute has not tripped this - one shader at a time, no blend
shaders - which is exactly why it is worth writing down before rendering
starts allocating more of them.

Not verified on this device. It is Panfork's claim plus a plausible
mechanism, not a measurement here, and the symptom would be a shader that
executes garbage rather than a clean failure. If shaders start misbehaving
once more than one is live, check their addresses share a 4 GB span before
anything else.

### Result

`driver_compute_probe --fill` passes 4/4: a `vkCmdFillBuffer` compute shader
dispatches, the fence comes back signalled by the GPU, and the buffer reads
back holding the pattern. The buffer is seeded with the complement of the
pattern first, so a pass cannot be memory that already happened to match.

That is the first shader this port has executed.

## Semaphores: the missing `vk_sync_type` callback is found only at runtime

`vkCreateSemaphore` failed with `VK_ERROR_FEATURE_NOT_PRESENT` until
`panvk_kbase_sync` advertised `VK_SYNC_FEATURE_GPU_WAIT`.
`get_semaphore_sync_type()` (`vk_semaphore.c:99`) requires it and nothing else
non-trivial, so no application that orders any work could get past device
setup. Fences never needed it, which is why compute worked without it.

Adding the bit is not by itself a claim to have GPU-side waits. What it
commits to is that a submission does not begin until its waits are satisfied,
and blocking the submitting thread satisfies that. The reason to stop there
rather than emit `SYNC_WAIT64` is in the wait loop in
`panvk_vX_kbase_queue.c`, and it is not laziness: a stream parked in
`SYNC_WAIT64` holds `CS_ACTIVE`, and every submit here first waits for
`CS_ACTIVE` to clear and then kicks anyway after 100ms. Real GPU-side waits
would therefore let the next submit overwrite a ring the GPU is still
executing. Fixing the serialisation - tracking consumption via `CS_EXTRACT`
rather than requiring idleness - has to come first.

### Two feature bits that are not independent

Setting `GPU_WAIT` without `WAIT_PENDING` puts `get_timeline_mode()`
(`vk_device.c:79`) on an `assert` that fires in a debug build and vanishes
under `NDEBUG`, leaving the mode selection running on a false premise. And
setting `GPU_WAIT` without `WAIT_BEFORE_SIGNAL` is deliberate: it selects
`VK_DEVICE_TIMELINE_MODE_ASSISTED`, where the runtime holds a submit on its
queue thread until the waits are pending instead of handing over a wait whose
signal has not been submitted. That is exactly the compensation this sync
type needs, because a slot carries no record that a signal is coming.

`WAIT_PENDING` itself is answered as a complete wait. A slot cannot
distinguish "a signal has been submitted" from "a signal has landed" - it
holds one value, written when the `SYNC_SET64` retires. Answering late is the
safe direction; it costs latency, where answering early would be a
correctness bug.

### `type->move` is mandatory, and the crash tells you nothing

The first run with `GPU_WAIT` set died with `SIGSEGV` at `pc 0` inside
`vkQueueSubmit`. `vk_queue.c:276` asserts `semaphore->permanent.type->move`
exists the moment a binary semaphore's permanent payload is waited on under
threaded submit, then
`vk_queue_submit_move_binary_waits_to_temps()` calls it unconditionally.
Under `NDEBUG` the assert is gone and the call goes through a NULL pointer.

This is the third time in this port that an `assert` compiled out under
`NDEBUG` has turned a clear precondition into a NULL dereference - the others
were `precomp_cache_get()` behind `assert(shader)`, and this one. A crash at
`pc 0` with a plausible caller frame is worth reading as "an optional-looking
function pointer was not optional" before anything else.

`llvm-symbolizer --obj=<unstripped .so> --functions=linkage` against the raw
frame offsets from `adb logcat -b crash` named the caller in one step. The
device cannot symbolize `/data/local/tmp` libraries itself.

### Moving a payload means moving the slot, not its contents

The obvious implementation of `move` - copy src's value into dst, zero src -
is wrong here, and wrong in a way that would have passed this probe.

At the point the runtime moves a payload, a submit that will signal src can
already be sitting in the ring with a `SYNC_SET64` naming src's slot
*address*. Copying the value that is there at that instant leaves the pending
write aimed at the slot src still owns, so dst - the object that inherited
the payload and the one the wait actually watches - would stay at 0 until it
timed out. Serialised submission makes this rare rather than impossible.

Swapping the two slot indices is correct instead: a slot *is* the payload, so
handing over the index hands over any write already in flight against it.
This is the behaviour a DRM syncobj gets for free by moving the underlying
fence, and it is why `move` exists as a callback rather than being emulated
by the runtime out of `get_value` and `signal`.

### Confirmed on hardware

`tests/driver_semaphore_probe`, 5/5 runs, no failures, alongside
`driver_pipeline_probe` still passing:

- binary and timeline semaphores create;
- two submits chained by a binary semaphore both complete, each writing its
  own buffer with its own push constant;
- a timeline semaphore reaches exactly the value the submit asked for -
  checked at 42 rather than 1, so a signal that took the binary path would
  fail here rather than pass.

Not checked, deliberately: that the second submit's work happens after the
first's. Submission is serialised anyway, so that would hold with the
semaphore removed entirely, and a test that passes for the wrong reason is
worse than no test.

## Kicks, CS_INSERT and the cost of waking an idle GPU

Revisits "A kick only lands on an idle CS" above, which was the basis for
waiting on `CS_ACTIVE` before every kick and therefore for serialising every
submission. That rule is roughly right about *when* a kick is needed and
badly wrong about *why*, and the difference is worth about 50x on
back-to-back submits.

All of this is `tests/kick_pipeline_probe` unless stated. It drives one bound
queue with a deliberately slow stream - a long run of synchronous
`FLUSH_CACHE2`, chosen because it is slow for a reason the hardware cannot
optimise away and because it always terminates, so nothing here can park a CS.

### CS_EXTRACT reports completion, not progress

Sample `CS_EXTRACT` every 200us through an 11.4ms stream and you see **one**
distinct value: it jumps from the start of the stream to its end. It does not
advance through it.

This matters beyond the kick question. Ring occupancy is only knowable at
stream granularity, so `wait_for_ring_space()` over-estimates how full the
ring is - safe, but it cannot be made precise. It also means "is the GPU
mid-stream" cannot be answered by watching `CS_EXTRACT`, which is why the
experiments below are timed rather than sampled.

### A running CS re-reads CS_INSERT; an idle one does not

The decisive pair of measurements.

| what | result |
|---|---|
| publish a second stream while the first is running, **no kick** | both done in **19.0 ms** (one stream = 11.4 ms) |
| publish a stream to an **idle** CS, **no kick** | never consumed, 3s timeout |

So firmware re-reads `CS_INSERT` when it reaches the end of what it already
knew about. Appending to a busy CS needs no kick at all - and 19.0ms for two
11.4ms streams means it did not even pay a round trip in between. An idle CS
has stopped looking, and only a kick restarts it.

That is the rule the submit path now implements: publish first, then kick
only if `CS_EXTRACT >= ` the pre-append `CS_INSERT`. Publishing before
sampling is what makes it race-free in the direction that matters - if
firmware has not yet reached the old insert point, the new value is already
stored and it will read it.

### The linger rule does not reproduce, but removing the wait still breaks it

Driving a CS into the exact state the original trace captured - stream
finished, `extract == insert`, `CS_ACTIVE` still 1 - and kicking: **10/10
consumed, most within 1.5ms.** The rule as written does not reproduce.

And yet removing the wait from the driver reproducibly breaks it, with three
subqueues publishing cleanly and the **compute** one - the one carrying the
`SYNC_SET64`s - silently not running, so the fence never signals. The probe
uses one CS; the driver uses three in one group. That difference is not
explained, and is the honest open question here.

Ruled out, with `PANVK_KBASE_KICK_MODE` on a shipped binary:

- **Re-kicking a stream that was not picked up.** An immediate second kick is
  a no-op. This is the strongest hint at the mechanism: `kbase_csf_queue_kick()`
  only queues the queue for the scheduler worker `if
  (list_empty(&queue->pending_kick_link))`, so a kick that is already pending
  swallows the next one.
- **Pacing kicks apart by a fixed delay.** Needs ~8ms to work; fails at 2ms.
  So it is not a rapid-fire race between the three CSs - it is just a worse
  spelling of waiting.

The ~8ms threshold and the ~12ms `CS_ACTIVE` linger both sit right around the
kbase CSF scheduler's tick period, which is the most plausible reading: a
kick on a queue whose CS is not idle takes effect on the next tick.

### Panfork tried the doorbell for this and disabled it

Worth knowing before anyone reaches for the hardware doorbell as a cheaper
wake than the `CS_QUEUE_KICK` ioctl. Panfork implemented exactly that and
then turned it off — `src/panfrost/base/pan_vX_base.c:1468`:

```c
bool active = CS_READ_REGISTER(cs, CS_ACTIVE);

CS_WRITE_REGISTER(cs, CS_INSERT, insert_offset);
cs->last_insert = insert_offset;

if (false /*active*/) {
        memory_barrier();
        CS_RING_DOORBELL(cs);
        memory_barrier();
        active = CS_READ_REGISTER(cs, CS_ACTIVE);
} else {
        kbase_cs_kick(k, cs);
}
```

The `if (false /*active*/)` is deliberate: the intended design was
"doorbell if the CS is still running, ioctl kick if it is idle", and the
doorbell half is dead code. Panfork pays the ioctl on every submit.

Two things follow.

**The doorbell is a dead end, or at least was for the one project that
tried it.** The `CS_RING_DOORBELL` macro writes `1` to the first word of
the user_io mapping — the page the table in "Prior art found: Panfork"
above identifies as the doorbell page. Nothing here needs re-deriving; it
was built, and abandoned, and the commented-out condition is the only
record of why.

**This repo's `auto` mode is ahead of that prior art, not behind it.**
Panfork's abandoned path still kicks a busy CS, just via a cheaper
mechanism. The measurement in "A running CS re-reads CS_INSERT" says a
busy CS needs *no* wake at all, so `auto` skips the work rather than
making it cheaper. That is a different and better answer than the one
Panfork was reaching for, and it is measured here rather than assumed.

It does not explain the open question above — why removing the wait breaks
three-subqueue submission when a one-CS probe cannot reproduce it. But it
does say that the obvious next optimisation has already been tried by
someone with working hardware, so that is not where the answer is.

Source: Icecream95's Panfork, <https://gitlab.com/panfork/mesa>, verified
byte-identical against the local `third_party/PANFORK` checkout (`832c3c7`)
and the ROCKNIX mirror.

### What the fix is worth

Measured on the Poco X8 Pro, `auto` (publish, kick only when caught up)
against `always` (the old wait-then-kick):

| workload | auto | always |
|---|---|---|
| `driver_pipeline_probe --burst=100` (back to back, one fence at the end) | **0.36 ms/submit** | 0.39 ms/submit |
| `driver_compute_probe --fill --loop=300` (one fence wait per submit) | 21.8 ms/submit | 22.6 ms/submit |

The gap between those two rows is the real finding, and it is not queueing.
When a submit arrives at an idle GPU, a kick is unavoidable and costs a ~12ms
wait; when submits arrive back to back, the CS is still running and no kick
happens at all. So this device rewards keeping work in flight far more than
it rewards anything the submit path can do, and an application that waits for
a fence between every submit pays wake-up latency no driver change here will
remove.

The old path was not slow because it serialised. It was slow because it woke
the GPU every time, and so is anything else that kicks unconditionally.

### Diagnosing this again

`PANVK_KBASE_KICK_MODE=always|nowait` and `PANVK_KBASE_KICK_LOG=1`, which logs
`insert`/`extract`/`active`/`kicked`/`waited` around every kick. Every failure
in this area looks identical from outside - a fence that never signals - so
the log is the only thing that distinguishes "not kicked" from "kicked and
ignored" from "still running". `PANVK_KBASE_KICK_LOG_AFTER=1` adds a sample
2ms later, which is what shows a kick returning 0 and doing nothing.
