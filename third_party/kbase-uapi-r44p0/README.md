# kbase r44p0 uapi headers

Vendored from:
https://nest-open-source.googlesource.com/manifest_repos/mali-driver
path: bifrost/r44p0/kernel/include/uapi/gpu/arm/midgard
commit: 0f8397eced2de6bc649a9cc32d0fae77a1dc34dc

Pulled to match a Mali-G615-MC2 target. CSF UK interface version, per
`csf/mali_kbase_csf_ioctl.h`'s `BASE_UK_VERSION_MAJOR`/`_MINOR` in this
same vendored copy, is **1.20** — not "44.10" (an earlier, incorrect
claim in this repo's docs conflated the r44p0 driver release name with
the separate CSF UK interface version number; see `docs/kbase-notes.md`
for how this was caught).
Unmodified except as noted in individual file diffs, if any.
Original license notices preserved in each file - see file headers,
not this README, for authoritative licensing.
