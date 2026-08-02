// Can this driver be loaded the way an emulator would load it?
//
// The last item on the presentation list, and the only one that is not
// driver work at all. On Android the Vulkan loader takes the system driver
// from /vendor/lib64/hw/vulkan.<ro.hardware>.so, which needs root to
// replace. Emulators (Winlator, Eden, Azahar, Skyline, PPSSPP) get around
// that with libadrenotools, whose README says plainly that Mali is not
// supported.
//
// That claim is about libadrenotools, not about the kernel or the linker,
// and the two are worth separating. libadrenotools does three things:
//
//   1. loads a driver .so through a purpose-built linker namespace,
//   2. redirects the Adreno blob's file accesses (its own /vendor data),
//   3. bcenabler, an Adreno texture-compression patch.
//
// Only (1) matters for a Mesa driver. (2) and (3) are what make the library
// Adreno-specific, and a Mesa driver needs neither. So the real question is
// whether (1) works for us, and that is a linker question with a testable
// answer.
//
// Why a namespace is needed at all - measured, not assumed. This driver's
// DT_NEEDED list is:
//
//   libdrm.so         NOT an Android public library
//   libhardware.so    NOT an Android public library
//   liblog.so, libnativewindow.so, libsync.so, libz.so, libm.so,
//   libdl.so, libc.so                                       all public
//
// An ordinary app namespace only resolves public libraries, so a plain
// dlopen() of this driver from an app would fail on libdrm and libhardware
// - both of which do exist on the device, in /system/lib64 and
// /vendor/lib64. A namespace whose search path includes those directories
// resolves them. That is precisely the mechanism, and precisely why it is
// not Adreno-specific.
//
// This probe builds that namespace itself and loads the driver through it:
// android_create_namespace() + android_link_namespaces() +
// android_dlopen_ext(ANDROID_DLEXT_USE_NAMESPACE), which is what
// libadrenotools does. The two namespace calls are not NDK-public - they are
// exported from the linker and reached by dlsym, exactly as libadrenotools
// reaches them - so the probe reports honestly if they cannot be found
// rather than failing to build.
//
// A shell process is not an app, so success here is evidence rather than
// proof. What it does establish is that nothing about a Mali/Mesa driver
// resists this loading mechanism, which is the part in doubt.
//
// No GPU work beyond enumerating a physical device.
//
// Usage: driver_namespace_probe <path-to-libvulkan_panfrost.so>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/dlext.h>
#include <vulkan/vulkan.h>

/* Bionic's namespace API. android_dlopen_ext() is NDK-public;
 * android_create_namespace()/android_link_namespaces() are not, and are
 * resolved by dlsym below.
 */
struct android_namespace_t;

typedef struct android_namespace_t *(*create_ns_fn)(
   const char *name, const char *ld_library_path,
   const char *default_library_path, uint64_t type,
   const char *permitted_when_isolated_path,
   struct android_namespace_t *parent);

typedef bool (*link_ns_fn)(struct android_namespace_t *from,
                           struct android_namespace_t *to,
                           const char *shared_libs_sonames);

#ifndef ANDROID_NAMESPACE_TYPE_ISOLATED
#define ANDROID_NAMESPACE_TYPE_ISOLATED 1
#endif
#ifndef ANDROID_NAMESPACE_TYPE_SHARED
#define ANDROID_NAMESPACE_TYPE_SHARED 2
#endif

/* The public libraries our driver needs, plus the ones any loaded code
 * needs. Linking these from the default namespace is what lets an isolated
 * namespace still use the platform.
 */
/* Built up empirically, one dlopen failure at a time - which is the honest
 * way to arrive at it, since the transitive set is not documented anywhere.
 *
 * The non-obvious entries are the last two. libhardware pulls in
 * libvndksupport (for android_load_sphal_library), which in turn needs
 * libdl_android.so from /apex/com.android.runtime/lib64/bionic. An isolated
 * namespace cannot reach into the apex, so both have to be linked in from
 * the default namespace rather than found on the search path.
 */
static const char *PUBLIC_SONAMES =
   "libc.so:libm.so:libdl.so:liblog.so:libz.so:libnativewindow.so:"
   "libsync.so:libandroid.so:libvulkan.so:"
   "libvndksupport.so:libdl_android.so";

/* Same libhardware ABI mirrors as the other driver probes. */
struct hw_module_methods_t;

#ifdef __LP64__
typedef uint64_t hw_reserved_t;
#else
typedef uint32_t hw_reserved_t;
#endif

