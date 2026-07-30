#ifndef CSF_USER_REGS_H
#define CSF_USER_REGS_H

/*
 * Byte offsets into the CS_USER_INPUT_BLOCK / CS_USER_OUTPUT_BLOCK pages
 * (the first two of the BASEP_QUEUE_NR_MMAP_USER_PAGES=3 pages mmap'd via
 * KBASE_IOCTL_CS_QUEUE_BIND's mmap_handle - see csf/mali_base_csf_kernel.h).
 * Not part of any vendored kbase-uapi header set (third_party/kbase-uapi-*)
 * since these are kernel-internal firmware-interface offsets, not part of
 * the ioctl uapi surface. Sourced from the same real kernel driver tree
 * the r49p1 uapi headers were vendored from (see
 * third_party/kbase-uapi-r49p1/README.md for provenance):
 *
 *   https://github.com/Mayuri-Chan/MTK_kernel_device_modules_6.6
 *   branch: lineage-22.1
 *   commit: 3b31307ea54d689cecab0f231d43208959287cd1
 *   path: drivers/gpu/mediatek/gpu_mali/mali_avalon/mali-r49p1/
 *         drivers/gpu/arm/midgard/csf/mali_kbase_csf_registers.h
 *
 * Confirmed against the same tree's mali_kbase_csf.c:
 * - init_user_io_pages(): page 0 = input (CS_INSERT_LO/HI, CS_EXTRACT_INIT_*),
 *   page 1 = output (CS_EXTRACT_LO/HI, CS_ACTIVE), both zeroed at bind time.
 * - kbase_csf_queue_kick(): the KICK ioctl handler only flags the queue and
 *   wakes the scheduler kthread; it does not itself read CS_INSERT or ring
 *   the doorbell. The scheduler does that asynchronously afterward, via
 *   kbase_csf_ring_cs_user_doorbell()/kbase_csf_ring_doorbell(), which
 *   writes a plain (u32)1 to a separate real hardware MMIO doorbell
 *   register - not part of this 3-page mmap, and not something userspace
 *   needs to touch directly for a queue that's already bound: writing
 *   CS_INSERT then calling the (already-ioctl-validated) KICK is
 *   sufficient for the kernel to notice and ring it on our behalf.
 */

/* CS_USER_INPUT_BLOCK (page 0) */
#define CSF_USER_CS_INSERT_LO 0x0000
#define CSF_USER_CS_INSERT_HI 0x0004
#define CSF_USER_CS_EXTRACT_INIT_LO 0x0008
#define CSF_USER_CS_EXTRACT_INIT_HI 0x000C

/* CS_USER_OUTPUT_BLOCK (page 1) */
#define CSF_USER_CS_EXTRACT_LO 0x0000
#define CSF_USER_CS_EXTRACT_HI 0x0004
#define CSF_USER_CS_ACTIVE 0x0008

#endif
