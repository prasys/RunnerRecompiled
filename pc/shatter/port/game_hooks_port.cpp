// Slide steering fix for the lifted MirrorsEdge.exe (RunnerRecompiled port).
//
// Derived from Shatter's shim/game_hooks.cpp (https://github.com/Arecsu/shatter, tag v1.0.7, commit bd59b4f, MIT, see
// ../LICENSE; (c) Arecsu). Only the slide part is taken (the algorithm, constants and comments of slideKeep); the
// developer movement/aim logger and the hardware-watchpoint code are not ported (see pc/shatter/UPSTREAM.md).
//
// The idea (upstream): steering on steep slide surfaces (the chapter 1 building slide) is applied by the current move's
// tick (0x01208D70) only while a pawn flag (+0x41E bit 0) is set. performPhysics (0x012B0960) clears it at the start of
// every step and the player's falling physics (0x012C0980, via 0x012BEB80) sets it again, with the surface normal at
// +0x71C, when that step touches a slide surface. At 62 fps the pawn touches the slope every frame; at high frame rates
// it floats a fraction of a unit above it between touches and steering works only on touch frames. After each top-level
// physics step: on a touching step, remove the small outward speed so the next step touches again; on a step without a
// touch, keep the flag on while the pawn is still falling, close to the plane it last touched, not moving away from it
// fast, and within a short time of that touch. At 62 fps every frame is a touch, so nothing changes.
//
// Port: upstream patches the four pawn vtables' performPhysics slot (0x3E8) in the exe's memory. Here the vtables are
// in guest memory (.rdata of the loaded image, read by the lifted code on every virtual call), so the same patch works:
// the slot gets a runtime thunk (SHATTER_THUNK) that runs hkPerformPhysics on the host, which calls the original lifted
// function with guestThiscall and then post-processes the pawn in guest memory. All four vtables hold the same function
// (0x012B0960), so one hook serves all of them (upstream needed four template instances only for the movelog).
//
// Off by default (it changes player movement): PXSHIM_SLIDEFIX=1. There is no shatter.ini here, only the environment.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "win_compat.h"

#include "shim.h"

