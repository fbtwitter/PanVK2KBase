#ifndef KCONFIG_SHIM_H
#define KCONFIG_SHIM_H

/*
 * third_party/kbase-uapi-r49p1's csf/mali_base_csf_kernel.h guards
 * MediaTek debug-dump fields with IS_ENABLED(CONFIG_MALI_MTK_DEBUG_DUMP),
 * assuming it's built inside a full kernel tree where <linux/kconfig.h>
 * already defines IS_ENABLED and the config system defines (or doesn't)
 * CONFIG_MALI_MTK_DEBUG_DUMP. A standalone userspace probe has neither.
 * Those guards only gate MTK debug-dump-only fields (see docs/kbase-notes.md),
 * so "not enabled" is the correct default here, not a workaround that
 * changes real ioctl/struct layout - the vendored header itself stays
 * untouched.
 */
#ifndef IS_ENABLED
#define IS_ENABLED(x) 0
#endif

#endif
