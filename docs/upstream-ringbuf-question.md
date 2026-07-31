# Upstream question: the render descriptor ringbuf's double mapping

The first change this port needs in *shared* PanVK code rather than in the
kbase backend, and therefore the first worth raising with Panfrost upstream
before writing (see Phase 9 in `ROADMAP.md`).

`init_render_desc_ringbuf()` maps one BO at `dev_addr` and again at
`dev_addr + size`. kbase cannot express that — see the `MEM_ALIAS` section in
`kbase-notes.md` for the measurements, and §1-2 below for the condensed form.
The fallback is to stop relying on the mapping to wrap and skip the tail
instead, which changes behaviour for panthor too.

Status: **drafted, not yet sent.**

Re-checked 2026-08-01 against the actual tree in `/opt/mesa-src`
(**26.3.0-devel**), because sending stale line numbers upstream would be
worse than not asking. Three things changed the question:

1. **The function moved.** `init_render_desc_ringbuf()` now lives in
   `csf/panvk_vX_gpu_queue.c`, not `panvk_vX_queue.c`. All five
   `panvk_vX_cmd_draw.c` line references below still resolve correctly
   (`:1199`, `:1208`, `:1632`, `:4005`, `:4108`) — verified individually.

2. **The double mapping has a documented reason, and the earlier draft
   asked as if it might not.** Directly above the VA reservation:

   ```c
   /* We choose the alignment to guarantee that we won't ever cross a 4G
    * boundary when accessing the mapping. This way we can encode the
    * wraparound using 32-bit operations. */
   dev_addr = panvk_as_alloc(dev, dev->as.priv_heap,
                             ringbuf->size * 2, ringbuf->size * 2);
   ```

   `cs_render_desc_ringbuf_move_ptr()` confirms the dependency: it
   increments and wraps `ptr_lo`, the **low 32 bits** of the 64-bit
   address, and never touches the high word. That only works while the
   whole window stays inside one 4G span.

   Note this is two separate design decisions wearing one number:
   the 2x *reservation* buys contiguity for reads that straddle the end,
   the 2x *alignment* buys the 32-bit arithmetic. Tail-padding removes
   the need for the first but not the second — a padded ring still wants
   alignment so `move_ptr` can stay 32-bit, just `size` rather than
   `size * 2`.

3. **Upstream already runs this with a single mapping.** In tracing mode
   only the first `vm_op` is submitted:

   ```c
   /* If tracing is enabled, we keep the second part of the mapping
    * unmapped to serve as a guard region. */
   ret = pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE,
                          vm_ops, tracing_enabled ? 1 : ARRAY_SIZE(vm_ops));
   ```

   and `:1208` passes `wrap_around = !tracing_enabled`, so the wrap is
   already a parameter that is already sometimes false. That is a much
   better thing to build on than a new kbase-only branch, and it means
   the ask is "extend an existing mode", not "add a special case".

Sharpened 2026-07-31, after disassembling a rejected command stream. The
earlier draft asked this from a weaker position — it implied the ringbuf
blocked bringing up the render subqueues at all. It does not. All three
subqueue contexts initialise and run on kbase; the only field that needs the
double mapping is `render.desc_ringbuf`, and the only thing that needs *that*
is actual draw work. Compute is complete and unaffected. So the question is
narrower than it was, and it is now the single remaining blocker rather than
one of several unknowns — worth stating that way, because it is the
difference between "help me port this" and "one specific design decision,
please confirm before I change shared code".

---

## The message

Short form, for `#panfrost` — an **IRC channel on OFTC**, not a native
Matrix room. Reachable either way:

- IRC directly: `irc.oftc.net`, `#panfrost` (webchat at `webchat.oftc.net`)
- Matrix, via the OFTC bridge: `#_oftc_#panfrost:matrix.org` — note the
  `#_oftc_#` prefix; plain `#panfrost:matrix.org` does not exist

**NickServ registration is required to speak in the channel** (anti-spam),
so joining and pasting immediately will not work — register the nick first.
That friction is worth weighing against filing a Mesa GitLab issue at
`gitlab.freedesktop.org/mesa/mesa/-/issues` instead, which is async and
citable but slower to get a design answer from.

The question is first on purpose — the rest only exists to show the
constraint is real.

