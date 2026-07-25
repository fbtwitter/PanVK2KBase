#include <stddef.h>
#include <stdio.h>

static void dump_hex(const unsigned char *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    printf("%02x ", buf[i]);
    if ((i + 1) % 16 == 0)
      printf("\n");
  }
  if (len % 16)
    printf("\n");
}