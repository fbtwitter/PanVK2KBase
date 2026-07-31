# PanVK2KBase

Translation layer for PanVK (Mesa's open-source Vulkan driver for Arm
Mali GPUs) to run on **kbase**, Arm's out-of-tree/vendor kernel driver —
instead of the upstream `panfrost`/`panthor` DRM drivers PanVK currently
requires. Target device: Mali-G615-MC2, kbase r44p0 / UK interface 1.20 (CSF).

**Motivating end-state:** essentially every shipping Android phone with a
Mali GPU runs kbase, not panthor/panfrost — so PanVK cannot run on real
Android hardware at all today. Solving that is the same shape of problem
Turnip solved for Adreno via its `kgsl` backend, which is what eventually
made Turnip loadable as a standalone, swappable Vulkan driver in Android
apps (custom-driver pickers in emulators like Eden and Azahar, GPU driver
managers, etc.). This repo is aimed at the same outcome for Mali: a PanVK
build that runs on stock Android via kbase, packaged the same way. See
`ROADMAP.md` for the full path there and honest expectations on how far
that is (Turnip took years of investment to get where it is; PanVK's own
Vulkan maturity on Mali is not there yet independent of the kbase problem
this repo solves).

**Status: pre-alpha.** Standalone device probing works (see below);
nothing wires into Mesa/PanVK yet.

## Repo layout

```
third_party/kbase-uapi-r44p0/  real vendored kbase UAPI headers for the
                                target kernel (r44p0 / UK 1.20 CSF)
third_party/kbase-uapi-r49p1/  second vendored header set (r49p1 / UK 1.30
                                CSF), confirmed against a real mt6899/
                                Mali-G720 device - see docs/kbase-notes.md
utils/parse_gpu_props.h        decodes KBASE_IOCTL_GET_GPUPROPS output:
                                GPU ID, model, shader/L2 core counts, etc.
tests/first_test/              standalone probe: open /dev/mali0, version
                                check, query GPU props, decode - zero
                                Mesa dependency
docs/                          architecture notes, kbase research notes,
                                references
ROADMAP.md                     phased plan from here to a working PanVK
                                backend on kbase
```

## Getting started

1. Read `ROADMAP.md` for the phased plan and current status.
2. `make` at the repo root builds `tests/first_test/first_test`.
3. Run it on the target device (as a user that can open `/dev/mali0`) to
   confirm the probe/decode still works before touching anything else.
4. See `docs/architecture.md` for the core problem (kbase is not a DRM
   device) that has to be solved before Mesa integration can start.

## Vendored headers

ALL THIRD PARTY LIBRARIES COME WITH THEIR OWN LICENSE AND USAGE, ALL CREDIT
GOES TO THE ORIGINAL AUTHORS.

### kbase r44p0 uapi headers

Vendored from:
https://nest-open-source.googlesource.com/manifest_repos/mali-driver
path: bifrost/r44p0/kernel/include/uapi/gpu/arm/midgard
commit: 0f8397eced2de6bc649a9cc32d0fae77a1dc34dc

Pulled to match UK interface 44.10 for a Mali-G615-MC2 target.
Unmodified except as noted in individual file diffs, if any.
Original license notices preserved in each file - see file headers,
not this README, for authoritative licensing.

## License

MIT, to match Mesa's licensing, so anything here can eventually be
upstreamed without relicensing friction.