namespace {

constexpr uint32_t kImageBase = 0x400000, kTimestamp = 0x4965FFF1;  // GOG 1.0.1.0 (SHATTER_GAME_EXE_TIMESTAMP upstream)

// Pawn classes sharing ATdPawn's movement code (TdPawn and its three native subclasses) [V upstream: slots equal].
constexpr uint32_t kPawnVtables[] = {0x01D05268, 0x01D05700, 0x01D05BC0, 0x01D29130};
constexpr uint32_t kSlotPerformPhysics = 0x3E8;
constexpr uint32_t kFnPerformPhysics = 0x012B0960;

uint32_t g_origPerformPhysics = 0;

const uint8_t* bytes(const void* p) { return static_cast<const uint8_t*>(p); }
float fieldF(const void* p, uint32_t off) {
  float f;
  memcpy(&f, bytes(p) + off, 4);
  return f;
}

constexpr float kSlideKeepTime = 0.25f;   // s of game time without a touch (300 fps: ~30 ms with the fix, 0.17 s before)
constexpr float kSlideKeepHeight = 4.0f;  // units off the slope plane (300 fps: 0.13 with the fix, 2 before)
// Away speed: a touch at high frame rates finds the pawn 0.05-0.06 units under the slope, pushes it out ~0.166 units and
// leaves it moving out at <= 50 units/s (upstream, 300 fps). A jump: hundreds.
constexpr float kSlideKeepAwaySpeed = 150;  // units/s away from the plane
constexpr float kPathStep = 10;             // units between stored touch locations
constexpr float kPathMin = 30;              // shortest touch path that corrects the normal
constexpr float kPathAge = 0.5f;            // s: older touch locations are dropped

struct SlideState {
  void* pawn;
  float airTime;     // game time since the last touch
  float clock;       // game time while tracked (touch path ages)
  float p[3], n[3];  // location and slope normal at the last touch
  float tp[8][4];    // touch path: locations at least kPathStep apart, with their clock, oldest first
  int tpCount;
  bool valid;
};
SlideState g_slide[16];

// Counters for the (rate-limited) log line; nothing here changes the behaviour.
struct Stats {
  uint32_t steps = 0;       // top-level pawn physics steps seen (all pawns)
  uint32_t touches = 0;     // steps after which the steering flag was set by the game (a slide touch)
  uint32_t corrections = 0; // touches where outward speed was removed
  double correctionSum = 0; // sum of the removed speeds (units/s)
  uint32_t kept = 0;        // steps where the fix kept the flag on
  uint64_t totalSteps = 0, totalTouches = 0, totalKept = 0;
} g_st;
double g_lastLog = 0, g_lastHeartbeat = 0;
bool g_seenFirst = false;

double hostNow() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

SlideState* slideState(void* pawn) {
  SlideState* free = nullptr;
  for (SlideState& s : g_slide) {
    if (s.pawn == pawn) return &s;
    if (!free && !s.valid) free = &s;
  }
  if (free) {
    *free = SlideState{};
    free->pawn = pawn;
  }
  return free;
}

// After a top-level performPhysics step of a pawn (guest memory through a host pointer).
void slideKeep(void* pawn, float dt) {
  uint8_t* base = const_cast<uint8_t*>(bytes(pawn));
  uint32_t flags;
  memcpy(&flags, base + 0x41C, 4);
  const bool falling = base[0x68] == 2;  // Physics byte: PHYS_Falling
  if (flags & 0x10000) {  // touched a slide surface in this step
    ++g_st.touches;
    SlideState* s = slideState(pawn);
    if (!s) return;
    if (!s->valid) s->tpCount = 0, s->clock = 0;
    s->valid = true, s->airTime = 0, s->clock += dt;
    float nc[3];
    for (int i = 0; i < 3; ++i) s->p[i] = fieldF(pawn, 0xE8 + 4 * i), nc[i] = fieldF(pawn, 0x71C + 4 * i);
    // Slope normal: the stored one is the collision normal, which at speed leans ~3 degrees toward the motion (upstream
    // measurement). Touch locations lie on the slope, so the path between them is in its plane: the lean along the
    // path is removed from the collision normal.
    while (s->tpCount > 0 && s->clock - s->tp[0][3] > kPathAge) {
      for (int k = 1; k < s->tpCount; ++k) memcpy(s->tp[k - 1], s->tp[k], sizeof s->tp[k]);
      --s->tpCount;
    }
    if (s->tpCount > 0) {
      const float* o = s->tp[0];
      float d[3], dd = 0;
      for (int i = 0; i < 3; ++i) d[i] = s->p[i] - o[i], dd += d[i] * d[i];
      if (dd >= kPathMin * kPathMin) {
        const float k = (nc[0] * d[0] + nc[1] * d[1] + nc[2] * d[2]) / dd;
        for (int i = 0; i < 3; ++i) nc[i] -= k * d[i];
      }
    }
    float m = sqrtf(nc[0] * nc[0] + nc[1] * nc[1] + nc[2] * nc[2]);
    m = m > 1e-6f ? 1 / m : 0;
    for (int i = 0; i < 3; ++i) s->n[i] = nc[i] * m;
    const float* last = s->tpCount ? s->tp[s->tpCount - 1] : nullptr;
    float step2 = 0;
    for (int i = 0; last && i < 3; ++i) step2 += (s->p[i] - last[i]) * (s->p[i] - last[i]);
    if (!last || step2 >= kPathStep * kPathStep) {
      if (s->tpCount == 8) {
        for (int k = 1; k < 8; ++k) memcpy(s->tp[k - 1], s->tp[k], sizeof s->tp[k]);
        --s->tpCount;
      }
      float* e = s->tp[s->tpCount++];
      e[0] = s->p[0], e[1] = s->p[1], e[2] = s->p[2], e[3] = s->clock;
    }
    // Stay on the surface: at high frame rates a touching step leaves the pawn moving off the slope; at 62 fps it
    // leaves at about -1, pressed into it. Removing the small outward part keeps every frame a touch, as at 62. A jump
    // (outward speed above kSlideKeepAwaySpeed) is left alone.
    float v[3];
    memcpy(v, base + 0x100, sizeof v);
    const float vn = v[0] * s->n[0] + v[1] * s->n[1] + v[2] * s->n[2];
    if (falling && vn > 0 && vn <= kSlideKeepAwaySpeed) {
      for (int i = 0; i < 3; ++i) v[i] -= vn * s->n[i];
      memcpy(base + 0x100, v, sizeof v);
      ++g_st.corrections;
      g_st.correctionSum += vn;
    }
    return;
  }
  SlideState* s = nullptr;
  for (SlideState& e : g_slide)
    if (e.pawn == pawn && e.valid) s = &e;
  if (!s) return;
  s->airTime += dt, s->clock += dt;
  float h = 0, vn = 0;  // height above the slope plane at the last touch, speed away from it
  for (int i = 0; i < 3; ++i)
    h += (fieldF(pawn, 0xE8 + 4 * i) - s->p[i]) * s->n[i], vn += fieldF(pawn, 0x100 + 4 * i) * s->n[i];
  // Not leaving the plane: a jump off the slide (hundreds of units/s along the normal) must not be pulled back into it,
  // since TdPawn's tick 0x012BED30 forces the slide move (0x26) while the flag is on.
  const bool sliding = vn <= kSlideKeepAwaySpeed;
  if (!falling || !sliding || s->airTime > kSlideKeepTime || h > kSlideKeepHeight || h < -kSlideKeepHeight) {
    s->valid = false;
    return;
  }
  flags |= 0x10000;  // the normal at +0x71C still holds the last touch's
  memcpy(base + 0x41C, &flags, 4);
  ++g_st.kept;
}

void logRateLimited() {
  const double t = hostNow();
  if (!g_seenFirst) {
    g_seenFirst = true;
    g_lastLog = t;
    rt_log("slidefix: hook running (first top-level performPhysics step seen)");
    return;
  }
  if (t - g_lastLog < 1.0) return;
  // Only report seconds in which the fix did something (slides are rare; every pawn's steps would flood the log).
  if (g_st.touches || g_st.kept) {
    rt_log("slidefix: last %.1fs: %u steps, %u slide touches (%u with outward speed removed, mean %.1f u/s), flag kept on "
           "%u times; total %llu touches, %llu kept",
           t - g_lastLog, g_st.steps, g_st.touches, g_st.corrections,
           g_st.corrections ? g_st.correctionSum / g_st.corrections : 0.0, g_st.kept,
           (unsigned long long)g_st.totalTouches, (unsigned long long)g_st.totalKept);
  }
  else if (t - g_lastHeartbeat >= 10.0) {  // proof of life while nothing slides
    g_lastHeartbeat = t;
    rt_log("slidefix: %llu pawn physics steps so far, %llu slide touches, flag kept on %llu times",
           (unsigned long long)g_st.totalSteps, (unsigned long long)g_st.totalTouches,
           (unsigned long long)g_st.totalKept);
  }
  g_lastLog = t;
  g_st.steps = g_st.touches = g_st.corrections = g_st.kept = 0;
  g_st.correctionSum = 0;
}

// ATdPawn::performPhysics(float dt, int iterations) (thiscall, callee pops 8). iterations is 0 for the step the engine
// asks for and > 0 for substeps it makes inside it: only the outer one is post-processed.
void hkPerformPhysics(void* self, void*, float dt, int iterations) {
  shim::guestThiscall<void>(g_origPerformPhysics, shim::ptr32(self), dt, iterations);
  if (iterations != 0) return;
  ++g_st.steps, ++g_st.totalSteps;
  const uint32_t beforeTouch = g_st.touches, beforeKept = g_st.kept;
  slideKeep(self, dt);
  g_st.totalTouches += g_st.touches - beforeTouch;
  g_st.totalKept += g_st.kept - beforeKept;
  logRateLimited();
}

}  // namespace

