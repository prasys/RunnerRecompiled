// Shatter on the lifted game (RunnerRecompiled port): what upstream's PhysXLoader.cpp and fluid_wrap.cpp do, for a
// PhysX core that is lifted guest code.
//
// Upstream replaces PhysXLoader.dll; here the lifted original loader stays and the runtime's export hook hands the game
// our NxCreatePhysicsSDK, which calls the original and then patches the core's NxPhysicsSDK and NxScene vtables in
// guest memory (the lifted code reads vtables from memory and dispatches thunk addresses to the host). From there on
// it is upstream's design: createFluid returns a ShimFluid (in guest memory, vtable of thunks to fluid_ours.cpp),
// simulate reads the scene and starts the fluid steps, fetchResults waits for them and writes the game's buffers.
//
// Switches (environment): ME_PC_SHATTER=0 leaves PhysX alone (the lifted original runs the effects). Upstream's
// PXSHIM_* developer switches work as before (PXSHIM_FLUID=ours|off, PXSHIM_THREADS, PXSHIM_REF_HZ, ...);
// PXSHIM_DEBUG=1 sends upstream's detailed log lines to the runtime log; PXSHIM_CLOTH=0 leaves NxCloth to the lifted
// core. The pass-through counting wrappers and the fluid trace (debug tools of upstream) are not ported.

#include "win_compat.h"

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>
#include <vector>

#include "cloth_ours.h"
#include "coverage.h"
#include "fluid_internal.h"
#include "shim.h"