typedef struct hw_module_t {
   uint32_t tag;
   uint16_t module_api_version;
   uint16_t hal_api_version;
   const char *id;
   const char *name;
   const char *author;
   struct hw_module_methods_t *methods;
   void *dso;
   hw_reserved_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_device_t {
   uint32_t tag;
   uint32_t version;
   struct hw_module_t *module;
   hw_reserved_t reserved[12];
   int (*close)(struct hw_device_t *device);
} hw_device_t;

typedef struct hw_module_methods_t {
   int (*open)(const struct hw_module_t *module, const char *id,
               struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hwvulkan_device_t {
   struct hw_device_t common;
   PFN_vkEnumerateInstanceExtensionProperties
      EnumerateInstanceExtensionProperties;
   PFN_vkCreateInstance CreateInstance;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

#define HWVULKAN_DEVICE_0 "vk0"

static int failures;

static void check(bool ok, const char *what) {
   printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
   if (!ok)
      failures++;
}

static const char *sum_api = "not_found";
static const char *sum_ns = "not_attempted";
static const char *sum_link = "not_attempted";
static const char *sum_load = "not_attempted";
static const char *sum_drive = "not_attempted";

int main(int argc, char **argv) {
   setvbuf(stdout, NULL, _IONBF, 0);

   if (argc < 2) {
      fprintf(stderr, "usage: %s <path-to-libvulkan_panfrost.so>\n", argv[0]);
      return 2;
   }

   /* Split the path: the namespace gets the directory, dlopen gets the
    * basename, which is how a namespace-scoped load is meant to work.
    */
   char dir[512], base[256];
   const char *slash = strrchr(argv[1], '/');
   if (slash) {
      size_t n = (size_t)(slash - argv[1]);
      if (n >= sizeof(dir))
         n = sizeof(dir) - 1;
      memcpy(dir, argv[1], n);
      dir[n] = '\0';
      snprintf(base, sizeof(base), "%s", slash + 1);
   } else {
      snprintf(dir, sizeof(dir), ".");
      snprintf(base, sizeof(base), "%s", argv[1]);
   }

   printf("\n=== the loading mechanism emulators use ===\n");
   printf("  driver dir : %s\n", dir);
   printf("  driver file: %s\n", base);

   /* ------------------------------------------- 1. is the API reachable? */
   /* Bionic exports these from libdl.so under __loader_-prefixed private
    * names; the unprefixed ones are not in the public interface and dlsym
    * returns NULL for them. libadrenotools resolves the prefixed names the
    * same way. Try both, so the report says which spelling exists here
    * rather than just "missing".
    */
   printf("\n=== bionic namespace API ===\n");
   void *libdl = dlopen("libdl.so", RTLD_NOW | RTLD_LOCAL);
   printf("  dlopen(libdl.so) -> %p\n", libdl);

   static const char *create_names[] = {"__loader_android_create_namespace",
                                        "android_create_namespace"};
   static const char *link_names[] = {"__loader_android_link_namespaces",
                                      "android_link_namespaces"};

   create_ns_fn create_ns = NULL;
   link_ns_fn link_ns = NULL;
   const char *create_used = NULL, *link_used = NULL;

   for (size_t i = 0; i < sizeof(create_names) / sizeof(create_names[0]); i++) {
      void *s = dlsym(RTLD_DEFAULT, create_names[i]);
      if (!s && libdl)
         s = dlsym(libdl, create_names[i]);
      printf("  %-36s %s\n", create_names[i], s ? "found" : "missing");
      if (s && !create_ns) {
         create_ns = (create_ns_fn)s;
         create_used = create_names[i];
      }
   }
   for (size_t i = 0; i < sizeof(link_names) / sizeof(link_names[0]); i++) {
      void *s = dlsym(RTLD_DEFAULT, link_names[i]);
      if (!s && libdl)
         s = dlsym(libdl, link_names[i]);
      printf("  %-36s %s\n", link_names[i], s ? "found" : "missing");
      if (s && !link_ns) {
         link_ns = (link_ns_fn)s;
         link_used = link_names[i];
      }
   }
   if (create_used && link_used)
      printf("  using: %s / %s\n", create_used, link_used);
   if (create_ns && link_ns)
      sum_api = "found";
   check(create_ns && link_ns,
         "the namespace API libadrenotools uses is reachable");
   if (!create_ns || !link_ns) {
      printf("\n  Without these the only route is the app's own classloader\n"
             "  namespace, which cannot resolve libdrm/libhardware.\n");
      goto summary;
   }

   /* --------------------------------------------- 2. build the namespace */
   printf("\n=== create a namespace that can see the vendor libs ===\n");
   char search_path[1024];
   snprintf(search_path, sizeof(search_path),
            "%s:/system/lib64:/vendor/lib64:/vendor/lib64/hw", dir);
   printf("  search path: %s\n", search_path);

   struct android_namespace_t *ns =
      create_ns("panvk-kbase", search_path, NULL,
                ANDROID_NAMESPACE_TYPE_SHARED | ANDROID_NAMESPACE_TYPE_ISOLATED,
                "/system/lib64:/vendor/lib64:/data/local/tmp", NULL);

   printf("  android_create_namespace -> %p\n", (void *)ns);
   sum_ns = ns ? "ok" : "failed";
   check(ns != NULL, "created a linker namespace for the driver");
   if (!ns)
      goto summary;

   bool linked = link_ns(ns, NULL, PUBLIC_SONAMES);
   printf("  android_link_namespaces(default) -> %s\n",
          linked ? "true" : "false");
   sum_link = linked ? "ok" : "failed";
   check(linked, "linked it to the platform's public libraries");

   /* ------------------------------------------------- 3. load the driver */
   printf("\n=== load the driver through that namespace ===\n");
   android_dlextinfo info = {
      .flags = ANDROID_DLEXT_USE_NAMESPACE,
      .library_namespace = ns,
   };
   void *h = android_dlopen_ext(base, RTLD_NOW | RTLD_LOCAL, &info);
   if (!h) {
      printf("  android_dlopen_ext(%s) failed: %s\n", base, dlerror());
      sum_load = "failed";
      check(false, "driver loaded through the custom namespace");
      goto summary;
   }
   printf("  android_dlopen_ext(%s) -> %p\n", base, h);
   sum_load = "ok";
   check(true, "driver loaded through the custom namespace");

   /* ------------------------------------------------- 4. and it works */
   printf("\n=== and it is a working driver, not just a loaded file ===\n");
   hw_module_t *mod = dlsym(h, "HMI");
   hw_device_t *hwdev = NULL;
   if (!mod || !mod->methods ||
       mod->methods->open(mod, HWVULKAN_DEVICE_0, &hwdev) != 0 || !hwdev) {
      printf("  hwvulkan HAL open failed\n");
      sum_drive = "failed";
      check(false, "hwvulkan HAL opened from the namespaced driver");
      goto summary;
   }
   printf("  HMI: id='%s' name='%s'\n", mod->id ? mod->id : "(null)",
          mod->name ? mod->name : "(null)");

   hwvulkan_device_t *vk = (hwvulkan_device_t *)hwdev;
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "panvk-kbase-namespace-probe",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance inst = VK_NULL_HANDLE;
   VkResult r = vk->CreateInstance(&ici, NULL, &inst);
   printf("  vkCreateInstance -> %d\n", r);

   if (r == VK_SUCCESS) {
      PFN_vkEnumeratePhysicalDevices enum_pd =
         (PFN_vkEnumeratePhysicalDevices) vk->GetInstanceProcAddr(
            inst, "vkEnumeratePhysicalDevices");
      PFN_vkGetPhysicalDeviceProperties get_props =
         (PFN_vkGetPhysicalDeviceProperties) vk->GetInstanceProcAddr(
            inst, "vkGetPhysicalDeviceProperties");
      uint32_t count = 1;
      VkPhysicalDevice pd = VK_NULL_HANDLE;
      r = enum_pd(inst, &count, &pd);
      if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && count > 0) {
         VkPhysicalDeviceProperties props;
         get_props(pd, &props);
         printf("  enumerated: %s (apiVersion %u.%u.%u)\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion),
                VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion));
         sum_drive = "ok";
      }
   }
   check(!strcmp(sum_drive, "ok"),
         "enumerated the GPU through the namespaced driver");

summary:
   printf("\n=== NAMESPACE LOAD SUMMARY ===\n");
   printf("NAMESPACE_API=%s\n", sum_api);
   printf("CREATE_NAMESPACE=%s\n", sum_ns);
   printf("LINK_NAMESPACES=%s\n", sum_link);
   printf("DLOPEN_EXT=%s\n", sum_load);
   printf("DRIVER_USABLE=%s\n", sum_drive);

   printf("\n=== %d failure(s) ===\n", failures);
   if (failures == 0)
      printf("\n=> The rootless driver-loading mechanism works for this Mali\n"
             "   driver. Nothing about it is Adreno-specific: libadrenotools'\n"
             "   Adreno parts are its file-redirect hooks and bcenabler, and\n"
             "   a Mesa driver needs neither. What an emulator would have to\n"
             "   add is the namespace search path, not new machinery.\n");
   return failures ? 1 : 0;
}
