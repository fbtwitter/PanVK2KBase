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

# --- Mesa CS instruction encoder (see docs/mesa-cs-builder.md) ---
# Builds real Mali CSF instructions via Mesa's own encoder
# (cs_builder.h) instead of sentinel bytes - see docs/kbase-notes.md's
# "poll()/read() on the kbase fd" section for why. Needs a real Mesa
# checkout, not vendored in this repo (see docs/mesa-cs-builder.md for
# the clone command - it's gitignored, ~600MB, local to whichever
# machine clones it) and a Python interpreter for the genxml codegen
# step below.
MESA_DIR ?= third_party/MESA-KMOD
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

clean:
	rm -f first_test