namespace shim {

char g_dir[260] = "";

const char* env(const char* name) {
  const char* v = getenv(name);
  return v ? v : "";
}

bool debug() {
  static const bool on = env("PXSHIM_DEBUG")[0] == '1';
  return on;
}

bool cpuOnly() { return true; }  // there is no PhysX hardware for the guest (rt_physx_support.c)

void logLine(const char* fmt, ...) {
  if (!debug()) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  rt_log("shatter: %s", buf);
}

// Always logged (a few lines per level).
static void note(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  rt_log("shatter: %s", buf);
}

bool dumpWords(const char* tag, const void* p, size_t bytes) {
  if (!debug() || !p) return false;
  const uint32_t* d = reinterpret_cast<const uint32_t*>(p);
  for (size_t i = 0; i < bytes / 4; i += 8) {
    char line[160];
    int n = snprintf(line, sizeof(line), "  %s+%03X:", tag, (unsigned)(i * 4));
    for (size_t j = i; j < i + 8 && j < bytes / 4; ++j) n += snprintf(line + n, sizeof(line) - n, " %08X", d[j]);
    logLine("%s", line);
  }
  return true;
}

bool patchSlot(uint32_t vt, uint32_t slot, uint32_t value, uint32_t* orig) {
  *orig = ld32(vt + 4 * slot);
  st32(vt + 4 * slot, value);
  logLine("patch vt=%08X slot %u: %08X -> %08X", vt, slot, *orig, value);
  return true;
}

float stepRefDt() {
  static const float refDt = [] {
    const char* s = env("PXSHIM_REF_HZ");
    const float hz = s[0] ? (float)atof(s) : 62.0f;
    return hz > 0 ? 1.0f / hz : 0.0f;
  }();
  return refDt;
}

float clothRefDt() {
  static const float refDt = [] {
    const char* s = env("PXSHIM_CLOTH_REF_HZ");
    const float hz = s[0] ? (float)atof(s) : 62.0f;
    return hz > 0 && stepRefDt() > 0 ? 1.0f / hz : 0.0f;
  }();
  return refDt;
}

void gameHooksInit() {}
void gameHooksShutdown() {}

// ---- fluid_wrap.cpp (the parts the hosted fluids need) ----
namespace {

const uint32_t kFluidSlots = 81;
const char* const kFluidNames[kFluidSlots] = {
    "~NxFluid", "createEmitter", "releaseEmitter", "getNbEmitters", "getEmitters", "addParticles",
    "removeAllParticles", "setParticlesWriteData", "getParticlesWriteData", "setParticleDeletionIdWriteData",
    "getParticleDeletionIdWriteData", "setParticleCreationIdWriteData", "getParticleCreationIdWriteData",
    "getNumReserveParticles", "setNumReserveParticles", "getCurrentParticleLimit", "setCurrentParticleLimit",
    "updateParticles", "setFluidPacketData", "getFluidPacketData", "getSimulationMethod", "setSimulationMethod",
    "getStiffness", "setStiffness", "getViscosity", "setViscosity", "getSurfaceTension", "setSurfaceTension",
    "getDamping", "setDamping", "getFadeInTime", "setFadeInTime", "getExternalAcceleration",
    "setExternalAcceleration", "getProjectionPlane", "setProjectionPlane", "getCollisionMethod",
    "setCollisionMethod", "getRestitutionForStaticShapes", "setRestitutionForStaticShapes",
    "getDynamicFrictionForStaticShapes", "setDynamicFrictionForStaticShapes", "getStaticFrictionForStaticShapes",
    "setStaticFrictionForStaticShapes", "getAttractionForStaticShapes", "setAttractionForStaticShapes",
    "getRestitutionForDynamicShapes", "setRestitutionForDynamicShapes", "getDynamicFrictionForDynamicShapes",
    "setDynamicFrictionForDynamicShapes", "getStaticFrictionForDynamicShapes", "setStaticFrictionForDynamicShapes",
    "getAttractionForDynamicShapes", "setAttractionForDynamicShapes", "setCollisionResponseCoefficient",
    "getCollisionResponseCoefficient", "setFlag", "getFlag", "getScene", "getMaxParticles",
    "getKernelRadiusMultiplier", "getMotionLimitMultiplier", "getCollisionDistanceMultiplier",
    "getPacketSizeMultiplier", "getRestParticlesPerMeter", "getRestDensity", "getRestParticleDistance",
    "getParticleMass", "setGroup", "getGroup", "setGroupsMask", "getGroupsMask", "getWorldBounds", "loadFromDesc",
    "saveToDesc", "getPPUTime", "setName", "getName", "getCompartment", "getForceFieldMaterial",
    "setForceFieldMaterial"};

std::vector<ShimFluid*> g_fluids;  // live hosted fluids
uint32_t g_nextIndex = 0, g_fluidsCreated = 0, g_fluidsRefused = 0;
uint32_t g_fluidCount[kFluidSlots];

ShimFluid* asShim(uint32_t p) {
  for (ShimFluid* f : g_fluids)
    if (ptr32(f) == p) return f;
  return nullptr;
}

void freeShim(ShimFluid* f) {
  f->magic = 0;
  f->~ShimFluid();
  rt_heap_free(ptr32(f));
}

}  // namespace

void countFluidCall(uint32_t slot, const ShimFluid* f, const char* fmt, ...) {
  const uint32_t n = __atomic_add_fetch(&g_fluidCount[slot], 1, __ATOMIC_RELAXED);
  if (!debug() || (n > 12 && (n & (n - 1)) != 0)) return;
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  logLine("NxFluid::%s #%u (fluid %u%s) %s", kFluidNames[slot], n, f->index, f->hosted ? ", ours" : "", msg);
}

// Upstream's trace file is not ported; these keep the per-slot call counts.
void traceAdd(const ShimFluid* f, const ParticleData* pd, uint32_t appendIds, uint32_t r) {
  const uint32_t req = pd && pd->numPtr ? *fromPtr32<const uint32_t>(pd->numPtr) : 0;
  countFluidCall(5, f, "n=%u appendIds=%u -> %u", req, appendIds & 0xff, r);
}
void traceRemoveAll(const ShimFluid* f) { countFluidCall(6, f, ""); }
void traceUpdate(const ShimFluid* f, const UpdateData* ud) {
  if (ud) countFluidCall(17, f, "mode=%u n=%u", ud->forceMode, ud->num);
}
void traceBounds(const ShimFluid* f, const float* b) {
  countFluidCall(72, f, "-> (%g %g %g)-(%g %g %g)", (double)b[0], (double)b[1], (double)b[2], (double)b[3],
                 (double)b[4], (double)b[5]);
}

void fluidInit() {}

uint32_t fluidCreate(void* scene, const void* desc, FnCreateFluid orig) {
  const uint32_t* d = static_cast<const uint32_t*>(desc);
  dumpWords("desc", desc, kDescDwords * 4);
  if (!hostWants(d)) return orig(scene, nullptr, desc);  // the core's own fluid (no counting wrapper in this port)
  const uint32_t mem = rt_heap_alloc(sizeof(ShimFluid), 1);
  if (!mem || (mem & 7)) rt_fatal("shatter: guest allocation for a fluid failed (%#x)", mem);
  ShimFluid* f = new (fromPtr32<void>(mem)) ShimFluid();
  f->vtbl = hostVtable();
  f->userData = d[kDescUserData];
  f->magic = kMagic;
  f->real = 0;
  f->scene = ptr32(scene);
  f->index = g_nextIndex++;
  memcpy(&f->write, d + kDescWriteData, sizeof(f->write));
  memcpy(&f->deletionIds, d + kDescDeletionIds, sizeof(f->deletionIds));
  memcpy(&f->creationIds, d + kDescCreationIds, sizeof(f->creationIds));
  memcpy(&f->packets, d + kDescPackets, sizeof(f->packets));
  f->hosted = hostCreate(f, d);
  if (!f->hosted) {
    ++g_fluidsRefused;
    note("createFluid: our fluid refused the descriptor (returned null, as the core does)");
    freeShim(f);
    return 0;
  }
  g_fluids.push_back(f);
  ++g_fluidsCreated;
  logLine("< NxScene::createFluid -> %08X (fluid %u)", ptr32(f), f->index);
  return ptr32(f);
}

void fluidRelease(void* scene, void* fluid, FnReleaseFluid orig) {
  ShimFluid* f = asShim(ptr32(fluid));
  if (!f) {
    orig(scene, nullptr, fluid);
    return;
  }
  hostDestroy(f);
  for (size_t i = 0; i < g_fluids.size(); ++i)
    if (g_fluids[i] == f) g_fluids.erase(g_fluids.begin() + i);
  freeShim(f);
}

void fluidOnSceneRelease(void* scene) {
  hostSceneReleased(ptr32(scene));
  for (size_t i = 0; i < g_fluids.size();) {
    if (g_fluids[i]->scene == ptr32(scene)) {
      logLine("scene %08X released with fluid %u still alive", ptr32(scene), g_fluids[i]->index);
      hostDestroy(g_fluids[i]);
      freeShim(g_fluids[i]);
      g_fluids.erase(g_fluids.begin() + i);
    } else {
      ++i;
    }
  }
}

void fluidOnSimulate(void* scene, float elapsed) {
  std::vector<ShimFluid*> hosted;
  for (ShimFluid* f : g_fluids)
    if (f->scene == ptr32(scene)) hosted.push_back(f);
  hostSimulateScene(ptr32(scene), hosted, elapsed);
}

void fluidOnFetched(void* scene, uint32_t, bool ok) {
  if (ok)
    for (ShimFluid* f : g_fluids)
      if (f->scene == ptr32(scene)) hostFetched(f);
}

void fluidLogSummary(const char* why) {
  logLine("fluid slot summary (%s): %u live", why, (unsigned)g_fluids.size());
  for (uint32_t i = 0; i < kFluidSlots; ++i)
    if (g_fluidCount[i]) logLine("  NxFluid[%2u] %-34s %u", i, kFluidNames[i], g_fluidCount[i]);
}

const char* fluidStatsLine() {
  static char buf[1024];
  uint32_t particles = 0, shapes = 0;
  for (const ShimFluid* f : g_fluids) hostCounts(f, &particles, &shapes);
  const int n = snprintf(buf, sizeof(buf), "%u fluids, %u particles, %u shapes near them; ", (unsigned)g_fluids.size(),
                         particles, shapes);
  hostGeometryStats(buf + n, sizeof(buf) - n);
  return buf;
}

void fluidShutdown() {}

}  // namespace shim

