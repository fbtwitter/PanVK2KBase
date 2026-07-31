CC ?= gcc
CFLAGS ?= -O0 -g -Wall -Wextra
MALIFLAGS ?= -DMALI_USE_CSF=1
 
# Which vendored kbase uapi header set to build against. Not limited to
# a fixed list - any third_party/kbase-uapi-<name>/ directory following
# the same layout works (see third_party/kbase-uapi-r49p1/README.md for
# what that layout is: an 18-19 file mirror of a real vendor kbase UAPI
# tree). Two are vendored right now: r44p0 (UK 1.20, Mali-G615-MC2
# target) and r49p1 (UK 1.30, confirmed match for the Poco X8 Pro /
# Mali-G720 device tested in docs/kbase-notes.md). Override on the
# command line to target any of them, e.g.:
#   make queue_group KBASE_VERSION=r49p1
#   make list-kbase-versions   # see what's actually vendored
KBASE_VERSION ?= r44p0
KBASE_UAPI_DIR := third_party/kbase-uapi-$(KBASE_VERSION)

ifeq ($(wildcard $(KBASE_UAPI_DIR)),)
$(error KBASE_VERSION=$(KBASE_VERSION) has no $(KBASE_UAPI_DIR)/ - run \
  'make list-kbase-versions' to see what's vendored, or vendor a new \
  one there first)
endif

# -include the kconfig shim so headers using IS_ENABLED() (r49p1, and
# potentially future MTK-derived versions) resolve it without a real
# kernel build tree (see src/utils/kconfig_shim.h). Harmless for header
# sets that don't use IS_ENABLED at all, like r44p0.
#
# Source files that use an ioctl/struct/flag not guaranteed present
# across every vendored version (like KBASE_IOCTL_INTERNAL_FENCE_WAIT,
# r49p1-only) should guard that usage with "#ifdef SYMBOL_NAME" so they
# build against any KBASE_VERSION and just skip what that header
# doesn't declare - see tests/fence_probe/fence_probe.c for the pattern
# to follow when adding new version-sensitive code.
INCLUDES := -I$(KBASE_UAPI_DIR) -Isrc/utils -include src/utils/kconfig_shim.h

.PHONY: all clean list-kbase-versions

all: first_test

list-kbase-versions:
	@ls -1 third_party | sed -n 's/^kbase-uapi-//p'
 
first_test: ./src/tests/first_test/first_test.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/first_test $<

memory: ./src/tests/memory/memory.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/memory $<

memory2: ./src/tests/memory/memory2.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/memory2 $<

queue_group: ./src/tests/queue_group/queue_group.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/queue_group $<

# Builds against any KBASE_VERSION - adapts at compile time to whether
# that header set declares KBASE_IOCTL_INTERNAL_FENCE_WAIT (r49p1: yes,
# r44p0: no) instead of requiring one specific version.
fence_probe: ./src/tests/fence_probe/fence_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/fence_probe $<

event_probe: ./src/tests/event_probe/event_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/event_probe $<

# Read-only query of the CSF firmware's global interface (slot counts,
# stream counts, interface version). No submission, no state change.
glb_iface_probe: ./src/tests/glb_iface_probe/glb_iface_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/glb_iface_probe $<

# dlopen()s a built PanVK Android driver to check it loads on-device.
# Doesn't touch /vendor - see the source for why.
driver_load_probe: ./src/tests/driver_load_probe/driver_load_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_load_probe $<

# Drives a built PanVK driver through its Android HAL entrypoint and calls
# vkEnumeratePhysicalDevices - the end-to-end test of the kbase backend.
driver_enum_probe: ./src/tests/driver_enum_probe/driver_enum_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_enum_probe $<

# Drives the kbase event-memory vk_sync through the real Vulkan API:
# timeline semaphore signal/get/wait and binary fence status/reset. Proves
# the sync type works, not merely that it registered.
driver_sync_probe: ./src/tests/driver_sync_probe/driver_sync_probe.c
	$(CC) $(CFLAGS) -o ./build/driver_sync_probe $<

# Settles what a SAME_VA kbase BO can be re-mmap-ed with (cookie vs resolved
# address). Decides kbase_kmod_bo_get_mmap_offset()'s implementation.
remap_probe: ./src/tests/remap_probe/remap_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/remap_probe $<

# Answers whether kbase can allocate at a caller-chosen GPU VA
# (KBASE_IOCTL_MEM_ALLOC_EX + BASE_MEM_FIXED). Decides whether pan_kmod's
# vm_bind contract is implementable at all - see docs/kbase-notes.md.
fixed_va_probe: ./src/tests/fixed_va_probe/fixed_va_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/fixed_va_probe $<

# Establishes that KBASE_IOCTL_VERSION_CHECK is once-per-fd.
double_handshake_probe: ./src/tests/double_handshake_probe/double_handshake_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/double_handshake_probe $<

# --- Mesa CS instruction encoder (see docs/mesa-cs-builder.md) ---
# Builds real Mali CSF instructions via Mesa's own encoder
# (cs_builder.h) instead of sentinel bytes - see docs/kbase-notes.md's
# "poll()/read() on the kbase fd" section for why. Needs a real Mesa
# checkout, not vendored in this repo (see docs/mesa-cs-builder.md for
# the clone command - it's gitignored, ~600MB, local to whichever
# machine clones it) and a Python interpreter for the genxml codegen
# step below.
MESA_DIR ?= third_party/MESA-KMOD
# llvm-nm from the same toolchain as CC, for the backend symbol check.
NM ?= llvm-nm
# Must match the target GPU's architecture major version, e.g. 12 for
# this repo's Poco X8 Pro/Mali-G720 target (see utils/parse_gpu_props.h's
# decode). A different device needs a different PAN_ARCH.
PAN_ARCH ?= 12
PYTHON ?= python3
MESA_GENXML := $(MESA_DIR)/src/panfrost/genxml
MESA_PACK_H := $(MESA_GENXML)/v$(PAN_ARCH)_pack.h
MESA_CS_INCLUDES := -I$(MESA_DIR)/src/panfrost -I$(MESA_DIR)/src -I$(MESA_DIR)/src/util -I$(MESA_DIR)/include
# Mesa's C11 threads/time compat shims (src/c11/) need to be told
# explicitly what the platform supports - normally supplied by meson.
# Android and Linux both have real pthreads and a real struct timespec.
MESA_CS_DEFS := -DPAN_ARCH=$(PAN_ARCH) -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC
# Dead-code-strip so only what cs_builder.h actually calls
# (reralloc_size, util_dynarray_is_data_stack_allocated) gets linked,
# not the GPU-shader-printf machinery ralloc.c would otherwise
# transitively pull in via whole-object linking - see
# docs/mesa-cs-builder.md.
MESA_CS_GC := -ffunction-sections -fdata-sections -Wl,--gc-sections

.PHONY: mesa-cs-pack

mesa-cs-pack: $(MESA_PACK_H)

$(MESA_PACK_H):
	@test -d "$(MESA_DIR)" || { echo "error: $(MESA_DIR) not found - see docs/mesa-cs-builder.md for the clone command"; exit 1; }
	cd $(MESA_GENXML) && $(PYTHON) gen_pack.py v$(PAN_ARCH).xml > v$(PAN_ARCH)_pack.h

cs_encode_probe: ./src/tests/cs_encode_probe/cs_encode_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/cs_encode_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# Combines the kbase ioctl surface with Mesa's CS encoder - builds a
# real instruction, writes it through the real CS_INSERT/KICK protocol
# (see utils/csf_user_regs.h), and checks for a real CSF notification.
live_kick_probe: ./src/tests/live_kick_probe/live_kick_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/live_kick_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# Determines empirically which of CS_QUEUE_BIND's 3 mmap'd pages is
# input/output/doorbell, rather than assuming. Exists because Panfork (a
# Panfrost driver that ran on real kbase hardware) disagrees with
# utils/csf_user_regs.h by one page - see "Prior art found" in
# docs/kbase-notes.md. Same setup as live_kick_probe so results are
# comparable.
user_io_probe: ./src/tests/user_io_probe/user_io_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/user_io_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# Gets a real completion signal out of the GPU: BASE_MEM_CSF_EVENT memory
# plus a SYNC_SET64 emitted into the command stream. This is the mechanism
# a non-DRM vk_sync has to be built on - see "Finding 2" in
# docs/kbase-notes.md.
event_slot_probe: ./src/tests/event_slot_probe/event_slot_probe.c $(MESA_PACK_H)
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESA_CS_DEFS) $(MESA_CS_INCLUDES) $(MESA_CS_GC) \
	  -o ./build/event_slot_probe $< \
	  $(MESA_DIR)/src/util/ralloc.c $(MESA_DIR)/src/util/u_dynarray.c

