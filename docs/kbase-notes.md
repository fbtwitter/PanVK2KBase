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

## Non-simul_use rendering works - the ringbuf blocker was narrower than stated

"Rendering is blocked on the ringbuf" has been this repo's and the
roadmap's conclusion since the `MEM_ALIAS` section above. Re-checking that
conclusion against Mesa's actual source (2026-08-01, `/opt/mesa-src`,
26.3.0-devel) while looking for unblocked work found it was broader than
the real hazard, and confirming that on real hardware turned out to
**unblock non-simultaneous-use rendering outright.**

### What the source actually says

Every reference to `render.desc_ringbuf` in `panvk_vX_cmd_draw.c` - all 10
of them, read individually rather than sampled - is conditional on
`VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT` (`simul_use`):
`get_tiler_desc()`'s choice between the ringbuf and a per-command-buffer
descriptor pool (`simul_use ? ringbuf : panvk_cmd_alloc_desc_array(...)`),
the FBD patch-copy (`copy_fbds = simul_use && cmdbuf->state.gfx.render.tiler`,
with the code's own comment: *"if... not simultaneous use of the command
buffer, we can avoid the copy"*), and the producer/consumer release pair at
the end of the fragment stream (`free_render_descs = simul_use &&
needs_tiling`). `panvk_vX_gpu_queue.c`'s two references - context init and
`PANVK_DEBUG(TRACE)` readback - are already correctly gated on `is_kbase`
or only fire under a tracing build. And the other render-subqueue fields a
real draw needs - `render.tiler_heap`, `render.geom_buf`, the tiler-OOM
scratch FBD - are already set unconditionally and correctly on kbase, from
this repo's tiler-heap work.

So a command buffer recorded **without** `SIMULTANEOUS_USE` never touches
the zeroed ringbuf field anywhere in the shared PanVK path. What was
actually stopping it was this repo's own submit-time gate in
`collect_cmdbuf_calls()` (`panvk_vX_kbase_queue.c`), which refused any
stream requesting tiler/IDVS/fragment resources regardless of `simul_use` -
more conservative than the hardware needed.

### The fix, and the measurement

The gate now requires `simul_use` alongside the resource-mask check,
matching how Mesa's own code computes the identical distinction
(`free_render_descs = simul_use && needs_tiling`). Rebuilt and run against
the existing regression set first (`driver_enum_probe`, the external-memory
gate, `driver_compute_probe --fill`, `driver_pipeline_probe`,
`driver_semaphore_probe`) to confirm the loosening changed nothing for
compute - 0 failures, all five.

Then `tests/render_clear_probe` - a render pass with `LOAD_OP_CLEAR` /
`STORE_OP_STORE` and **no draw calls**, deliberately smaller than a
triangle so it isolates "does VERTEX_TILER/FRAGMENT execute at all" from
"does rasterization produce correct output" - run on the Poco X8 Pro,
gated behind `--i-know-it-hangs` the way `tests/alias_cs_probe` is, since
this is the first real VERTEX_TILER/FRAGMENT execution ever attempted on
this device in this repo and the static analysis above is software-side
only.

**It worked. Twice, reproducibly, with the device fully healthy
afterward** - `driver_compute_probe --fill` re-run clean immediately after.
`vkQueueSubmit` returned `0` (the gate accepted the stream), the fence
signalled from the GPU, and the readback buffer held the exact clear colour
(`2ab35cff`) in all 16 pixels - not a silently-skipped clear, not stale
memory, the GPU actually ran `FRAGMENT`'s clear path and the result came
back correct.

### What this does and does not mean

**Does:** a real render pass - entering and leaving one, with a clear - now
works on kbase, for command buffers that avoid `SIMULTANEOUS_USE`. That is
most real usage; it is an opt-in flag most application and test code never
sets. The upstream ringbuf question
(`docs/upstream-ringbuf-question.md`) is still worth its answer, but Phase
5 (headless triangle) is **not** blocked on it the way the roadmap said.

**Does not, yet:** prove an actual draw call works. This probe deliberately
recorded zero draws, to isolate the render-pass-entry hazard (the one just
retired) from rasterization, IDVS, or pipeline correctness, which are
untested here and could still surface their own first-time-on-this-device
issues. That is the next, separate step - not taken in this pass, and not
to be taken without a fresh check-in given what a real fault costs here.

### Why this stayed hidden until now

Two things share the blame. First, "rendering is blocked on the ringbuf"
was true when it was concluded - it was written *before* the driver's own
submit-time gate existed in its current form, and the gate was written
conservatively (block all render-work resource requests) rather than
precisely (block only the ones that touch the actual hazard), which was the
reasonable choice at the time given how little was known. Second, nobody
had reason to re-derive the conclusion once it was load-bearing elsewhere
in the docs and roadmap - it read as settled. The lesson generalises past
this one finding: a conclusion that was correct when written can become
stale as the surrounding code changes shape, and the way to catch that is
re-reading the source behind a claim, not re-reading the claim.

## A real triangle renders correctly on kbase

Same session, immediately after the finding above, with the same
`--i-know-it-hangs` caution: `tests/render_triangle_probe` is the actual
draw call that `render_clear_probe` deliberately did not attempt. A vertex
shader (hardcoded positions, indexed by `gl_VertexIndex`, no vertex
buffers) through this driver's real compiler for the first time on a
graphics stage, `vkCmdDraw(3, 1, 0, 0)`, IDVS, tiling, and a fragment
shader (hardcoded magenta, no descriptor sets) - a 16x16 render target,
triangle covering roughly the lower-left half with margin so a correct
result has to show partial coverage, not all-or-nothing.

**It rendered correctly. Twice, reproducibly, device fully healthy after**
- confirmed by re-running both `driver_compute_probe --fill` and
`render_clear_probe` clean immediately after. Readback: 190 clear-colour
pixels, 66 triangle-colour pixels, **zero pixels holding anything else** -
no garbage, no stale memory, no blended-edge artifacts, an exact two-colour
result with real geometric coverage. `vkCreateGraphicsPipelines` compiled
both shaders (Bifrost, not the precompiled path
`tests/driver_compute_probe` used), `vkQueueSubmit` was accepted by the
loosened gate, and the fence signalled from the GPU.

This is the first triangle this project has ever rendered, and the first
real graphics-pipeline execution on kbase in this repo. Same discipline as
above: this proves a non-`simul_use` draw with no descriptors and no
vertex buffers works. It does not yet prove descriptor sets, push
constants, textures, depth/stencil, multiple draws in one render pass, or
anything Vulkan-conformance-shaped - each of those is its own
first-time-on-this-device unknown, not implied by this result.

## Vertex attribute fetch also works, and matches the hardcoded result exactly

Same session, next single variable: `tests/render_vbo_probe` is
`render_triangle_probe`'s exact triangle, changed in exactly one way -
positions come from a real bound `VkBuffer` (`vkCmdBindVertexBuffers`, a
real `VkVertexInputBindingDescription`/`VkVertexInputAttributeDescription`)
instead of `gl_VertexIndex` into a shader-hardcoded array. Worth doing
separately: the hardcoded-position trick `render_triangle_probe` used is a
rare idiom, not how real applications draw, so vertex fetch was still an
open unknown even with a triangle already proven.

Ran twice on the Poco X8 Pro. Both times: **190 clear-colour, 66
triangle-colour, 0 other pixels - an exact match with
`render_triangle_probe`'s result on the same geometry.** That match is
itself a cross-check, not just a pass/fail: the probe prints both counts
and compares them, so a subtly-wrong vertex fetch (an off-by-one stride, a
wrong format, reading the wrong buffer) would likely have produced a
different but still "valid-looking" split rather than an exact match.
Device confirmed healthy after via `driver_compute_probe --fill` and
`render_triangle_probe`, both clean.

Three real hardware-risk probes in a row now, all clean on the first
attempt: render-pass entry, a full draw, and vertex fetch. Still
unexercised, per the list above: descriptor sets, push constants,
textures, depth/stencil, multiple draws in one pass.

## Push constants reach a graphics-stage fragment shader too

Fourth probe, same session, one more variable: `tests/render_push_probe`
takes `render_vbo_probe`'s exact triangle and changes only the fragment
shader's colour source, from hardcoded to a push constant
(`layout(push_constant) uniform PushConstants { vec4 color; } pc;`,
`vkCmdPushConstants` with `VK_SHADER_STAGE_FRAGMENT_BIT`). Worth checking
separately from `driver_pipeline_probe`'s already-proven compute push
constants: the graphics pipeline is different code (no IDVS, no tiler, no
fragment stage on the compute side), so that result implied nothing here.

The check is stricter than the earlier probes': it compares against the
*specific value pushed at record time* (`66cc33ff`, deliberately not the
magenta earlier probes used), not a hardcoded expectation living
separately in the file - so a fragment shader that silently fell back to
some other value (a stale register, zero-initialised memory, a leftover
from a previous run) would show up as a mismatch rather than an accidental
pass.

Ran twice. Both times: 190 clear-colour, 66 pushed-colour, 0 other -
again an exact match with the earlier probes' split on the same geometry,
and the pushed colour came back byte-exact. Device confirmed healthy after
via `driver_compute_probe --fill` and `render_vbo_probe`, both clean.

Four hardware-risk probes run this session, four clean on the first
attempt: render-pass entry, a full draw, vertex fetch, push constants.
Still unexercised: descriptor sets, textures, depth/stencil, multiple
draws in one render pass.

## Descriptor sets work in a graphics pipeline too - the last basic mechanism

Fifth probe, same session: `tests/render_ubo_probe` takes
`render_push_probe`'s triangle and swaps the push constant for a uniform
buffer read through a real descriptor set -
`VkDescriptorSetLayout`/`VkDescriptorPool`/`VkAllocateDescriptorSets`/
`VkUpdateDescriptorSets`/`vkCmdBindDescriptorSets`, the full allocation and
binding path, not a shortcut. This was the last basic plumbing mechanism
this port had not exercised in a graphics pipeline - a texture-sampling or
transform-matrix shader needs the same binding machinery with a different
descriptor type, not a new mechanism, so this was the gating unknown for
"does anything resembling a real shader work."

Same cross-check discipline as the push-constant probe: checked against
the specific colour written into the UBO (`3399ccff`, a third distinct
value from every earlier probe), not a hardcoded expectation. Ran twice.
Both times: 190 clear-colour, 66 UBO-colour, 0 other - the same exact
split every probe on this geometry has gotten, and the UBO's specific
colour came back byte-exact. Device confirmed healthy after via
`driver_compute_probe --fill` and `render_push_probe`, both clean.

**Five hardware-risk probes run this session, five clean on the first
attempt:** render-pass entry, a full draw, vertex fetch, push constants,
descriptor sets. Every basic Vulkan plumbing mechanism a simple textured,
transformed shader would need is now proven except sampled images
specifically and multi-attachment/depth state. Still unexercised: textures
(a `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` binding, not fundamentally
different from what this probe just proved, but untested), depth/stencil,
multiple draws in one render pass.

## Texture sampling works, but only through TRANSFER_SRC_OPTIMAL - and one part of this is still unexplained

Sixth probe, same session: `tests/render_texture_probe` swaps
`render_ubo_probe`'s uniform buffer for a combined image sampler bound to
a real 1x1 texture. This is the first probe this session that did **not**
pass on the first attempt, and the honest account of both the failure and
the fix - including the part that is still not understood - is worth
having in full.

### First attempt: a clean failure, not a hang

Upload sequence was `UNDEFINED -> TRANSFER_DST_OPTIMAL` (copy in) `->
SHADER_READ_ONLY_OPTIMAL`, a spec-legal single-step transition. No hang,
no fault, `vkQueueSubmit` accepted, the fence signalled - but the readback
came back **190 clear-colour, 0 texture-colour, 66 "other," all zero**
(`00000000`). Confirmed device health immediately after
(`driver_compute_probe --fill` and `render_ubo_probe`, both clean) and
checked `adb logcat -d -s MESA` for any driver warning - none. The
operation completed successfully by the driver's own accounting and
produced a wrong answer, which is a different and in some ways more
concerning class of result than anything else this session: every earlier
failure mode in this repo has either hung the device or been refused
outright. A silent wrong answer is the one a test suite can miss.

### The fix

Added an intermediate `TRANSFER_SRC_OPTIMAL` stage between the upload and
the shader-read transition - `TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL
-> SHADER_READ_ONLY_OPTIMAL`, an extra `vkCmdPipelineBarrier` that has no
obvious reason to be required by the Vulkan spec. With it: **190
clear-colour, 66 texture-colour, 0 other - an exact match with every
earlier probe, reproduced twice.** The texture's specific uploaded colour
(`e63399ff`) came back correctly through the shader both times.

### What is still not understood

The intermediate stage was added alongside a diagnostic - copying the
texture straight back out to a host-visible buffer immediately after
upload, before it is ever sampled, to bisect "the upload did not reach the
image" from "sampling is wrong." That diagnostic **still reads back
`00000000` on every run**, even in the passing version where the shader
correctly samples `e63399ff` moments later in the same command buffer.

So there are, reproducibly, two different answers to "what does the
texture hold" depending on which GPU path asks: `vkCmdCopyImageToBuffer`
says zero, the fragment shader's `texture()` sampler says correct. Both
read the same image, same layout at time of read
(`TRANSFER_SRC_OPTIMAL`/`SHADER_READ_ONLY_OPTIMAL` respectively, each
reached by its own correctly-ordered barrier from the same upload). This
was not chased to a root cause - the primary result (real shader sampling
is correct and reproducible) was the thing worth having, and further
device-side iteration to fully explain a secondary diagnostic anomaly
is exactly the kind of open-ended debugging this repo's own history
warns against doing live without a specific reason to keep going.

Two live hypotheses, neither confirmed: a cache/coherency gap specific to
reading a just-written image back through the transfer-copy path on this
device (as opposed to the texture-sampling path, which may go through
different cache handling), or a bug in the diagnostic itself unrelated to
the real fix. Worth revisiting if a future texture probe needs the
transfer-readback path to work (e.g. reading a render target back as a
texture within the same command buffer) rather than only the
sample-in-a-shader path this probe actually needed.

**Six hardware-risk probes run this session; five clean on the first
attempt, one (this one) genuinely wrong on the first attempt and fixed
with a specific, reproducible, but not fully explained change.** Recorded
as found rather than smoothed over, because "add a barrier that
shouldn't be necessary" is exactly the kind of workaround that needs to
stay visible if it turns out to matter for the render descriptor ringbuf
work or anything else that moves data between transfer and shader access
on this device.

## Depth test and depth write work, clean on the first attempt

Seventh probe, same session: `tests/render_depth_probe` adds a real depth
attachment to `render_vbo_probe`'s triangle - `VkPipelineDepthStencilState`
with `depthTestEnable`/`depthWriteEnable` true and `depthCompareOp =
LESS`, a `D32_SFLOAT` attachment cleared to `1.0`. The Z-test unit and
depth write-back had no prior art in this repo at all - `docs/kbase-notes.md`
had flagged this as untested territory before this session started.

Learned from the texture probe immediately before it: went through an
explicit `TRANSFER_SRC_OPTIMAL` intermediate stage for *both* attachments'
readback from the start, rather than assuming a direct
`ATTACHMENT_OPTIMAL -> TRANSFER` transition would work - since that
assumption had just failed once, for a different attachment type, in the
same session.

Checked more strictly than any earlier probe: not just colour output, but
the depth buffer itself, read back independently. Both matched exactly,
both times run: **190 far-depth (`1.0`, the untouched clear value) + 66
near-depth (`0.0`, the triangle) + 0 anything else** - the identical split
colour rendering has produced on this geometry all session, now
independently confirmed by a second hardware path. The Z-test unit wrote
precisely where the rasterizer covered and nowhere else.

Clean on the first attempt - no repeat of the texture probe's failure.
Device confirmed healthy after via `driver_compute_probe --fill` and
`render_texture_probe`, both clean.

**Seven hardware-risk probes run this session: six clean on the first
attempt, one (texture sampling) genuinely wrong on the first attempt and
fixed with a documented, only-partially-understood change.** Between them:
render-pass entry, a full draw, vertex fetch, push constants, descriptor
sets, texture sampling, depth test/write.

## Multiple draws in one render pass work - the basic-plumbing list is now empty

Eighth probe, same session, and the last item on the list above:
`tests/render_multidraw_probe` draws two non-overlapping triangles in one
render pass with one `vkCmdBeginRendering`/`vkCmdEndRendering` pair - one
bound vertex buffer holding both triangles' vertices, one bound pipeline,
two `vkCmdPushConstants`/`vkCmdDraw` pairs with a different colour pushed
before each. Built entirely from two already-proven mechanisms (vertex
buffers, push constants) combined in a shape neither earlier probe tested:
twice, with a state change in between.

What this isolates: does pipeline/vertex-buffer binding state persist
correctly across two separate `vkCmdDraw` calls, does a state change
between draws (the push constant) actually take effect for the second draw
rather than leaking the first draw's value or failing to apply, and does a
non-zero `firstVertex` on the second draw correctly fetch its own vertices
rather than reusing the first draw's.

Clean on the first attempt, reproduced twice. Both runs: **184 clear + 66
triangle-A + 6 triangle-B + 0 other.** Triangle A's count is the exact
66-pixel baseline every probe on this geometry has produced since
`render_vbo_probe` - present here as a cross-check that the first draw's
result is completely unaffected by a second draw following it in the same
pass. Triangle B - new geometry, in the opposite corner, a sixth distinct
colour from every earlier probe's - came out present, correctly coloured,
and non-overlapping with A. Zero pixels held anything else, meaning
draw 2's push constant did not bleed backward into draw 1's already-shaded
pixels. Device confirmed healthy after via `driver_compute_probe --fill`
and `render_depth_probe`, both clean.

**Eight hardware-risk probes run this session: seven clean on the first
attempt, one (texture sampling) genuinely wrong on the first attempt and
fixed with a documented, only-partially-understood change.** Between them:
render-pass entry, a full draw, vertex fetch, push constants, descriptor
sets, texture sampling, depth test/write, multiple draws per pass. That is
every basic Vulkan plumbing mechanism a real, conformance-shaped shader
needs. What is left is CTS scale (Phase 7) - dEQP-VK, real applications,
extensions - not basic-plumbing scale; the open question that still gates
some of it is `SIMULTANEOUS_USE` rendering, which remains blocked on the
ringbuf and the still-unanswered upstream question.

## deqp-vk runs against this driver, through a purpose-built ICD shim

The problem CTS integration had to solve: `deqp-vk` (and any standard
Vulkan loader) expects a driver `.so` to export `vkGetInstanceProcAddr`
directly. This driver, like every Android Vulkan driver, instead exports
Android's hwvulkan HAL ABI - a single `HMI` symbol, `dlsym`'d and opened
through `hw_module_t`/`hw_device_t` methods to reach the real entrypoints
(see the "Android hwvulkan HAL ABI" note earlier in this file). Installing
the driver as the system's actual Vulkan HAL to let Android's real loader
bridge that gap was ruled out deliberately - not something to do to a real
device's system partition for test purposes.

`src/tests/icd_shim/panvk_kbase_icd_shim.c` closes the gap instead: a small
shared library that does exactly what Android's own `libvulkan.so` does
internally - `dlopen`s the real driver by path (default
`/data/local/tmp/libvulkan_panfrost.so`, overridable via
`PANVK_KBASE_ICD_DRIVER`), walks the HAL open() sequence once, and re-
exports a standard `vkGetInstanceProcAddr` that forwards to it. Verified
against VK-GL-CTS's own source
(`framework/platform/android/tcuAndroidPlatform.cpp`): its `VulkanLibrary`
does exactly one thing with a driver path - `dlsym("vkGetInstanceProcAddr")`
- no ICD manifest, no loader-negotiation handshake required for this code
path. Checked standalone first with `tests/icd_shim/icd_shim_probe.c` (only
calls entrypoints already proven safe elsewhere this session), clean on
device, before touching CTS at all.

Built `deqp-vk` for Android from a fresh `--depth 1` clone of
`KhronosGroup/VK-GL-CTS` (`third_party/VK-GL-CTS/`, gitignored) via its own
CMake/Ninja Android build (`-DDEQP_TARGET=android
-DDEQP_TARGET_TOOLCHAIN=ndk-modern -DDEQP_ANDROID_EXE=ON`, targeting the
same NDK used for this repo's own builds). Configure and the `deqp-vk`
target build (1455 objects) both completed clean. Stripped 1.0GB → 69MB
(`llvm-strip`) before pushing to device - the unstripped binary carries
full debug info for every one of CTS's ~forty-some Vulkan test modules
(ray tracing, video, mesh shaders, etc.), none of which this device
exercises.

Ran through the shim, library path selected via `deqp-vk`'s own
`--deqp-vk-library-path` flag (no source patch needed - this is a standard,
documented CTS option):

```
deqp-vk --deqp-case='dEQP-VK.info.*' \
        --deqp-vk-library-path=/data/local/tmp/libpanvk_kbase_icd_shim.so
```

`dEQP-VK.info.build` (compile-time constants only, no Vulkan calls) passed
first, smallest possible case. Escalated to the full `dEQP-VK.info.*` group
next (19 cases - device/instance property and extension queries, no
rendering or dispatch): 15 passed, 2 failed on genuine spec-conformance
gaps worth recording (`device_extension_dependencies`:
`VK_EXT_hdr_metadata` missing a dependency it declares;
`instance_extension_dependencies`: `VK_EXT_headless_surface` likewise;
`device_memory_budget_multi_instance`: heap usage not observed to increase
- plausible given this device's unified memory), 1 correctly reported
`NotSupported` (device groups - single GPU, as expected), then **one
crashed the test binary itself: `dEQP-VK.info.platform`, SIGSEGV.**

Root-caused, not just observed: `deqp-vk`'s Android build has no
standalone-executable-specific platform implementation - `createPlatform()`
(`framework/platform/android/tcuAndroidPlatform.cpp:668`) unconditionally
constructs `tcu::Android::NativeActivity activity(NULL)`, i.e. wraps a null
`ANativeActivity*` even when `DEQP_ANDROID_EXE=ON` and there is no real
Activity. `dEQP-VK.info.platform` calls `describePlatform()`, which passes
that null pointer into `tcu::Android::describePlatform()` unchecked. This
is a gap in upstream CTS's own Android-EXE support, not a defect in this
driver or the shim - confirmed by the fact that every case before and
after it in the same run, through the same shim, against the same driver,
behaved exactly as its own spec-conformance answer predicts. Device
confirmed healthy after via `driver_compute_probe --submit --fill` (clean,
GPU-signalled fence, correct readback) - a process crash, not a GPU hang;
nothing about it resembled the D-state hangs this repo's own probes have
hit before.

**This is the first time any code from outside this repo has run against
this driver.** `dEQP-VK.info.*` doesn't exercise rendering or compute
dispatch, so it doesn't yet corroborate the render-probe or compute-probe
findings above - what it corroborates is the integration path itself: a
standard, unmodified Vulkan test consumer, using only the documented
loader/ICD contract, reaching this driver through nothing but the shim and
getting back real, differentiated answers (some pass, some genuine
conformance fails, one platform-layer crash unrelated to the driver).

## The first external rendering test passes: dEQP-VK.api.smoke.*

`dEQP-VK.api.*` alone contains **267,166** cases (confirmed by dumping the
case tree with `--deqp-runmode=stdout-caselist` before running anything -
not a number to discover by attempting to run it unattended). Rather than
starting a run that size blind, used CTS's own purpose-built entry point
for exactly this situation: `dEQP-VK.api.smoke.*`, 6 cases, meant to be the
first thing run against any new Vulkan implementation.

```
dEQP-VK.api.smoke.asm_triangle                 Pass (Rendering succeeded)
dEQP-VK.api.smoke.asm_triangle_no_opname       Pass (Rendering succeeded)
dEQP-VK.api.smoke.create_sampler               Pass (Creating sampler succeeded)
dEQP-VK.api.smoke.create_shader                Pass (Creating shader module succeeded)
dEQP-VK.api.smoke.triangle                     Pass (Rendering succeeded)
dEQP-VK.api.smoke.unused_resolve_attachment    Pass (Rendering succeeded)
```

**6/6 pass, first attempt.** Four of the six actually render a triangle and
read back the framebuffer, through CTS's own shader-compilation and
pipeline-construction code - entirely independent of every render probe
this repo wrote earlier in the session. This is the first external,
standard-conformance-suite confirmation that this driver's rendering path
works, not just this repo's own hand-written probes reporting success.
Device confirmed healthy after via `driver_compute_probe --submit --fill`
(clean).

Next: broader non-rendering suites first (`dEQP-VK.api.*` scoped to
sub-groups rather than the whole 267k-case tree at once, `dEQP-VK.query_pool.*`
and similar), each run and checked before the next, `dEQP-VK.info.platform`
excluded from future runs as a known upstream-CTS gap rather than
re-triggered every time.

## dEQP-VK.api.* is 267,166 cases; object_management surfaces a real resource-limit finding

Before running any of `dEQP-VK.api.*` at scale, dumped its case tree
(`--deqp-runmode=stdout-caselist`) to see what "the next suite" actually
means size-wise, rather than discovering it mid-run:

```
201346  dEQP-VK.api.copy_and_blit          (huge format/size parameter sweep)
 45636  dEQP-VK.api.image_clearing         (huge format/size parameter sweep)
 10481  dEQP-VK.api.info                   (per-extension query sweep)
  3076  dEQP-VK.api.buffer
  1320  dEQP-VK.api.ds_color_copy
  1004  dEQP-VK.api.buffer_view
   457  dEQP-VK.api.object_management
   ...  (long tail of small groups)
```

`copy_and_blit` and `image_clearing` alone are 246,982 of the 267,166 -
deliberately deferred, not attempted blind. Picked
`dEQP-VK.api.object_management` (457 cases: create/destroy lifecycle
across every core object type - buffers, images, pipelines, descriptor
sets, devices, etc.) as a bounded, foundational next step.

Ran it; the run **aborted at case 67 of 457**, at
`max_concurrent.device`:

```
Passed:        61/66 (92.4%)
Failed:        1/66 (1.5%)
Warnings:      4/66 (6.1%)
Test run was ABORTED!
```

The 4 warnings (`QualityWarning: Allocation callbacks not called`, on
`descriptor_set_layout_*` and `pipeline_layout_*`) are benign and expected
- this driver doesn't route those allocations through the app-supplied
`VkAllocationCallbacks`, which CTS flags as a quality note, not a
correctness failure.

The abort: `max_concurrent.device` is a stress test that creates `VkDevice`
objects in a loop until creation fails, to find the practical concurrent-
device limit. It hit `VK_ERROR_OUT_OF_DEVICE_MEMORY`
(`vktCustomInstancesDevices.cpp:456`) - a spec-legal error response, but
CTS categorizes running out of resources mid-stress-test as
`ResourceError`, which is treated as fatal to the *whole test run* (later
cases can't be trusted once a resource leak or exhaustion state is
possible), not just a failure of that one case. This is a genuine
data point - either this device's concurrent-VkDevice ceiling is lower
than CTS expects, or there's a real leak somewhere in the kbase-backed
device-creation path - not yet distinguished, and not chased further this
session. Device confirmed healthy afterward via
`driver_compute_probe --submit --fill` (clean) - this was a resource
ceiling hit deliberately by a stress test, not a hang or crash.

Next: re-run `object_management` with `max_concurrent.device` excluded to
get the remaining ~390 cases' results, and note whether `max_concurrent.*`
as a category (stress/limit-finding tests) needs the same
run-in-isolation treatment as `dEQP-VK.info.platform`.

## A real finding: a hard ~307-case ceiling on cumulative VkInstance/VkDevice creation

Followed up on the `max_concurrent.device` abort by excluding it and
re-running - `max_concurrent.device_group` hit the identical
`VK_ERROR_OUT_OF_DEVICE_MEMORY` at `vktCustomInstancesDevices.cpp:456`.
Excluded both, and every other `*.device`/`*.device_group` leaf across the
whole 457-case group (16 total - most of `object_management`'s subgroups
each have their own `device`/`device_group` leaf, since most of these
tests spin up a fresh custom `VkInstance`+`VkDevice` per
`vktCustomInstancesDevices.cpp`, not the shared default device), then
re-ran the remaining 441.

**That run aborted too - not on a `device` leaf at all, but on
`private_data.buffer_storage_large`, at exactly 307 cases passed.**
Excluded that specific case and ran again: **aborted again, at exactly the
same 307-case count**, this time on the very next case in sequence
(`private_data.buffer_storage_small`) rather than a different one.

That's the load-bearing detail: **the failure point is a fixed case
count, not a fixed test name.** A slow memory leak whose per-case leak
size varies (small buffers vs. large ones, single-threaded vs.
multithreaded resource creation) would exhaust at a point that shifts
depending on which cases ran and in what order. A count that lands on
307 twice in a row, regardless of which case happens to be case 308,
looks instead like a **fixed-size resource table or slot count** being
filled - each `object_management` test case that isn't excluded creates
its own throwaway `VkInstance`+`VkDevice` pair
(`vktCustomInstancesDevices.cpp`'s pattern), and something in this
driver's or kbase's device/instance teardown path is not releasing
whatever that fixed resource is (plausible candidates, not yet checked:
kbase context slots, an fd class with a low `RLIMIT`, a fixed-size handle
table in `pan_kmod_kbase.c` or the CSF queue-group allocator) before the
next `VkDevice` tries to claim one.

**Not yet root-caused** - this would need instrumenting the actual
resource in question (likely starting with `lsof`/`/proc/<pid>/fd` count
on the `deqp-vk` process across the run, and checking whether kbase
context creation ioctls succeed/fail at the same point) rather than
guessing further from CTS's black-box error message. Recorded here as a
genuine, reproducible driver-quality finding from CTS - not a hang, not a
crash, and confirmed not to affect the GPU/device itself
(`driver_compute_probe --submit --fill` stayed clean throughout, since
each probe run is a fresh process). This is exactly the kind of defect
CTS work exists to surface, and is a better next investigation than
grinding through more of the case tree with the same leak silently
capping every future run's coverage at ~300-some cases.

With those 17 known-bad leaves (16 `device`/`device_group` variants + the
one `private_data` case that happened to land on the ceiling) excluded,
**306/312 passed, 4 quality warnings (benign, allocation-callback
routing), 1 boundary case left aborting the tail of the run** - the
remaining ~145 untested `object_management` cases are simply past where
the ceiling bites, not independently interesting until the ceiling itself
is understood.

## Root-cause dig: what the ~307-case ceiling is NOT

Built `tests/device_churn_probe` to isolate the create/destroy cycle from
everything else `deqp-vk`'s `object_management` cases do, and tested each
suspect mechanism in controlled isolation, well past 307 each time. All
of the following completed clean, with no `VK_ERROR_OUT_OF_DEVICE_MEMORY`
and no fd growth:

- **500 bare `vkCreateInstance`→`vkCreateDevice`→`vkDestroyDevice`→
  `vkDestroyInstance` cycles, sequential.** `mali0` fd count stayed at 0
  throughout (this minimal device never even opens a persistently-visible
  `/dev/mali0` handle by the time `readlink` samples it - the fd is short-
  lived within `pan_kmod_dev_create`/`dev_destroy`, not held open the way
  the earlier live-sampled CTS run showed 2-18 held `mali0` fds - itself
  a useful data point that a "thin" device leaves less resident state
  than the real CTS ones).
- **400 cycles adding `vkGetDeviceQueue` + create/destroy a command pool
  and a primary command buffer each cycle** (`--use-queue`) - the first
  place kbase-specific CSF group/queue allocation could plausibly happen
  even without a submit. Clean.
- **400 cycles adding a real `vkCreateBuffer`→`vkAllocateMemory`→
  `vkBindBufferMemory`→free round trip** (`--alloc-buffer`, combined with
  `--use-queue`) - a genuine `KBASE_IOCTL_MEM_ALLOC_EX`/`MEM_FREE` pair
  through `kbase_kmod_bo_alloc`/`kbase_kmod_bo_free`
  (`src/mesa/pan_kmod_kbase.c`), not just the FIXED_VA probe device
  creation itself already does. Clean.
- **8 threads × 50 devices concurrently** (`--threads 8`), matching what
  `object_management`'s `multithreaded_per_thread_device` group actually
  stresses (many threads each creating their own device at the same
  time). Clean, 400/400 - once the test harness's own bug was fixed (see
  below).

One genuine bug found and fixed along the way, in the *test probe*, not
the driver: the ICD shim's (`tests/icd_shim/`) lazy `init_once()` has no
locking around its first-caller-wins guard. Multiple threads racing to
call `vkGetInstanceProcAddr(NULL, ...)` as their simultaneous first call
hit a real `g_hwdevice` torn-write race and SIGSEGV'd immediately. Real
`deqp-vk` never hits this - it bootstraps the loader once, single-
threaded, before any test spawns worker threads - so the probe was fixed
to pre-warm the shim from the main thread before spawning workers, which
correctly matches real CTS behavior rather than mistaking a test-harness
artifact for a driver finding. (The shim itself was left as-is: every
prior probe using it was single-threaded by design, so hardening its
init for concurrent first-callers isn't warranted just for this one
diagnostic tool - noted here so it isn't mistaken for a live issue if
this file is read out of context.)

**What none of this rules out yet:** every scenario above repeats the
*same small pattern* many times, always freeing before moving on. The
CTS groups that actually precede the failure - `max_concurrent.*` and
`multiple_*` - do something structurally different: they hold **many
objects simultaneously alive on one device** (e.g.
`max_concurrent.buffer_storage_large` allocates buffers in a loop until
it finds the practical concurrent limit, then frees them all at the
end), not one object created and freed before the next begins. That
pattern - sustained high live-object count, not creation *count* over
time - is the leading remaining candidate and hasn't been tested in
isolation yet.

**Open-source research (asked for explicitly): this is not a previously-
solved problem.** Searched for prior art on three fronts:

1. Whether Mesa/PanVK upstream has ever dealt with this: no - upstream
   PanVK only targets the DRM-based `panfrost`/`panthor` kernel drivers.
   Arm's kbase is Arm's out-of-tree, non-DRM proprietary driver; there is
   no upstream Mesa kbase backend to compare against, because this repo's
   `pan_kmod_kbase.c` *is* that backend, written from scratch this
   project. There is nothing to diff against.
2. Whether the Poco X8 Pro's own kernel source (which would let this be
   checked against the actual GPL-licensed kbase kernel driver for this
   exact device/firmware) is available: as of this session, no - Xiaomi
   has not yet published kernel sources for this device
   (`MiCode/Xiaomi_Kernel_OpenSource` issue #40922 is an open request for
   exactly this). Public Mali kbase kernel source confirms Bifrost/Valhall
   GPUs multiplex many contexts (`kctx`) onto a small number of hardware
   address spaces (historically up to 16) via context scheduling - a real
   kbase concept, but it doesn't line up numerically with a ~307-case
   ceiling (16 vs. 307 is too large a gap to be the same limit unless
   something is also failing to release AS slots on a very slow leak,
   which is speculative, not confirmed).
3. Whether other community kbase-based driver projects have hit this: the
   one found (`SolDev69/panfrost-gallium-mesa`, Panfork-derived, "works on
   the kbase driver") is Gallium/OpenGL only, not Vulkan/PanVK - a
   different userspace code path against the same kernel driver, and its
   documentation has no mention of context-churn limits either.

Conclusion: this is genuinely unexplored territory, not a known-and-fixed
issue elsewhere to port a fix in from. The next concrete step is testing
the "many simultaneously-live objects on one device" pattern directly
(extend `device_churn_probe` or write a new probe that allocates N
buffers on one device without freeing them until the end, N large enough
to bracket where `max_concurrent.buffer_storage_large` itself would stop)
rather than continuing to vary the create/destroy-churn pattern, which is
now well-covered ground.

## Root-cause dig, second pass: many-simultaneous-objects also comes up clean

Extended `device_churn_probe` with `--many-objects N` (N buffers, each
with its own real memory allocation, all held alive at once per device
before freeing) and `--many-pipelines N` (N compute pipelines from one
shader module, all held alive at once). Both ran well past any scale
`object_management`'s own stress tests plausibly reach:

- **250 devices × 500 simultaneous buffers = 125,000 cumulative
  allocations.** Clean, 250/250, no fd growth, no failure.
- **350 devices × 20 simultaneous compute pipelines = 7,000 cumulative
  pipeline creations.** Clean, 350/350.
- **1 device × 1,000 simultaneous compute pipelines.** Clean - but
  completed in 0.04s, which is far too fast for 1,000 independent
  NIR→Bifrost compiles. This probe reuses one shader module and one
  pipeline layout for every pipeline in the loop (only the source's own
  choice to keep the test simple), so this result is likely showing an
  internal pipeline/shader cache deduplicating identical compiles rather
  than genuinely exercising 1,000 independent `EXEC_VA` allocations.
  **Flagged honestly rather than counted as a clean result** - this
  specific test doesn't prove what it set out to prove, and would need
  per-iteration shader/layout variation (or an explicit disabled
  `VkPipelineCache`) to actually stress independent executable-BO
  allocation at scale.

Pipelines were the leading candidate going into this pass specifically
because `kbase_kmod_bo_free`'s own comment calls out that executable BOs
(kbase's `EXEC_VA` zone, kernel-chosen address) are freed differently
from ordinary buffers - not returned to this backend's VA heap tracking
the way a fixed-address buffer is. That asymmetry is still real and still
unverified at scale, just not disproven or confirmed by this pass.

**State after two passes: every controlled, synthetic reproduction
attempt has come up clean**, including patterns that structurally match
what `object_management`'s own groups do (many-simultaneous-objects,
many-simultaneous-pipelines, concurrent multi-threaded device creation,
real memory alloc/free churn). Two explanations remain open, and neither
is confirmed:

1. The trigger needs the *specific mix* CTS produces - many different
   object types, extensions, and `pNext` chains varying test-to-test in
   one process - not any single repeated pattern a synthetic probe can
   easily produce.
2. The trigger needs something no probe here has replicated at all, e.g.
   different queue-family/queue-count combinations per device,
   `VK_EXT_private_data`'s slot-request `pNext` chain specifically (the
   case that actually failed), or genuinely defeating any internal
   pipeline cache the way case 1's caching artifact above suggests may be
   needed.

**Recommended next step, changing approach rather than trying more
synthetic variants:** instrument the real driver directly (temporary
`mesa_logi`/counter prints in `kbase_kmod_dev_create`/`_destroy`,
`kbase_init_va_heap`, and `kbase_kmod_bo_alloc`/`_free` in
`src/mesa/pan_kmod_kbase.c`) and re-run the actual failing
`object_management` caselist against that instrumented build, rather than
continuing to guess the reproduction shape with hand-written probes. The
real run already reliably reproduces the failure at a known, fixed point
(case 307/308) - instrumenting it directly will show what's actually
happening there far faster than further synthetic reproduction attempts.

## Root-cause dig, third pass: the real driver, instrumented - found the exact mechanism

Added counters and step-by-step logging to `src/mesa/pan_kmod_kbase.c`
(device/BO create-destroy, per-candidate FIXED_VA probing) and
`src/mesa/panvk_vX_kbase_queue.c` (group/subqueue/tiler-heap
create-destroy - real kbase kernel objects a synthetic buffer/pipeline
probe never touches, since every `vkCreateDevice` that requests a queue
triggers this file's `create_kbase_queue()` automatically). Gated behind
`PANVK_KBASE_DEBUG_COUNTERS=1`, silent otherwise - matches this file's
existing `PANVK_KBASE_FIXED_VA_BASE` convention. Uses atomics: CTS's
`multithreaded_*` groups create devices from multiple threads at once,
and a torn counter would make the log lie about what was actually live
at the moment of a real failure.

One build wrinkle worth recording: Mesa's `mesa_logi()` routes to Android
logcat by default on this platform (`MESA_LOG_CONTROL_ANDROID`,
`src/util/log.c`), not stderr - the instrumentation was silent on the
first attempt until `MESA_LOG=file` was also set, which routes it to
`stderr` (captured the same way as everything else in this repo's
probes). Worth remembering for any future driver-side debug logging in
this repo, not just this instrumentation.

Rebuilt `libvulkan_panfrost.so` via `wsl-build-android.sh` (both scripts
have pre-existing CRLF line endings from the Windows checkout, worked
around with `tr -d '\r'` piped into `bash`, not committed as a fix since
`git status` shows no modification, and it may work fine on a native
Linux/WSL clone), deployed it to the on-device path the ICD shim already
expects, and confirmed the instrumented build has zero behavioral
difference from the uninstrumented one with the env var unset (clean
`driver_compute_probe --submit --fill`).

Re-ran the exact caselist that reliably fails
(`objmgmt_filtered3.txt` - `object_management` minus the
already-known-bad `device`/`device_group` leaves) with
`PANVK_KBASE_DEBUG_COUNTERS=1 MESA_LOG=file`. The failure, captured with
full instrumentation:

```
Test case 'dEQP-VK.api.object_management.private_data.buffer_storage_large'..
kbase-dbg: dev_create #792 entry, fd=6, live_devices=2
kbase-dbg: va_heap candidate 0 (0x800200000000) accepted
kbase: CSF iface v3.6.0, 8 CSG slots x 8 streams
kbase-dbg: dev_create #792 SUCCESS, live_devices=3
kbase: MEM_ALLOC_EX at 0x8003fffff000 failed: Out of memory
kbase-dbg: dev_destroy, fd=6, owns_fd=1, live_devices=2
  ResourceError (... VK_ERROR_OUT_OF_DEVICE_MEMORY at vktCustomInstancesDevices.cpp:456)
```

**This changes the picture substantially - three findings that weren't
visible from the black-box CTS output alone:**

1. **The device creation itself succeeds.** `kbase_kmod_dev_create()`
   completes cleanly - the VA heap FIXED_VA probe, the CSF interface
   query, everything. The failure is the *first buffer allocation after*
   a successful device creation, at `0x8003fffff000` - the same address
   every fresh device's first allocation gets in every clean run this
   session has produced (confirmed in the `device_churn_probe` logs
   above). The address itself is unremarkable; the kernel ioctl behind
   it (`KBASE_IOCTL_MEM_ALLOC_EX`) is what returned real `ENOMEM`.
2. **Only 3 devices are live at the moment of failure** - far below what
   this exact instrumented run demonstrably handles cleanly elsewhere:
   grepping the same log for the peak `live_devices` value anywhere in
   the run shows **18**, reached earlier without incident. Live device
   *count* is not the trigger.
3. **It follows immediately after the `multithreaded_*` groups**
   (`multithreaded_per_thread_device`, `multithreaded_per_thread_resources`,
   `multithreaded_shared_resources`), landing on the very first case of
   `private_data`, in a `SingletonDevice` that requests
   `VK_EXT_private_data` with actual private-data-slot counts via a
   `VkDevicePrivateDataCreateInfoEXT` `pNext` chain - the combination of
   "right after multithreaded tests" and "a device configuration nothing
   else in this run or in `device_churn_probe` has exercised" is the
   most specific fact this pass has produced.

**Also directly falsified by this pass:** re-ran `device_churn_probe`'s
plain sequential mode for **1000 iterations in one process** (2000
cumulative `dev_create` calls, confirmed in the instrumented log) -
clean, no failure. The real run fails at cumulative `dev_create` **#792**
- lower than what this synthetic run comfortably passed. So it is not
simply "any device-creation pattern eventually exhausts something after
enough cumulative creations either" - ruling that out too, on top of
everything ruled out in passes one and two.

**Where this leaves it:** the failure is a genuine kernel-level `ENOMEM`
on an otherwise-ordinary buffer allocation, triggered by some state that
`multithreaded_*` (thread-based device/resource creation with real
synchronization, unlike `device_churn_probe --threads`' simpler uniform
worker loop) and/or `private_data`'s slot-requesting `pNext` chain leaves
behind - not visible as an elevated live-device or live-BO count, so
whatever it is isn't tracked by any counter added so far. Two concrete,
narrower next steps, now that the black-box mystery is a specific,
characterized mechanism rather than an unknown one:

1. Add a probe mode that matches `multithreaded_shared_resources`'
   actual shape - multiple threads operating on *one shared* device, not
   each thread owning its own (what `device_churn_probe --threads`
   currently does) - since that specific pattern has not been tested at
   all.
2. Add a probe mode that requests `VK_EXT_private_data` with real slot
   counts via the same `pNext` chain shape CTS uses, in case the
   extension path itself - untested by anything so far - is what
   allocates the exhausted resource.

The `PANVK_KBASE_DEBUG_COUNTERS` instrumentation is committed and stays
in the tree (silent by default) - re-running the caselist against
whichever of the two candidates above is tried next will show the same
level of detail without re-adding logging from scratch.

## Root-cause dig, fourth pass: both specific candidates also ruled out

Tested both candidates from the third pass directly, in isolation, at
large scale, with the same instrumentation on.

**`VK_EXT_private_data`, replicated exactly:** added
`device_churn_probe --private-data` / `--private-data-slots N M`,
matching `SingletonDevice::createPrivateDataDevice`
(`vktApiObjectManagementTests.cpp`) precisely - the same
`VkDevicePrivateDataCreateInfoEXT` chain, the same
`VkPhysicalDevicePrivateDataFeaturesEXT` struct, the extension named, and
`pEnabledFeatures` populated from a real `vkGetPhysicalDeviceFeatures`
query (a device-creation shape nothing tested before this pass had used -
every earlier probe left `pNext`/extensions/features untouched). Checked
the CTS source first rather than guessing which slot config the actual
failing case uses: `createPrivateDataTest` loops through all 5
`SingletonDevice` instances *on the first test case that runs in the
group*, but the instrumented log showed only **one** `dev_create` before
failure - meaning it failed on the very first
(`SingletonDevice` index 0, `requestedSlots[0] = {0, 0}`), which requests
**zero** actual private-data slots. Ran 1000 iterations of exactly that
`{0,0}` configuration in one process (2000 cumulative counting the
instance-probe device too, past the real failure's cumulative count of
792): **clean, 1000/1000.** The BO allocation pattern was byte-for-byte
identical to the plain baseline probe - enabling the extension with zero
slots doesn't even allocate anything extra at the kbase level. This
candidate is ruled out.

**`multithreaded_shared_resources`'s actual shape:** added
`device_churn_probe --shared-threads N --shared-iters M`, matching
`multithreadedCreateSharedResourcesTest` structurally: N threads (real
CTS clamps to `[2, 8]` logical cores) all create+destroy buffers on **one
already-existing shared device** concurrently - not each thread owning
its own device, which is what `--threads` tests and had already come up
clean. Included the same synchronization CTS uses: a spin barrier every
`iterations / 5` operations "to make entering driver at the same time
more likely" (CTS's own comment), maximizing the odds of concurrent
`kbase_kmod_bo_alloc`/`_free` calls actually overlapping in the kernel,
not just in userspace scheduling. After each round, immediately tried
creating one more, completely unrelated fresh device as a direct check:
does *this* pattern leave the kind of residue that breaks a later,
unrelated device creation, matching what the real run showed. Ran 60
rounds × 8 threads × 200 iterations (96,000 concurrent buffer
create/destroy cycles, cumulative `bo_alloc` count past 96,800): **clean,
60/60**, the post-round fresh-device check never failed once. This
candidate is ruled out too.

**Both specific, well-reasoned candidates from the third pass came up
empty.** Four full passes now: fd leak, every device/queue/buffer/
pipeline churn pattern this session could construct (single-threaded and
multi-threaded, sequential and many-simultaneous), and both specific
hypotheses the instrumented real-driver log directly suggested. None
reproduce the failure in isolation.

**What that leaves open, honestly:** the failure may need the *exact*
cumulative sequence of everything that runs before it in the real
caselist - `alloc_callback_fail*`, `max_concurrent.*`,
`multiple_shared_resources`, `multiple_unique_resources`,
`multithreaded_per_thread_device`, `multithreaded_per_thread_resources`,
*then* `multithreaded_shared_resources`, all in that specific order,
touching many different object types (not just buffers, which is all
every probe here has stressed at scale) - not any single isolated
pattern repeated on its own, however large. It's also worth naming a
hypothesis outside this driver entirely, not yet checked: real system
memory pressure on the Android device itself, from something unrelated
to this driver's own resource accounting (background processes, memory
fragmentation from a long-running `deqp-vk` process, an Android-level
cgroup limit) - `KBASE_IOCTL_MEM_ALLOC_EX` returning genuine `ENOMEM`
would look identical either way from userspace, and nothing captured so
far distinguishes "this driver leaked something" from "the system was
genuinely low on memory at that moment for unrelated reasons." Sampling
`/proc/meminfo` (or `dumpsys meminfo`) alongside the instrumented log
during a real caselist run would distinguish the two directly and is the
cheapest next check, if this is picked up again.

Given four passes of directed, hypothesis-driven testing have not
reproduced this outside the real CTS run itself, further work here should
either (a) instrument and re-run the *real* caselist rather than inventing
a fifth synthetic pattern, watching `/proc/meminfo` this time, or (b) be
treated as a known, precisely-characterized-but-unresolved finding and
set aside in favor of broader CTS coverage - the mechanism (kernel
`ENOMEM` on a first post-creation buffer allocation, unrelated to
live-device count, following the `multithreaded_*`/`private_data` region
of the caselist) is now well enough understood to recognize immediately
if it recurs elsewhere.

## Root-cause dig, fifth pass: memory-sampled the real run - not system memory pressure either

Followed the fourth pass's own recommendation instead of inventing a
fifth synthetic pattern: instrumented the *real* failing caselist run
with live `/proc/meminfo` sampling (`MemFree`, `MemAvailable`, `CmaFree`,
`Cached`, `SwapFree`, and the `deqp-vk` process's own `VmRSS`), polled
throughout, correlated against case progress via the same
`PANVK_KBASE_DEBUG_COUNTERS=1 MESA_LOG=file` instrumentation from the
third pass. `CmaFree` specifically - the Contiguous Memory Allocator pool
size, what Mali/kbase GPU allocations typically draw from on this
MediaTek SoC - was the target: this device has no accessible debugfs
(`/sys/kernel/debug/` is present but empty for the shell user, `mali0`'s
own debug directory does not exist) and no kernel log access
(`dmesg`/`klogctl`/`/proc/kmsg` all return `Permission denied` without
root), so `/proc/meminfo`'s `CmaFree` line is the closest thing to a
GPU-memory-specific signal available from userspace on this device.

**First pass at 0.5s resolution found something real and unexpected:**
`CmaFree` crashed from a healthy ~105,000 kB down to **236 kB** - visibly
near-zero - around case 59-79 (during `max_concurrent.*`, which is
exactly the group that stress-tests many simultaneous buffer/image
allocations). That is a genuine, severe CMA crunch, not a measurement
artifact. But it fully **recovered** by the next sample (case 183,
`CmaFree` back to ~92,000 kB) and then stayed stable in the ~95,000-
116,000 kB range for the rest of that run, including its last captured
sample at case 300 - just before the 0.5s-interval sampling missed the
actual failure (the process exited before the next poll).

**Re-ran at 0.1s resolution to close that gap.** This time the samples
extend to **case 305**, two cases before the abort point (307-308) that
every prior run has landed on:

```
case 292: cmafree=115684  memfree=392920  memavail=5916728  rss=133024
case 300: cmafree=115128  memfree=394060  memavail=5920880  rss=132136
case 302: cmafree=115316  memfree=389848  memavail=5921092  rss=131752
case 305: cmafree=115060  memfree=395704  memavail=5920428  rss=131712
```

**Every single indicator is completely ordinary** - the same healthy
range it had held since case 183, no trend, no decline, nothing
distinguishing this moment from any of the hundreds of cases that passed
cleanly before it. `deqp-vk`'s own process RSS is small (~130 MB) and
flat. There is no observable system-wide or CMA-specific memory pressure
at the moment `KBASE_IOCTL_MEM_ALLOC_EX` returns real kernel `ENOMEM` two
cases later.

**This rules out the fourth pass's remaining external hypothesis.** The
earlier severe CMA crunch (case 59-79) was real but is not the cause -
it resolved over 200 cases before the failure and stayed resolved.
Whatever is exhausted is not visible in `/proc/meminfo` at all, which
narrows it to one of two things, neither checkable from userspace on
this device without further access:

1. **Physical fragmentation not reflected in aggregate free-byte
   counts** - a specific allocation shape (contiguity, alignment, a
   particular CMA sub-region) unavailable even though the totals look
   fine. Weakened by the failing allocation being tiny (one 4 KB page,
   per the `bo_alloc #1`-style address every fresh device's first
   allocation gets) - a single free page being unfindable while
   `CmaFree` reports 115 MB free would be an extreme case of
   fragmentation.
2. **A kbase kernel-driver-internal accounting structure or limit** -
   a table, an ID space, an internal memory pool with a cap - entirely
   separate from the general page allocator and therefore invisible to
   `/proc/meminfo` regardless of how carefully it's sampled.

**Investigation is now blocked by environment access, not by remaining
hypotheses to test** - and root access on the device is deliberately not
an option being pursued here (explicit instruction; the alternative is
checking whether this was already explained in open source, which it
was - see below). Five full passes (fd leak and every churn/concurrency
pattern constructible; both specific hypotheses the driver instrumentation
suggested; and live system and CMA memory sampling of the real run) have
converged on a precise, well-evidenced characterization: kernel-level
resource exhaustion, invisible to `/proc/meminfo`, following a
severe-but-resolved CMA crunch earlier in the run.

## Closing this dig: matches a documented, public kbase kernel mechanism - not a bug in this repo

Rather than pursuing root/kernel-log access, searched for whether this
exact symptom shape - a real memory crunch that visibly recovers,
followed much later by an unrelated allocation failing while every
`/proc/meminfo` metric looks ordinary - is already explained in kbase's
own public source. It is. Arm's kbase kernel driver keeps an internal
page **memory pool** per device (`kbase_mem_pool`, publicly available in
Google's own published kernel source,
`kernel/google-modules/gpu/.../mali_kbase_mem_pool.c` - not this device's
exact tree, which still is not public, but the same subsystem present
across kbase kernel versions generally), backed by a Linux kernel
**shrinker**: under memory pressure, the shrinker reclaims pages the pool
was holding back to the general kernel allocator - which is consistent
with the CmaFree crash and recovery observed around case 59-79.
Growing that pool back later - `kbase_mem_pool_grow()` - can fail and
return `ENOMEM` independently of whether the kernel considers memory
"available" in the ordinary sense: the pool has to re-request fresh pages
from the kernel allocator, and that request can fail for internal
allocator reasons (fragmentation, allocation-class exhaustion, or simply
losing a race with the shrinker again) even while `/proc/meminfo` reports
plenty free. That is exactly the mismatch this pass's data showed - every
metric ordinary at case 305, real kernel `ENOMEM` two cases later.

This is also a documented **category** of kbase driver issues, not a
one-off: GitHub Security Lab has published two advisories in the same
memory-pool/eviction subsystem specifically -
[GHSL-2022-127](https://securitylab.github.com/advisories/GHSL-2022-127_Arm_Mali/)
("Free Memory Access in Arm Mali", CVE-2022-46395) and
[GHSL-2023-005](https://securitylab.github.com/advisories/GHSL-2023-005_Android/)
("GPU memory accessed after it's freed"), both rooted in the pool/
shrinker/eviction-list interaction under memory pressure. Neither is
this exact bug, but both confirm the subsystem is a genuine, recurring
source of surprising behavior in kbase - not something specific to this
repo's from-scratch backend.

**Conclusion: this is very likely downstream kbase kernel-driver
behavior** (the memory pool/shrinker interaction after the CMA crunch
`max_concurrent.*` causes), not a bug in `pan_kmod_kbase.c` or
`panvk_vX_kbase_queue.c`. Any userspace Vulkan driver on this kernel -
including Arm's own proprietary one - would be subject to the same
pool-regrowth failure mode after the same kind of memory-pressure burst.
That reclassifies this from "an unresolved defect in this port" to "a
known category of upstream kbase kernel quirk this port has now
precisely characterized, consistent with public prior art" - genuinely
useful context for anyone hitting a similar `VK_ERROR_OUT_OF_DEVICE_MEMORY`
after a memory-heavy CTS group on kbase hardware, and not a blocker for
CTS work generally: the practical workaround (excluding the specific
leaves that land on this ceiling, and treating any future occurrence as
"probably this again" rather than a fresh mystery) is already in use.

## Widening CTS coverage: dEQP-VK.api.* (excluding the huge sweeps), 2194/2194 cases run

With the resource-ceiling dig closed, moved on to running the rest of
`dEQP-VK.api.*` - everything except `copy_and_blit` (201k),
`image_clearing` (45k), `info` (10k, a per-extension query sweep),
`buffer`/`ds_color_copy`/`buffer_view`/`image_compression_control`
(medium sweeps deferred for a later pass), and `object_management`
(already covered above). Ran as one filtered caselist
(`--deqp-caselist-file`), excluding specific leaves as each was found to
either hit the closed resource-ceiling pattern again or crash the test
binary, until the remaining ~2194 cases completed cleanly in one run:
**1258 passed, 931 correctly `NotSupported` (unimplemented optional
extensions - `VK_KHR_maintenance7`, `VK_KHR_cooperative_matrix`,
`VK_KHR_fragment_shading_rate`, `VK_KHR_surface`/WSI, etc. - all expected
for this driver's current feature set), 5 genuine failures.**

**Rebooted the device mid-pass** (a normal power cycle, not root access -
deliberately not pursued, see the closing note above) after the resource
ceiling started recurring within 10-30 cases of a fresh process instead
of the ~792 cumulative creates the original `object_management` finding
needed, suspecting this session's very heavy earlier testing (thousands
of device creations, a 96,000-cycle stress test, a 125,000-buffer stress
test) had left the kernel's own memory pool in a degraded state that
persists across process invocations. All deployed files
(`deqp-vk`, the driver `.so`, the ICD shim, the `vulkan/` CTS data
directory, caselist files) survive a reboot (`/data` is not wiped);
redeployed nothing, just waited for `sys.boot_completed` and resumed.

**The reboot produced a genuinely informative negative result:** the very
same test (`device_init.create_device_global_priority.basic`) failed at
the exact same point, identically, before and after. That rules out
"cumulative session wear" as *that* test's explanation - it's
deterministic, tied to that specific device configuration
(`VK_EXT_global_priority`), not to prior session history. Consistent with
this repo's own acknowledged gap: `panvk_vX_kbase_queue.c`'s comment on
`create_kbase_queue` states outright that "mapping Vulkan global priority
onto kbase priorities properly is left for when submission works" - this
is likely that gap surfacing as a real, fixable bug, not a recurrence of
the closed kbase-kernel mystery. Filed separately below rather than
lumped in with the resource-ceiling findings.

Leaves excluded en route, with their real classification (not all the
same issue - resist the temptation to lump every `VK_ERROR_OUT_OF_DEVICE_MEMORY`
together):

- **Likely the closed resource-ceiling pattern recurring** (transient,
  consistent with the kbase memory-pool/shrinker characterization above):
  `buffer_marker.graphics.default_mem.bottom_of_pipe.memory_dep.buffer_copy`
  (`ResourceError` on `vkCreateDevice`),
  `buffer_memory_requirements.create_sparse_binding_sparse_residency.ext_mem_flags_excluded.method1.other_usage_bits`
  (`ResourceError` on `vkCreateBuffer` - sparse allocations are exactly
  the kind of memory-heavy pattern that would stress the same pool).
- **A likely real, fixable driver bug** (not the closed mystery -
  deterministic, survives reboot):
  `device_init.create_device_global_priority.basic` - see above.
- **Real crashes (`SIGSEGV`), confirmed the device/GPU stayed healthy
  after each** (`driver_compute_probe --submit --fill` clean both times -
  userspace process crashes, not GPU hangs):
  - `device_init.create_instance_device_intentional_alloc_fail.basic` -
    this test deliberately injects allocation failures (via
    `VkAllocationCallbacks` that fail on cue) to verify the driver
    propagates `VK_ERROR_OUT_OF_HOST_MEMORY` gracefully everywhere in
    instance/device creation rather than crashing. This driver crashes
    instead somewhere in that path - a real, standard robustness bug
    (dereferencing a pointer from an allocation that was made to fail),
    not specific to kbase.
  - `null_handle.destroy_device` - `vkDestroyDevice(VK_NULL_HANDLE, ...)`
    must be a safe no-op per spec (destroying `VK_NULL_HANDLE` is valid
    for every `vkDestroy*`/`vkFree*` entry point). This driver crashes
    instead - missing a null check Mesa's common `vk_device` layer
    usually provides, or this backend bypasses it somewhere.
- **A CTS-side bug, not this driver's** (`SIGABRT`, an assertion in CTS's
  own test utility): the whole
  `external.memory.android_hardware_buffer` subtree hits
  `assertion "!(sdkVersion >= 33)" failed` in
  `vktExternalMemoryAndroidHardwareBufferUtil.cpp` - this CTS version's
  AHardwareBuffer test helper hard-codes an assumption that doesn't hold
  on this device's Android SDK version (36). Same category as the
  earlier `dEQP-VK.info.platform` finding: an environment/CTS-version
  mismatch, not a driver defect.
- **Slow, deferred for pacing, not excluded for correctness reasons**:
  `command_buffers` (131 cases, each doing real GPU submission - far
  slower per-case than everything else in this batch). Partially run
  before being deferred; surfaced two genuine rendering-correctness
  failures worth recording now rather than re-finding later:
  `many_indirect_draws_on_secondary` and `record_many_draws_secondary_2`
  both fail with wrong pixel colors - both specifically about **drawing
  from secondary command buffers**, a shape none of this session's own
  render probes exercised (they all drew from primary command buffers
  directly). A real, scoped lead for future rendering-correctness work.

**Five genuine failures in the completed 2194-case run itself** (beyond
the excluded leaves above):

1. `device_init.create_instance_layer_name_abuse.basic` - "Runtime check
   failed: `!gotInstance`" - this driver doesn't reject a malformed/abusive
   layer name the way the test expects; likely because this driver
   doesn't implement layer validation at all yet (plausible for a driver
   at this stage - not urgent, but real).
2. `driver_properties.conformance_version` - "Wrong driver conformance
   version (older than used API version)" - `VkPhysicalDeviceDriverProperties.conformanceVersion`
   is reported inconsistently with the Vulkan API version this driver
   claims (1.4.0, per `version_check.version` passing). A metadata/
   reporting bug, likely a quick fix once looked at directly - this driver
   is reporting a conformance version that doesn't match reality (it
   isn't conformant, so arguably any specific version claimed needs
   re-examining, not just "made consistent").
3. `extension_duplicates.device.by_names` / `by_pointers` - both fail
   with `VK_ERROR_OUT_OF_DEVICE_MEMORY` specifically when duplicate
   extension names/pointers are passed to `vkCreateDevice` (the test's
   whole point - verifying duplicates are handled gracefully, e.g.
   deduplicated, not double-processed). Worth checking whether this is
   the closed resource-ceiling pattern again or a real over-allocation
   bug specific to *processing* duplicate extension entries (allocating
   per-listed-extension rather than per-unique-extension, for instance) -
   not distinguished yet, flagged for follow-up rather than assumed.
4. `version_check.unavailable_entry_points` - fails with
   `VK_ERROR_EXTENSION_NOT_PRESENT` from the generic custom-device-creation
   helper; not yet determined whether this is the correct response
   mis-categorized by CTS's harness or a genuine mishandling on this
   driver's part.

Device confirmed healthy after every excluded/crashing case via
`driver_compute_probe --submit --fill`, run individually after each one
before continuing - the same discipline as every hardware-risk step this
session.

**Next**: the deferred medium sweeps (`buffer`, `ds_color_copy`,
`buffer_view`, `image_compression_control`, `info`), `command_buffers`
run to completion with a longer timeout (each case does real GPU work, so
it is slow rather than risky), and `dEQP-VK.query_pool.*` per the
standing plan - none attempted yet this pass.

## Fixed: vkDestroyDevice(VK_NULL_HANDLE) segfault

First of the two `SIGSEGV` crashes above triaged and fixed. Symbolized
the crash tombstone against the **unstripped** local build of the driver
(`third_party/VK-GL-CTS/build-android/.../deqp-vk` and
`/opt/mesa-src/build-android/.../libvulkan_panfrost.so` - both still
present locally from the earlier build, never stripped except the
*deployed* copy of `deqp-vk`) with the NDK's own `llvm-addr2line`,
entirely from userspace, no root needed:

```
llvm-addr2line -e libvulkan_panfrost.so -f -C -i 0x995510
-> panvk_DestroyDevice
   panvk_physical_device.c:0
```

`panvk_DestroyDevice()` (`src/panfrost/vulkan/panvk_physical_device.c`,
plain upstream PanVK code - not one of this repo's own kbase-specific
files, and not copied in by `wsl-build.sh`) dereferences the device
handle before checking whether it's `VK_NULL_HANDLE`:

```c
VK_FROM_HANDLE(panvk_device, device, _device);
struct panvk_physical_device *physical_device =
   to_panvk_physical_device(device->vk.physical);   /* segfault: device == NULL */
```

Every Vulkan `vkDestroy*`/`vkFree*` command accepts `VK_NULL_HANDLE` for
the object being destroyed as a defined no-op - `vkDestroyDevice` is no
exception, and `dEQP-VK.api.null_handle.destroy_device` exercises exactly
that. The tombstone's fault address (`0x70`) lines up with
`vk_device.physical`'s offset inside the struct, confirming exactly where
the null dereference lands.

Fixed via `src/mesa/patch-panvk-null-device-destroy.py`, a new hand-run
idempotent patch script matching this directory's existing
`patch-panvk-kbase-*.py` convention - except **not kbase-specific**:
this bug and fix are in shared PanVK code also used by the panthor and
panfrost backends, so it's named without `kbase` and is a real candidate
for reporting upstream once verified against actual Mesa (not just this
repo's vendored checkout) - matching this project's standing discipline
of verifying on hardware before treating anything as a conclusion.
Rebuilt, deployed, and verified: `dEQP-VK.api.null_handle.destroy_device`
now passes (`Pass (OK: no observable change)`), the full
`dEQP-VK.api.null_handle.*` group passes 23/24 (1 correctly
`NotSupported`, no regressions), and `driver_compute_probe --submit
--fill` stayed clean throughout.

Second crash (`create_instance_device_intentional_alloc_fail`) not yet
triaged - deeper dig, tracked separately.

## Fixed: intentional-alloc-fail crash - kbase enumeration lost a real error behind VK_ERROR_INCOMPATIBLE_DRIVER

Second of the two `SIGSEGV` crashes triaged and fixed. An `Explore` agent
did the initial read-only tracing (across `panvk_instance.c`,
`vk_instance.c`, and this repo's own `panvk_physical_device.c` additions);
followed up by reading `src/mesa/patch-panvk-kbase-enumeration.py` directly
to confirm the exact generated code and design the fix.

**What the crash actually was**, corrected from the first guess: not a null
function pointer (the dispatch table is fine) but `physicalDevices[0]` on an
**empty** `vector<VkPhysicalDevice>` -
`vktApiDeviceInitializationTests.cpp:2512`, inside
`dEQP-VK.api.device_init.create_instance_device_intentional_alloc_fail`. That
test wraps `VkAllocationCallbacks` to fail at a chosen index, retries
`vkCreateInstance` until it stops seeing `VK_ERROR_OUT_OF_HOST_MEMORY`, then
assumes `vkEnumeratePhysicalDevices` succeeding means at least one device -
reasonably, since the Vulkan spec doesn't expect success with an empty list.

**Root cause**: `create_kbase_kmod_dev()` in
`src/panfrost/vulkan/panvk_physical_device.c` - generated by this repo's own
`patch-panvk-kbase-enumeration.py`, not upstream PanVK code, and not
something the panthor/panfrost backends can hit - opened `/dev/mali0` and
handed the fd to the generic `pan_kmod_dev_create()` dispatcher. That
dispatcher returns `NULL` for two unrelated reasons the caller could not
tell apart: the fd genuinely isn't kbase (correctly "let DRM enumeration
have a turn"), or the fd *is* kbase but `kbase_kmod_dev_create()`
(`pan_kmod_kbase.c`) failed for a real reason - exactly the host allocation
this CTS test targets, via `pan_kmod_alloc()`. Both collapsed to
`VK_ERROR_INCOMPATIBLE_DRIVER`, which Mesa's `vk_instance.c` treats as "try
the next enumeration method" - silently swallowing a genuine allocation
failure into `vkEnumeratePhysicalDevices` reporting `VK_SUCCESS` with zero
devices. Confirmed via the upstream DRM twin in the same file,
`create_kmod_dev()`, which already handles the equivalent case correctly
(`panvk_errorf(instance, VK_ERROR_OUT_OF_HOST_MEMORY, "cannot create
device")`) - this was purely a gap in this repo's own kbase-enumeration
patch, nothing to report upstream.

The existing code carried an explicit warning against probing
`pan_kmod_fd_is_kbase()` a second time in this function, since
`KBASE_IOCTL_VERSION_CHECK` is once-per-fd (`-EPERM` on a second call) - real
and still respected. The fix doesn't call it twice: it calls
`pan_kmod_fd_is_kbase()` directly, once, and then calls
`kbase_kmod_ops.dev_create()` directly too (both already exported by
`pan_kmod_kbase.h`), bypassing the generic dispatcher entirely for the kbase
path rather than going through it and re-probing. This is exactly the same
pair of calls `pan_kmod_dev_create()` makes internally for its kbase branch
- just relocated to the caller, so the caller can distinguish "not kbase"
(`VK_ERROR_INCOMPATIBLE_DRIVER`) from "kbase but failed"
(`VK_ERROR_OUT_OF_HOST_MEMORY`, correctly propagated).

Applied to `src/mesa/patch-panvk-kbase-enumeration.py`'s `kbase_fn` heredoc
(so a fresh Mesa clone gets the fix) and hand-edited into the already-applied
live `/opt/mesa-src` tree to match (same technique as crash #1's comment
fix - re-running the patch script would have no-op'd, since its idempotency
check is keyed on the function's *name* being present, not its current
content, the same caveat this directory's own `README.md` already documents
for every `patch-panvk-kbase-*.py` script).

Rebuilt, deployed, verified:
`dEQP-VK.api.device_init.create_instance_device_intentional_alloc_fail.basic`
now passes (the many retry warnings in its output confirm the alloc-fail
injection loop genuinely exercised the fix, not just a trivial first-try
pass). The full `dEQP-VK.api.device_init.*` group, with the two
already-known-and-separately-tracked unrelated cases excluded
(`create_device_global_priority` - a real but different bug, see the CTS
widening notes above; `create_device_various_queue_counts` - likewise), ran
to completion with **no abort**: 227/236 passed, 8 correctly `NotSupported`,
1 failure (`create_instance_layer_name_abuse` - the same pre-existing,
unrelated finding already documented above, not a regression).
`driver_compute_probe --submit --fill` stayed clean throughout, including
after the group run - this function is on the hot path for every single
`vkCreateInstance`, so a regression here would have been immediately
visible.

**Both `SIGSEGV` crashes found by this session's CTS widening are now
fixed and verified on hardware.**

## Triaging the remaining findings: three are not code bugs, one sharpened into a real cold-start lead

The CTS widening pass left four more findings untriaged. Dispatched three
parallel `Explore` agents (read-only) plus a direct read for the fourth.
Three resolved to "not a code bug to fix"; one turned into a genuinely new
and much sharper finding through a short, staged follow-up CTS session.

### `driver_properties.conformance_version` - an honest signal, not a bug

`get_conformance_version()` (`panvk_vX_physical_device.c:782-788`) returns
`{1,4,1,2}` only for `PAN_ARCH == 10` (Mali-G610, PanVK's one
officially-conformant target) and `{0,0,0,0}` for every other architecture,
including this device's. The CTS check fails because reporting API version
1.4 alongside conformance version 0.0 is spec-inconsistent - but `0.0.0.0`
for an architecture that has genuinely never been conformance-tested is the
*honest* answer, consistent with this driver's own explicit runtime warning
("panvk is not a conformant Vulkan implementation, testing use only").
Faking a conformance claim to pass the test would be actively wrong. No
code change - accepted, expected failure.

### `extension_duplicates.device.*` - confirmed resource-ceiling recurrence

The `Explore` agent read the actual CTS source: duplication factor is small
and bounded (2-4x per extension, never more), the requested extension set is
identical to what hundreds of other passing cases already request, and
PanVK's extension-enable walk (`vk_device.c:182-207`) is allocation-free -
setting a bool flag twice costs nothing. Both `by_names` and `by_pointers`
fail identically, which a real pointer/dedup bug would not produce but a
device-level kernel `ENOMEM` would. Matches the closed resource-ceiling
finding's exact signature (device creation succeeds, then the first real
kbase allocation returns `ENOMEM`, unrelated to any property of the
request). No code change - exclude these two leaves in future runs, same
treatment as `object_management`'s already-excluded leaves.

### `create_instance_layer_name_abuse` - confirmed architectural, not a driver gap

Layer-name validation/rejection is the Vulkan **loader's** job, not the
ICD's - confirmed by reading `vk_instance.c` (validates extensions, never
reads `enabledLayerCount` at all) and grepping all of Mesa's drivers for
`ppEnabledLayerNames` (zero ICDs touch it). This project's CTS runs go
through `src/tests/icd_shim/panvk_kbase_icd_shim.c` directly - deliberately,
to avoid installing the driver as the system Vulkan HAL - bypassing the
loader entirely, so the layer-rejection stage this test expects structurally
doesn't exist in the tested path. Adding non-standard layer validation to
`panvk_CreateInstance` would diverge from every Mesa driver and be rejected
upstream. This upgrades the earlier hypothesis ("doesn't implement layer
validation yet") to a confirmed architectural attribution: it's an artifact
of testing through a bare ICD, not a driver bug, and not fixable here
without either a real loader in front of CTS or a non-upstreamable
divergence. No code change - accepted limitation of this testing setup.

### Secondary command buffer draws - sharpened into a real, specific cold-start lead

The two rendering-correctness failures
(`many_indirect_draws_on_secondary`, `record_many_draws_secondary_2`) got a
genuinely new characterization through a short staged follow-up, not from
code reading alone (the `Explore` agent traced the whole path - CTS test
setup, `panvk_per_arch(CmdExecuteCommands)`, `collect_cmdbuf_calls()`,
indirect-draw handling - and found the machinery structurally complete, no
clear missing branch; low-to-medium confidence on any single cause from
reading alone).

Ran the two known failures alongside two structurally-similar variants that
had never been tried: `record_many_draws_secondary_1` (a much smaller,
128x128 version of the same secondary-buffer draw test) and
`record_many_draws_primary_2` (the same heavy draw count, but from a
**primary** buffer, no secondary/nesting at all). All four in one
`deqp-vk` process:

```
many_indirect_draws_on_secondary   Fail (unchanged)
record_many_draws_primary_2        Pass
record_many_draws_secondary_1      Pass
record_many_draws_secondary_2      Pass   <- previously failed in isolation!
```

`record_many_draws_secondary_2` passing here, after failing in the original
CTS widening run, was the live wire. Re-ran it alone three times: **fails
100% of the time in isolation.** Then ran `record_many_draws_secondary_1`
(much smaller, otherwise the same shape) immediately before it in the same
process: **`record_many_draws_secondary_2` passes.** This is fully
deterministic and reproducible, not flakiness -
`record_many_draws_secondary_2` fails if and only if it is the *first*
secondary-command-buffer draw sequence executed in a fresh process; any
prior secondary-buffer draw, however small, makes it pass. A classic
cold-start / lazy-initialization bug signature: something set up once per
process (not per-command-buffer, not per-draw) is either missing or wrong
on its first use and self-corrects as a side effect of that first use
happening at all, regardless of what specifically triggered it.

Tried to test whether `many_indirect_draws_on_secondary` shares this exact
mechanism, using `deqp-vk`'s own case selection to precede it with a
warm-up case (`record_many_draws_secondary_1` or others) - both a
comma-separated `--deqp-case` list and an explicitly ordered
`--deqp-caselist-file` were tried, and neither changed execution order:
`deqp-vk` walks its selected cases in a fixed order (alphabetical by full
case path, not source-registration order or caselist-file order) regardless
of how they were selected. Dumped the full sorted `command_buffers.*`
caselist to check: the only cases that sort alphabetically before
`many_indirect_draws_on_secondary` are `allocate_many_secondary` /
`allocate_single_secondary` (allocate a secondary buffer, record and
execute nothing) and `many_indirect_disps_on_secondary` (compute
*dispatch*, not draw) - none of them actually draws, so none can serve as
a warm-up case through `deqp-vk`'s own selection mechanism. Testing the
same hypothesis for this case would need a small dedicated probe (record
one trivial draw, then reproduce this test's exact sequence) rather than
CTS case reordering - a well-scoped, still-cheap next step, but a new probe
rather than "just run more CTS," and out of scope for this pass.

**Where this leaves it**: `record_many_draws_secondary_2` is fully
characterized - a deterministic, reproducible cold-start bug in whatever
this driver initializes once per process for secondary-command-buffer
draws (plausible candidates, not yet checked: lazy pipeline/shader setup
specific to the secondary-execution path, first-time tiler/geometry-buffer
sizing, or something in `cmd_prepare_exec_cmd_for_draws`/
`cmd_inherit_render_state` that only runs correctly once already-warm
state exists). `many_indirect_draws_on_secondary` may well be the exact
same bug landing on the alphabetically-first case, or may be a distinct
indirect-draw-specific issue - genuinely unresolved, and the next concrete
step (a small warm-up probe) is now precisely scoped. Device confirmed
healthy after every run in this investigation via
`driver_compute_probe --submit --fill`.

## The warm-up probe: a clean negative result, ruling out the simple unification

Built `tests/render_secondary_warmup_probe` to test the hypothesis directly
on hardware, since `deqp-vk`'s fixed alphabetical case-execution order made
it untestable through CTS case selection alone. Three rounds, same device,
same process - the same shape that proved the cold-start mechanism for
`record_many_draws_secondary_2`:

1. **Cold**: one `vkCmdDrawIndirect` recorded into a secondary command
   buffer, executed via a primary within a dynamic-rendering render pass
   (`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`,
   `VkCommandBufferInheritanceRenderingInfo` for the secondary's begin) -
   as the very first secondary-buffer operation in the process.
2. **Warm-up**: an ordinary, non-indirect draw from a secondary buffer, to
   a throwaway target - exactly the kind of draw that fixed
   `record_many_draws_secondary_2` when run first.
3. **Warm**: the identical indirect-draw-from-secondary as round 1, on a
   fresh target, after the warm-up.

Reused `render_vbo_probe`'s exact shaders (`warmup.vert`/`.frag`, copied
verbatim - vertex-buffer-fetched position and fixed magenta output, both
already proven, neither under test here) so only the mechanism actually
being tested - secondary command buffers plus indirect draws - was new.
Ran with `--i-know-it-hangs` per this repo's render-probe convention,
though the real hang risk was lower than usual: `deqp-vk` itself had
already executed this exact combination (secondary buffer + indirect
draw) many times this session without ever hanging, only ever producing
wrong pixels.

**Result: both rounds passed cleanly, first attempt.**

```
[cold indirect result] 190 clear, 66 triangle, 0 other (of 256 total)
[warm indirect result] 190 clear, 66 triangle, 0 other (of 256 total)
```

The cold round - a single indirect draw from a secondary command buffer,
as the first secondary op ever in the process - rendered correctly. This
is a real, clean negative result, not an inconclusive one: it rules out
the simple hypothesis that `many_indirect_draws_on_secondary` is the exact
same cold-start bug as `record_many_draws_secondary_2`, landing on the
alphabetically-first case. Whatever breaks CTS's own test is not "any
indirect draw from a secondary buffer while cold" - something more
specific to that test's actual shape must be involved. Candidates this
probe did not replicate and has not ruled out: the sheer volume (CTS's
test issues 4096 separate `vkCmdDrawIndirect` calls in one secondary
buffer, this probe issued 1), the primitive topology (CTS uses a point
list, this probe a triangle list), or the target size (CTS renders to
64x64, this probe to 16x16 to match every other probe's established
baseline). Device confirmed healthy after via
`driver_compute_probe --submit --fill`.

**Where this leaves it**: `record_many_draws_secondary_2` remains fully
characterized as a genuine cold-start bug (from the prior CTS-only
investigation). `many_indirect_draws_on_secondary` is now confirmed
**not** simply the same bug - it needs its own investigation, most
directly by scaling this same probe's cold round up toward CTS's actual
4096-draw, point-list, 64x64 shape to find which specific factor (volume,
topology, or size) is load-bearing, rather than guessing further. Not
attempted in this pass - the probe and its clean negative result are the
deliverable, not a full re-investigation.

## Scaling the probe: draw COUNT alone also ruled out

Added an `indirect-repeat-count` argument to `render_secondary_warmup_probe`
(defaults to 1, matching the original run) so the indirect draw could be
issued as **N separate `vkCmdDrawIndirect` calls** recorded into the same
secondary buffer - matching CTS's actual shape exactly (`many_indirect_draws_on_secondary`
records 4096 separate calls, each `drawCount=1`, not one call with a high
`drawCount` - each is its own CS-stream-building call, which is why draw
*count* specifically, not just rendered pixel volume, was worth isolating
as its own variable).

Ran with `indirect-repeat-count=4096`, CTS's exact value, same three-round
cold/warm-up/warm structure, same 16x16 triangle-list target as before
(everything held constant except call count):

```
[cold indirect result] 190 clear, 66 triangle, 0 other (of 256 total)
[warm indirect result] 190 clear, 66 triangle, 0 other (of 256 total)
```

**Both rounds passed cleanly again**, including cold, with all 4096 calls
recorded and executed. This rules out draw-call *count* alone as the
trigger too, at exactly the value CTS itself uses - not an approximation.
Device confirmed healthy after via `driver_compute_probe --submit --fill`.

**Two clean, independently-verified negative results now**: neither "a
single indirect draw from a secondary buffer while cold" nor "4096 separate
indirect draws from a secondary buffer, cold or warm" reproduces the
failure. The two remaining untested variables - point-list topology and
64x64 target size - are very likely *entangled* in CTS's actual test, not
independent: 4096 draws exactly equals 64x64 pixels, strongly suggesting
each of the 4096 separate indirect draws places one point at a distinct
pixel coordinate (a full-coverage test: every pixel must end up written),
not 4096 draws piled on the same location the way this probe's repeat-count
does. Properly testing that would mean reproducing the per-draw varying
position logic (point list topology, `gl_PointSize`, and per-draw vertex
data addressing a specific target pixel) - substantially closer to
reimplementing CTS's own test than the two clean, cheap isolations already
done. Stopping here rather than continuing to escalate the probe further
without a check-in: the volume/count hypothesis is now thoroughly ruled
out through two independent, exact-value tests, and the remaining
candidate (point-list full-coverage) is a bigger, more speculative next
step worth a deliberate decision rather than silent continuation.

## `buffer` group stall on `storage_texel.storage.create.binding` - live analysis

Resuming the `dEQP-VK.api.buffer.*` widening sweep (after excluding the
first stalling case, `storage.indirect.create.binding`, and applying
`svc power stayon true` to rule out screen-off as a factor), the run
stalled again at case 598/~842, on
`dEQP-VK.api.buffer.suballocation.storage_texel.storage.create.binding`.
Confirmed NOT a screen-off artifact (stayon was already active) and NOT a
GPU/device hang (`driver_compute_probe --submit --fill` stayed clean
throughout, run concurrently while the stalled process was still alive -
valid since kbase supports multiple concurrent contexts).

Root cause traced by reading the CTS source
(`external/vulkancts/modules/vulkan/api/vktApiBufferTests.cpp`,
`getMaxBufferSize()` at line 66, `BufferTestInstance::bufferCreateAndAllocTest()`
at line 185): for a UMA system (`limits.totalDeviceLocalMemory == 0`, true
for this driver), the test derives its buffer size from
`totalSystemMemory - alignment` (this device reports ~11.6GB total), then
halves that again for `maxBufferSize` - so the *first* `vkCreateBuffer`/
`vkAllocateMemory` attempt targets roughly a multi-GB allocation. On
failure the test shrinks the size by `>>4` (16x) per retry and tries
again, down to `memReqs.alignment`, before giving up with a `fail`.

On-device evidence matches a kernel-side reclaim stall, not a userspace
spin or logic bug: `ps -o pid,etime,time,stat` on the live process showed
only ~1m29s of accumulated CPU time across 28+ minutes of elapsed time,
state `S` (sleeping). The `.qpa` output file's mtime and the stdout log's
last line (`Test case '...storage_texel.storage.create.binding'..` with no
following result line) were both frozen from the moment the case started -
zero forward progress, consistent with being parked inside a blocking
kernel call (most likely `KBASE_IOCTL_MEM_ALLOC`/`MEM_ALLOC_EX` triggering
synchronous memory reclaim/compaction) rather than a tight failing retry
loop, which would show much higher accumulated CPU time over the same
window.

This is the same underlying mechanism as the already-closed
resource-ceiling finding (`kbase_mem_pool` + Linux shrinker behavior under
memory pressure), just manifesting here as a multi-minute reclaim stall on
a single oversized request instead of a fast, clean `ENOMEM`. Not a driver
correctness bug - the driver isn't misbehaving, the kernel is legitimately
churning trying to satisfy a request sized against a memory limit
(`totalSystemMemory`) far larger than what's actually available for GPU
use on this device/kernel configuration. The retry loop is bounded (it
terminates once shrunk size reaches `memReqs.alignment` or `0`), so this
is expected to eventually resolve one way or another (test result, or the
run's own on-device `timeout` wrapper) rather than deadlock permanently.
Left running rather than killed, to observe which outcome actually
happens; GPU health reconfirmed healthy at each check-in via
`driver_compute_probe --submit --fill`.

## kbase CAN import a dma-buf — measured, with two caveats that matter

This closes the "Does your kernel's kbase expose the ioctls you'll need for
dma-buf import/export, or only its own private memory model?" checkbox that
has been open in the "Things worth checking early, not late" list since
Phase 1. Answered by `tests/dmabuf_import_probe`, which does **no GPU work
at all** — no queue group, no `CS_QUEUE_KICK`, no command stream — because
the value under test is exactly the kind that wedged the device twice via
`alias_cs_probe`.

**Import works.** `KBASE_IOCTL_MEM_IMPORT` with
`BASE_MEM_IMPORT_TYPE_UMM` accepts a dma-buf fd from `/dev/dma_heap/system`
on the first try, with plain `CPU_RD|CPU_WR|GPU_RD|GPU_WR` flags — no
`BASE_MEM_IMPORT_SHARED`, no `BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP` needed.
`in.phandle` is a **pointer to** the `int` fd, not the fd by value; the
kernel does `get_user()` on it.

```
OUT_FLAGS=0x500f  [CPU_RD CPU_WR GPU_RD GPU_WR CACHED_CPU NEED_MMAP]
GPU_VA_RAW=0x41000        -> a cookie, not an address
RESOLVED_GPU_VA=0x7104dfc000 (after mmap)
VA_PAGES=1, COVERS_BUFFER=yes
IMPORT_VA_IN_FIXED_ZONE=no
FIXED_ALLOC_BEFORE_IMPORT=pass   FIXED_ALLOC_AFTER_IMPORT=pass
```

Four things a Mesa-side implementation has to respect, each measured rather
than assumed:

- **`out.gpu_va` is an mmap cookie, not an address** — `BASE_MEM_NEED_MMAP`
  is set. That is the same trap as `MEM_ALIAS`, and it is checked two ways
  here, not one: the flag, and the magnitude against the header's own cookie
  range (`BASE_MEM_COOKIE_BASE` = `0x40000` up to
  `BASE_MEM_FIRST_FREE_ADDRESS`; the observed `0x41000` sits inside it). The
  probe treats disagreement between those two as a hard stop, because
  `alias_cs_probe` is the case where one signal said "usable" and the device
  needed a reboot. `mmap()`ing the cookie resolves it to a real address, the
  way Panfork does.
- **The cookie is single-use.** A second `mmap()` of the same cookie fails
  `EINVAL`. So an imported BO's mapping must be kept — it cannot be
  re-derived — and `bo_get_mmap_offset()` cannot serve one.
- **It frees like a SAME_VA region.** `KBASE_IOCTL_MEM_FREE` on the resolved
  address returns `EINVAL`; `munmap()` is the free. Getting this backwards
  would leak a kernel region per imported buffer.
- **The imported VA lands outside the FIXED_VA zone** (`0x800200000000` +
  8 GB) that `pan_kmod_kbase.c`'s `util_vma_heap` owns, and a
  `BASE_MEM_FIXED` allocation still succeeds *after* an import — so an
  import does **not** poison the context the way `BASE_MEM_FIXABLE` does.
  Both were open questions and both came back clean.

**Caveat 1: there is no CPU view of an imported region.** `mmap()` of the
cookie succeeds, but touching the result takes `SIGBUS` — the mapping exists,
the pages are not backed. Identical across all three flag combinations
(`base`, `IMPORT_SHARED`, `SYNC_ON_MAP_UNMAP`), so this is a property of UMM
imports on this kernel rather than a flag choice. kbase evidently attaches
the dma_buf lazily and `mmap()` alone does not set up CPU access.

This does not block the use case that matters — a gralloc buffer is written
by the *GPU*, not the CPU — but it does mean an imported `VkDeviceMemory`
cannot honestly offer `vkMapMemory`, and it means the probe cannot prove
same-memory identity from the CPU side. Proving that needs GPU work, i.e.
the driver-level probe, not this one.

(The probe survives the fault rather than dying on it: it installs a
`SIGBUS`/`SIGSEGV` guard with `sigsetjmp` around each access. A probe that
crashes tells you less than one that says "this mapping is not accessible,
here is everything else I learned.")

**Caveat 2 — a real gralloc buffer imports fine, but NOT through the fd
`panvk_android.c` reads.** This one nearly went into the repo as "the
AHardwareBuffer path does not work", which was wrong, and the thing that
caught it was dumping the whole `native_handle` instead of trusting the
first fd.

`AHardwareBuffer_allocate` succeeds from the unprivileged `shell` user, so
binder access to the graphics allocator is not a problem. MediaTek's handle
for a 4 KiB `BLOB` carries **three** fds:

```
native_handle: version=12 numFds=3 numInts=62
  data[0] = fd 7, lseek(SEEK_END) = -1     <- NOT a dma-buf
  data[1] = fd 8, lseek(SEEK_END) = 4096   <- the actual buffer
  data[2] = fd 9, lseek(SEEK_END) = 6480   <- metadata, presumably
```

`panvk_android.c` does `int dma_buf_fd = handle->data[0];`. On this device
`data[0]` fails `lseek`, gives `EACCES` on `mmap`, and is refused `ENOMEM`
by every import combination — because it is not the dma-buf. **`data[1]`
imports on the first try**, and everything else about it is identical to the
dma-heap case: `NEED_MMAP` cookie, resolves via `mmap`, outside the FIXED_VA
zone, `FIXED` still works afterwards, `munmap`-only free.

```
IMPORT_RESULT=ok   IMPORTED_HANDLE_FD_INDEX=1
VERDICT=IMPORT_WORKS
```

So the result for presentation is the good one: **kbase can import the exact
buffers Android WSI will hand it.** The `data[0]` convention is common but
is not a guarantee, and it does not hold here — a Mesa-side implementation
has to find the dma-buf in the handle rather than assume index 0. That is
worth knowing before writing the import path, and it is the kind of thing
that would otherwise present as an unexplained `ENOMEM` deep inside
`vkAllocateMemory`.

Note `lseek` on the gralloc fd is not a reliable size either — use
`out.va_pages * 4096` from the import, as the probe does.

`/dev/ion` does not exist on this device (dma-heaps only), and `/dev/ashmem`
is 0666 but ashmem is not a dma-buf, so `dma_buf_get()` rejects it — neither
is worth retrying.

## dma-buf import works end to end through Vulkan, including a gralloc buffer

The Mesa-side half of the previous section. `tests/driver_dmabuf_probe`
imports a dma-buf as a `VkDeviceMemory`, binds it to a `VkBuffer`, has the
GPU write a pattern into it, and reads that pattern back **through an
independent `mmap()` of the dma-buf fd** — deliberately not through
`vkMapMemory`, which a driver that imported nothing and quietly allocated
fresh memory would also satisfy.

Confirmed on the Poco X8 Pro (Mali-G720, r49p1):

- `/dev/dma_heap/system` buffer: **50/50 rounds clean** (the loop exists so a
  leaked kernel region per import is visible rather than invisible).
- **AHardwareBuffer — the real Android WSI case — works too.**

The path is `vkAllocateMemory(VkImportMemoryFdInfoKHR{DMA_BUF})` →
`pan_kmod_bo_import()` → `ops->bo_import_fd` (new, added by
`src/mesa/patch-pan-kmod-import-fd.py`) → `kbase_kmod_bo_import_fd()` →
`KBASE_IOCTL_MEM_IMPORT`. Confirmed running, not merely linked, via
`PANVK_KBASE_DEBUG_COUNTERS=1`:

```
MESA: kbase-dbg: bo_import_fd gpu_va=0x707922c000, size=4096, cookie=0x41000
MESA: kbase-dbg: bo_free (imported) gpu_va=0x707922c000
```

**The one thing that cost a debugging cycle, and would cost anyone else the
same: CPU access to an imported dma-buf must be bracketed with
`DMA_BUF_IOCTL_SYNC`.** The first run of this probe reported the GPU write
had not landed — the readback still held the CPU's pre-import pattern —
while the import itself had demonstrably succeeded. kbase grants imported
regions `CACHED_CPU`, so the readback was returning stale cache lines. With
`DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ` before the read, the same code
passes. Worth internalising: on this device *a missing cache sync is
indistinguishable from a broken import*, and the failure points at the wrong
layer. `dmabuf_cpu_read_begin()`/`_end()` in
`src/tests/dmabuf_import_probe/dmabuf_source.h` exist so there is one place
that gets this right.

**What this does and does not unblock.** It closes the import half of the
Android presentation chain, which was its first link. Still open, in the
order they will be hit:

1. The Android build uses `-Dandroid-stub=true`, so `hw_get_module()` is a
   stub returning 0 *without writing `*module`* and `vk_android_get_ugralloc()`
   cannot succeed — so `VK_ANDROID_native_buffer` is never advertised.
2. `panvk_android.c` reads `handle->data[0]` as the dma-buf. On this device
   that is not the dma-buf (`data[1]` is), so even with gralloc initialised
   the import would be handed the wrong fd. Shared PanVK code; plausibly an
   upstream fix rather than a local one.
3. Android acquire/release needs sync-fd import/export on semaphores, which
   `panvk_kbase_sync.c` does not implement.
4. There is still no Mali equivalent of `libadrenotools` for loading a
   custom ICD without root.

## The Android memory path works — and "gralloc cannot initialise" was wrong

ROADMAP listed `-Dandroid-stub=true` preventing gralloc init as the first
blocker for Android presentation. **That was wrong**, and it is worth
recording why, because the reasoning was plausible and still false.

`-Dandroid-stub=true` only affects **link** time. It builds no-op
`libhardware`/`libnativewindow`/`liblog`/`libsync` shared objects so the
cross build has something to link against. On the device the loader resolves
the **real** `libhardware.so`, so `hw_get_module()` is the genuine article.
Confirmed by which fallback message appears in logcat:

```
W MESA: Gralloc doesn't support lock_ycbcr (video buffers won't be supported)
I MESA: Using fallback gralloc implementation
```

That is the `else if (!gr->gralloc_module->lock_ycbcr)` branch, reachable
only when `hw_get_module()` returned **0 and a valid module**. And
`u_gralloc_fallback_create()` never returns NULL anyway — it warns and
returns a usable object — so `vk_android_get_ugralloc()` was always
non-NULL. The CrOS backend correctly declined (MediaTek's gralloc is not
CrOS gralloc) and the fallback took over.

Consequence: `VK_ANDROID_native_buffer` (rev 8) and
`VK_ANDROID_external_memory_android_hardware_buffer` (rev 5) have been
advertised all along — 143 device extensions total. Measured by
`tests/driver_android_wsi_probe`.

### The real blocker: `handle->data[0]` in three places, two of them in
### Mesa's shared Vulkan runtime

`vkGetAndroidHardwareBufferPropertiesANDROID` failed
`VK_ERROR_INVALID_EXTERNAL_HANDLE`, and the only clue was one log line —
`invalid dmabuf size` from `pan_kmod_bo_import`, several layers below the
actual mistake and pointing at the importer rather than at the handle.

The cause is the `data[0]` convention, which does not hold here (see the
gralloc-handle section above: MediaTek puts the dma-buf at `data[1]`). It is
assumed in three places:

| file | what it does | whose code |
|---|---|---|
| `src/vulkan/runtime/vk_android.c` | `lseek(handle->data[0], ...)` for `allocationSize`, and passes `data[0]` to `GetMemoryFdPropertiesKHR` | **Mesa's shared Vulkan runtime** — every Mesa Vulkan driver on Android |
| `src/panfrost/vulkan/panvk_android.c` | `int dma_buf_fd = handle->data[0];` in the AHB allocate path | PanVK |
| `src/util/u_gralloc/u_gralloc_fallback.c` | `out->fds[0] = hnd->handle->data[0];` | Mesa's gralloc helper |

`src/mesa/patch-panvk-android-gralloc-fd.py` fixes all three: pick the first
fd in the handle that behaves like a dma-buf (dma-bufs implement `llseek`
and report their size; metadata and fence fds do not), falling back to the
historical index so a platform where `data[0]` was always right cannot
regress.

**Result, on hardware:**

```
vkGetAndroidHardwareBufferPropertiesANDROID -> 0
  allocationSize=69632 memoryTypeBits=0x3
vkAllocateMemory(VkImportAndroidHardwareBufferInfoANDROID) -> 0
```

A real 64x64 RGBA8 AHardwareBuffer with GPU usage now imports as
`VkDeviceMemory`. **That is the same memory path a swapchain image takes**,
and it runs through the dma-buf import this port gained earlier.

None of this is kbase-specific. The `data[0]` assumption would bite any
Mesa Vulkan driver on a gralloc that orders its handle differently, and the
runtime sites in particular are a strong upstream bug report.

### What is left for presentation

1. ~~gralloc init~~ — was never broken.
2. ~~`data[0]`~~ — fixed here, upstreaming candidate.
3. **sync-fd semaphores.** `vk_common_AcquireImageANDROID` needs
   `ImportSemaphoreFdKHR`/`ImportFenceFdKHR` with `SYNC_FD`, and
   `QueueSignalReleaseImageANDROID` needs `GetSemaphoreFdKHR`;
   `panvk_kbase_sync.c` implements none of them. Unexplored lead:
   `KBASE_IOCTL_STREAM_CREATE` is a fence-stream ioctl the vendor blob calls.
4. **Loading the ICD** without root, for which no Mali equivalent of
   `libadrenotools` exists.

## Sync fds on kbase: import is free, export must be the spec's -1

The last driver-side blocker for Android presentation, settled by
`tests/sync_fd_probe` rather than by reading kernel source and hoping.

**What the runtime actually requires** (read out of Mesa's
`vk_android.c`, not assumed):

- `vk_common_AcquireImageANDROID` calls `ImportSemaphoreFdKHR` /
  `ImportFenceFdKHR` with `SYNC_FD`, and **only** when the application
  passed a semaphore or fence handle. `fd == -1` is legal and means "already
  signalled".
- `vk_common_QueueSignalReleaseImageANDROID` returns `-1` outright when
  `waitSemaphoreCount == 0`. Otherwise it submits, then calls
  `GetSemaphoreFdKHR` with `SYNC_FD`.

**The import half needs nothing from kbase.** A `sync_file` is pollable, so
"wait for this fd" is `poll()`. This driver already satisfies waits on the
CPU (see `panvk_kbase_sync.c`), so it fits the existing model exactly rather
than requiring a new one.

**The export half is the real question, and the answer is measured:**

```
STREAM_CREATE=ok                 -> fd 4
STREAM_IS_SW_SYNC=no             -> SW_SYNC_IOC_CREATE_FENCE gives ENOTTY
FENCE_FROM_STREAM=failed
FENCE_VALIDATE_EVENTFD=rejected  -> it is a real type check
EXPORT_STRATEGY=return_minus_one
```

`KBASE_IOCTL_STREAM_CREATE` succeeds and hands back what its own header calls
a timeline — but it is **not** a userspace-drivable sw_sync timeline. In
kbase that stream is consumed internally by job atoms
(`BASE_JD_REQ_SOFT_FENCE_TRIGGER`), a JM-era mechanism with no CSF
equivalent: there is no ioctl that says "signal this fence now". So
userspace cannot manufacture a fence that signals on GPU completion.

That leaves the option the Vulkan spec explicitly provides: exporting
`SYNC_FD` may return **-1**, meaning the payload is already signalled. It is
legal, and it is honest **provided the driver actually waits before
returning it** — which this one can, since it already CPU-waits everywhere
else.

The cost is real and worth stating plainly: a present would block the
calling thread until the GPU finishes, instead of handing the compositor a
fence to wait on itself. Combined with the ~12 ms idle-GPU kick cost already
measured, that is a per-frame tax. It is correct, not fast, and it is the
honest option while kbase exposes no way to do better.

**`FENCE_VALIDATE` is worth keeping in mind for the implementation**: it
rejected both an eventfd and the stream fd itself, so it is a genuine "is
this really a fence?" check rather than a rubber stamp, and can be used to
refuse a bogus import rather than failing later and further away.

### The shape the implementation should take

- `panvk_kbase_sync.c` gains `import_sync_file` / `export_sync_file` on its
  `vk_sync_type`. The runtime keys off those pointers being non-NULL; there
  is no feature bit to add.
- `import_sync_file`: `fd == -1` means already-signalled. Otherwise validate
  with `KBASE_IOCTL_FENCE_VALIDATE`, keep the fd, and have the sync's wait
  path `poll()` it. Ownership transfers to the driver on success.
- `export_sync_file`: wait for the slot to reach its value, then return -1.
- A patch script must advertise `SYNC_FD` in
  `vkGetPhysicalDeviceExternalSemaphoreProperties` and the fence equivalent,
  or the runtime will not use any of it.

## Rootless driver loading works for Mali — libadrenotools' "no Mali" is about libadrenotools

The last item on the presentation list, and the only one that is not driver
work. On Android the Vulkan loader takes the system driver from
`/vendor/lib64/hw/vulkan.<ro.hardware>.so`, which needs root to replace.
Emulators get around that with **libadrenotools**, whose README says plainly
that Mali is not supported — which this repo had recorded as a hard blocker,
possibly the longest pole.

That claim is about the library, not about the linker, and separating the
two turns out to matter. libadrenotools does three things:

1. loads a driver `.so` through a purpose-built linker namespace,
2. redirects the Adreno blob's own file accesses,
3. `bcenabler`, an Adreno texture-compression patch.

Only (1) is needed by a Mesa driver. (2) and (3) are what make the library
Adreno-specific, and Mesa needs neither. So the question reduces to whether
(1) works here — a linker question with a testable answer.

**Why a namespace is needed at all**, measured rather than assumed. The
driver's `DT_NEEDED` set splits cleanly:

| library | app-accessible? | present on device |
|---|---|---|
| `libdrm.so` | **no** — not in `public.libraries.txt` | `/system/lib64`, `/vendor/lib64` |
| `libhardware.so` | **no** | `/system/lib64` |
| `liblog`, `libnativewindow`, `libsync`, `libz`, `libm`, `libc`, `libdl` | yes | — |

An ordinary app namespace resolves only public libraries, so a plain
`dlopen()` from an app fails on the first two. Both exist on the device; the
namespace is what makes them reachable.

**It works.** `tests/driver_namespace_probe` builds the namespace itself and
loads the driver through it:

```
NAMESPACE_API=found       (__loader_android_create_namespace)
CREATE_NAMESPACE=ok
LINK_NAMESPACES=ok
DLOPEN_EXT=ok
DRIVER_USABLE=ok          -> enumerated: Mali-G720 MC8 (apiVersion 1.4.354)
```

### The recipe, including the part that is not obvious

1. Resolve `__loader_android_create_namespace` and
   `__loader_android_link_namespaces` — **the unprefixed names do not
   exist**; `dlsym` returns NULL for them. Bionic exports the private
   `__loader_`-prefixed spellings, which is how libadrenotools reaches them.
2. Create the namespace `SHARED | ISOLATED`, with a search path covering the
   driver's own directory plus `/system/lib64` and `/vendor/lib64`.
3. Link it against the default namespace for the public sonames **plus
   `libvndksupport.so` and `libdl_android.so`**. This is the step that is
   easy to get wrong and gives a confusing error: `libhardware` pulls in
   `libvndksupport`, which needs `libdl_android.so` from
   `/apex/com.android.runtime/lib64/bionic` — a path an isolated namespace
   cannot reach, so it has to be linked in rather than found. Without it the
   failure reads *"library libdl_android.so needed or dlopened by
   /system/lib64/libvndksupport.so is not accessible"*, which points at a
   library nothing in this project references.
4. `android_dlopen_ext(..., ANDROID_DLEXT_USE_NAMESPACE)`.

**Caveat worth stating:** a shell process is not an app. This is strong
evidence, not proof, that the same works inside an emulator's process. What
it does establish is that nothing about a Mali/Mesa driver resists the
mechanism — which was the open question.

`tools/package-driver.sh` produces the Adrenotools-convention zip
(`meta.json` + the `.so`) that pickers already know how to consume.

**So the ask to an emulator author is now small and specific** — not "add
Mali support", but "when loading a non-Adreno driver, put `/system/lib64`
and `/vendor/lib64` on the namespace search path and link `libvndksupport`
and `libdl_android` from the default namespace". That is a much easier
conversation than the one this repo thought it was going to have.

## Sync-fd semaphores implemented — the last driver-side presentation blocker

Implements what the previous section measured. `panvk_kbase_sync.c` gains
`import_sync_file` / `export_sync_file`, and with them the Android
acquire/release handshake has both halves it needs.

**No patch script was required**, which is worth recording because the plan
assumed one. `vk_sync_semaphore_import_types()` derives `SYNC_FD` support
directly from `type->export_sync_file != NULL` (for binary semaphores), so
adding the ops is sufficient — the runtime advertises the capability by
itself. Confirmed on device: `SYNC_FD semaphore features = 0x3` (import and
export both set).

**Import** takes ownership of the fd and makes it the payload. `fd == -1`
is legal and common — it is what `vkAcquireImageANDROID` passes whenever the
compositor has nothing outstanding — so it is recorded as an imported
payload that is immediately satisfied, not rejected. A real fd is validated
with `KBASE_IOCTL_FENCE_VALIDATE` first (via
`pan_kmod_kbase_fence_validate()`, which lives in `pan_kmod_kbase.c` because
that is the only TU with the kbase UAPI headers), so a non-fence is refused
at the point the mistake was made rather than becoming a `poll()` that never
completes. Waiting is then `poll()` on the fd.

**Export** waits for the payload and returns -1, the spec's "already
signalled". If the payload is *itself* an imported fence it hands back a
`dup()` of it instead, which is both cheaper and more useful than collapsing
a perfectly good fence to -1.

**The bookkeeping that is easy to get wrong**: an imported fence *replaces*
the slot as the payload, so `signal`, `reset` and `move` all have to drop it
— otherwise a stale fd keeps answering waits with someone else's
completion. `move` swaps the imported state along with the slot index, for
the same reason it swaps the slot rather than copying its contents.

**Verified on hardware** (`tests/driver_android_wsi_probe`, now covering
this):

```
SYNC_FD semaphore features = 0x3 (import=1 export=1)
vkImportSemaphoreFdKHR(fd=-1, "already signalled") -> 0
vkGetSemaphoreFdKHR -> 0, fd=-1
```

**One trap worth the ink**: the extension must be *enabled* on the device,
not merely supported by it. Without `VK_KHR_external_semaphore_fd` in
`ppEnabledExtensionNames`, `vkImportSemaphoreFdKHR`/`vkGetSemaphoreFdKHR`
do not resolve at all — which reads as "the driver does not implement this"
when it does. That cost a cycle here.

**The cost, restated because it is real**: export blocks the calling thread
until the GPU finishes, instead of handing the compositor a fence to wait
on. On top of the ~12 ms idle-GPU kick already measured, an application that
presents every frame pays both. Correct, not fast, and it is what kbase
allows.

### Follow-up: what an emulator actually has to change, and what a shell cannot prove

Investigated against a real emulator (Azahar/AzaharPlus, a Citra fork) since
"the ask is small" is worth checking rather than asserting. Recorded here
because the clone itself is gitignored.

**There are two blockers, not one, and only one of them is libadrenotools'.**

1. **The emulator's own gate.** `SupportsCustomDriver()` in
   `src/android/app/src/main/jni/native.cpp` is
   `android_get_device_api_level() >= 28 && CheckKgslPresent()`, where
   `CheckKgslPresent()` is `access("/dev/kgsl-3d0", F_OK) == 0` — Qualcomm's
   kernel driver. On a Mali device that is false, so the custom-driver UI is
   never offered no matter what the driver can do. The check conflates
   "which GPU is this" with "can drivers be loaded without root"; the API
   level is about the namespace APIs and is not GPU-specific. Accepting
   `/dev/mali0` alongside KGSL is the whole fix.

2. **The driver namespace**, in `libadrenotools`
   (`src/hook/hook_impl.cpp`). The hook intercepts the Vulkan loader's
   `android_dlopen_ext` and builds a namespace for the custom driver with
   only `customDriverDir` on its search path and only `libandroid.so`
   linked over. A Mesa driver needs `/system/lib64` and `/vendor/lib64` on
   the path (for `libdrm.so`, `libhardware.so`) and needs
   `libvndksupport.so`/`libdl_android.so` linked in.

**The honest limit of the local verification.** Running the same namespace
construction from a shell binary passes *whether or not* the extra paths and
links are present, so a shell process cannot demonstrate that the change is
needed. The reason is worth internalising: a shell sits in the default
namespace, which already sees `/system/lib64` and `/vendor/lib64`, and
`ANDROID_NAMESPACE_TYPE_SHARED` inherits that. An app's namespace is the
restricted one.

What *is* measured, by `tests/driver_namespace_probe` under its ISOLATED
shape (which cuts the default namespace off and so models the restricted
case):

```
stock config    -> dlopen failed: library "libdl_android.so" needed or
                   dlopened by "/system/lib64/libvndksupport.so" is not
                   accessible for the namespace
with the paths  -> loads, enumerates Mali-G720 MC8
```

So the change is demonstrably necessary under isolation and demonstrably
sufficient in both shapes. Whether an app's namespace is restrictive enough
to require it is a reasonable inference, not a measurement — settling that
needs an APK, which is the same gap that blocks the swapchain work.

Both patches are written and committed on branches in the local clone.
**Deliberately not proposed upstream yet** — the priority is this repo.

## What a frame actually costs — and it is not the sync-fd design

`tests/driver_present_loop_probe` reproduces the driver-visible sequence of a
present loop — `vkImportSemaphoreFdKHR(SYNC_FD, -1)` for acquire, a real
render pass, then `vkGetSemaphoreFdKHR(SYNC_FD)` for release — and times each
phase. It cannot call `vkAcquireImageANDROID`/`vkQueueSignalReleaseImageANDROID`
(those need a swapchain, which needs an app), but it performs exactly what
those two do, so the numbers are the driver's cost rather than a swapchain's.

**1280x720, 3 images, 120 frames, trivial GPU work (clear + one triangle):**

```
                 min      med      p95      max     mean
frame total      3.15    27.97    35.75    38.33    24.09
  acquire        0.00     0.01     0.01     0.05     0.01
  record+submit  2.11    26.54    32.30    34.41    21.75
  release export 0.97     1.87     4.55     5.02     2.33

120 frames in 2890.9 ms -> 24.09 ms/frame, 41.5 fps
```

**This refutes the prediction that had been driving the roadmap.** The
expectation — written down in several places — was that the CPU-blocking
`vkGetSemaphoreFdKHR` would dominate, because kbase cannot produce a real
fence and the compositor therefore cannot wait on our behalf. It does not.
Release costs **~2 ms**. The dominant cost is `record+submit` at ~22 ms, and
`acquire` is free.

**Where the 22 ms actually goes.** Running the same loop with
`--mode=pipelined`, which drops only the blocking release and never waits:

```
frame total  min 0.02  med 0.03  p95 0.04  max 4.89  mean 0.08   (0.08 ms/frame)
```

So the CPU-side cost of building and submitting a frame is **0.08 ms**.
Essentially all of the 22 ms is the submit *waiting*, not working — it is
`pan_kmod_kbase_queue_wait_idle()` blocking for `CS_ACTIVE` to clear from the
previous frame, which is documented to linger 30-40 ms after a stream ends.

That gives the real causal chain, which is one step removed from where the
cost appears:

> blocking release each frame → GPU goes idle → the *next* frame's submit
> must kick → the kick must first wait for `CS_ACTIVE` → ~22 ms.

The release is what *causes* the cost but not where it is *paid*, which is
why measuring only the export made it look cheap and innocent.

**`PANVK_KBASE_KICK_MODE=nowait` is not a way out.** Dropping the pre-kick
wait made the loop stop making progress entirely — 60 frames did not finish
inside a 180 s timeout, where the same loop takes ~1.4 s otherwise. That
matches the previously recorded and still-unexplained finding that removing
the wait "reproducibly loses the compute subqueue's stream". Device confirmed
healthy afterwards (no D-state process, `driver_compute_probe --submit
--fill` clean), so this is a lost stream rather than a wedge.

### What this means

- **~41 fps of pure driver overhead at 720p with a triangle.** Fits a 30 fps
  budget; does not fit 60. Real application work adds on top, so an emulator
  targeting 60 fps has no headroom and one targeting 30 has little.
- **The sync-fd design is not the problem** and does not need revisiting.
  Returning -1 from the export costs ~2 ms.
- **The bottleneck is the pre-kick idle wait**, which this repo already knew
  was "correct-but-slow" and already flagged as its open question. This
  measurement upgrades it from a known inefficiency to *the* thing standing
  between this driver and being usable, and gives it a number.
- The distribution is bimodal (min 3.15 vs med 27.97), which is what a
  per-frame wake-up cost looks like and why the percentiles are reported
  rather than a mean.

The next question is therefore the old one, now worth real effort: why does a
kick need the CS to be idle first, and what is actually being lost when it
does not? That is the difference between ~41 fps and something far higher —
the pipelined number says the ceiling is not the driver's CPU work.

## The pre-kick wait was never necessary — 41 fps to 64 fps

The bottleneck the present-loop measurement identified, solved. It closes a
question this repo had had open since the submit path was written: *why does
a kick need the CS to be idle first?*

**It does not.** The question contained a wrong assumption. Two behaviours
had been measured and correctly recorded:

- a CS that is still *executing* re-reads `CS_INSERT`, so work appended to it
  runs with no kick at all;
- a kick issued while `CS_ACTIVE` is set does not take effect, and dropping
  the wait (`PANVK_KBASE_KICK_MODE=nowait`) loses the stream.

Both are true, and together they look like "so you must wait for `CS_ACTIVE`
to clear before kicking". That conclusion skips a third option nobody had
tried: **when `CS_ACTIVE` is set, do not kick at all** — just publish
`CS_INSERT` and let the still-resident CS pick it up, exactly as the
`extract < old_insert` case already did.

That works, and it means the CS **stops needing a kick well before it stops
reporting active**. The wait was paying 30-40 ms for a state transition that
was irrelevant to whether the work would run.

`PANVK_KBASE_KICK_MODE=defer` implements it and is now **the default**;
`auto` still selects the old wait-before-kick behaviour without a rebuild.

**Measured** (`tests/driver_present_loop_probe`, 1280x720, 120 frames):

```
                     AUTO (old)   DEFER (new)
  frame total (med)    27.97 ms     15.33 ms
  record+submit (med)  26.54 ms      0.19 ms     <- the wait, gone
  release export (med)  1.87 ms     14.78 ms     <- unchanged; now visible
  throughput           41.5 fps     64.4 fps
```

and on a different workload, 200 fence-waited compute submits:
**4769 ms -> 3063 ms** (~23.8 -> ~15.3 ms each).

Note what happened to the breakdown rather than just the total. The release
export did not get slower; it was always going to wait for the GPU to finish
the frame. It only *looks* larger now because the 22 ms of submit-side
waiting that used to sit in front of it is gone, so the GPU has less of a
head start by the time the export asks. The remaining ~15 ms is real work
plus real completion latency, not overhead.

**Correctness is the part that matters here**, because a change that loses
streams would show up as *better* numbers. All nine render probes pass
pixel-exact under it (a lost stream shows as wrong pixels or a timeout, not
as a faster wrong answer), plus the full driver tier, `--burst=100`, and 300
fence-waited compute submits. Full suite: 30/30 with the new default.

### What this changes about the outlook

The earlier conclusion — "~41 fps of driver overhead, so 30fps has little
headroom and 60fps none" — is superseded. At 64 fps the driver is no longer
obviously the thing that makes an emulator unusable, and the remaining
per-frame cost is dominated by honest GPU completion latency rather than by
waiting for a state flag.

The next lever, if one is wanted, is no longer the submit path: it is that
`vkGetSemaphoreFdKHR` must block at all, which is a consequence of kbase
being unable to produce a real fence. That is a genuine kernel limitation
rather than a driver choice, and it is what a compositor would otherwise
absorb.

## CTS: `command_buffers` run to completion (and the defer submit path at scale)

The first of the deferred Phase 7 sweeps, and the first large-scale exercise
of the new `KBASE_KICK_DEFER` submit path — CTS drives submission far harder
than the probe suite does, so if deferring the kick lost streams this is
where it would show.

**Result: 130 of 131 cases, 57 Pass / 1 Fail / 71 NotSupported.**

The single failure is `many_indirect_draws_on_secondary` — one of the two
secondary-command-buffer failures already known and triaged. **No new
failures**, so the submit-path change caused no regression at this scale.

Worth noting what *didn't* fail: `record_many_draws_secondary_2`, the other
known one, passed. That is consistent with the cold-start finding recorded
earlier — it fails only as the *first* secondary-buffer draw in a process,
and in a full alphabetical run `many_indirect_draws_on_secondary` (m) runs
before `record_many_draws_secondary_2` (r) and warms it up. The theory
predicted this, which is mild independent support for it.

The 71 `NotSupported` is high but expected for this group: much of it needs
features this driver does not advertise.

**The 131st case, `trim_command_pool`, stalled** — the run sat on it for 30+
minutes. This is the previously-documented pattern rather than a new hang:

```
PID   STAT  ELAPSED   CPU-TIME   CMD
6548  S     30:56     00:00:53   deqp-vk
```

State `S` (sleeping), 53 seconds of CPU across 31 minutes of wall time, no
`D`-state process anywhere, and `driver_compute_probe --submit --fill` run
*concurrently* against the same device came back clean. So the GPU and the
kbase context are fine and something above them is waiting — the same
signature as the `buffer` group stall recorded earlier, which was closed as
kernel-side memory-pool reclaim rather than a driver defect.

Two things follow. `trim_command_pool` joins the exclusion list for
unattended runs. And the ability to run a health probe against the device
*while* a CTS process is stalled remains the cheapest way to tell "the
driver is wedged" from "something else is slow" — worth reaching for first,
because the two look identical from the test runner's point of view.

## CTS: the deferred sweeps, sampled — and `vkCmdClearColorImage` is broken

`copy_and_blit`, `image_clearing` and `query_pool` were deferred as "huge
sweeps". Enumerating them shows why, and settles how they can be run at all:

```
api.copy_and_blit   201,346 cases
api.image_clearing   45,636 cases
query_pool           18,880 cases
```

~266,000 cases. At the rate `command_buffers` ran (~2 s/case for GPU-heavy
ones) that is days of wall time, so running them whole is not a thing that
happens. Instead each was sampled with a **uniform stride over the
alphabetically-sorted caselist** (`--deqp-runmode=stdout-caselist`, then
`awk 'NR % stride == 1'`), giving ~400 cases per group. A uniform stride
hits every subgroup rather than clustering in whichever one sorts first,
which a `head -400` would not.

### `copy_and_blit` — 401 sampled: 181 pass, 1 fail, 219 not supported

One failure:
`core.use_after_copy.r32g32b32_sfloat.transfer_dst_optimal.32x32x1_regions_3d_img_linear`
— "Unexpected output in color buffer". A single case in a
copy-then-reuse-the-image path, not obviously a family. Worth a look, not
alarming.

### `image_clearing` — 401 sampled: 152 pass, **106 fail**, 143 not supported

**This is a real driver bug and the most valuable thing the sweeps found.**
103 of the 106 failures are `clear_color_image`; 3 are
`clear_depth_stencil_image`. Of 272 `clear_color_image` cases sampled, 103
failed.

The signature is identical everywhere: the readback is **(0,0,0,x)** — the
clear simply did not happen — across a wide spread of formats
(`r8g8b8a8_unorm`, `r8g8b8a8_uint`, `r5g6b5_unorm_pack16`,
`b4g4r4a4_unorm_pack16`, `b10g11r11_ufloat_pack32`, `r16g16_unorm`,
`r16g16_uint`, ...):

```
Fail (Color value mismatch! Ref:(0.1, 0.5, 0.3, 0.9) Mask:(1,1,1,1)
      Threshold:(0.0039,...) Color:(0, 0, 0, 0))
```

The spread by axis, which is where the first reading of this went wrong:

```
1d.linear 20   1d.optimal 18
2d.linear 28   2d.optimal 20
3d.linear  4   3d.optimal 13

single_layer 45  multiple_layers 21  remaining_array_layers 25
                                     remaining_array_layers_twostep 12
```

**CORRECTION — the first version of this section said the failures were
"not confined to any axis" and that `r8g8b8a8_unorm` failing proved it was
"not an exotic-format problem". Both claims were wrong**, and the counts
above are what misled: every cell being non-zero is not the same as the
failures being uniform.

Running individual cases shows the plainest configuration **passes**:

```
2d.optimal.single_layer.r8g8b8a8_unorm   -> Pass (cmdClearColorImage passed)
```

The `r8g8b8a8_unorm` failure that prompted the "not exotic" claim was
`1d.linear.remaining_array_layers.r8g8b8a8_unorm` — 1D, linear-tiled, with
array layers. Not a plain case at all; the format was the only ordinary
thing about it.

Listing the failing `single_layer` cases shows what they actually have in
common — they are `1d.*`, `2d.linear.*`, or `2d.optimal` with a less common
format:

```
1d.linear.single_layer.r64_uint
1d.optimal.single_layer.a1r5g5b5_unorm_pack16
1d.optimal.single_layer.r16_sfloat
1d.optimal.single_layer.r32g32b32_sfloat
1d.optimal.single_layer.r8_snorm
1d.optimal.single_layer.r8g8b8_srgb
2d.linear.single_layer.a8b8g8r8_uint_pack32_200x180
2d.linear.single_layer.r8g8b8a8_snorm_200x180
2d.optimal.single_layer.r32_uint
```

### The actual finding: the non-AFBC clear path is broken

The "three axes" reading above was also wrong — or rather, it was three
symptoms of one cause. Isolating each axis with the format held constant,
then re-running the same eight cases with `PANVK_DEBUG=noafbc`:

| case | default | `noafbc` |
|---|---|---|
| `2d.optimal.single_layer.r8g8b8a8_unorm` | **Pass** | **Fail** |
| `2d.optimal.multiple_layers.r8g8b8a8_unorm` | **Pass** | **Fail** |
| `2d.optimal.single_layer.r16_sfloat` | **Pass** | **Fail** |
| `2d.optimal.single_layer.r8g8b8_srgb` | **Pass** | **Fail** |
| `1d.optimal.single_layer.r8g8b8a8_unorm` | Fail | Fail |
| `3d.optimal.single_layer.r8g8b8a8_unorm` | Fail | Fail |
| `2d.linear.single_layer.r8g8b8a8_unorm` | Fail | Fail |
| `2d.optimal.single_layer.r32_uint` | Fail | Fail |

**Disabling AFBC makes it worse, not better: 4/8 failing becomes 8/8.** So
AFBC is not the cause — it is the thing making the passing cases pass. Every
data point collapses into one statement:

> `vkCmdClearColorImage` works on AFBC surfaces and fails on everything else.

And that explains the apparent axes without needing three separate bugs.
1D images, 3D images, linear tiling and formats like `r32_uint` are all
cases where AFBC does not apply, so they take the uncompressed path and get
black. Array layers were never an axis, which is why `multiple_layers`
passed.

This is a better bug report than "the entry point is broken", and it is also
a **worse** bug than the correction above suggested: it is not confined to
odd configurations, it is the entire uncompressed clear path. It only looks
narrow because AFBC-by-default hides it for the most common surfaces.

**Probably not a kbase-port bug.** Everything this hinges on — image layout,
AFBC eligibility, the framebuffer descriptors `vk_meta`'s render pass writes
— is upstream PanVK code that this port does not touch, and the kbase submit
gate is demonstrably not involved (no refusal appears in logcat for a
failing case; the stream runs and produces black). Not provable from here
without panthor hardware, but it is where the evidence points, and it means
the fix belongs upstream rather than in `src/mesa/`.

**Confirmed on this hardware: it is not "non-AFBC clears are broken", it is
specific to the `vkCmdClearColorImage` entry point.** `tests/render_clear_probe`
gained three flags (`--linear`, `--general`, `--no-ca-usage`) to reproduce, via
a real `LOAD_OP_CLEAR` render pass, every condition the table above uses to
explain the `vkCmdClearColorImage` failures:

- `--linear` — `VK_IMAGE_TILING_LINEAR`, the same tiling as the failing
  `2d.linear.single_layer.r8g8b8a8_unorm` case, and AFBC-ineligible for the
  same reason.
- `--general` — `VK_IMAGE_LAYOUT_GENERAL` for the render target instead of
  `COLOR_ATTACHMENT_OPTIMAL`, matching the layout `vk_meta_clear_color_image`
  actually passes through to `VkRenderingAttachmentInfo`.
- `--no-ca-usage` — the image created with only `TRANSFER_DST_BIT`, with
  `COLOR_ATTACHMENT_BIT` added after the fact on the view via
  `VkImageViewUsageCreateInfo`, which is what an application calling
  `vkCmdClearColorImage` (never `vkCmdBeginRendering`) actually does.

All three ran clean on the Poco X8 Pro (Mali-G720, r49p1): 16/16 pixels held
the exact clear colour in every case, individually and matching baseline.
Re-ran `driver_compute_probe --fill` after — 0 failures, device not left
degraded.

So a render-pass clear survives every condition that makes the standalone
entry point fail. That rules out "the uncompressed/non-AFBC clear path is
broken" as the cause — the uncompressed path clearly works, render passes
use it constantly and are pixel-exact. What's actually broken is narrower
and lives somewhere in `vk_meta_clear_color_image` itself (or the internal
render pass it builds) — not in image layout handling, AFBC eligibility, or
tiling in general. This does not change the "probably upstream" conclusion,
it sharpens it: the bug is in `vk_meta`'s clear implementation, not in the
framebuffer/AFBC machinery it shares with every other render-pass clear.

**Why no probe caught this.** Every render probe in `src/tests/` clears via
`VK_ATTACHMENT_LOAD_OP_CLEAR` — a render-pass clear, which works and is
pixel-exact. `vkCmdClearColorImage` is a different entry point: a standalone
clear outside a render pass. Nothing in this repo exercised it, so a whole
code path was unverified while the probes all passed. That is the case for
running CTS rather than trusting a hand-written suite, and it is worth
remembering the next time coverage looks complete.

Not yet root-caused. It is squarely in the area the recently-rewritten
`pan_fb` framebuffer abstraction and AFBC-by-default both touch, which makes
`PANVK_DEBUG=noafbc` an obvious experiment — though the corrected axis
analysis above (1D / linear / uncommon formats) points more at format and
layout handling than at compression.

**On Arm's guidance that this entry point should be avoided.** Arm's
"picking the most efficient load/store operations" advises against
`vkCmdClearColorImage` on Mali because a tiler clears for free via
`VK_ATTACHMENT_LOAD_OP_CLEAR`, whereas an explicit clear forces a
full-surface write to external memory. That is about *bandwidth*, and it is
correct, but it does not bear on this finding: a slow path still has to
produce the right pixels, and `vkCmdClearColorImage` is core Vulkan 1.0
rather than optional, which is why CTS tests it.

Where the guidance does apply is priority and blame-assignment. It plausibly
explains why this went unnoticed — if applications follow it, the path sees
little traffic — and it explains why no probe here caught it, since every
render probe in `src/tests/` clears via `LOAD_OP_CLEAR`. Combined with the
correction above (the common configuration passes), the realistic impact is
smaller than 103 failures suggests. It is still a conformance defect.

### `query_pool` — see below

### Method note worth keeping

Sampling by stride makes a 200k-case group answerable in ~20 minutes, and
the failure grouping (`sed`/`cut`/`uniq -c` over the failing case names) is
what turned 106 individual failures into one sentence. Do that before
reading any individual log.

### `query_pool` — blocked by the resource ceiling, not by query support

Of 401 sampled cases, **56 ran**: 8 pass, 1 fail, 47 not supported. The run
aborted three times, and each abort is the same thing:

```
ResourceError (vk.createDevice(...): VK_ERROR_OUT_OF_DEVICE_MEMORY
               at vkRefUtilImpl.inl:347)
```

CTS treats a `ResourceError` as fatal and stops the whole run, so one such
case costs every case after it.

Two distinct cases trigger it reproducibly:

- `query_pool.maintenance7.query_32b_wrap_required` — aborted at case 2 on
  two consecutive fresh processes, so it is that case rather than
  accumulated state.
- `query_pool.statistics_query.compute_shader_invocations.32bits_dstoffset_cmdcopyquerypoolresults_stride_zero_primary_cq`
  — aborted at case 56 once the first was excluded.

Excluding the first took the run from 2 cases to 56, which is what
establishes these as specific cases rather than a cumulative wall.

This is the **known resource-ceiling family** (Phase 7, closed as downstream
kbase pool/shrinker behaviour rather than a bug in this port), and the
device was healthy throughout: no `D`-state process, `driver_compute_probe
--submit --fill` clean afterwards, and `MemAvailable` 4.6 GB with
`CmaFree` 108 MB — so system memory is not exhausted, which matches the
earlier finding that this ceiling is invisible to `/proc/meminfo`.

Worth separating two things that look alike: `vkCreateDevice` returning
`VK_ERROR_OUT_OF_DEVICE_MEMORY` here is *not* the same as the feature being
unsupported — CTS reports genuinely unsupported features as `NotSupported`
and carries on, as it did for 47 cases in this very run. So these two cases
are asking for a device configuration that makes creation fail rather than
one the driver declines, and that distinction is where a root-cause attempt
should start.

**Practical consequence:** `query_pool` cannot be swept without an exclusion
list built by bisection, one aborting case at a time. Both cases above go on
it alongside `trim_command_pool`.

### Item 2 summary

| group | sampled | pass | fail | notsupported | notes |
|---|---|---|---|---|---|
| `command_buffers` | 131 (all) | 57 | 1 | 71 | 1 known failure; `trim_command_pool` stalled |
| `copy_and_blit` | 401 / 201,346 | 181 | 1 | 219 | one `use_after_copy` failure |
| `image_clearing` | 401 / 45,636 | 152 | **106** | 143 | **`vkCmdClearColorImage` broken** |
| `query_pool` | 56 / 18,880 | 8 | 1 | 47 | aborted by the resource ceiling |

The headline is `vkCmdClearColorImage`. Everything else is either a known
issue or a single case.