// ---- cloth_wrap.cpp: NxScene createCloth/releaseCloth, NxPhysicsSDK createClothMesh (the cooked-stream tap) ----
using namespace shim;

namespace {

const uint32_t kSceneCreateCloth = 131, kSceneReleaseCloth = 132;
const uint32_t kSdkCreateClothMesh = 21;
const uint32_t kClfHardwareBit = 1u << 13;  // NX_CLF_HARDWARE
const uint32_t kClothDescBytes = 280, kOffClothFlags = 264, kOffClothCompartment = 276;

GuestThis<uint32_t(const void* desc)> g_origCreateCloth;
GuestThis<void(void* cloth)> g_origReleaseCloth;
GuestThis<uint32_t(void* stream)> g_origCreateClothMesh;
uint32_t g_clothsCreated = 0, g_clothsToCore = 0;

uint32_t hkCreateCloth(void* self, void*, const void* desc) {
  const uint8_t* d8 = static_cast<const uint8_t*>(desc);
  GuestScratch g;  // a copy without NX_CLF_HARDWARE and the compartment (upstream cpuOnly), in guest memory
  if (d8 && (ld32(ptr32(d8) + kOffClothFlags) & kClfHardwareBit || ld32(ptr32(d8) + kOffClothCompartment))) {
    const uint32_t copy = g.alloc(kClothDescBytes);
    memcpy(fromPtr32<void>(copy), desc, kClothDescBytes);
    st32(copy + kOffClothFlags, ld32(copy + kOffClothFlags) & ~kClfHardwareBit);
    st32(copy + kOffClothCompartment, 0);
    desc = fromPtr32<void>(copy);
  }
  if (oursEnabled()) {
    const uint32_t c = oursCreate(self, desc);
    if (c) {
      ++g_clothsCreated;
      return c;
    }
  }
  ++g_clothsToCore;
  return g_origCreateCloth(self, nullptr, desc);
}

void hkReleaseCloth(void* self, void*, void* cloth) {
  if (oursOwns(ptr32(cloth))) {
    oursRelease(ptr32(cloth));
    return;
  }
  g_origReleaseCloth(self, nullptr, cloth);
}

// The cooked-stream tap: the core reads the game's NxStream through a guest object of ours whose 13 slots
// (~NxStream, readByte..readBuffer, storeByte..storeBuffer) call the game's stream and record what was read
// (upstream StreamTap). createClothMesh is not re-entered, so one tap at a time.
uint32_t g_tapObj = 0, g_tapInner = 0;
std::vector<uint8_t> g_tapRec;

uint32_t tapForward(X86Ctx* c, uint32_t slot, int nargs) {
  uint32_t args[3] = {0, 0, 0};
  for (int i = 0; i < nargs; ++i) args[i] = rt_arg(c, i);
  c->ecx = g_tapInner;
  return rt_call(c, ld32(ld32(g_tapInner) + 4 * slot), nargs, args);
}
void tapAdd(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  g_tapRec.insert(g_tapRec.end(), b, b + n);
}
void tapDtor(X86Ctx* c) { c->eax = g_tapObj; }
template <uint32_t kSlot, uint32_t kBytes>
void tapReadInt(X86Ctx* c) {  // readByte / readWord / readDword: AL / AX / EAX
  const uint32_t v = tapForward(c, kSlot, 0);
  tapAdd(&v, kBytes);
}
void tapReadFloat(X86Ctx* c) {  // ST(0) stays for the caller
  tapForward(c, 4, 0);
  const float f = (float)c->st[c->ftop & 7];
  tapAdd(&f, 4);
}
void tapReadDouble(X86Ctx* c) {
  tapForward(c, 5, 0);
  const double v = c->st[c->ftop & 7];
  tapAdd(&v, 8);
}
void tapReadBuffer(X86Ctx* c) {
  const uint32_t buf = rt_arg(c, 0), size = rt_arg(c, 1);
  tapForward(c, 6, 2);
  if (buf && size) tapAdd(fromPtr32<void>(buf), size);
}
template <uint32_t kSlot, int kArgs>
void tapStore(X86Ctx* c) {  // store*: forwarded; they return NxStream& (this)
  tapForward(c, kSlot, kArgs);
  c->eax = g_tapObj;
}

uint32_t tapObject() {
  static uint32_t obj = [] {
    const uint32_t fns[13] = {
        rt_hle_thunk_fn("shatter", "tap_dtor", tapDtor, 1),
        rt_hle_thunk_fn("shatter", "tap_readByte", tapReadInt<1, 1>, 0),
        rt_hle_thunk_fn("shatter", "tap_readWord", tapReadInt<2, 2>, 0),
        rt_hle_thunk_fn("shatter", "tap_readDword", tapReadInt<3, 4>, 0),
        rt_hle_thunk_fn("shatter", "tap_readFloat", tapReadFloat, 0),
        rt_hle_thunk_fn("shatter", "tap_readDouble", tapReadDouble, 0),
        rt_hle_thunk_fn("shatter", "tap_readBuffer", tapReadBuffer, 2),
        rt_hle_thunk_fn("shatter", "tap_storeByte", tapStore<7, 1>, 1),
        rt_hle_thunk_fn("shatter", "tap_storeWord", tapStore<8, 1>, 1),
        rt_hle_thunk_fn("shatter", "tap_storeDword", tapStore<9, 1>, 1),
        rt_hle_thunk_fn("shatter", "tap_storeFloat", tapStore<10, 1>, 1),
        rt_hle_thunk_fn("shatter", "tap_storeDouble", tapStore<11, 2>, 2),
        rt_hle_thunk_fn("shatter", "tap_storeBuffer", tapStore<12, 2>, 2)};
    const uint32_t vt = rt_heap_alloc(sizeof fns, 1);
    memcpy(fromPtr32<void>(vt), fns, sizeof fns);
    const uint32_t o = rt_heap_alloc(8, 1);
    st32(o, vt);
    return o;
  }();
  return obj;
}

uint32_t hkCreateClothMesh(void* self, void*, void* stream) {
  if (!oursEnabled() || !stream) return g_origCreateClothMesh(self, nullptr, stream);
  g_tapObj = tapObject();
  g_tapInner = ptr32(stream);
  g_tapRec.clear();
  const uint32_t r = g_origCreateClothMesh(self, nullptr, fromPtr32<void>(g_tapObj));
  g_tapInner = 0;
  if (r) oursRegisterCookedMesh(r, g_tapRec.data(), g_tapRec.size());
  logLine("NxPhysicsSDK::createClothMesh -> %08X (%zu cooked bytes)", r, g_tapRec.size());
  return r;
}

bool clothOn() {
  static const bool on = [] {
    const char* m = env("PXSHIM_CLOTH");
    return strcmp(m, "off") != 0 && strcmp(m, "0") != 0;
  }();
  return on;
}

void clothInstallScene(uint32_t vt) {
  if (!clothOn()) return;
  uint32_t o = 0;
  if (patchSlot(vt, kSceneCreateCloth, SHATTER_THUNK(hkCreateCloth), &o)) g_origCreateCloth = decltype(g_origCreateCloth)(o);
  if (patchSlot(vt, kSceneReleaseCloth, SHATTER_THUNK(hkReleaseCloth), &o))
    g_origReleaseCloth = decltype(g_origReleaseCloth)(o);
}

void clothInstallSdk(uint32_t vt) {
  if (!clothOn() || !strcmp(env("PXSHIM_CLOTH_MESH"), "0")) return;
  uint32_t o = 0;
  if (patchSlot(vt, kSdkCreateClothMesh, SHATTER_THUNK(hkCreateClothMesh), &o))
    g_origCreateClothMesh = decltype(g_origCreateClothMesh)(o);
}

}  // namespace

