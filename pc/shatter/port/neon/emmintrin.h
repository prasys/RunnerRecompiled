// RR port: SSE intrinsics on arm64 through sse2neon (NEON). Results match SSE2 bit for bit with -ffp-contract=off
// (all of Shatter's fluid and cloth scenarios compared against an x86-64 SSE2 build).
#pragma once
#include "../../third_party/sse2neon.h"
