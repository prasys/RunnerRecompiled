/* Guest->host Direct3D 9 bridge: proxies, guest vtables and the methods that cannot be generated
   (pointer-carrying structs, Lock/Unlock, IUnknown). See rt_d3d9.h for the proxy layout. */
#include "rt_d3d9.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RET(v) (ctx->eax = (uint32_t)(v))
#define D3DERR_INVALIDCALL 0x8876086Cu
#define E_NOINTERFACE 0x80004002u

void *host_d3d9_create(uint32_t sdk_version);   /* rt_host_*.c */
void *rt_user32_host_hwnd(uint32_t guest_hwnd); /* rt_user32.c */
uint32_t rt_user32_guest_hwnd(void *host_hwnd);

static uint32_t vtables[D3D_IFACE_COUNT];
static HostMutex *d3d_lock;

/* ---- proxies ---- */
#define MAP_BITS 16
static struct { void *host; uint32_t proxy; } map[1u << MAP_BITS];

static uint32_t map_slot(void *host)
{
    uint32_t h = (uint32_t)(((uintptr_t)host >> 4) * 2654435761u) >> (32 - MAP_BITS);
    while (map[h].host && map[h].host != host) h = (h + 1) & ((1u << MAP_BITS) - 1);
    return h;
}

void *d3d_host(uint32_t proxy)
{
    if (!proxy) return NULL;
    void *p;
    memcpy(&p, rt_ptr(proxy + 8), sizeof p);
    return p;
}

uint32_t d3d_iface(uint32_t proxy) { return ld32(proxy + 4); }

uint32_t d3d_wrap(void *host, int iface)
{
    if (!host) return 0;
    host_lock(d3d_lock);
    uint32_t s = map_slot(host);
    if (map[s].host) {
        uint32_t p = map[s].proxy;
        if (ld32(p + 4) == (uint32_t)iface) {
            host_unlock(d3d_lock);
            return p;
        }
        /* Same address, another interface: the old object died without its proxy being forgotten
           (surfaces of a texture live and die with the texture; their Release never reaches 0)
           and the host reused the memory (DXVK on Android does, e.g. for a vertex buffer). The
           old proxy stays allocated: the guest may still hold it. */
        static int logged;
        if (logged++ < 20) rt_log("D3D9: host object %p reused (proxy %#x interface %u -> %d)", host, p, ld32(p + 4), iface);
    }
    uint32_t p = rt_heap_alloc(16, 1);
    st32(p, vtables[iface]);
    st32(p + 4, (uint32_t)iface);
    memcpy(rt_ptr(p + 8), &host, sizeof host);
    map[s].host = host;
    map[s].proxy = p;
    host_unlock(d3d_lock);
    return p;
}

/* Removes the proxy of a host object whose last reference is gone (linear-probing delete). */
static void d3d_forget(void *host)
{
    host_lock(d3d_lock);
    uint32_t s = map_slot(host);
    if (map[s].host) {
        rt_heap_free(map[s].proxy);
        map[s].host = NULL;
        for (uint32_t j = (s + 1) & ((1u << MAP_BITS) - 1); map[j].host; j = (j + 1) & ((1u << MAP_BITS) - 1)) {
            void *h = map[j].host;
            uint32_t pr = map[j].proxy;
            map[j].host = NULL;
            uint32_t k = map_slot(h);
            map[k].host = h;
            map[k].proxy = pr;
        }
    }
    host_unlock(d3d_lock);
}

void *d3d_host_hwnd(uint32_t guest_hwnd) { return guest_hwnd ? rt_user32_host_hwnd(guest_hwnd) : NULL; }

/* ---- guest vtables and Direct3DCreate9 ---- */
static void build_vtables(void)
{
    d3d_lock = host_mutex_new();
    for (int i = 0; i < D3D_IFACE_COUNT; i++) {
        const D3dIfaceTable *t = &d3d_iface_tables[i];
        if (!t->methods) continue;
        HleDef *defs = calloc(t->count, sizeof *defs);
        for (uint32_t k = 0; k < t->count; k++) {
            defs[k].dll = "d3d9.bridge";
            defs[k].name = t->methods[k].name;
            defs[k].fn = t->methods[k].fn;
            defs[k].nargs = t->methods[k].nargs;
        }
        rt_hle_register(defs, t->count);
        vtables[i] = rt_heap_alloc(4 * t->count, 1);
        for (uint32_t k = 0; k < t->count; k++) st32(vtables[i] + 4 * k, rt_hle_thunk("d3d9.bridge", t->methods[k].name));
    }
}

static void d_Direct3DCreate9(X86Ctx *ctx)
{
    if (!vtables[D3D_IDirect3D9]) build_vtables();
    void *d3d = host_d3d9_create(ARG(0));
    rt_log("Direct3DCreate9(%u) -> host %p", ARG(0), d3d);
    RET(d3d ? d3d_wrap(d3d, D3D_IDirect3D9) : 0);
}

/* D3DX 9 (d3dx9_35): the game compiles shaders only when its shader cache misses; the mesh
   functions belong to the editor. Until a compiler is wired in, they fail visibly. */
static void x_CheckVersion(X86Ctx *ctx) { RET(1); }
static void x_DebugMute(X86Ctx *ctx) { RET(0); }

static void x_Fail(X86Ctx *ctx)
{
    rt_log("D3DX function called (not available yet), return address %#x", ld32(ctx->esp));
    RET(0x80004005u); /* E_FAIL */
}

/* D3DXMatrixInverse(out, determinant or NULL, m): cofactor expansion in double, stored as float;
   NULL when the matrix is singular, as D3DX does. */
static void x_MatrixInverse(X86Ctx *ctx)
{
    uint32_t out = ARG(0), pdet = ARG(1), mp = ARG(2);
    double m[16], inv[16];
    for (int i = 0; i < 16; i++) m[i] = ldf(mp + 4u * (uint32_t)i);
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (pdet) stf(pdet, (float)det);
    if (det == 0.0) { RET(0); return; }
    for (int i = 0; i < 16; i++) stf(out + 4u * (uint32_t)i, (float)(inv[i] / det));
    RET(out);
}

static const HleDef d3d9_exports[] = {
    {"d3dx10_35.dll", "D3DXMatrixInverse", x_MatrixInverse, 3},
    {"d3dx10_35.dll", "D3DX10CheckVersion", x_CheckVersion, 2},
    {"d3d9.dll", "Direct3DCreate9", d_Direct3DCreate9, 1},
    {"d3dx9_35.dll", "D3DXCheckVersion", x_CheckVersion, 2},
    {"d3dx9_35.dll", "D3DXDebugMute", x_DebugMute, 1},
    {"d3dx9_35.dll", "D3DXCompileShader", x_Fail, 10},
    {"d3dx9_35.dll", "D3DXPreprocessShader", x_Fail, 6},
    {"d3dx9_35.dll", "D3DXDisassembleShader", x_Fail, 4},
    {"d3dx9_35.dll", "D3DXCreateMesh", x_Fail, 6},
    {"d3dx9_35.dll", "D3DXSimplifyMesh", x_Fail, 7},
    {"d3dx9_35.dll", "D3DXWeldVertices", x_Fail, 7},
    {"d3dx9_35.dll", "D3DXCleanMesh", x_Fail, 5},
    {"d3dx9_35.dll", "D3DXUVAtlasCreate", x_Fail, 18},
};

