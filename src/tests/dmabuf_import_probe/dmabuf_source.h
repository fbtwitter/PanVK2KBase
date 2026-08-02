#ifndef DMABUF_SOURCE_H
#define DMABUF_SOURCE_H

/* Getting a real dma-buf fd as an unprivileged Android shell user.
 *
 * Factored into a header because two probes need it and there should be
 * exactly one place that knows how: tests/dmabuf_import_probe (does kbase
 * accept one?) and tests/driver_dmabuf_probe (does the whole Vulkan import
 * path work?). If the answer to "where do I get an fd" ever changes, it
 * changes once.
 *
 * Two sources, deliberately:
 *
 *   heap - /dev/dma_heap/system. Reachable by shell on this device (mode
 *          0444, and SELinux permits it from domain shell). Zero library
 *          dependencies. This is the substrate Android's own gralloc
 *          allocates from, so a buffer from here is representative.
 *
 *   ahb  - AHardwareBuffer via libnativewindow, dlopen'd rather than linked
 *          so a missing library degrades to "source unavailable" instead of
 *          a binary that will not start. This is the *ground truth* source:
 *          handle->data[0] here is literally the fd PanVK's Android WSI path
 *          hands to vkAllocateMemory (panvk_android.c does exactly this).
 *
 * Ruled out and worth not re-testing: /dev/ion does not exist on this device
 * (dma-heaps only), and ashmem - despite /dev/ashmem being 0666 - is not a
 * dma-buf, so dma_buf_get() rejects it and BASE_MEM_IMPORT_TYPE_UMM cannot
 * consume it.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Mirrored locally rather than pulled from <cutils/native_handle.h>, the
 * same way driver_extmem_probe mirrors the libhardware structs: it keeps
 * this a plain C file with no Android build-system entanglement.
 */
typedef struct dmabuf_native_handle {
  int version;
  int numFds;
  int numInts;
  int data[0];
} dmabuf_native_handle_t;

struct dmabuf_src {
  int fd;          /* the dma-buf fd, or -1 */
  size_t size;     /* what we asked for */
  const char *how; /* human-readable provenance */
  char err[192];   /* why fd == -1, if it is */
  void *ahb;       /* AHardwareBuffer* to release, or NULL */
  void *dso;       /* libnativewindow handle, or NULL */
};

/* --------------------------------------------------------------- dma-heap */
static int dmabuf_from_heap(struct dmabuf_src *s, const char *heap_name,
                            size_t len) {
  char path[128];
  snprintf(path, sizeof(path), "/dev/dma_heap/%s", heap_name);

  int heap = open(path, O_RDONLY | O_CLOEXEC);
  if (heap < 0) {
    snprintf(s->err, sizeof(s->err), "open(%s): %s", path, strerror(errno));
    return -1;
  }

  /* O_RDONLY on the heap node is correct and not a workaround: the node is
   * mode 0444 and libdmabufheap itself opens it read-only. The allocated
   * buffer's own access mode comes from fd_flags below.
   */
  struct dma_heap_allocation_data d = {
      .len = len,
      .fd_flags = O_RDWR | O_CLOEXEC,
      .heap_flags = 0,
  };

  if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
    snprintf(s->err, sizeof(s->err), "DMA_HEAP_IOCTL_ALLOC on %s: %s", path,
             strerror(errno));
    close(heap);
    return -1;
  }
  close(heap);

  s->fd = (int)d.fd;
  s->size = len;
  s->how = "dma_heap";
  return 0;
}

/* --------------------------------------------------------- AHardwareBuffer */
/* Only the few AHardwareBuffer bits we need, so this file does not depend on
 * <android/hardware_buffer.h> being present at the -I paths a raw probe uses.
 */
#define DMABUF_AHB_FORMAT_BLOB 0x21
#define DMABUF_AHB_USAGE_CPU_READ_OFTEN 0x3ULL
#define DMABUF_AHB_USAGE_CPU_WRITE_OFTEN 0x30ULL
#define DMABUF_AHB_USAGE_GPU_BUFFER 0x1000000ULL

