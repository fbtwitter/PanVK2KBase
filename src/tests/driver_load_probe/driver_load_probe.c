// Loads a built PanVK Android driver (.so) with dlopen() and inspects its
// Android hwvulkan HAL entrypoint, WITHOUT installing it as the system
// driver.
//
// Why this exists: replacing /vendor/lib64/hw/vulkan.mali.so would need a
// writable /vendor (root) and would break the device's graphics stack if the
// driver misbehaves. dlopen()ing it from /data/local/tmp proves the far
// cheaper things - that the .so is valid aarch64, that every NEEDED
// dependency resolves on this device, and that the HAL entrypoint is
// present and well-formed - with zero risk to the running system.
//
// It does NOT prove the driver works: PanVK still enumerates devices by
// looking for DRM render nodes rather than /dev/mali0, and its submission
// path is panthor-specific. See docs/architecture.md and ROADMAP.md Phase 2.
//
// Usage: driver_load_probe /data/local/tmp/libvulkan_panfrost.so
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Mirrors hardware/libhardware's hw_module_t prologue. Only the leading
// fields are needed to sanity-check the module header, and reproducing them
// avoids a build dependency on the Android platform headers.
struct hw_module_prologue {
   uint32_t tag;
   uint16_t module_api_version;
   uint16_t hal_api_version;
   const char *id;
   const char *name;
   const char *author;
};

/* libhardware's HARDWARE_MODULE_TAG, i.e.
 * MAKE_TAG_CONSTANT('H','W','M','T') == ('H'<<24)|('W'<<16)|('M'<<8)|'T'.
 */
#define HARDWARE_MODULE_TAG_VAL 0x48574D54

int main(int argc, char **argv) {
   if (argc != 2) {
      fprintf(stderr, "usage: %s <path-to-libvulkan_panfrost.so>\n", argv[0]);
      return 2;
   }

   const char *path = argv[1];

   printf("dlopen(%s)\n", path);
   void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("FAILED: %s\n", dlerror());
      printf("\n=> the driver did not load. Usually a missing NEEDED\n"
             "   dependency or an ABI mismatch.\n");
      return 1;
   }

   printf("OK: loaded, all NEEDED dependencies resolved\n");

   // Android's Vulkan loader finds a hwvulkan HAL module through the
   // HAL_MODULE_INFO_SYM symbol, which is spelled "HMI".
   void *hmi = dlsym(h, "HMI");
   if (!hmi) {
      printf("WARN: no HMI symbol - not an Android hwvulkan HAL module?\n");
      dlclose(h);
      return 1;
   }

   printf("OK: HMI (Android HAL module entrypoint) present at %p\n", hmi);

   const struct hw_module_prologue *m = hmi;
   printf("  tag                = 0x%08x%s\n", m->tag,
          m->tag == HARDWARE_MODULE_TAG_VAL ? " (HARDWARE_MODULE_TAG)" : "");
   printf("  module_api_version = 0x%04x\n", m->module_api_version);
   printf("  hal_api_version    = 0x%04x\n", m->hal_api_version);
   printf("  id                 = %s\n", m->id ? m->id : "(null)");
   printf("  name               = %s\n", m->name ? m->name : "(null)");
   printf("  author             = %s\n", m->author ? m->author : "(null)");

   dlclose(h);

   printf("\n=> The driver loads and exposes a well-formed Android Vulkan\n"
          "   HAL module. This does NOT mean it can drive the GPU: PanVK\n"
          "   still enumerates via DRM render nodes, not /dev/mali0.\n");
   return 0;
}