void rt_d3d9_register(void) { rt_hle_register(d3d9_exports, sizeof d3d9_exports / sizeof *d3d9_exports); }

/* ---- calling host methods by index ---- */
typedef uint32_t (D3D_CALL *Fn0)(void *);
typedef uint32_t (D3D_CALL *Fn1p)(void *, void *);
typedef uint32_t (D3D_CALL *Fn2pp)(void *, void *, void *);
#define CALL(obj, idx, type, ...) (((type)D3D_VT(obj)[idx])(obj, ##__VA_ARGS__))

void d3d_unsupported(X86Ctx *ctx)
{
    rt_log("D3D9: unsupported method called on proxy %#x (return %#x)", ARG(0), ld32(ctx->esp));
    RET(D3DERR_INVALIDCALL);
}

/* ---- IUnknown ---- */
static int iid_iface(uint32_t riid)
{
    for (uint32_t i = 0; i < d3d_iids_count; i++)
        if (!memcmp(rt_ptr(riid), &d3d_iids[i].iid, 16)) return d3d_iids[i].iface;
    return -1;
}

static void query(X86Ctx *ctx, uint32_t self_proxy, int idx, uint32_t riid, uint32_t ppv, int has_riid_first)
{
    (void)has_riid_first;
    void *self = d3d_host(self_proxy), *out = NULL;
    uint32_t hr = CALL(self, idx, Fn2pp, rt_ptr(riid), &out);
    int iface = iid_iface(riid);
    if (iface < 0 || iface == D3D_IDirect3DResource9 || iface == D3D_IDirect3DBaseTexture9)
        iface = (int)d3d_iface(self_proxy); /* IUnknown or a base interface: same object, same proxy */
    if (ppv) st32(ppv, out && !(hr & 0x80000000u) ? d3d_wrap(out, iface) : 0);
    RET(hr);
}

void d3d_special_QueryInterface(X86Ctx *ctx) { query(ctx, ARG(0), 0, ARG(1), ARG(2), 1); }

void d3d_special_AddRef(X86Ctx *ctx) { RET(CALL(d3d_host(ARG(0)), 1, Fn0)); }

void d3d_special_Release(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    uint32_t n = CALL(self, 2, Fn0);
    if (!n) d3d_forget(self);
    RET(n);
}

void d3d_special_GetContainer(X86Ctx *ctx)
{
    int idx = d3d_iface(ARG(0)) == D3D_IDirect3DSurface9 ? D3D_IDX_IDirect3DSurface9_GetContainer
                                                         : D3D_IDX_IDirect3DVolume9_GetContainer;
    query(ctx, ARG(0), idx, ARG(1), ARG(2), 1);
}

/* ---- frame statistics ----
   One line a second while the game presents: frames per second, draw calls, clears and scenes per frame. The first
   Present, the first draw and the first clear are logged when they happen, so a log tells whether anything is
   drawn at all. ME_PC_FRAME_LOG=0 turns it off. */
static _Atomic uint32_t stat_count[4];
/* ME_PC_LOG_QUERIES=1: occlusion query results, added to the once-a-second line (GetData calls, results not ready yet,
   results with 0 visible pixels, results with some). UE3 hides what its occlusion queries report as invisible. */
static int log_queries = -1;
static _Atomic uint32_t q_calls, q_pending, q_zero, q_visible, q_failed;
static _Atomic int stat_seen[4];
static uint64_t stat_last_ns;
static uint32_t stat_hitches;
static uint64_t stat_worst_ns;
static uint32_t stat_total_frames;
static int stat_enabled = -1;

/* ---- backbuffer dump (debugging) ----
   ME_PC_DUMP_FRAMES=600,1800 writes frame_<n>.ppm (to ME_PC_DUMP_DIR, default ".") from the device's back buffer just
   before the n-th Present, and logs how much of it is not black. Tells "the game draws black" from "the picture does
   not reach the window". Host D3D9 objects are called through their vtables, like the rest of the bridge. */
typedef uint32_t (D3D_CALL *DFnGetBB)(void *, uint32_t, uint32_t, uint32_t, void **);
typedef uint32_t (D3D_CALL *DFnCreatePlain)(void *, uint32_t, uint32_t, uint32_t, uint32_t, void **, void *);
typedef uint32_t (D3D_CALL *DFnGetRTData)(void *, void *, void *);
typedef uint32_t (D3D_CALL *DFnGetDesc)(void *, uint32_t *);
typedef uint32_t (D3D_CALL *DFnLock)(void *, void *, void *, uint32_t);
typedef uint32_t (D3D_CALL *DFnUnlock)(void *);
typedef uint32_t (D3D_CALL *DFnRelease)(void *);

