#ifndef CSF_USER_REGS_H
#define CSF_USER_REGS_H

/*
 * Byte offsets into the CS_USER_INPUT_BLOCK / CS_USER_OUTPUT_BLOCK pages
 * of the BASEP_QUEUE_NR_MMAP_USER_PAGES=3 pages mmap'd via
 * KBASE_IOCTL_CS_QUEUE_BIND's mmap_handle - see csf/mali_base_csf_kernel.h.
 *
 * PAGE ORDER IS [doorbell][input][output], NOT [input][output][doorbell].
 * Measured on-device, 6/6 reproducible, by tests/user_io_probe: writing
 * CS_INSERT at page 1 + 0x00 and kicking makes page 2 + 0x00 advance to
 * the CS size a few ms later without userspace touching it; doing the same at
 * page 0 changes nothing anywhere. Page 0 also survives across processes
 * while pages 1 and 2 come up freshly zeroed, which is what a shared HW
 * doorbell page vs. per-queue I/O blocks look like.
 *
 * This corrects an earlier reading of this file's own source (below) that
 * had it as [input][output][doorbell] and cost a long detour - see
 * "Prior art found" in docs/kbase-notes.md. The uapi comment at
 * csf/mali_base_csf_kernel.h:117 ("A pair of input/output pages and a Hw
 * doorbell page") describes the contents, not the order. Panfork's
 * pan_vX_base.c:1434 had it right.
 *
 * Use CSF_USER_INPUT_PAGE / CSF_USER_OUTPUT_PAGE / CSF_USER_DOORBELL_PAGE
 * below rather than hardcoding page indices.
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

/* Which of the 3 mmap'd pages is which. Measured, see above. */
#define CSF_USER_DOORBELL_PAGE 0
#define CSF_USER_INPUT_PAGE 1
#define CSF_USER_OUTPUT_PAGE 2

/* CS_USER_INPUT_BLOCK (page CSF_USER_INPUT_PAGE) */
#define CSF_USER_CS_INSERT_LO 0x0000
#define CSF_USER_CS_INSERT_HI 0x0004
#define CSF_USER_CS_EXTRACT_INIT_LO 0x0008
#define CSF_USER_CS_EXTRACT_INIT_HI 0x000C

/* CS_USER_OUTPUT_BLOCK (page CSF_USER_OUTPUT_PAGE) */
#define CSF_USER_CS_EXTRACT_LO 0x0000
#define CSF_USER_CS_EXTRACT_HI 0x0004
#define CSF_USER_CS_ACTIVE 0x0008

#endif
