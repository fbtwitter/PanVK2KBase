# References

## Mesa / Panfrost / PanVK
- Mesa source (upstream, GitLab): https://gitlab.freedesktop.org/mesa/mesa
- Panfrost docs: https://docs.mesa3d.org/drivers/panfrost.html
- `pan_kmod` lives at `src/panfrost/lib/` in the Mesa tree — this is the
  abstraction layer a `pan_kmod_kbase` backend needs to plug into (see
  Phase 2 in `ROADMAP.md`).
- `#panfrost` on Matrix / OFTC IRC — the right place to ask questions and
  announce intent to work on this before you're deep into code.

## Panfork — the closest prior art, and the one to read first
Panfrost (GL) running on **kbase** on CSF/Valhall v10, i.e. this repo's
problem already solved one layer down. See the "Prior art found" section
in `docs/kbase-notes.md` for what was extracted and the two findings that
came out of it.
- Canonical repo `https://gitlab.com/panfork/mesa` is **dead** — emptied to
  a "use upstream instead" README once panthor landed. Use a mirror:
  `https://github.com/ROCKNIX/mesa-panfork` (cloned to the gitignored
  `third_party/PANFORK/`; re-run the clone if missing).
- The kbase layer is `src/panfrost/base/` — `pan_vX_base.c` is the one
  that matters (CS bind/submit/wait, event memory, syncobjs).
- `https://github.com/PojavLauncherTeam/panfork_offscreen_rootless` and
  `https://github.com/SolDev69/panfrost-gallium-mesa` — the same thing on
  **stock unrooted Android on kbase**. Relevant to the no-root blocker in
  `kbase-notes.md`.
- `https://gitlab.com/icecream95/panloader` — `pantrace` tracing tool.
- `https://gitlab.com/icecream95/kbase-valhall` — kbase patched for
  `MALI_NO_MALI`: run the blob userspace against a fake kernel driver, no
  Mali hardware and no root. Possible substitute for a rooted device.
- Write-ups: `https://icecream95.gitlab.io/` — the "Mali G610 Reverse
  Engineering" series and "The Mali CSF Command Stream Instruction Set".

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
- This repo's actual target is Mali-G615-MC2 (UK 1.20 CSF / r44p0) — verify
  its conformance status separately; do not assume it inherits G610's
  conformant status. See the Phase 0 note in `ROADMAP.md`.
- Panthor (CSF, v10+) and its in-progress Rust reimplementation "Tyr" are
  the stated upstream kernel-driver priority — kbase support is not on
  the public roadmap, which is why this is a from-scratch effort.