static void dump_backbuffer(void *dev, uint32_t frame)
{
    void *bb = NULL, *sys = NULL;
    uint32_t desc[8] = {0}; /* Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height */
    uint32_t hr = CALL(dev, D3D_IDX_IDirect3DDevice9_GetBackBuffer, DFnGetBB, 0, 0, 0, &bb);
    if (hr || !bb) { rt_log("D3D9 dump: GetBackBuffer failed %#x", hr); return; }
    CALL(bb, D3D_IDX_IDirect3DSurface9_GetDesc, DFnGetDesc, desc);
    uint32_t w = desc[6], h = desc[7];
    hr = CALL(dev, D3D_IDX_IDirect3DDevice9_CreateOffscreenPlainSurface, DFnCreatePlain, w, h, desc[0], 2 /* SYSTEMMEM */, &sys, NULL);
    if (hr || !sys) { rt_log("D3D9 dump: CreateOffscreenPlainSurface failed %#x", hr); CALL(bb, 2, DFnRelease); return; }
    hr = CALL(dev, D3D_IDX_IDirect3DDevice9_GetRenderTargetData, DFnGetRTData, bb, sys);
    struct { int pitch; void *bits; } lr = {0, NULL};
    if (!hr) hr = CALL(sys, D3D_IDX_IDirect3DSurface9_LockRect, DFnLock, &lr, NULL, 0x10 /* READONLY */);
    if (hr || !lr.bits) rt_log("D3D9 dump: GetRenderTargetData/LockRect failed %#x", hr);
    else {
        const char *dir = getenv("ME_PC_DUMP_DIR");
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%u.ppm", dir && *dir ? dir : ".", frame);
        FILE *f = fopen(path, "wb");
        if (f) fprintf(f, "P6\n%u %u\n255\n", w, h);
        uint64_t lit = 0, sum[3] = {0, 0, 0};
        uint8_t *row = malloc((size_t)w * 3);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *p = (const uint8_t *)lr.bits + (size_t)y * (size_t)lr.pitch; /* B G R A (A8R8G8B8, little endian) */
            for (uint32_t x = 0; x < w; x++) {
                uint8_t b = p[x * 4], g = p[x * 4 + 1], r = p[x * 4 + 2];
                row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
                sum[0] += r; sum[1] += g; sum[2] += b;
                if (r | g | b) lit++;
            }
            if (f) fwrite(row, 1, (size_t)w * 3, f);
        }
        free(row);
        if (f) fclose(f);
        uint64_t n = (uint64_t)w * h;
        rt_log("D3D9 dump: frame %u %ux%u format %u: %.1f%% of pixels not black, mean RGB %.1f/%.1f/%.1f -> %s", frame, w, h, desc[0],
               100.0 * (double)lit / (double)n, (double)sum[0] / (double)n, (double)sum[1] / (double)n, (double)sum[2] / (double)n,
               f ? path : "(cannot write)");
    }
    if (lr.bits) CALL(sys, D3D_IDX_IDirect3DSurface9_UnlockRect, DFnUnlock);
    CALL(sys, 2, DFnRelease);
    CALL(bb, 2, DFnRelease);
}

static uint32_t dump_frames[8];
static int dump_count = -1;

static void dump_maybe(void *dev, uint32_t frame)
{
    if (dump_count < 0) {
        dump_count = 0;
        const char *e = getenv("ME_PC_DUMP_FRAMES");
        while (e && *e && dump_count < 8) {
            char *end;
            unsigned long v = strtoul(e, &end, 10);
            if (end == e) break;
            dump_frames[dump_count++] = (uint32_t)v;
            e = *end == ',' ? end + 1 : end;
        }
    }
    for (int i = 0; i < dump_count; i++)
        if (dump_frames[i] == frame) dump_backbuffer(dev, frame);
}

/* only the device's Present is dumped, not a swap chain's: the thunk passes the host object, whose kind is not known
   here, so the caller marks the device through d3d_stat_device(). */
static void *stat_device;
void d3d_stat_device(void *dev) { stat_device = dev; }
static int stat_dump_device(void *self) { return self == stat_device; }

uint32_t d3d_frames_presented(void) { return stat_total_frames; } /* host scripted input (ME_PC_AUTO_INPUT) */

void d3d_stat(int what, void *self)
{
    if (stat_enabled < 0) { const char *e = getenv("ME_PC_FRAME_LOG"); stat_enabled = !(e && *e == '0'); }
    if (!stat_enabled) return;
    static const char *const names[4] = {"Present", "BeginScene", "Clear", "draw call"};
    if (what == D3D_STAT_PRESENT && self && D3D_VT(self) && stat_dump_device(self)) dump_maybe(self, stat_total_frames + 1);
    atomic_fetch_add(&stat_count[what], 1);
    if (!atomic_exchange(&stat_seen[what], 1)) rt_log("D3D9: first %s", names[what]);
    if (what != D3D_STAT_PRESENT) return;
    uint64_t now = host_ticks_ns();
    stat_total_frames++;
    {   /* frame pacing: frames that took longer than 25 ms, and the longest, per logged second */
        static uint64_t prev_present;
        if (prev_present) {
            uint64_t dt = now - prev_present;
            if (dt > 25000000ull) stat_hitches++;
            if (dt > stat_worst_ns) stat_worst_ns = dt;
        }
        prev_present = now;
    }
    if (!stat_last_ns) { stat_last_ns = now; return; }
    if (now - stat_last_ns < 1000000000ull) return;
    uint32_t frames = atomic_exchange(&stat_count[D3D_STAT_PRESENT], 0), draws = atomic_exchange(&stat_count[D3D_STAT_DRAW], 0),
             clears = atomic_exchange(&stat_count[D3D_STAT_CLEAR], 0), scenes = atomic_exchange(&stat_count[D3D_STAT_SCENE], 0);
    double secs = (double)(now - stat_last_ns) / 1e9;
    if (!frames) frames = 1;
    rt_log("D3D9 frames: %.1f fps, %u draws/frame, %u clears/frame, %u scenes/frame (%u frames so far), %u frames > 25 ms, "
           "longest %.1f ms", frames / secs, draws / frames, clears / frames, scenes / frames, stat_total_frames, stat_hitches,
           (double)stat_worst_ns / 1e6);
    stat_hitches = 0;
    stat_worst_ns = 0;
    if (log_queries > 0)
        rt_log("D3D9 occlusion queries per frame: %u GetData, %u not ready, %u zero pixels, %u visible, %u failed",
               atomic_exchange(&q_calls, 0) / frames, atomic_exchange(&q_pending, 0) / frames, atomic_exchange(&q_zero, 0) / frames,
               atomic_exchange(&q_visible, 0) / frames, atomic_exchange(&q_failed, 0) / frames);
    stat_last_ns = now;
}

typedef uint32_t (D3D_CALL *QFnGetType)(void *);

void d3d_query_stat(void *query, uint32_t hr, uint32_t data, uint32_t size, uint32_t flags)
{
    (void)flags;
    if (log_queries < 0) { const char *e = getenv("ME_PC_LOG_QUERIES"); log_queries = e && *e == '1'; }
    if (!log_queries || ((QFnGetType)D3D_VT(query)[D3D_IDX_IDirect3DQuery9_GetType])(query) != 9 /* OCCLUSION */) return;
    atomic_fetch_add(&q_calls, 1);
    if (hr == 1 /* S_FALSE */) atomic_fetch_add(&q_pending, 1);
    else if (hr) atomic_fetch_add(&q_failed, 1);
    else if (data && size >= 4) atomic_fetch_add(ld32(data) ? &q_visible : &q_zero, 1);
}

/* ---- present parameters: 32-bit guest layout (56 bytes) <-> host layout (HWND is pointer-wide) ---- */
typedef struct {
    uint32_t w, h, format, count, ms_type, ms_quality, swap_effect;
    void *hwnd;
    uint32_t windowed, auto_depth, depth_format, flags, refresh, interval;
} HostPresent;

void rt_user32_set_client_size(void *host, int w, int h);