```
Hi — I'm porting PanVK to the legacy Arm kbase driver (Mali-G720, r49p1,
out-of-tree experiment: github.com/fbtwitter/PanVK2KBase). Most of the CSF
path ports cleanly. Compute works end to end: a compute pipeline built from
application SPIR-V, vkCmdDispatch, and a VkFence signalled by the GPU via
SYNC_SET64 into BASE_MEM_CSF_EVENT memory (kbase has no fences, so that's the
completion primitive). All three subqueue contexts initialise and run.

The one thing left is init_render_desc_ringbuf(), and I'd rather ask than
guess, because the change I can see touches shared code.

It maps one BO at dev_addr and again at dev_addr + size. kbase can't express
that. KBASE_IOCTL_MEM_ALIAS composes the region correctly (entries do share
alloc->pages), but the result always carries BASE_MEM_NEED_MMAP, so there's
no GPU mapping until userspace mmaps the cookie — and kbase_context_mmap()
rejects nr_pages > stride, so a mapping can never cover both windows. Since
the GPU address is assigned at mmap time, the GPU can only ever address one
window. I tried stride = full span and a BASE_MEM_FIXABLE source; both still
come back NEED_MMAP. Measurements and the kernel-side reasoning are written
up if useful.

To be precise about what this does and doesn't block, since I had it wrong
myself at first: it isn't the render subqueues. Those come up fine — what an
ordinary command buffer dereferences on VERTEX_TILER and FRAGMENT is just the
subqueue context register, and vkEndCommandBuffer appends that epilogue to
every subqueue whether the app drew anything or not. render.desc_ringbuf is
the only field that needs the double mapping, and only draw work reads it.
So this is a rendering blocker, not a bring-up blocker.

I think I understand what the two mappings buy, and I don't want to break
either property:

 - contiguity, so a block straddling the end stays addressable —
   cmd_draw.c:1632 does address arithmetic past desc_ringbuf.ptr to reach
   the FBDs, so a split block would break;
 - and, from the alignment comment, keeping the window inside one 4G span
   so move_ptr() can wrap ptr_lo with 32-bit ops.

Tail-padding (skip to offset 0 when a block would straddle) drops the need
for the first but keeps the second — it'd still want size-aligned VA, just
not size*2.

What made me want to ask rather than just write it: tracing mode already
runs this with a single mapping, submitting only vm_ops[0] and leaving the
second half as a guard region, with wrap_around=false at :1208. So there is
already a one-mapping configuration and the wrap is already a parameter.

So: is tail-padding a reasonable thing to generalise that into, or is the
double mapping load-bearing in a way I've missed? If it's reasonable I'm
happy to write it — it'd drop the 2x VA reservation for panthor too.

The wrinkle I can see is that producer and consumer both advance by
calc_render_descs_size() independently, so padding has to be accounted
symmetrically or the ring leaks. Putting the decision inside move_ptr()
seems to keep them in lockstep, since the consumer retires in creation
order, but that's the part I'd most like checked before writing it.
```

---

## The follow-up detail

Backing detail, if asked. Device: Mali-G720 MC8 (Poco X8 Pro), kbase r49p1,
UK interface 1.30, CSF firmware iface v3.6.0.

### 1. What was measured

All from `tests/alias_probe`, CPU-only, no GPU work. Source allocation is
4 pages; the goal is one VA range covering two adjacent 4-page windows onto
the same pages.

| variant | result |
|---|---|
| alias handle = allocation's reported `gpu_va` | `ENOMEM` |
| alias handle = real GPU VA, `SAME_VA` requested | OK — `gpu_va=0x41000`, `va_pages=8`, `out.flags=0x400d` |
| `stride` = full span (8 pages) instead of window | OK — `gpu_va=0x43000`, `va_pages=16`, `out.flags=0x400d` |
| source allocated `BASE_MEM_FIXABLE` | source at `0x800200000000` (real addr, `flags=0x2000000f`); alias OK — `gpu_va=0x44000`, `va_pages=8`, `out.flags=0x400c` |

Flag decode: `0x400d` = `NEED_MMAP | GPU_WR | GPU_RD | CPU_RD`.
`0x400c` = same without `CPU_RD`. `SAME_VA` is absent in every case —
`kbase_mem_alias()` strips it.

**Every variant sets `BASE_MEM_NEED_MMAP`.** So `out.gpu_va` is an mmap
cookie, not an address, and the region has no GPU mapping until userspace
mmaps it.

mmap attempts on those cookies:

| shape | result |
|---|---|
| `PROT_READ\|PROT_WRITE`, 2x span | `EPERM` |
| `PROT_READ`, `stride` pages, fresh cookie | `ENOMEM` |
| `PROT_READ`, `nr_pages == va_pages`, GPU-only alias | `EPERM` |