namespace shim {

void gameHooksInit() {
  if (env("PXSHIM_SLIDEFIX")[0] != '1') return;
  // The runtime places the exe at its preferred base (it refuses anything else), so exe addresses are guest addresses.
  if ((ld32(kImageBase) & 0xFFFF) != 0x5A4D) {  // "MZ"
    rt_log("slidefix: no image at %#x: slide steering fix off", kImageBase);
    return;
  }
  const uint32_t ts = ld32(kImageBase + ld32(kImageBase + 0x3C) + 8);
  if (ts != kTimestamp) {
    rt_log("slidefix: exe timestamp %08X, expected %08X: slide steering fix off", (unsigned)ts, (unsigned)kTimestamp);
    return;
  }
  for (uint32_t vt : kPawnVtables)
    if (ld32(vt + kSlotPerformPhysics) != kFnPerformPhysics) {
      rt_log("slidefix: pawn vtable %08X does not hold the expected performPhysics (%08X): not hooked", (unsigned)vt,
             (unsigned)ld32(vt + kSlotPerformPhysics));
      return;
    }
  const uint32_t thunk = SHATTER_THUNK(hkPerformPhysics);
  g_origPerformPhysics = kFnPerformPhysics;
  uint32_t o = 0;
  for (uint32_t vt : kPawnVtables) patchSlot(vt, kSlotPerformPhysics / 4, thunk, &o);
  rt_log("slidefix: on. performPhysics %08X hooked in %d pawn vtables (thunk %08X)", kFnPerformPhysics,
         (int)(sizeof kPawnVtables / sizeof kPawnVtables[0]), (unsigned)thunk);
}

void gameHooksShutdown() {
  if (g_st.totalSteps)
    rt_log("slidefix: at exit %llu pawn steps, %llu slide touches, flag kept on %llu times",
           (unsigned long long)g_st.totalSteps, (unsigned long long)g_st.totalTouches,
           (unsigned long long)g_st.totalKept);
}

}  // namespace shim

// Called by rt_main.c once the game image is loaded (the vtables are in guest memory from then on).
extern "C" void rt_shatter_game_ready(void) { shim::gameHooksInit(); }
