#pragma once

// Portable shims for POSIX / Windows differences.

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Portable open_memstream / close pair.
// On POSIX, delegates to open_memstream + fclose.
// On Windows, uses tmpfile() + manual buffer extraction (open_memstream doesn't exist in ucrt).

#ifdef _WIN32

static inline FILE* compat_open_memstream(char** buf, size_t* size) {
  *buf = NULL;
  *size = 0;
  return tmpfile();
}

static inline void compat_close_memstream(FILE* f, char** buf, size_t* size) {
  long len = ftell(f);
  if (len < 0) {
    len = 0;
  }
  *buf = (char*)malloc((size_t)len + 1);
  rewind(f);
  *size = fread(*buf, 1, (size_t)len, f);
  (*buf)[*size] = '\0';
  fclose(f);
}

#else

static inline FILE* compat_open_memstream(char** buf, size_t* size) { return open_memstream(buf, size); }

static inline void compat_close_memstream(FILE* f, char** buf, size_t* size) {
  (void)buf;
  (void)size;
  fclose(f);
}

#endif

// Compiler command for LLVM IR compilation tests.
static inline const char* compat_llvm_cc(void) {
  const char* cc = getenv("LLVM_CC");
  if (cc) {
    return cc;
  }
#ifdef __APPLE__
  return "xcrun clang";
#else
  return "clang";
#endif
}
