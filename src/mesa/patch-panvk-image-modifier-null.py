#!/usr/bin/env python3
"""Reject an unsupported DRM format modifier instead of segfaulting on it.

Not kbase-specific. This is a NULL-check missing from shared PanVK code, in
the same family as patch-panvk-null-device-destroy.py, and an upstreaming
candidate rather than a local workaround.

pan_mod_get_handler() returns NULL when no registered handler matches the
modifier (src/panfrost/lib/pan_mod.c):

    const struct pan_mod_handler *
    GENX(pan_mod_get_handler)(uint64_t modifier)
    {
       for (...)
          if (pan_mod_handlers[i].match(modifier))
             return &pan_mod_handlers[i];

       return NULL;
    }

panvk_image.c takes that result and stores it in the image without checking:

    const struct pan_mod_handler *mod_handler =
       pan_mod_get_handler(arch, image->vk.drm_format_mod);
    ...
    .mod_handler = mod_handler,

and pan_image_layout_init() then does

    assert(image->mod_handler);
    const struct pan_mod_handler *mod_handler = image->mod_handler;

which is a no-op in a release build, so the next dereference faults. Measured
on device as SIGSEGV at pan_image_layout_init+212, fault addr 0x10:

    #00 pan_image_layout_init +212
    #01 panvk_image_init +1832
    #02 panvk_android_create_gralloc_image +368

Three separate things hit this in one session, all with the same backtrace:
DRM_FORMAT_MOD_INVALID from a gralloc whose modifier cannot be queried, an
AFBC modifier the driver does not advertise, and an AFBC modifier it does
advertise under PANVK_DEBUG=wsi_afbc. The common factor is not AFBC - it is
that an unmatched modifier produces NULL and nothing checks.

A driver handed a modifier it has no handler for should fail image creation,
not crash the process. VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT
is the specific error the spec provides for a modifier the implementation
cannot use, and vkCreateImage is allowed to return it.

This does not make any modifier work. It turns a segfault into a diagnosable
error, which is the difference between "the app died" and "the driver told
you it cannot do that".

Usage: patch-panvk-image-modifier-null.py [<mesa-src-dir>]
"""
import sys
import os

mesa = sys.argv[1] if len(sys.argv) > 1 else "/opt/mesa-src"
IMAGE = os.path.join(mesa, "src/panfrost/vulkan/panvk_image.c")

src = open(IMAGE).read()

if "no handler for DRM format modifier" in src:
    print("    panvk_image.c: already patched")
    sys.exit(0)

anchor = """   const struct pan_mod_handler *mod_handler =
      pan_mod_get_handler(arch, image->vk.drm_format_mod);"""

assert anchor in src, \
    "panvk_image.c: pan_mod_get_handler call site not found - PanVK moved"

src = src.replace(anchor, anchor + """

   /* pan_mod_get_handler() returns NULL when nothing matches the modifier.
    * pan_image_layout_init() only asserts on that, which is compiled out in
    * a release build, so the NULL reaches a dereference and the process
    * dies. Fail here instead, where the caller can still be told why.
    *
    * Reached in practice by importing an Android gralloc buffer whose
    * modifier cannot be determined, and by any modifier the arch has no
    * handler for. See docs/kbase-notes.md - "no handler for DRM format
    * modifier".
    */
   if (!mod_handler) {
      return panvk_error(image->vk.base.device,
                         VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
   }""", 1)

open(IMAGE, "w").write(src)
print("    panvk_image.c: unsupported modifier now rejected, not dereferenced")