static void pp_in(uint32_t g, HostPresent *p, int force_windowed)
{
    p->w = ld32(g); p->h = ld32(g + 4); p->format = ld32(g + 8); p->count = ld32(g + 12);
    p->ms_type = ld32(g + 16); p->ms_quality = ld32(g + 20); p->swap_effect = ld32(g + 24);
    p->hwnd = d3d_host_hwnd(ld32(g + 28));
    p->windowed = ld32(g + 32); p->auto_depth = ld32(g + 36); p->depth_format = ld32(g + 40);
    p->flags = ld32(g + 44); p->refresh = ld32(g + 48); p->interval = ld32(g + 52);
    rt_log("D3D9 present parameters: %ux%u format %u x%u ms %u swap %u windowed %u depth %u/%u flags %#x refresh %u interval %#x",
           p->w, p->h, p->format, p->count, p->ms_type, p->swap_effect, p->windowed, p->auto_depth, p->depth_format,
           p->flags, p->refresh, p->interval);
#ifdef __APPLE__
    {   /* macOS: present in step with the display. With IMMEDIATE (the game's UseVsync=False) frames reach a 120 Hz
           ProMotion screen at uneven times and motion judders ("camera skipping"); measured: frames of 20-34 ms every
           second during a steady pan, none over 25 ms with vsync. ME_PC_VSYNC=0 keeps what the game asked for. */
        const char *v = getenv("ME_PC_VSYNC");
        if (p->interval == 0x80000000u /* D3DPRESENT_INTERVAL_IMMEDIATE */ && !(v && *v == '0')) p->interval = 1;
    }
#endif
    if (force_windowed && !p->windowed) {
        /* Bring-up on the PC: never take over the user's display. */
        p->windowed = 1;
        p->refresh = 0;
    }
    if (p->hwnd && p->w && p->h) rt_user32_set_client_size(p->hwnd, (int)p->w, (int)p->h);
}

static void pp_out(uint32_t g, const HostPresent *p)
{
    st32(g, p->w); st32(g + 4, p->h); st32(g + 8, p->format); st32(g + 12, p->count);
    st32(g + 16, p->ms_type); st32(g + 20, p->ms_quality); st32(g + 24, p->swap_effect);
    st32(g + 28, rt_user32_guest_hwnd(p->hwnd));
    st32(g + 36, p->auto_depth); st32(g + 40, p->depth_format); st32(g + 44, p->flags);
    st32(g + 48, p->refresh); st32(g + 52, p->interval);
    /* Windowed stays as the guest asked. */
}

typedef uint32_t (D3D_CALL *FnCreateDevice)(void *, uint32_t, uint32_t, void *, uint32_t, HostPresent *, void **);

void d3d_special_CreateDevice(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0)), *dev = NULL;
    uint32_t flags = ARG(4);
    HostPresent p;
    pp_in(ARG(5), &p, 1);
    {   /* what display modes the game is offered (it picks its resolution from these) */
        const char *lm = getenv("ME_PC_LOG_MODES");
        if (lm && *lm == '1') {
            typedef uint32_t (D3D_CALL *FnCnt)(void *, uint32_t, uint32_t);
            typedef uint32_t (D3D_CALL *FnEnum)(void *, uint32_t, uint32_t, uint32_t, uint32_t *);
            uint32_t n = ((FnCnt)D3D_VT(self)[6])(self, ARG(1), 22);
            rt_log("D3D9 display modes (X8R8G8B8): %u", n);
            for (uint32_t i = 0; i < n && i < 64; i++) {
                uint32_t m[4] = {0, 0, 0, 0};
                ((FnEnum)D3D_VT(self)[7])(self, ARG(1), 22, i, m);
                rt_log("  mode %u: %ux%u @%u", i, m[0], m[1], m[2]);
            }
        }
    }
    uint32_t hr = ((FnCreateDevice)D3D_VT(self)[D3D_IDX_IDirect3D9_CreateDevice])(
        self, ARG(1), ARG(2), d3d_host_hwnd(ARG(3)), flags | 0x4 /* MULTITHREADED */, &p, &dev);
    rt_log("D3D9 CreateDevice(adapter %u, type %u, focus %#x -> %p, device window %p, flags %#x) = %#x, device %p",
           ARG(1), ARG(2), ARG(3), d3d_host_hwnd(ARG(3)), p.hwnd, flags, hr, dev);
    if (!(hr & 0x80000000u)) pp_out(ARG(5), &p);
    if (dev) d3d_stat_device(dev);
    if (ARG(6)) st32(ARG(6), dev ? d3d_wrap(dev, D3D_IDirect3DDevice9) : 0);
    /* Without D3DCREATE_FPU_PRESERVE, D3D9 sets the calling thread's x87 to 24-bit precision,
       round to nearest; the guest's x87 model follows (x87_round). */
    if (!(flags & 0x2) && !(hr & 0x80000000u)) ctx->fcw = (ctx->fcw & ~0x0F00u);
    RET(hr);
}

typedef uint32_t (D3D_CALL *FnReset)(void *, HostPresent *);

void d3d_special_Reset(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    HostPresent p;
    pp_in(ARG(1), &p, 1);
    uint32_t hr = ((FnReset)D3D_VT(self)[D3D_IDX_IDirect3DDevice9_Reset])(self, &p);
    if (!(hr & 0x80000000u)) pp_out(ARG(1), &p);
    RET(hr);
}

typedef uint32_t (D3D_CALL *FnSwapChain)(void *, HostPresent *, void **);

void d3d_special_CreateAdditionalSwapChain(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0)), *sc = NULL;
    HostPresent p;
    pp_in(ARG(1), &p, 1);
    uint32_t hr = ((FnSwapChain)D3D_VT(self)[D3D_IDX_IDirect3DDevice9_CreateAdditionalSwapChain])(self, &p, &sc);
    if (ARG(2)) st32(ARG(2), sc ? d3d_wrap(sc, D3D_IDirect3DSwapChain9) : 0);
    RET(hr);
}

void d3d_special_GetPresentParameters(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    HostPresent p;
    memset(&p, 0, sizeof p);
    uint32_t hr = CALL(self, D3D_IDX_IDirect3DSwapChain9_GetPresentParameters, Fn1p, &p);
    if (!(hr & 0x80000000u)) { pp_out(ARG(1), &p); st32(ARG(1) + 32, p.windowed); }
    RET(hr);
}

void d3d_special_GetCreationParameters(X86Ctx *ctx)
{
    struct { uint32_t adapter, type; void *hwnd; uint32_t flags; } c;
    void *self = d3d_host(ARG(0));
    uint32_t hr = CALL(self, D3D_IDX_IDirect3DDevice9_GetCreationParameters, Fn1p, &c);
    uint32_t g = ARG(1);
    st32(g, c.adapter); st32(g + 4, c.type); st32(g + 8, rt_user32_guest_hwnd(c.hwnd)); st32(g + 12, c.flags);
    RET(hr);
}

