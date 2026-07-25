CC ?= gcc
CFLAGS ?= -O0 -g -Wall -Wextra
MALIFLAGS ?= -DMALI_USE_CSF=1
 
# Path to the vendored kbase r44p0 uapi headers, relative to this Makefile.
# Adjust if you placed third_party/ somewhere else relative to this file.
KBASE_UAPI_DIR := third_party/kbase-uapi-r44p0
 
INCLUDES := -I$(KBASE_UAPI_DIR) -Isrc/utils
 
.PHONY: all clean
 
all: first_test
 
first_test: ./src/tests/first_test/first_test.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MALIFLAGS) -o ./build/first_test $<
 
clean:
	rm -f first_test