# --- Phase 2: pan_kmod_kbase Mesa backend (see src/mesa/README.md) ---
# The Mesa checkout is gitignored, so the backend source lives in this repo
# and is synced into it. MESA_DIR/PAN_ARCH/KBASE_VERSION are shared with the
# cs_encode_probe targets above.
MESA_KMOD_DIR := $(MESA_DIR)/src/panfrost/lib/kmod

.PHONY: mesa-backend-sync mesa-backend-check

mesa-backend-sync:
	@test -d "$(MESA_KMOD_DIR)" || { echo "error: $(MESA_KMOD_DIR) not found - see docs/mesa-cs-builder.md for the Mesa clone command"; exit 1; }
	cp src/mesa/pan_kmod_kbase.c src/mesa/pan_kmod_kbase.h $(MESA_KMOD_DIR)/
	cp src/mesa/panvk_kbase_sync.c src/mesa/panvk_kbase_sync.h $(MESA_DIR)/src/panfrost/vulkan/
	@echo ""
	@echo "Copied pan_kmod_kbase.{c,h} into $(MESA_KMOD_DIR)/"
	@echo "Copied panvk_kbase_sync.{c,h} into $(MESA_DIR)/src/panfrost/vulkan/"
	@echo "Still to apply (kept as readable patches since upstream moves):"
	@echo "  - src/mesa/pan_kmod.c.kbase.patch      -> $(MESA_KMOD_DIR)/pan_kmod.c"
	@echo "  - src/mesa/meson.build.kbase.patch     -> $(MESA_KMOD_DIR)/meson.build"
	@echo "  - src/mesa/patch-panvk-kbase-enumeration.py <mesa-dir>  (PanVK enumeration)"
	@echo "  - src/mesa/patch-panvk-kbase-sync.py        <mesa-dir>  (PanVK vk_sync)"