/* D3DADAPTER_IDENTIFIER9 is packed to 4 bytes on x86 (1100 bytes) and naturally aligned on 64-bit
   hosts (1104: the trailing padding after WHQLLevel); the fields line up, so the first 1100 bytes
   are copied. Writing the host size straight into the guest overran a stack buffer by 4 bytes. */
typedef uint32_t (D3D_CALL *FnAdapterId)(void *, uint32_t, uint32_t, void *);
void d3d_special_GetAdapterIdentifier(X86Ctx *ctx)
{
    union { uint64_t align; uint8_t b[1112]; } id;
    void *self = d3d_host(ARG(0));
    uint32_t hr = CALL(self, D3D_IDX_IDirect3D9_GetAdapterIdentifier, FnAdapterId, ARG(1), ARG(2), id.b);
    if (hr == 0 && ARG(3)) memcpy(rt_ptr(ARG(3)), id.b, 1100);
    RET(hr);
}

/* Display modes: the real ones (a Mac offers only 16:10 and Retina sizes) plus common 16:9 sizes, so the game can
   be set to 1280x720. The window follows the backbuffer and the host scales it, so these need not match the display. */
static const uint32_t extra_modes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
#define N_EXTRA_MODES ((uint32_t)(sizeof extra_modes / sizeof extra_modes[0]))

static int extra_mode_format(uint32_t fmt) { return fmt == 22 || fmt == 21; } /* X8R8G8B8, A8R8G8B8 */

void d3d_special_GetAdapterModeCount(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    uint32_t n = ((uint32_t (D3D_CALL *)(void *, uint32_t, uint32_t))D3D_VT(self)[6])(self, ARG(1), ARG(2));
    RET(n + (extra_mode_format(ARG(2)) ? N_EXTRA_MODES : 0));
}

void d3d_special_EnumAdapterModes(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    uint32_t adapter = ARG(1), fmt = ARG(2), idx = ARG(3), out = ARG(4);
    uint32_t n = ((uint32_t (D3D_CALL *)(void *, uint32_t, uint32_t))D3D_VT(self)[6])(self, adapter, fmt);
    if (extra_mode_format(fmt) && idx >= n && idx < n + N_EXTRA_MODES) {
        st32(out, extra_modes[idx - n][0]); st32(out + 4, extra_modes[idx - n][1]); /* D3DDISPLAYMODE */
        st32(out + 8, 60); st32(out + 12, fmt);
        RET(0);
        return;
    }
    RET(((uint32_t (D3D_CALL *)(void *, uint32_t, uint32_t, uint32_t, void *))D3D_VT(self)[7])(self, adapter, fmt, idx, D3D_PTR(out)));
}

void d3d_special_GetAdapterMonitor(X86Ctx *ctx) { RET(0x00020001u); } /* the HMONITOR USER32 reports */

void d3d_special_GetDC(X86Ctx *ctx) { RET(D3DERR_INVALIDCALL); }
void d3d_special_ReleaseDC(X86Ctx *ctx) { RET(D3DERR_INVALIDCALL); }

/* ---- Lock/Unlock through guest staging buffers ----
   Host-locked memory lies outside the guest's 4 GB, so a lock hands the guest a buffer in guest
   memory: filled from the host on lock (unless the contents are discarded) and copied back on
   unlock (unless read-only). Keyed by host object and subresource. */
#define LOCK_READONLY 0x10u
#define LOCK_DISCARD 0x2000u

typedef struct { void *host; uint32_t sub; uint32_t guest, size, readonly; void *bits; uint32_t fmt, w, h, flags; } Staging;
static int log_tex = -1;
/* Level streaming keeps many textures locked across frames; the scan stops at the high-water mark. */
#define MAX_STAGING 8192
static Staging staging[MAX_STAGING];
static int staging_top;

static Staging *stage_get(void *host, uint32_t sub, int create)
{
    host_lock(d3d_lock);
    Staging *free_slot = NULL;
    for (int i = 0; i < staging_top; i++) {
        if (staging[i].host == host && staging[i].sub == sub) { host_unlock(d3d_lock); return &staging[i]; }
        if (!staging[i].host && !free_slot) free_slot = &staging[i];
    }
    if (create && !free_slot && staging_top < MAX_STAGING) free_slot = &staging[staging_top++];
    if (create && free_slot) { free_slot->host = host; free_slot->sub = sub; free_slot->guest = 0; free_slot->size = 0; }
    host_unlock(d3d_lock);
    if (!free_slot && create) rt_fatal("D3D9: more than %d resources locked at once", MAX_STAGING);
    return create ? free_slot : NULL;
}

static uint32_t stage_begin(void *host, uint32_t sub, void *bits, uint32_t size, uint32_t flags)
{
    Staging *s = stage_get(host, sub, 1);
    if (s->size != size || !s->guest) {
        if (s->guest) rt_heap_free(s->guest);
        s->guest = rt_heap_alloc(size ? size : 1, 0);
        s->size = size;
    }
    s->bits = bits;
    s->readonly = (flags & LOCK_READONLY) != 0;
    if (!(flags & LOCK_DISCARD) && size) memcpy(rt_ptr(s->guest), bits, size);
    return s->guest;
}

static void stage_end(void *host, uint32_t sub)
{
    Staging *s = stage_get(host, sub, 0);
    if (!s) return;
    if (!s->readonly && s->size) memcpy(s->bits, rt_ptr(s->guest), s->size);
    if (s->fmt) {
        if (log_tex < 0) { const char *e = getenv("ME_PC_LOG_TEX"); log_tex = e && *e == '1'; }
        if (log_tex) {
            uint32_t nz = 0, ff = 0; const uint8_t *q = rt_ptr(s->guest);
            for (uint32_t i = 0; i < s->size; i++) { nz += q[i] != 0; ff += q[i] == 0xFF; }
            rt_log("D3D9 tex unlock: fmt %#x (%.4s) %ux%u sub %u flags %#x size %u nonzero %u ff %u ro %u", s->fmt,
                   (const char *)&s->fmt, s->w, s->h, s->sub, s->flags, s->size, nz, ff, s->readonly);
        }
    }
    /* Only resources that are locked right now hold a slot and a guest buffer. */
    host_lock(d3d_lock);
    rt_heap_free(s->guest);
    memset(s, 0, sizeof *s);
    host_unlock(d3d_lock);
}

static int is_block_format(uint32_t fmt)
{
    return fmt == 0x31545844u || fmt == 0x32545844u || fmt == 0x33545844u || fmt == 0x34545844u ||
           fmt == 0x35545844u || fmt == 0x31495441u || fmt == 0x32495441u; /* DXT1-5, ATI1/ATI2 */
}