// ---- PhysXLoader.cpp: SDK and scene hooks ----
namespace {

const uint32_t kSdkCreateScene = 4, kSdkReleaseScene = 5;
const uint32_t kSdkGetHWVersion = 31, kSdkGetNbPPUs = 32;
const uint32_t kSceneCreateFluid = 126, kSceneReleaseFluid = 127, kSceneSimulate = 140, kSceneFetchResults = 142;
const uint32_t kSceneSlotsSame = 153, kSdkSlotsChecked = 33;

GuestThis<uint32_t(const void* desc)> g_origCreateScene;
GuestThis<void(void* scene)> g_origReleaseScene;
FnCreateFluid g_origCreateFluid;
FnReleaseFluid g_origReleaseFluid;
GuestThis<void(float elapsed)> g_origSimulate;
GuestThis<bool(uint32_t status, uint32_t block, uint32_t errorState)> g_origFetchResults;

uint32_t g_origCreateSdk = 0;
uint32_t g_sdkVt = 0, g_sceneVt = 0;
bool g_passThrough = false;
uint32_t g_simCalls = 0, g_fetchCalls = 0;

struct FrameStats {
  double core = 0, ours = 0, coreMax = 0, oursMax = 0, frameCore = 0, frameOurs = 0;
  double wall = 0, wallMax = 0;
  uint32_t frames = 0;
} g_stats;
double g_lastFetchMs = 0;

double nowMs() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / 1e6;
}

