# Upstream submission: panvk NULL modifier handler crash

Ready to send. Verified on hardware before writing, per this project's rule
that a patch is a conclusion rather than an opinion.

Not kbase-specific: this is shared PanVK code and the bug is reachable by any
Mesa Vulkan user on Mali who imports a buffer whose modifier the driver has
no handler for. Same category as the `null-device-destroy` patch already
sent (`Joshua-Micheletti/PanVK2KBase#2` established the pattern: evidence
attached, claim narrow).

## The patch

Against `src/panfrost/vulkan/panvk_image.c`, in `panvk_image_init_layouts()`.
Note the comment differs from the local version - the reference to
`docs/kbase-notes.md` is dropped, since that path does not exist upstream.

```diff
--- a/src/panfrost/vulkan/panvk_image.c
+++ b/src/panfrost/vulkan/panvk_image.c
@@ -456,6 +456,17 @@ panvk_image_init_layouts(struct panvk_image *image,
    const struct pan_mod_handler *mod_handler =
       pan_mod_get_handler(arch, image->vk.drm_format_mod);
 
+   /* pan_mod_get_handler() returns NULL when no handler matches the
+    * modifier. pan_image_layout_init() only assert()s on that, which is
+    * compiled out in release builds, so the NULL reaches a dereference and
+    * the process dies. Fail here instead, where the caller can be told why.
+    *
+    * Reachable by importing a buffer whose modifier the driver has no
+    * handler for - an Android gralloc buffer whose modifier cannot be
+    * determined is one way to get there.
+    */
+   if (!mod_handler)
+      return panvk_error(image->vk.base.device,
+                         VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
+
    /* initialize pan_image props and mod_handler */
```

## Commit message

```
panvk: reject unsupported DRM format modifiers instead of dereferencing NULL

pan_mod_get_handler() returns NULL when none of the registered handlers
match the modifier. panvk_image_init_layouts() stored that result in the
image without checking it, and pan_image_layout_init() only guards it with

    assert(image->mod_handler);

which is compiled out in a release build. The NULL then reaches

    const struct pan_mod_handler *mod_handler = image->mod_handler;
    ...

and the process takes SIGSEGV inside pan_image_layout_init() rather than
vkCreateImage() returning an error.

Observed on a Mali-G720 (v10) importing an Android gralloc buffer whose
format modifier could not be determined, so VK_ANDROID_native_buffer import
passed DRM_FORMAT_MOD_INVALID down:

    signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x10
      #00 pan_image_layout_init+212
      #01 panvk_image_init+1832
      #02 panvk_android_create_gralloc_image+368

The same crash is reachable without Android, by creating an image with
VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and any modifier the arch has no
handler for.

Return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT instead, which
is the error the spec provides for a modifier the implementation cannot use.

This does not make any additional modifier work; it turns a crash into a
diagnosable error.
```

## Merge request description

Extra context for reviewers, beyond the commit message:

> Found while bringing up VK_ANDROID_native_buffer on a device whose gralloc
> cannot be queried for a format modifier (u_gralloc falls back to its
> fallback backend, which reports DRM_FORMAT_MOD_INVALID while returning
> success). That is arguably a separate issue in u_gralloc; this patch is
> only about PanVK not crashing when it is handed a modifier it has no
> handler for.
>
> Three distinct cases hit this in one debugging session with an identical
> backtrace: DRM_FORMAT_MOD_INVALID from the Android import path, an AFBC
> modifier the driver does not advertise, and an AFBC modifier it does
> advertise under PANVK_DEBUG=wsi_afbc. The common factor is not AFBC or
> Android - it is that an unmatched modifier yields NULL and nothing checks.
>
> Verified on hardware (Mali-G720 MC8, v10). Before the patch, a loop that
> creates one image per modifier returned by
> vkGetPhysicalDeviceFormatProperties2 kills the process on the first
> unsupported one. After it, the same loop runs to completion and reports
> one clean rejection among nine modifiers. Unrelated paths are unaffected:
> the driver's own render tests and dEQP-VK.transform_feedback.simple.basic_*
> are unchanged (37/38, same as before).

## Before sending

- Rebase onto current Mesa main and re-check the anchor - this project's
  tree is pinned, and `panvk_image_init_layouts()` may have moved.
- Consider whether `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT`
  or `VK_ERROR_INITIALIZATION_FAILED` is the better error here. The former
  is more specific and is what a reviewer is likely to prefer, but it is
  only listed for vkCreateImage, so check the call paths into
  panvk_image_init_layouts() reach it only from image creation.
- The local copy of this fix lives in
  `src/mesa/patch-panvk-image-modifier-null.py` and references
  `docs/kbase-notes.md`; the upstream version above drops that.