static uint32_t rows_of(uint32_t fmt, uint32_t height, uint32_t rect)
{
    uint32_t h = height;
    if (rect) h = ld32(rect + 12) - ld32(rect + 4); /* bottom - top */
    return is_block_format(fmt) ? (h + 3) / 4 : h;
}

typedef struct { uint32_t format, type, usage, pool, ms_type, ms_quality, width, height; } SurfDesc;
typedef struct { int32_t pitch; void *bits; } HostLockedRect;
typedef uint32_t (D3D_CALL *FnLockSurf)(void *, HostLockedRect *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLockLevel)(void *, uint32_t, HostLockedRect *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLockFace)(void *, uint32_t, uint32_t, HostLockedRect *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLevelDesc)(void *, uint32_t, SurfDesc *);
typedef uint32_t (D3D_CALL *FnU32)(void *, uint32_t);
typedef uint32_t (D3D_CALL *FnU32U32)(void *, uint32_t, uint32_t);

void d3d_special_LockRect(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    HostLockedRect lr = {0, NULL};
    SurfDesc d;
    uint32_t hr, sub, rect, flags, out;
    memset(&d, 0, sizeof d);
    if (iface == D3D_IDirect3DSurface9) {
        out = ARG(1); rect = ARG(2); flags = ARG(3); sub = 0;
        hr = ((FnLockSurf)D3D_VT(self)[D3D_IDX_IDirect3DSurface9_LockRect])(self, &lr, D3D_PTR(rect), flags);
        CALL(self, D3D_IDX_IDirect3DSurface9_GetDesc, Fn1p, &d);
    } else if (iface == D3D_IDirect3DTexture9) {
        sub = ARG(1); out = ARG(2); rect = ARG(3); flags = ARG(4);
        hr = ((FnLockLevel)D3D_VT(self)[D3D_IDX_IDirect3DTexture9_LockRect])(self, sub, &lr, D3D_PTR(rect), flags);
        ((FnLevelDesc)D3D_VT(self)[D3D_IDX_IDirect3DTexture9_GetLevelDesc])(self, sub, &d);
    } else if (iface == D3D_IDirect3DCubeTexture9) {
        uint32_t face = ARG(1), level = ARG(2);
        sub = face * 32 + level; out = ARG(3); rect = ARG(4); flags = ARG(5);
        hr = ((FnLockFace)D3D_VT(self)[D3D_IDX_IDirect3DCubeTexture9_LockRect])(self, face, level, &lr, D3D_PTR(rect), flags);
        ((FnLevelDesc)D3D_VT(self)[D3D_IDX_IDirect3DCubeTexture9_GetLevelDesc])(self, level, &d);
    } else {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (hr & 0x80000000u) { RET(hr); return; }
    uint32_t size = (uint32_t)lr.pitch * rows_of(d.format, d.height, rect);
    st32(out, (uint32_t)lr.pitch);
    uint32_t g = stage_begin(self, sub, lr.bits, size, flags);
    { Staging *st = stage_get(self, sub, 0); if (st) { st->fmt = d.format; st->w = d.width; st->h = d.height; st->flags = flags; } }
    st32(out + 4, g);
    RET(hr);
}

void d3d_special_UnlockRect(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    if (iface == D3D_IDirect3DSurface9) {
        stage_end(self, 0);
        RET(CALL(self, D3D_IDX_IDirect3DSurface9_UnlockRect, Fn0));
    } else if (iface == D3D_IDirect3DTexture9) {
        stage_end(self, ARG(1));
        RET(((FnU32)D3D_VT(self)[D3D_IDX_IDirect3DTexture9_UnlockRect])(self, ARG(1)));
    } else if (iface == D3D_IDirect3DCubeTexture9) {
        stage_end(self, ARG(1) * 32 + ARG(2));
        RET(((FnU32U32)D3D_VT(self)[D3D_IDX_IDirect3DCubeTexture9_UnlockRect])(self, ARG(1), ARG(2)));
    } else {
        RET(D3DERR_INVALIDCALL);
    }
}

typedef struct { uint32_t format, type, usage, pool, width, height, depth; } VolDesc;
typedef struct { int32_t row_pitch, slice_pitch; void *bits; } HostLockedBox;
typedef uint32_t (D3D_CALL *FnLockVol)(void *, HostLockedBox *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLockVolLevel)(void *, uint32_t, HostLockedBox *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnVolLevelDesc)(void *, uint32_t, VolDesc *);

void d3d_special_LockBox(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    HostLockedBox lb = {0, 0, NULL};
    VolDesc d;
    uint32_t hr, sub, box, flags, out;
    memset(&d, 0, sizeof d);
    if (iface == D3D_IDirect3DVolume9) {
        out = ARG(1); box = ARG(2); flags = ARG(3); sub = 0;
        hr = ((FnLockVol)D3D_VT(self)[D3D_IDX_IDirect3DVolume9_LockBox])(self, &lb, D3D_PTR(box), flags);
        CALL(self, D3D_IDX_IDirect3DVolume9_GetDesc, Fn1p, &d);
    } else if (iface == D3D_IDirect3DVolumeTexture9) {
        sub = ARG(1); out = ARG(2); box = ARG(3); flags = ARG(4);
        hr = ((FnLockVolLevel)D3D_VT(self)[D3D_IDX_IDirect3DVolumeTexture9_LockBox])(self, sub, &lb, D3D_PTR(box), flags);
        ((FnVolLevelDesc)D3D_VT(self)[D3D_IDX_IDirect3DVolumeTexture9_GetLevelDesc])(self, sub, &d);
    } else {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (hr & 0x80000000u) { RET(hr); return; }
    uint32_t depth = box ? ld32(box + 20) - ld32(box + 16) : d.depth;
    uint32_t size = (uint32_t)lb.slice_pitch * depth;
    st32(out, (uint32_t)lb.row_pitch);
    st32(out + 4, (uint32_t)lb.slice_pitch);
    st32(out + 8, stage_begin(self, sub, lb.bits, size, flags));
    RET(hr);
}

void d3d_special_UnlockBox(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    if (iface == D3D_IDirect3DVolume9) {
        stage_end(self, 0);
        RET(CALL(self, D3D_IDX_IDirect3DVolume9_UnlockBox, Fn0));
    } else {
        stage_end(self, ARG(1));
        RET(((FnU32)D3D_VT(self)[D3D_IDX_IDirect3DVolumeTexture9_UnlockBox])(self, ARG(1)));
    }
}

typedef uint32_t (D3D_CALL *FnLockBuf)(void *, uint32_t, uint32_t, void **, uint32_t);
typedef struct { uint32_t format, type, usage, pool, size, fvf; } BufDesc;

void d3d_special_Lock(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy), *bits = NULL;
    uint32_t offset = ARG(1), size = ARG(2), out = ARG(3), flags = ARG(4);
    int vb = iface == D3D_IDirect3DVertexBuffer9;
    uint32_t hr = ((FnLockBuf)D3D_VT(self)[vb ? D3D_IDX_IDirect3DVertexBuffer9_Lock : D3D_IDX_IDirect3DIndexBuffer9_Lock])(
        self, offset, size, &bits, flags);
    if (hr & 0x80000000u) { RET(hr); return; }
    if (!size) {
        BufDesc d;
        memset(&d, 0, sizeof d);
        CALL(self, vb ? D3D_IDX_IDirect3DVertexBuffer9_GetDesc : D3D_IDX_IDirect3DIndexBuffer9_GetDesc, Fn1p, &d);
        size = d.size - offset;
    }
    st32(out, stage_begin(self, 0, bits, size, flags));
    RET(hr);
}

void d3d_special_Unlock(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0);
    void *self = d3d_host(proxy);
    int vb = d3d_iface(proxy) == D3D_IDirect3DVertexBuffer9;
    stage_end(self, 0);
    RET(CALL(self, vb ? D3D_IDX_IDirect3DVertexBuffer9_Unlock : D3D_IDX_IDirect3DIndexBuffer9_Unlock, Fn0));
}

