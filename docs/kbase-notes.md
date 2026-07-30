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

**Why this can't be chased further from userspace on this device.**
The failure path is `dev_dbg`-only and otherwise silent, so it needs
kernel-side visibility — and `dmesg` returns `klogctl: Permission
denied` for the unprivileged `shell` user here (KTRACE/debugfs likewise
out of reach without root). Candidate next steps, all needing more than
this probe: a rooted device or a userdebug build to read the driver's
own diagnostics; comparing against a trace of the vendor blob driver
doing a real submission (it clearly gets groups onto slots); or
checking whether group creation needs more setup than this probe does
(e.g. a tiler heap via `CS_TILER_HEAP_INIT`, or the `dvs_buf` field
left zero in `GROUP_CREATE`) before the scheduler will consider the
group schedulable.

## Where to ask

The `#panfrost` channel (Matrix, bridged to OFTC IRC) is where Panfrost/
PanVK/Panthor upstream discussion happens. Worth lurking before you start
and posting once Phase 1 (standalone probe — already working here) is
solid — see Phase 9 in `ROADMAP.md` for why raising it early matters.