Two error codes that are easy to misread, and cost real time here:
`ENOMEM` from `MEM_ALIAS` means "no allocation at that handle", not a
resource limit — it is what you get for passing a `FIXABLE` allocation's CPU
pointer as the handle, since the `SAME_VA` rule that CPU pointer == GPU
address does not hold there. And `EPERM` from `mmap()` is the flags check,
which fires before and therefore masks the page-count check.

### 2. Why it cannot work, from the kernel source

Source is the tree this device actually runs:
`Mayuri-Chan/MTK_kernel_device_modules_6.6`, branch `lineage-22.1`,
`.../mali-r49p1/drivers/gpu/arm/midgard/mali_kbase_mem_linux.c`.

- `kbase_mem_alias()` accepted-flag mask is
  `GPU_RD | GPU_WR | COHERENT_SYSTEM | COHERENT_LOCAL | CPU_RD |
  COHERENT_SYSTEM_REQUIRED`. No `CPU_WR`, and `SAME_VA` is explicitly
  stripped.
- Entries genuinely share backing pages —
  `reg->gpu_alloc->imported.alias.aliased[i].alloc =
  kbase_mem_phy_alloc_get(alloc)`. The aliasing itself is real; it is the
  addressability that fails.
- `kbase_context_mmap()` rejects `nr_pages > alias.stride` with `EINVAL`, and
  returns `EPERM` on "VM flags inconsistent with region flags".
- `stride`, `offset` and `length` are all in **pages**;
  `num_pages = nents * stride`.

The contradiction, in one line:

- covering both windows needs `nr_pages = 2 x window`;
- with `stride = window` that is `nr_pages > stride` → `EINVAL`;
- raising `stride` to `2 x window` satisfies the check but changes the
  layout — entries are spaced `stride` apart, so the two copies land
  `2 x window` apart with a hole between them, i.e. no longer adjacent,
  which is not the mapping the ringbuf wants either.

Since the GPU address is assigned at mmap time, and no mmap can span both
windows, **the GPU can only ever address one window.**

### 3. What the consumers appear to need

From `csf/panvk_vX_cmd_draw.c`. Stated as inference, not conclusion — this is
the part a maintainer is most likely to correct.

- `cs_render_desc_ringbuf_move_ptr()` already wraps the *pointer* in the CS
  (`pos += size`, subtract `RENDER_DESC_RINGBUF_SIZE` if past the end), so
  the second mapping is not what makes the pointer wrap.
- `:1632` loads `desc_ringbuf.ptr` and computes
  `dst_fbd_ptr = cur_tiler + pan_size(TILER_CONTEXT) * td_count` — address
  arithmetic past `ptr` to reach the FBDs. A block split across the ring end
  would break this.

So the second mapping's job looks like: let one allocation straddle the end
and stay contiguous. Hence the question about tail-padding.

### 4. The wrinkle in tail-padding, if it is acceptable

Producer and consumer both advance by `calc_render_descs_size(cmdbuf)`,
computed independently:

- producer: `cs_render_desc_ringbuf_reserve()` at `:1199`, then
  `move_ptr()` at `:1208`;
- consumer: syncobj release at `:4005`, then `move_ptr()` at `:4108`.

They stay in lockstep because they agree on that number. Naive tail-padding
breaks it — the producer would consume `size + padding` while the consumer
releases `size`, so the free-space counter leaks until the ring deadlocks.

The approach proposed: put the decision inside `move_ptr()`, changing the
rule from "add then subtract" to "if `pos + size > SIZE`, allocate from 0".
Both sides then derive the same padding from their own `pos`, and because the
consumer retires blocks in creation order its `pos` matches the producer's.
`reserve()` and the `:4005` release would both need to account for
`padding = pos + size > SIZE ? SIZE - pos : 0`, computed in the CS rather
than as a constant.

New invariant that would need asserting: worst case consumption is
`padding + size < 2 x size`, so `2 x max_block <= RENDER_DESC_RINGBUF_SIZE`.
The current `assert(size <= RENDER_DESC_RINGBUF_SIZE)` in `reserve()` would
no longer be sufficient.

Doing it unconditionally rather than gated on the backend seems better —
two ring disciplines in this code means the padding arithmetic has to agree
across three call sites in both variants, and the kbase one would be
untestable on panthor CI. It would also drop panthor's `2 x size` VA
reservation. But it changes shared behaviour in a path that cannot be tested
here, and the failure mode is a slow leak rather than a crash, which is why
this is a question first.