/* ---- per-draw state trace (debugging) ----
   ME_PC_TRACE_FRAME=<n> logs, for every draw of the n-th frame (as counted by d3d_stat), the primitive, the shaders
   (bytecode version, size and a hash), the render target, the blend/alpha/sRGB render states, and every bound texture
   with its type, format, size and sampler states. ME_PC_TRACE_MAX limits the number of draws (default 400).
   ME_PC_TRACE_DUMP=1 also writes the shader bytecode (ps_/vs_<hash>.bin) and level 0 of each bound A8R8G8B8/L8/A8
   texture that can be locked (tex_<n>.ppm with RGB, tex_<n>_a.pgm with alpha) into ME_PC_DUMP_DIR. Those files are
   derived from game data: keep them in work/. Host objects are queried through their vtables, like the rest of the
   bridge; every Get that returns an object is released again. */
typedef uint32_t (D3D_CALL *TFnGetU32)(void *, uint32_t, uint32_t *);
typedef uint32_t (D3D_CALL *TFnGetU32U32)(void *, uint32_t, uint32_t, uint32_t *);
typedef uint32_t (D3D_CALL *TFnGetObjIdx)(void *, uint32_t, void **);
typedef uint32_t (D3D_CALL *TFnGetObj)(void *, void **);
typedef uint32_t (D3D_CALL *TFnGetFunction)(void *, void *, uint32_t *);
typedef uint32_t (D3D_CALL *TFnType)(void *);
typedef uint32_t (D3D_CALL *TFnGetConstF)(void *, uint32_t, float *, uint32_t);

static int trace_frame = -1, trace_max = 400, trace_dump;
static uint32_t trace_draws;

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static const char *trace_dir(void)
{
    const char *d = getenv("ME_PC_DUMP_DIR");
    return d && *d ? d : ".";
}

/* Bytecode version token, size and hash of a shader; the bytecode is written once per hash when dumping. */
static void trace_shader(char *out, size_t cap, void *sh, const char *kind)
{
    if (!sh) { snprintf(out, cap, "%s none", kind); return; }
    uint32_t size = 0;
    CALL(sh, D3D_IDX_IDirect3DPixelShader9_GetFunction, TFnGetFunction, NULL, &size);
    uint8_t *code = size ? malloc(size) : NULL;
    if (code) CALL(sh, D3D_IDX_IDirect3DPixelShader9_GetFunction, TFnGetFunction, code, &size);
    uint32_t ver = code && size >= 4 ? (uint32_t)code[0] | (uint32_t)code[1] << 8 | (uint32_t)code[2] << 16 | (uint32_t)code[3] << 24 : 0;
    uint32_t h = code ? fnv(code, size) : 0;
    snprintf(out, cap, "%s %u.%u %ub #%08x", kind, (ver >> 8) & 0xFF, ver & 0xFF, size, h);
    if (code && trace_dump) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s_%08x.bin", trace_dir(), kind, h);
        FILE *f = fopen(path, "rb");
        if (f) fclose(f);
        else if ((f = fopen(path, "wb"))) { fwrite(code, 1, size, f); fclose(f); }
    }
    free(code);
}

/* Level 0 of a 2D texture as PPM (RGB) and PGM (alpha); only formats with one byte per channel. */
static void trace_dump_texture(void *tex, uint32_t id, const SurfDesc *d)
{
    uint32_t bpp = d->format == 21 || d->format == 22 ? 4 : d->format == 50 || d->format == 28 ? 1 : 0;
    if (!bpp) return;
    HostLockedRect lr = {0, NULL};
    uint32_t hr = ((FnLockLevel)D3D_VT(tex)[D3D_IDX_IDirect3DTexture9_LockRect])(tex, 0, &lr, NULL, LOCK_READONLY);
    if (hr || !lr.bits) { rt_log("D3D9 trace: texture %u cannot be locked (%#x, pool %u)", id, hr, d->pool); return; }
    char path[512];
    snprintf(path, sizeof path, "%s/tex_%u.ppm", trace_dir(), id);
    FILE *f = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/tex_%u_a.pgm", trace_dir(), id);
    FILE *fa = fopen(path, "wb");
    if (f) fprintf(f, "P6\n%u %u\n255\n", d->width, d->height);
    if (fa) fprintf(fa, "P5\n%u %u\n255\n", d->width, d->height);
    for (uint32_t y = 0; y < d->height; y++) {
        const uint8_t *p = (const uint8_t *)lr.bits + (size_t)y * (size_t)lr.pitch;
        for (uint32_t x = 0; x < d->width; x++) {
            uint8_t rgb[3], a;
            if (bpp == 4) { rgb[0] = p[x * 4 + 2]; rgb[1] = p[x * 4 + 1]; rgb[2] = p[x * 4]; a = d->format == 21 ? p[x * 4 + 3] : 255; }
            else if (d->format == 28) { rgb[0] = rgb[1] = rgb[2] = 0; a = p[x]; }
            else { rgb[0] = rgb[1] = rgb[2] = p[x]; a = 255; }
            if (f) fwrite(rgb, 1, 3, f);
            if (fa) fwrite(&a, 1, 1, fa);
        }
    }
    if (f) fclose(f);
    if (fa) fclose(fa);
    ((FnU32)D3D_VT(tex)[D3D_IDX_IDirect3DTexture9_UnlockRect])(tex, 0);
}

/* Small ids for host texture pointers, so a log can be read without addresses. */
static void *trace_tex_seen[1024];
static uint32_t trace_tex_count;

