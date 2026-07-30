# Mesa's CS instruction encoder (`cs_builder.h`)

## Why this exists

`docs/kbase-notes.md`'s "poll()/read() on the kbase fd" investigation
found that `tests/queue_group/queue_group.c`'s `KICK` never produces a
CSF notification, most likely because it only writes sentinel
`0xdeadbeef` words into the queue buffer instead of a real command
stream. Confirming (or ruling out) the notification channel needs an
actual, correctly-encoded Mali CSF instruction - guessing at the byte
encoding by hand and running it on real GPU firmware is a meaningfully
higher-risk experiment than any ioctl-level probe so far (wrong bytes
are executed by firmware directly, not just rejected by a syscall), so
this uses Mesa's own encoder (`src/panfrost/genxml/cs_builder.h`)
instead of hand-rolled bytes.

`tests/cs_encode_probe/cs_encode_probe.c` is step one: prove the
encoder produces correct bytes for this device's architecture,
entirely offline (no kbase/device access). Confirmed output for a
single `cs_move32_to(..., 0x1234)`:

```
root chunk size = 8 bytes (1 instructions)
  [0] 0x0200000000001234  opcode(top byte)=0x02
```

`0x02` matches `MOVE32` exactly in `v12.xml`'s opcode table, and the
low 32 bits match the immediate value. Wiring this into a live `KICK`
with the correct insert-pointer protocol is the next step, not yet
done - see `docs/kbase-notes.md`.

## One-time setup

### 1. Clone Mesa

Not vendored in this repo - gitignored (`/third_party/MESA-KMOD` in
`.gitignore`), ~600MB, local to whichever machine needs it:

```
git clone --depth 1 --filter=blob:none https://gitlab.freedesktop.org/mesa/mesa.git third_party/MESA-KMOD
```

### 2. A working Python interpreter

The genxml codegen step (`gen_pack.py`, below) needs Python 3. On a
normal Linux dev box `python3` is standard and the `PYTHON` makefile
variable defaults to it - nothing else to do.

**Windows gotcha, hit setting this up:** `python3`/`python` on PATH may
resolve to the Microsoft Store's app-execution-alias stub instead of a
real interpreter, even after installing Python (via `winget install
--id Python.Python.3.12`, in this case) - the stub sits earlier on
PATH than wherever winget's actual install landed. The Python Launcher
(`py.exe`, ships with the official installer) reliably resolves to the
real interpreter regardless. Override the makefile variable to use it:

```
make cs_encode_probe PYTHON="/c/Users/<you>/AppData/Local/Programs/Python/Launcher/py.exe -3.12"
```

(Adjust the path/launcher invocation for your machine - `py -0` lists
installed versions if unsure which to target.)

## Building

```
make cs_encode_probe          # generates v<PAN_ARCH>_pack.h if missing, then builds
```

`PAN_ARCH` defaults to `12` (this repo's Poco X8 Pro/Mali-G720 target -
see `utils/parse_gpu_props.h`'s decode, architecture 12.8). A different
device needs a different `PAN_ARCH` (see the version list in
`third_party/MESA-KMOD/src/panfrost/genxml/meson.build`'s `pan_packers`
- `4/5/6/7/9/10/12/13/14` at time of writing) and its own generated
pack header, e.g. `make cs_encode_probe PAN_ARCH=10`.

Cross-compiling for the device (same NDK toolchain as every other test
in this repo - see `docs/kbase-notes.md`):

```
CC="$ANDROID_NDK/toolchains/llvm/prebuilt/<host>/bin/aarch64-linux-android26-clang" \
  make cs_encode_probe
```

## What actually needs linking, and why

`cs_builder.h` itself is header-only (`static inline` throughout), but
it calls into two Mesa `util/` functions that aren't:
`reralloc_size` (`src/util/ralloc.c`) and
`util_dynarray_is_data_stack_allocated` (`src/util/u_dynarray.c`).
The `cs_encode_probe` makefile target compiles those two `.c` files
directly alongside the test - no other Mesa source files, no meson, no
building the rest of Mesa.

Two things that weren't obvious from the error messages alone:

- **`-DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC`**: `ralloc.c` transitively
  includes `src/c11/threads.h` and `src/c11/time.h`, Mesa's C11
  compat shims. Both branch on macros meson normally supplies after
  probing the target platform; without them, `threads.h` hits its
  "not supported" `#error` branch and `time.h` redefines `struct
  timespec` on top of the platform's own (causing a redefinition
  error). Android and Linux both have real pthreads and a real
  `struct timespec`, so both macros are safe to define unconditionally
  here.
- **`-ffunction-sections -fdata-sections -Wl,--gc-sections`**:
  without dead-code stripping, linking against `ralloc.c` pulls in
  its `ralloc_vasprintf`/`linear_vasprintf` functions too (same
  object file, always linked as a whole unit), which need
  `u_printf.c` for GPU-shader-printf serialization - which itself
  needs `blob.c` and `_mesa_hash_table_u64` and `simple_mtx.c` and
  `u_call_once.c`. None of that is reachable from `main()` in this
  test; `--gc-sections` discards the unreachable functions (and their
  otherwise-unresolved dependencies) before the linker ever needs to
  resolve them, instead of pulling in that whole chain for one
  function (`reralloc_size`) that doesn't need any of it.

## Regenerating `v<N>_pack.h`

`make mesa-cs-pack` (or just building `cs_encode_probe`, which depends
on it) runs `gen_pack.py v<N>.xml > v<N>_pack.h` inside
`third_party/MESA-KMOD/src/panfrost/genxml/`. This is a generated
artifact of the gitignored Mesa clone, not tracked by this repo -
regenerate it any time by deleting it and rebuilding, or if the Mesa
clone is refreshed to a newer commit.
