# Upstream question: dma-buf import goes through DRM before the backend

The second change this port needs in *shared* code rather than in the kbase
backend. Companion to `docs/upstream-ringbuf-question.md`, and small enough
that it is probably worth raising in the same conversation rather than as a
separate approach.

`pan_kmod_bo_import()` converts the fd to a GEM handle with
`drmPrimeFDToHandle()` before dispatching to `ops->bo_import`, so a non-DRM
backend can never be reached — even though kbase itself can import dma-bufs
perfectly well.

Status: **drafted, not sent.** Scoped 2026-08-01 against Mesa 26.3.0-devel.
A third, broader question also exists —
`docs/upstream-project-status-question.md`, asking whether kbase support
is wanted upstream at all — intended to be sent after both technical
questions, not before.

Unlike the ringbuf question this one has an obvious shape of answer, so it is
phrased as a proposal with a fallback rather than an open question. It is
also strictly smaller: it adds a dispatch point, and changes no behaviour for
panthor or panfrost.

---

## The message

```
Second, smaller kbase question, unrelated to the ringbuf one except that it
is also above the backend rather than in it.

pan_kmod_bo_import() does:

    int ret = drmPrimeFDToHandle(dev->fd, fd, &handle);
    if (ret)
       goto err_unlock;
    ...
    bo = dev->ops->bo_import(dev, handle, size);

so the fd becomes a GEM handle before any backend dispatch, and the
bo_import hook receives that handle rather than the fd. On kbase - a misc
device with no DRM ioctls - drmPrimeFDToHandle() fails, so the hook is never
called, and its signature could not use the value anyway.

kbase can import dma-bufs: KBASE_IOCTL_MEM_IMPORT with
BASE_MEM_IMPORT_TYPE_UMM takes the fd directly, and Panfork has been doing
exactly that for years. So the capability is there; it is the plumbing that
excludes it.

What I think the minimal change is: give pan_kmod_ops an optional
fd-taking import hook, tried before the DRM path, with the existing
handle-based one unchanged as the fallback. Something like

    struct pan_kmod_bo *(*bo_import_fd)(struct pan_kmod_dev *dev, int fd,
                                        uint64_t size);

and in pan_kmod_bo_import(), if ops->bo_import_fd exists, use it and skip
drmPrimeFDToHandle() entirely. Backends that do not set it behave exactly as
today, so panthor and panfrost are untouched.

The wrinkle is the handle_to_bo cache. It is keyed on the GEM handle, which
is what makes repeated imports of the same dma-buf return the same
pan_kmod_bo. Without a GEM handle there is no such key. Panfork solves it by
walking its own table and comparing with os_same_file_description(), which
is O(n) but n is small. Options I can see:

  - let the fd-taking backend own its dedup entirely and skip the shared
    cache (simplest, but the refcounting semantics then differ per backend,
    which seems worse than the problem);
  - key the shared cache on something the backend supplies - have
    bo_import_fd return a backend-chosen uint32_t alongside the BO, so the
    cache stays shared and only the key's provenance changes;
  - or keep the cache DRM-only and accept that non-DRM backends re-import.

I lean towards the second, but it touches the cache for everyone, so I would
rather ask than pick.

Is that shape acceptable in principle? If dma-buf import on a non-DRM
backend is simply out of scope upstream, that is a fine answer too - I would
rather know before writing it, and it is easy to carry as a local patch.

For context on why it is worth having at all: this is what WSI will need on
Android. Export is a separate matter and I am not asking for it - kbase has
no export path in its UAPI at all, so there is nothing to plumb.
```

---

## Backing detail, if asked

### Why this is not fixable in the backend

`pan_kmod_bo_import()` in `src/panfrost/lib/kmod/pan_kmod.c`:

| step | on panthor | on kbase |
|---|---|---|
| `drmPrimeFDToHandle(dev->fd, fd, &handle)` | ok | fails — misc device, no DRM ioctls |
| `util_sparse_array_get(&dev->handle_to_bo.array, handle)` | ok | unreached |
| `dev->ops->bo_import(dev, handle, size)` | backend | **unreached** |

The failure is `goto err_unlock` and a `NULL` return, so the caller sees an
import failure with no way to tell it apart from a genuine one.

### What kbase offers instead

`KBASE_IOCTL_MEM_IMPORT` with `union kbase_ioctl_mem_import`:

- `in.phandle` — pointer to the fd (note: to the `int`, not the value)
- `in.type` — `BASE_MEM_IMPORT_TYPE_UMM` for a dma-buf
- `in.flags` — the usual CPU/GPU RW set
- `out.flags`, `out.gpu_va`, `out.va_pages`

`BASE_MEM_IMPORT_TYPE_USER_BUFFER` also exists, for importing plain host
memory, which pan_kmod has no equivalent of today.

Reference implementation: Panfork's `kbase_import_dmabuf()`
(`src/panfrost/base/pan_vX_base.c`), which dups the fd, calls the ioctl, and
handles the `BASE_MEM_NEED_MMAP` case in the result.

### Why export is not part of the ask

There is no export path in kbase's UAPI — no PRIME, no dmabuf-out, nothing
that turns an allocation into an fd. And `pan_kmod_bo_export()` is a
`static inline` in `pan_kmod.h` that calls `drmPrimeHandleToFD()` itself,
invoking `ops->bo_export` only as an optional post-export hook once the fd
exists. Both halves would need changing, and the kernel half does not exist.
Import is a one-way door on this driver, so the question is scoped to import
only.

### Status of the local side

`kbase_kmod_bo_import()` is a stub that logs and returns `NULL`, tagged
`BELONGS-UPSTREAM(pan_kmod)`. The driver no longer advertises dma-buf import
or export on kbase at all (`patch-panvk-kbase-external-memory.py`, verified
on-device by `tests/driver_extmem_probe`), so nothing currently reaches the
stub — an application is refused at query time instead. That gating is the
correct behaviour regardless of how this question is answered; if the
fd-taking hook lands, the import half of it gets removed.
