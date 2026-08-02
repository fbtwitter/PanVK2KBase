#!/usr/bin/env python3
"""Give pan_kmod an fd-taking dma-buf import hook, so non-DRM backends can import.

pan_kmod_bo_import() converts the dma-buf fd to a GEM handle *before* it
dispatches to the backend:

    int ret = drmPrimeFDToHandle(dev->fd, fd, &handle);
    if (ret)
       goto err_unlock;
    ...
    bo = dev->ops->bo_import(dev, handle, size);

On /dev/mali0 - a misc device with no DRM ioctls - that first call fails and
the import returns NULL having never reached ops->bo_import. The hook's
signature is handle-taking too, so even arriving there would hand the
backend a number that means nothing to it.

This adds an optional ops->bo_import_fd(dev, fd, size), used *instead of*
the drmPrimeFDToHandle() path when a backend provides it. Backends that do
not set it - panthor, panfrost - are completely unaffected: the added branch
is skipped and the existing code runs unchanged.

Why the shared handle_to_bo cache is still used, and why dedup is left to the
backend
------------------------------------------------------------------------
docs/upstream-import-question.md framed this as a choice between "backend
owns dedup" and "backend returns a cache key". That framing does not survive
contact with the code: the cache's dedup value comes from
drmPrimeFDToHandle() being *idempotent* - it maps an fd to a stable handle
without creating anything, so a repeated import of the same fd finds the
existing BO. kbase has no such call. MEM_IMPORT creates a new region every
time, so there is nothing to look up *before* importing.

So a backend-supplied key can serve insertion and teardown, but not
lookup-before-import. The cache is therefore kept for storage and teardown -
which is not optional, because pan_kmod_bo_put() unconditionally does
util_sparse_array_get(&dev->handle_to_bo.array, bo->handle) and writes NULL
into that slot; a BO that was never inserted would have that write land on
whatever else occupies its handle's slot - and dedup is left to the backend.
Not deduping is spec-legal: each vkAllocateMemory import is a distinct
VkDeviceMemory, and aliasing between them is the application's problem. The
hook shape still allows a backend to dedup later by returning an existing BO
with its refcount already incremented.

Also defines PAN_KMOD_HAS_BO_IMPORT_FD, which pan_kmod_kbase.c uses to
compile its implementation in only when this patch has been applied - so the
backend builds against a patched or unpatched tree either way.

Applied as a script rather than a diff because upstream moves. Idempotent.

Usage: patch-pan-kmod-import-fd.py [<mesa-src-dir>]
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"
H = os.path.join(mesa, "src/panfrost/lib/kmod/pan_kmod.h")
C = os.path.join(mesa, "src/panfrost/lib/kmod/pan_kmod.c")

# ----------------------------------------------------------------- pan_kmod.h
src = open(H).read()

if "PAN_KMOD_HAS_BO_IMPORT_FD" in src:
    print("    pan_kmod.h: already patched")
else:
    anchor = """   struct pan_kmod_bo *(*bo_import)(struct pan_kmod_dev *dev, uint32_t handle,
                                    uint64_t size);"""

    assert anchor in src, "pan_kmod.h: bo_import hook not found - upstream moved"

    replacement = anchor + """

   /* Import a buffer object directly from a dma-buf fd.
    *
    * Optional. When a backend sets this, pan_kmod_bo_import() calls it
    * *instead of* converting the fd to a GEM handle with
    * drmPrimeFDToHandle() - which a backend on a non-DRM character device
    * (kbase) cannot do, and which otherwise fails before ops->bo_import is
    * ever reached.
    *
    * The returned BO must already be initialised with pan_kmod_bo_init()
    * and carry a handle unique within the device; the common layer uses it
    * as the handle_to_bo cache key, and pan_kmod_bo_put() relies on that
    * slot existing.
    *
    * Deduplicating repeated imports of the same buffer is the backend's
    * business: unlike drmPrimeFDToHandle(), there is not necessarily a way
    * to map an fd to an existing object without creating one.
    *
    * Return NULL on failure.
    */
   struct pan_kmod_bo *(*bo_import_fd)(struct pan_kmod_dev *dev, int fd,
                                       uint64_t size);"""

    src = src.replace(anchor, replacement, 1)

    # A compile-time marker so a backend can provide .bo_import_fd only when
    # the field exists, and still build against an unpatched tree.
    guard = "#define PAN_KMOD_HAS_BO_IMPORT_FD 1\n\n"
    inc_anchor = "struct pan_kmod_ops {"
    assert inc_anchor in src, "pan_kmod.h: struct pan_kmod_ops not found"
    src = src.replace(inc_anchor, guard + inc_anchor, 1)

    open(H, "w").write(src)
    print("    pan_kmod.h: added ops->bo_import_fd + PAN_KMOD_HAS_BO_IMPORT_FD")

# ----------------------------------------------------------------- pan_kmod.c
src = open(C).read()

if "ops->bo_import_fd" in src:
    print("    pan_kmod.c: already patched")
    sys.exit(0)

anchor = """   uint32_t handle;
   int ret = drmPrimeFDToHandle(dev->fd, fd, &handle);
   if (ret)
      goto err_unlock;"""

assert anchor in src, "pan_kmod.c: drmPrimeFDToHandle call site not found - upstream moved"

replacement = """   /* Backends on a non-DRM device import the fd directly: drmPrimeFDToHandle()
    * below would fail on them before any dispatch happened, so the hook has
    * to come first rather than after. Backends that do not set bo_import_fd
    * take the unchanged path.
    */
   if (dev->ops->bo_import_fd) {
      size_t fd_size = lseek(fd, 0, SEEK_END);
      if (fd_size == 0 || fd_size == (size_t)-1) {
         mesa_loge("invalid dmabuf size");
         goto err_unlock;
      }

      bo = dev->ops->bo_import_fd(dev, fd, fd_size);
      if (!bo)
         goto err_unlock;

      slot = util_sparse_array_get(&dev->handle_to_bo.array, bo->handle);
      if (!slot) {
         dev->ops->bo_free(bo);
         goto err_unlock;
      }

      /* A backend that deduplicated will have returned an existing BO with
       * its refcount already bumped, and the slot already points at it.
       */
      if (*slot != bo)
         *slot = bo;

      simple_mtx_unlock(&dev->handle_to_bo.lock);
      return bo;
   }

   uint32_t handle;
   int ret = drmPrimeFDToHandle(dev->fd, fd, &handle);
   if (ret)
      goto err_unlock;"""

src = src.replace(anchor, replacement, 1)
open(C, "w").write(src)
print("    pan_kmod.c: bo_import_fd dispatched before drmPrimeFDToHandle()")