# Real libdrm, fetched via Mesa's own meson wrap (pan_kmod.h includes
# <xf86drm.h>, and a shallow clone doesn't fetch subprojects). Falls back to
# the minimal stub in src/mesa/syntax-check-stubs/ when absent.
MESA_LIBDRM := $(firstword $(wildcard $(MESA_DIR)/subprojects/libdrm-*))
ifeq ($(MESA_LIBDRM),)
DRM_INC := -Isrc/mesa/syntax-check-stubs
DRM_KIND := stubbed libdrm
else
DRM_INC := -I$(MESA_LIBDRM) -I$(MESA_LIBDRM)/include/drm
DRM_KIND := real libdrm ($(notdir $(MESA_LIBDRM)))
endif

MESA_BACKEND_INC := $(DRM_INC) \
  -I$(MESA_DIR)/src/panfrost/lib -I$(MESA_KMOD_DIR) \
  -I$(MESA_DIR)/src/panfrost/model -I$(MESA_DIR)/src/panfrost/perf \
  -I$(MESA_DIR)/src/panfrost -I$(MESA_DIR)/src -I$(MESA_DIR)/src/util \
  -I$(MESA_DIR)/include -I$(KBASE_UAPI_DIR)

.PHONY: mesa-libdrm

# Fetch real libdrm into the Mesa checkout via its own pinned wrap.
mesa-libdrm:
	cd $(MESA_DIR) && $(PYTHON) -m mesonbuild.mesonmain subprojects download libdrm

# Compiles the backend to a real object file (not just -fsyntax-only)
# against the real Mesa and kbase headers, then checks that the kbase
# symbols pan_kmod.c needs are exactly the ones the backend defines. This
# is a compile + symbol-resolution check, NOT a full Mesa build - see
# src/mesa/README.md for why a full build needs LLVM.
mesa-backend-check: mesa-backend-sync
	@mkdir -p build/mesa-backend
	@echo "using: $(DRM_KIND)"
	$(CC) -c -O1 -Wall $(MALIFLAGS) -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC \
	  -include src/utils/kconfig_shim.h $(MESA_BACKEND_INC) \
	  -o build/mesa-backend/pan_kmod_kbase.o \
	  $(MESA_KMOD_DIR)/pan_kmod_kbase.c
	@echo "pan_kmod_kbase.c: compiles clean against real Mesa + kbase headers"
	@echo "defines: $$($(NM) --defined-only -g build/mesa-backend/pan_kmod_kbase.o | awk '{print $$NF}' | tr '\n' ' ')"

clean:
	rm -f first_test