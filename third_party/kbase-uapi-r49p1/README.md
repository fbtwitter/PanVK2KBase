# kbase r49p1 uapi headers

Vendored from:
https://github.com/Mayuri-Chan/MTK_kernel_device_modules_6.6
branch: lineage-22.1
commit: 3b31307ea54d689cecab0f231d43208959287cd1
path: drivers/gpu/mediatek/gpu_mali/mali_avalon/mali-r49p1/drivers/gpu/arm/midgard/include/uapi/gpu/arm/midgard

This is MediaTek's vendored copy of Arm's kbase driver for the
`mali_avalon` tree (used by, among others, the mt6899 / Dimensity 8500
Ultra SoC). Confirmed to match the CSF UK interface version reported by a
real device on this driver: `KBASE_IOCTL_VERSION_CHECK` on a Poco X8 Pro
(mt6899, Mali-G720/"Mali-TTIX") returned `major=1, minor=30`, which is
exactly what `csf/mali_kbase_csf_ioctl.h`'s `BASE_UK_VERSION_MAJOR` /
`BASE_UK_VERSION_MINOR` say here — see `docs/kbase-notes.md` for the full
test writeup. Unlike `kbase-uapi-r44p0`, this pairing has been verified
against a live device's `VERSION_CHECK` response, not just assumed from
the driver release name.

File set mirrors `third_party/kbase-uapi-r44p0/` exactly (same 18 files,
same relative layout) so the two can be swapped by changing
`KBASE_UAPI_DIR` in the root `makefile`.

Unmodified except as noted in individual file diffs, if any.
Original license notices preserved in each file - see file headers,
not this README, for authoritative licensing (SPDX `GPL-2.0 WITH
Linux-syscall-note`, `(C) ARM Limited`).