// The vtable and its first `slots` entries all lie in the lifted physxcore.dll (upstream: the core's own module and a
// 2.8.0/2.8.1 version resource; the lifted core here is the game's 2.8.0).
bool vtableInCore(uint32_t vt, uint32_t slots) {
  for (uint32_t i = 0; i <= slots; ++i) {
    const uint32_t a = i == slots ? vt : ld32(vt + 4 * i);
    RtModule* m = rt_module_at(a);
    if (!m || strcmp(m->name, "physxcore.dll") != 0) return false;
  }
  return true;
}

uint32_t hkCreateFluid(void* self, void* edx, const void* desc);
void hkReleaseFluid(void* self, void* edx, void* fluid);
void hkSimulate(void* self, void* edx, float elapsed);
bool hkFetchResults(void* self, void* edx, uint32_t status, uint32_t block, uint32_t errorState);
uint32_t hkCreateScene(void* self, void* edx, const void* desc);
void hkReleaseScene(void* self, void* edx, void* scene);
uint32_t hkNoHardware(void* self, void* edx);

void installSceneHooks(uint32_t scene) {
  const uint32_t vt = ld32(scene);
  if (!vt || g_passThrough) return;
  if (g_sceneVt) {
    if (vt != g_sceneVt) note("warning: second NxScene vtable %08X ignored (first %08X)", vt, g_sceneVt);
    return;
  }
  g_sceneVt = vt;
  if (!vtableInCore(vt, kSceneSlotsSame)) {
    g_passThrough = true;
    note("NxScene vtable %08X is not the lifted 2.8.0 core's: pass-through (fluids stay on the core)", vt);
    return;
  }
  uint32_t o = 0;
  if (patchSlot(vt, kSceneCreateFluid, SHATTER_THUNK(hkCreateFluid), &o)) g_origCreateFluid = FnCreateFluid(o);
  if (patchSlot(vt, kSceneReleaseFluid, SHATTER_THUNK(hkReleaseFluid), &o)) g_origReleaseFluid = FnReleaseFluid(o);
  if (patchSlot(vt, kSceneSimulate, SHATTER_THUNK(hkSimulate), &o)) g_origSimulate = decltype(g_origSimulate)(o);
  if (patchSlot(vt, kSceneFetchResults, SHATTER_THUNK(hkFetchResults), &o))
    g_origFetchResults = decltype(g_origFetchResults)(o);
  clothInstallScene(vt);
  note("NxScene vtable %08X hooked: createFluid, releaseFluid, simulate, fetchResults%s (native solver: fluids %s, "
       "cloth %s)", vt, clothOn() ? ", createCloth, releaseCloth" : "", env("PXSHIM_FLUID")[0] ? env("PXSHIM_FLUID") : "all",
       clothOn() ? "on" : "off");
}

