#ifndef AMDGPU_POC_COMMON_H
#define AMDGPU_POC_COMMON_H

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static inline void die_errno(const char *message) {
  fprintf(stderr, "%s: %s\n", message, strerror(errno));
  exit(1);
}

static inline void dief(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
  exit(1);
}

static inline void check_ret(int ret, const char *message) {
  if (ret) {
    fprintf(stderr, "%s: %s (%d)\n", message, strerror(-ret), ret);
    exit(1);
  }
}

static inline u64 align_up_u64(u64 value, u64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

#endif