static uint32_t trace_tex_id(void *tex, int *is_new)
{
    for (uint32_t i = 0; i < trace_tex_count; i++)
        if (trace_tex_seen[i] == tex) { *is_new = 0; return i; }
    *is_new = 1;
    if (trace_tex_count < 1024) trace_tex_seen[trace_tex_count] = tex;
    return trace_tex_count++;
}

void d3d_trace_draw(void *dev, int kind, uint32_t prim, uint32_t count)
{
    if (trace_frame < 0) {
        const char *e = getenv("ME_PC_TRACE_FRAME"), *m = getenv("ME_PC_TRACE_MAX"), *dd = getenv("ME_PC_TRACE_DUMP");
        trace_frame = e && *e ? atoi(e) : 0;
        if (m && *m) trace_max = atoi(m);
        trace_dump = dd && *dd == '1';
    }
    if (!trace_frame || stat_total_frames + 1 != (uint32_t)trace_frame || trace_draws >= (uint32_t)trace_max) return;
    uint32_t n = trace_draws++;
    static const char *const kinds[4] = {"DP", "DIP", "DPUP", "DIPUP"};
    void *ps = NULL, *vs = NULL, *rt = NULL;
    CALL(dev, D3D_IDX_IDirect3DDevice9_GetPixelShader, TFnGetObj, &ps);
    CALL(dev, D3D_IDX_IDirect3DDevice9_GetVertexShader, TFnGetObj, &vs);
    char pss[96], vss[96];
    trace_shader(pss, sizeof pss, ps, "ps");
    trace_shader(vss, sizeof vss, vs, "vs");
    SurfDesc rd;
    memset(&rd, 0, sizeof rd);
    CALL(dev, D3D_IDX_IDirect3DDevice9_GetRenderTarget, TFnGetObjIdx, 0, &rt);
    if (rt) CALL(rt, D3D_IDX_IDirect3DSurface9_GetDesc, Fn1p, &rd);
    void *ds = NULL;
    SurfDesc dsd;
    memset(&dsd, 0, sizeof dsd);
    CALL(dev, D3D_IDX_IDirect3DDevice9_GetDepthStencilSurface, TFnGetObj, &ds);
    if (ds) { CALL(ds, D3D_IDX_IDirect3DSurface9_GetDesc, Fn1p, &dsd); CALL(ds, 2, DFnRelease); }
    /* ALPHATESTENABLE 15, SRCBLEND 19, DESTBLEND 20, ZENABLE 7, ZWRITEENABLE 14, ALPHAREF 24, ALPHAFUNC 25,
       ALPHABLENDENABLE 27, COLORWRITEENABLE 168, BLENDOP 171, SRGBWRITEENABLE 194, SEPARATEALPHABLENDENABLE 206,
       SRCBLENDALPHA 207, DESTBLENDALPHA 208, CULLMODE 22, FVF via GetFVF */
    static const uint32_t rs[] = {27, 19, 20, 171, 206, 207, 208, 15, 24, 25, 168, 194, 7, 14, 22, 23, 195, 175};
    uint32_t v[sizeof rs / sizeof *rs], fvf = 0;
    for (size_t i = 0; i < sizeof rs / sizeof *rs; i++) { v[i] = 0; CALL(dev, D3D_IDX_IDirect3DDevice9_GetRenderState, TFnGetU32, rs[i], &v[i]); }
    CALL(dev, D3D_IDX_IDirect3DDevice9_GetFVF, Fn1p, &fvf);
    rt_log("D3D9 trace %u: %s prim %u x%u | %s | %s | fvf %#x | rt %ux%u fmt %u | blend %u %u/%u op %u sep %u %u/%u | atest %u ref %u func %u | "
           "cwrite %#x srgbw %u | z %u zw %u cull %u zfunc %u bias %g slope %g ds %u", n, kinds[kind & 3], prim, count, vss, pss, fvf,
           rd.width, rd.height, rd.format, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14],
           v[15], (double)u2f(v[16]), (double)u2f(v[17]), dsd.format);
    if (ps) {
        float c[16];
        memset(c, 0, sizeof c);
        typedef uint32_t (D3D_CALL *TFnGetPSConst)(void *, uint32_t, float *, uint32_t);
        CALL(dev, D3D_IDX_IDirect3DDevice9_GetPixelShaderConstantF, TFnGetPSConst, 0, c, 4);
        rt_log("D3D9 trace %u:   ps c0 %g %g %g %g | c1 %g %g %g %g | c2 %g %g %g %g | c3 %g %g %g %g", n, c[0], c[1], c[2], c[3],
               c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
    }
    for (uint32_t s = 0; s < 16; s++) {
        void *tex = NULL;
        CALL(dev, D3D_IDX_IDirect3DDevice9_GetTexture, TFnGetObjIdx, s, &tex);
        if (!tex) continue;
        uint32_t type = CALL(tex, D3D_IDX_IDirect3DTexture9_GetType, TFnType);
        SurfDesc d;
        memset(&d, 0, sizeof d);
        if (type == 3 /* TEXTURE */) ((FnLevelDesc)D3D_VT(tex)[D3D_IDX_IDirect3DTexture9_GetLevelDesc])(tex, 0, &d);
        else if (type == 5 /* CUBE */) ((FnLevelDesc)D3D_VT(tex)[D3D_IDX_IDirect3DCubeTexture9_GetLevelDesc])(tex, 0, &d);
        uint32_t levels = CALL(tex, D3D_IDX_IDirect3DTexture9_GetLevelCount, TFnType);
        /* ADDRESSU 1, ADDRESSV 2, MAGFILTER 5, MINFILTER 6, MIPFILTER 7, MAXMIPLEVEL 9, SRGBTEXTURE 11 */
        static const uint32_t ss[] = {1, 2, 5, 6, 7, 9, 11};
        uint32_t sv[7];
        for (int i = 0; i < 7; i++) { sv[i] = 0; CALL(dev, D3D_IDX_IDirect3DDevice9_GetSamplerState, TFnGetU32U32, s, ss[i], &sv[i]); }
        int is_new;
        uint32_t id = trace_tex_id(tex, &is_new);
        rt_log("D3D9 trace %u:   s%u tex %u type %u fmt %#x (%.4s) %ux%u levels %u usage %#x pool %u | addr %u/%u filt %u/%u/%u maxmip %u srgb %u",
               n, s, id, type, d.format, d.format > 0xFF ? (const char *)&d.format : "", d.width, d.height, levels, d.usage, d.pool,
               sv[0], sv[1], sv[2], sv[3], sv[4], sv[5], sv[6]);
        if (is_new && trace_dump && type == 3) trace_dump_texture(tex, id, &d);
        CALL(tex, 2, DFnRelease);
    }
    if (ps) CALL(ps, 2, DFnRelease);
    if (vs) CALL(vs, 2, DFnRelease);
    if (rt) CALL(rt, 2, DFnRelease);
}
