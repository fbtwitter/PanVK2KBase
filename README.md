# PanVK2KBase

Translation layer for PanVK (Mesa's open-source Vulkan driver for Arm
Mali GPUs) to run on **kbase**, Arm's out-of-tree/vendor kernel driver —
instead of the upstream `panfrost`/`panthor` DRM drivers PanVK currently
requires. Target device: Mali-G615-MC2, kbase r44p0 / UK interface 1.20 (CSF).

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

## License

MIT, to match Mesa's licensing, so anything here can eventually be
upstreamed without relicensing friction.
