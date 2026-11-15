// RR port: the few Windows names the vendored Shatter files use (they included <windows.h>).
#pragma once

#include <stdint.h>
#include <stdlib.h>  // <windows.h> brought it in (atof, atoi)
#include <time.h>

#include "guest.h"

#define __fastcall  // the vendored host functions are plain functions here; guest.h makes them __thiscall thunks

// Upstream probes guest pointers with these; here a guest pointer is valid when it lies in the guest mapping (the
// runtime does not track page protection).
inline bool IsBadReadPtr(const void* p, size_t) { return !shim::inGuestMemory(p); }
inline bool IsBadStringPtrA(const char* p, size_t) { return !shim::inGuestMemory(p); }

typedef union {
  struct {
    uint32_t LowPart;
    int32_t HighPart;
  } u;
  int64_t QuadPart;
} LARGE_INTEGER;

inline int QueryPerformanceFrequency(LARGE_INTEGER* f) {
  f->QuadPart = 1000000000;
  return 1;
}
inline int QueryPerformanceCounter(LARGE_INTEGER* t) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  t->QuadPart = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
  return 1;
}