struct dmabuf_ahb_desc {
  uint32_t width, height, layers, format;
  uint64_t usage;
  uint32_t stride, rfu0;
  uint64_t rfu1;
};

static int dmabuf_from_ahb(struct dmabuf_src *s, size_t len) {
  void *dso = dlopen("libnativewindow.so", RTLD_NOW);
  if (!dso) {
    snprintf(s->err, sizeof(s->err), "dlopen(libnativewindow.so): %s",
             dlerror());
    return -1;
  }

  int (*ahb_alloc)(const struct dmabuf_ahb_desc *, void **) =
      (int (*)(const struct dmabuf_ahb_desc *, void **))dlsym(
          dso, "AHardwareBuffer_allocate");
  const dmabuf_native_handle_t *(*ahb_handle)(const void *) =
      (const dmabuf_native_handle_t *(*)(const void *))dlsym(
          dso, "AHardwareBuffer_getNativeHandle");
  void (*ahb_release)(void *) =
      (void (*)(void *))dlsym(dso, "AHardwareBuffer_release");

  if (!ahb_alloc || !ahb_handle || !ahb_release) {
    snprintf(s->err, sizeof(s->err),
             "libnativewindow.so is missing AHardwareBuffer_* symbols");
    dlclose(dso);
    return -1;
  }

  struct dmabuf_ahb_desc desc = {
      .width = (uint32_t)len,
      .height = 1,
      .layers = 1,
      .format = DMABUF_AHB_FORMAT_BLOB,
      .usage = DMABUF_AHB_USAGE_CPU_READ_OFTEN |
               DMABUF_AHB_USAGE_CPU_WRITE_OFTEN | DMABUF_AHB_USAGE_GPU_BUFFER,
  };

  void *ahb = NULL;
  int rc = ahb_alloc(&desc, &ahb);
  if (rc != 0 || !ahb) {
    snprintf(s->err, sizeof(s->err),
             "AHardwareBuffer_allocate: rc=%d (needs the graphics allocator "
             "HAL over binder; shell may not be permitted)",
             rc);
    dlclose(dso);
    return -1;
  }

  const dmabuf_native_handle_t *h = ahb_handle(ahb);
  if (!h || h->numFds < 1) {
    snprintf(s->err, sizeof(s->err),
             "AHardwareBuffer_getNativeHandle gave %s",
             h ? "numFds=0" : "NULL");
    ahb_release(ahb);
    dlclose(dso);
    return -1;
  }

  /* data[0] is the dma-buf fd - the same index panvk_android.c reads. */
  s->fd = h->data[0];
  s->size = len;
  s->how = "AHardwareBuffer(BLOB)";
  s->ahb = ahb;
  s->dso = dso;
  return 0;
}

/* ------------------------------------------------------------------ entry */
static int dmabuf_source_open(struct dmabuf_src *s, const char *source,
                              const char *heap_name, size_t len) {
  memset(s, 0, sizeof(*s));
  s->fd = -1;

  if (!strcmp(source, "heap"))
    return dmabuf_from_heap(s, heap_name, len);
  if (!strcmp(source, "ahb"))
    return dmabuf_from_ahb(s, len);

  snprintf(s->err, sizeof(s->err), "unknown source '%s'", source);
  return -1;
}

static void dmabuf_source_close(struct dmabuf_src *s) {
  if (s->ahb) {
    void (*ahb_release)(void *) =
        (void (*)(void *))dlsym(s->dso, "AHardwareBuffer_release");
    /* Releasing the AHB closes its handle fds, so do not close s->fd too. */
    if (ahb_release)
      ahb_release(s->ahb);
    s->ahb = NULL;
  } else if (s->fd >= 0) {
    close(s->fd);
  }
  s->fd = -1;
  if (s->dso) {
    dlclose(s->dso);
    s->dso = NULL;
  }
}

#endif