void logFrameCost() {
  const uint32_t n = g_stats.frames;
  if (!n) return;
  char cloth[512];
  oursStatsLine(cloth, sizeof cloth);
  if (!g_fluids.empty() || g_clothsCreated || debug())
    note("frame cost over %u frames: game frame avg %.2f ms max %.2f | core avg %.2f ms max %.2f | hosted fluids and cloths "
         "on the game thread avg %.2f ms max %.2f | %s | cloths: %u created on the native solver, %u left to the core; %s",
         n, g_stats.wall / n, g_stats.wallMax, g_stats.core / n, g_stats.coreMax, g_stats.ours / n, g_stats.oursMax,
         fluidStatsLine(), g_clothsCreated, g_clothsToCore, cloth);
  g_stats = FrameStats();
}

uint32_t hkCreateFluid(void* self, void*, const void* desc) {
  const uint32_t* d = static_cast<const uint32_t*>(desc);
  if (d && cpuOnly() && ((d[0x4f] & 0x10) || d[0x52])) {
    GuestScratch g;  // a copy without the hardware flag and compartment, in guest memory (the core may get it)
    const uint32_t copy = g.alloc(kDescDwords * 4);
    uint32_t* c = fromPtr32<uint32_t>(copy);
    memcpy(c, d, kDescDwords * 4);
    logLine("cpu only: fluid desc flags %08X compartment %08X -> software", c[0x4f], c[0x52]);
    c[0x4f] &= ~0x10u;
    c[0x52] = 0;
    return fluidCreate(self, c, g_origCreateFluid);
  }
  return fluidCreate(self, desc, g_origCreateFluid);
}

