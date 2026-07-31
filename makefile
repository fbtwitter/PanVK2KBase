CC ?= gcc
CFLAGS ?= -O0 -g -Wall -Wextra
MALIFLAGS ?= -DMALI_USE_CSF=1
MESAFLAGS ?= -DPAN_ARCH=10
 
# Path to the vendored kbase r44p0 uapi headers, relative to this Makefile.
# Adjust if you placed third_party/ somewhere else relative to this file.
KBASE_UAPI_DIR := third_party/kbase-uapi-r44p0
MESA_DIR := third_party/mesa/src
MESA_GENXML_DIR := third_party/mesa/src/panfrost
MESA_INCLUDE_DIR := third_party/mesa/include
STUB_INCLUDE_DIR := stubs
 
INCLUDES := -I$(STUB_INCLUDE_DIR) -I$(KBASE_UAPI_DIR) -I$(MESA_DIR) -I$(MESA_GENXML_DIR) -I$(MESA_INCLUDE_DIR) -Isrc/utils
 
.PHONY: all clean
 
all: first_test
 
first_test: ./src/tests/first_test/first_test.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/first_test $<

memory: ./src/tests/memory/memory.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/memory $<

memory2: ./src/tests/memory/memory2.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/memory2 $<

queue_group: ./src/tests/queue_group/queue_group.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/queue_group $<
 
command: ./src/tests/command/command.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) $(MESAFLAGS) -o ./build/command $<

clean:
	rm -f first_test