# References

## Mesa / Panfrost / PanVK
- Mesa source (upstream, GitLab): https://gitlab.freedesktop.org/mesa/mesa
- Panfrost docs: https://docs.mesa3d.org/drivers/panfrost.html
- `pan_kmod` lives at `src/panfrost/lib/` in the Mesa tree — this is the
  abstraction layer a `pan_kmod_kbase` backend needs to plug into (see
  Phase 2 in `ROADMAP.md`).
- `#panfrost` on Matrix / OFTC IRC — the right place to ask questions and
  announce intent to work on this before you're deep into code.

## Turnip / kgsl (the precedent to study)
- Freedreno/Turnip source: `src/freedreno/vulkan/` in the Mesa tree.
- `tu_knl_kgsl.cc` — the kgsl winsys backend; read this before writing
  a `pan_kmod_kbase.c` structurally, not for ioctl details (different
  hardware, different ABI) but for how the "non-DRM device" problem was
  actually solved in the enumeration/instance-creation layer.

## kbase UAPI (this repo's vendored header)
- Source: `https://nest-open-source.googlesource.com/manifest_repos/mali-driver`
  — see `third_party/kbase-uapi-r44p0/README.md` for the exact path and
  commit this repo pulled from.

## Android driver loading (separate problem, noted for later)
- `libadrenotools` (Turnip's Android driver-override loader) — the
  mechanism that lets apps like emulators load a custom Vulkan ICD
  without root, by exploiting Android's Vulkan loader's ability to load
  a driver `.so` from app-private storage. No Mali equivalent exists
  today. Out of scope until Phase 6 (WSI) is working and this needs to be
  usable inside an actual Android app rather than just on a Linux/
  embedded target.

## Hardware/software context this repo was scoped against (mid-2026)
- PanVK is Vulkan-conformant on Mali-G610 (Valhall, v10, CSF).
- This repo's actual target is Mali-G615-MC2 (UK 44.10 / r44p0) — verify
  its conformance status separately; do not assume it inherits G610's
  conformant status. See the Phase 0 note in `ROADMAP.md`.
- Panthor (CSF, v10+) and its in-progress Rust reimplementation "Tyr" are
  the stated upstream kernel-driver priority — kbase support is not on
  the public roadmap, which is why this is a from-scratch effort.