void hkReleaseFluid(void* self, void*, void* fluid) { fluidRelease(self, fluid, g_origReleaseFluid); }

void hkSimulate(void* self, void*, float elapsed) {
  ++g_simCalls;
  const double t0 = nowMs();
  fluidOnSimulate(self, elapsed);
  oursOnSimulate(self, elapsed);  // hosted cloths
  const double t1 = nowMs();
  g_origSimulate(self, nullptr, elapsed);
  g_stats.frameOurs += t1 - t0;
  g_stats.frameCore += nowMs() - t1;
}

bool hkFetchResults(void* self, void*, uint32_t status, uint32_t block, uint32_t errorState) {
  ++g_fetchCalls;
  const double t0 = nowMs();
  const bool r = g_origFetchResults(self, nullptr, status, block, errorState);
  const double t1 = nowMs();
  fluidOnFetched(self, status, r);
  oursOnFetched(self, r);
  g_stats.frameCore += t1 - t0;
  g_stats.frameOurs += nowMs() - t1;
  if (r) {
    g_stats.core += g_stats.frameCore, g_stats.ours += g_stats.frameOurs;
    if (g_stats.frameCore > g_stats.coreMax) g_stats.coreMax = g_stats.frameCore;
    if (g_stats.frameOurs > g_stats.oursMax) g_stats.oursMax = g_stats.frameOurs;
    g_stats.frameCore = g_stats.frameOurs = 0;
    const double now = nowMs();
    if (g_lastFetchMs > 0) {
      g_stats.wall += now - g_lastFetchMs;
      if (now - g_lastFetchMs > g_stats.wallMax) g_stats.wallMax = now - g_lastFetchMs;
    }
    g_lastFetchMs = now;
    if (++g_stats.frames == 600) logFrameCost();
  }
  return r;
}

