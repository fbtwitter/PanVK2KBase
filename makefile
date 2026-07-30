CC ?= gcc
CFLAGS ?= -O0 -g -Wall -Wextra
MALIFLAGS ?= -DMALI_USE_CSF=1
 
# Which vendored kbase uapi header set to build against. Two are vendored:
# r44p0 (UK 1.20, Mali-G615-MC2 target) and r49p1 (UK 1.30, confirmed match
# for the Poco X8 Pro / Mali-G720 device tested in docs/kbase-notes.md).
# Override on the command line to target the other, e.g.:
#   make queue_group KBASE_VERSION=r49p1
KBASE_VERSION ?= r44p0
KBASE_UAPI_DIR := third_party/kbase-uapi-$(KBASE_VERSION)

# -include the kconfig shim so r49p1's IS_ENABLED() guards resolve
# without a real kernel build tree (see src/utils/kconfig_shim.h). Harmless
# for r44p0, which doesn't use IS_ENABLED at all.
INCLUDES := -I$(KBASE_UAPI_DIR) -Isrc/utils -include src/utils/kconfig_shim.h
 
.PHONY: all clean
 
all: first_test
 
first_test: ./src/tests/first_test/first_test.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/first_test $<

memory: ./src/tests/memory/memory.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/memory $<

memory2: ./src/tests/memory/memory2.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/memory2 $<

queue_group: ./src/tests/queue_group/queue_group.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/queue_group $<

# KBASE_IOCTL_INTERNAL_FENCE_WAIT only exists in r49p1's headers.
fence_probe: ./src/tests/fence_probe/fence_probe.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/fence_probe $<

clean:
	rm -f first_test