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

clean:
	rm -f first_test