uint32_t hkNoHardware(void*, void*) { return 0; }  // NX_HW_VERSION_NONE / no PPUs

void installSdkHooks(uint32_t sdk) {
  if (!sdk) return;
  const uint32_t vt = ld32(sdk);
  if (!vt) return;
  if (g_sdkVt) {
    if (vt != g_sdkVt) note("warning: second NxPhysicsSDK vtable %08X ignored (first %08X)", vt, g_sdkVt);
    return;
  }
  g_sdkVt = vt;
  if (!vtableInCore(vt, kSdkSlotsChecked)) {
    g_passThrough = true;
    note("NxPhysicsSDK vtable %08X is not the lifted core's: pass-through", vt);
    return;
  }
  uint32_t o = 0;
  if (patchSlot(vt, kSdkCreateScene, SHATTER_THUNK(hkCreateScene), &o)) g_origCreateScene = decltype(g_origCreateScene)(o);
  if (patchSlot(vt, kSdkReleaseScene, SHATTER_THUNK(hkReleaseScene), &o))
    g_origReleaseScene = decltype(g_origReleaseScene)(o);
  clothInstallSdk(vt);
  patchSlot(vt, kSdkGetHWVersion, SHATTER_THUNK(hkNoHardware), &o);
  patchSlot(vt, kSdkGetNbPPUs, SHATTER_THUNK(hkNoHardware), &o);
  note("NxPhysicsSDK %08X (vtable %08X) hooked", sdk, vt);
}

uint32_t hkCreateScene(void* self, void*, const void* desc) {
  const uint32_t r = g_origCreateScene(self, nullptr, desc);
  logLine("NxPhysicsSDK::createScene -> %08X", r);
  if (r) installSceneHooks(r);
  return r;
}

void hkReleaseScene(void* self, void*, void* scene) {
  logFrameCost();
  fluidLogSummary("releaseScene");
  covLog("releaseScene", true);
  if (g_fluidsCreated || g_fluidsRefused)
    note("scene %08X released: %u fluids created on the native solver so far (%u refused), %u simulate calls", ptr32(scene),
         g_fluidsCreated, g_fluidsRefused, g_simCalls);
  oursLogSummary("releaseScene");
  fluidOnSceneRelease(scene);
  oursOnSceneRelease(scene);
  g_origReleaseScene(self, nullptr, scene);
}

// NxCreatePhysicsSDK(version, allocator, outputStream, desc, errorCode*), __cdecl: the original, then the hooks.
uint32_t hkNxCreatePhysicsSDK(uint32_t version, uint32_t alloc, uint32_t out, uint32_t desc, uint32_t err) {
  const uint32_t args[5] = {version, alloc, out, desc, err};
  const uint32_t sdk = rt_call(guestCtx(), g_origCreateSdk, 5, args);
  note("NxCreatePhysicsSDK(%#x) -> %08X", version, sdk);
  installSdkHooks(sdk);
  return sdk;
}

uint32_t exportHook(const char* module, const char* name, uint32_t addr) {
  if (strcmp(module, "physxloader.dll") != 0) return addr;
  if (!strcmp(name, "NxCreatePhysicsSDK")) {
    g_origCreateSdk = addr;
    return SHATTER_CDECL(hkNxCreatePhysicsSDK);
  }
  if (!strcmp(name, "NxCreatePhysicsSDKWithID")) note("warning: the game asked for NxCreatePhysicsSDKWithID (not hooked)");
  return addr;
}

}  // namespace

extern "C" void rt_shatter_register(void) {
  const char* on = getenv("ME_PC_SHATTER");
  if (on && on[0] == '0') {
    rt_log("shatter: off (ME_PC_SHATTER=0): PhysX fluids run on the lifted core");
    return;
  }
  oursInit();
  rt_set_export_hook(exportHook);
}
