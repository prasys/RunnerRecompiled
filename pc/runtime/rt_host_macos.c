/* Host layer for macOS (Apple Silicon): the same contract as rt_host_win.c and rt_host_android.c.

   - Memory: one 4 GB PROT_NONE mmap. The host page is 16 KB, the guest's 4 KB. The VM layer (rt_core.c) reserves
     in 64 KB granules and only ever releases whole regions, and VirtualProtect is a no-op, so the host rounds a
     commit out to 16 KB (mprotect read/write) and a decommit is a MAP_FIXED anonymous remap, which hands back
     fresh zero pages with no access. MADV_DONTNEED does not zero on macOS and is not used.
     See pc/spikes/macos-addrspace/README.md.
   - Faults: PROT_NONE accesses arrive as SIGBUS on macOS (SIGSEGV elsewhere); both are handled, on a
     per-thread alternate stack. The faulting pc comes from the arm64 thread state.
   - Locks are recursive (Windows critical sections are); timed waits use pthread_cond_timedwait_relative_np
     (macOS has no pthread_condattr_setclock).
   - Files: names are matched without regard to case when the exact one is missing (APFS is usually
     case-insensitive already; case-sensitive volumes exist).
   - Windows, input, audio: SDL3. Cocoa wants the event loop on the process's main thread, so main() here runs
     the SDL loop and starts the game (rt_main.c's main, renamed me_pc_main by the build) on a worker thread.
     SDL windows are created on the main thread on request (SDL_RunOnMainThread). The host window handle the
     game passes to Direct3D is the SDL_Window*, which is what DXVK Native's SDL back end expects.
   - Direct3D 9: DXVK Native (libdxvk_d3d9.dylib, loaded at run time) on MoltenVK. */
#ifdef __APPLE__
#include <SDL3/SDL.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"

#define HOST_PAGE 0x4000u /* arm64 macOS; the spike measured it with getpagesize() */

/* ---- memory ---- */
void *host_reserve(size_t size)
{
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int host_commit(void *p, size_t size)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(HOST_PAGE - 1), e = ((uintptr_t)p + size + HOST_PAGE - 1) & ~(uintptr_t)(HOST_PAGE - 1);
    return mprotect((void *)a, e - a, PROT_READ | PROT_WRITE) == 0;
}

int host_decommit(void *p, size_t size)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(HOST_PAGE - 1), e = ((uintptr_t)p + size + HOST_PAGE - 1) & ~(uintptr_t)(HOST_PAGE - 1);
    /* a fresh anonymous mapping over the range: zero pages, no access, memory returned to the system */
    return mmap((void *)a, e - a, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED;
}

/* ---- locks, time, threads ---- */
struct HostMutex { pthread_mutex_t m; };
struct HostCond { pthread_cond_t c; };

HostMutex *host_mutex_new(void)
{
    HostMutex *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

void host_lock(HostMutex *m) { pthread_mutex_lock(&m->m); }
void host_unlock(HostMutex *m) { pthread_mutex_unlock(&m->m); }

uint64_t host_real_ticks_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- hiding shader-compile stalls from the game ----
   DXVK (macOS patch) calls host_stall_hook(1) when the picture stalls on a pipeline compile or a warm-up hold
   and host_stall_hook(0) when it ends. The game then sees neither the time nor hears the audio of the stall:
   the guest clock (host_ticks_ns, host_filetime) stops while a stall is open, and the mixer is not pulled
   (silence) once a stall has lasted STALL_MUTE_NS, so a level that starts with a long cold compile resumes
   exactly where it was, the last presented frame (the loading movie) staying on screen meanwhile.
   A stall stays open STALL_GRACE_NS after the last compile ends, so a burst of compiles is one stall.
   ME_PC_STALL_HIDE=0 turns it off. */
#define STALL_GRACE_NS 150000000ull
#define STALL_MUTE_NS  100000000ull
static pthread_mutex_t st_mu = PTHREAD_MUTEX_INITIALIZER;
static int st_active, st_open, st_enabled = -1;
static uint64_t st_start, st_last_end, st_total, st_logged_ms;

static void st_settle(uint64_t now) /* st_mu held: close a stall whose grace period has passed */
{
    if (st_open && !st_active && now >= st_last_end + STALL_GRACE_NS) {
        st_total += st_last_end + STALL_GRACE_NS - st_start;
        st_open = 0;
        st_logged_ms = (st_last_end - st_start) / 1000000ull; /* logged by host_stall_hook: rt_log may read the clock */
    }
}

static uint64_t st_excluded(uint64_t now) /* st_mu held */
{
    st_settle(now);
    if (!st_open) return st_total;
    uint64_t end = st_active ? now : st_last_end + STALL_GRACE_NS;
    if (end > now) end = now;
    return st_total + (end - st_start);
}

void host_stall_hook(int begin)
{
    if (st_enabled < 0) { const char *e = getenv("ME_PC_STALL_HIDE"); st_enabled = !(e && *e == '0'); }
    if (!st_enabled) return;
    uint64_t now = host_real_ticks_ns();
    pthread_mutex_lock(&st_mu);
    st_settle(now);
    if (begin) {
        if (!st_open) { st_open = 1; st_start = now; }
        st_active++;
    } else if (st_active > 0) {
        st_active--;
        st_last_end = now;
    }
    uint64_t logms = st_logged_ms;
    st_logged_ms = 0;
    pthread_mutex_unlock(&st_mu);
    if (logms && getenv("ME_PC_LOG_STALLS")) rt_log("stall hidden from the game: %llu ms", (unsigned long long)logms);
}

static int host_stall_audio_muted(void)
{
    if (!st_open) return 0; /* cheap unlocked test first; st_open is only a hint here */
    uint64_t now = host_real_ticks_ns();
    pthread_mutex_lock(&st_mu);
    st_settle(now);
    int m = st_open && now - st_start >= STALL_MUTE_NS;
    pthread_mutex_unlock(&st_mu);
    return m;
}

uint64_t host_ticks_ns(void)
{
    uint64_t now = host_real_ticks_ns();
    pthread_mutex_lock(&st_mu);
    uint64_t v = now - st_excluded(now);
    pthread_mutex_unlock(&st_mu);
    return v;
}

uint64_t host_filetime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    uint64_t now = host_real_ticks_ns();
    pthread_mutex_lock(&st_mu);
    uint64_t ex = st_excluded(now);
    pthread_mutex_unlock(&st_mu);
    return ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100u - ex / 100u;
}

void host_sleep_ms(uint32_t ms)
{
    if (!ms) { sched_yield(); return; }
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

static uint32_t self_tid(void)
{
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return (uint32_t)t;
}

uint32_t host_thread_id(void) { return self_tid(); }

HostCond *host_cond_new(void)
{
    HostCond *c = malloc(sizeof *c);
    pthread_cond_init(&c->c, NULL);
    return c;
}

int host_cond_wait(HostCond *c, HostMutex *m, uint32_t timeout_ms)
{
    if (timeout_ms == 0xFFFFFFFFu) return pthread_cond_wait(&c->c, &m->m) == 0;
    struct timespec rel = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
    return pthread_cond_timedwait_relative_np(&c->c, &m->m, &rel) == 0;
}

void host_cond_broadcast(HostCond *c) { pthread_cond_broadcast(&c->c); }

/* every thread that runs guest code gets an alternate stack: a guest stack overflow is one of the faults */
static void set_alt_stack(void)
{
    stack_t ss = {.ss_sp = malloc(64 * 1024), .ss_size = 64 * 1024};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}

typedef struct { void (*fn)(void *); void *arg; } ThreadStart;

static void *thread_entry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    set_alt_stack();
    s.fn(s.arg);
    return NULL;
}

int host_thread_start(void (*fn)(void *), void *arg)
{
    ThreadStart *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int ok = pthread_create(&t, &a, thread_entry, s) == 0;
    pthread_attr_destroy(&a);
    if (!ok) free(s);
    return ok;
}

/* ---- faults: report the guest address and the last dispatch, then stop ---- */
uint32_t rt_self_last_target(void), rt_self_last_ret(void); /* rt_core.c: the faulting thread's last dispatch */

/* Host-side crash report: thread name and a frame-pointer walk with symbols (best effort, we are crashing anyway). */
static void fault_backtrace(void *uc)
{
    ucontext_t *c = (ucontext_t *)uc;
    char name[64] = "?";
    pthread_getname_np(pthread_self(), name, sizeof name);
    rt_log("fault thread: \"%s\"", name);
    uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(c->uc_mcontext->__ss);
    uintptr_t lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(c->uc_mcontext->__ss);
    uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(c->uc_mcontext->__ss);
    uintptr_t frames[24];
    int n = 0;
    frames[n++] = pc;
    frames[n++] = lr;
    while (n < 24 && fp && !(fp & 15)) {
        uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
        if (!ret) break;
        frames[n++] = ret;
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    for (int i = 0; i < n; i++) {
        Dl_info d;
        if (dladdr((void *)frames[i], &d))
            rt_log("  #%d %p %s + %lu (%s)", i, (void *)frames[i], d.dli_sname ? d.dli_sname : "?",
                   (unsigned long)(frames[i] - (uintptr_t)(d.dli_saddr ? d.dli_saddr : d.dli_fbase)),
                   d.dli_fname ? strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname : "?");
        else
            rt_log("  #%d %p", i, (void *)frames[i]);
    }
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    uintptr_t a = (uintptr_t)si->si_addr, base = x86_mem_base;
    uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(((ucontext_t *)uc)->uc_mcontext->__ss);
    Dl_info info;
    uintptr_t image = dladdr((void *)pc, &info) ? (uintptr_t)info.dli_fbase : 0;
    if (a >= base && a - base < 0x100000000ull)
        rt_fatal("host signal %d: guest address %#x, host pc %p (image +%#lx); last dispatch %#x (return %#x)", sig,
                 (uint32_t)(a - base), (void *)pc, (unsigned long)(pc - image), rt_self_last_target(), rt_self_last_ret());
    extern const char *rt_thunk_name(uint32_t t);
    fault_backtrace(uc);
    rt_fatal("host signal %d at host address %p, pc %p (image +%#lx); last dispatch %#x %s (return %#x)", sig, (void *)a,
             (void *)pc, (unsigned long)(pc - image), rt_self_last_target(), rt_thunk_name(rt_self_last_target()), rt_self_last_ret());
}

void host_install_fault_handler(void)
{
    set_alt_stack();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/* ---- Direct3D 9: DXVK Native on MoltenVK ---- */
void host_wait_window(void) {} /* the SDL window exists before the device is created */

static void *dxvk_lib;

/* Tells DXVK's pipeline warm-up which map is loading (macOS DXVK patch; an ordinary DXVK has no such symbol). */
void host_warm_set_map(const char *name)
{
    static void (*fn)(const char *);
    if (!fn && dxvk_lib) fn = (void (*)(const char *))dlsym(dxvk_lib, "dxvk_warm_set_map");
    if (fn) fn(name);
}

void *host_d3d9_create(uint32_t sdk_version)
{
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    const char *log = getenv("ME_PC_LOG_PATH");
    if (log && !getenv("DXVK_LOG_PATH")) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s", log);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; setenv("DXVK_LOG_PATH", dir, 0); }
    }
    /* the Vulkan loader finds MoltenVK through its ICD file (Homebrew: molten-vk) */
    char icdb[1024];
    const char *icd = "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json";
    const char *bundle = getenv("ME_PC_BUNDLE"); /* the launcher's Resources/vendor dir: MoltenVK, Vulkan loader, SDL3 */
    if (bundle) {
        snprintf(icdb, sizeof icdb, "%s/vulkan/icd.d/MoltenVK_icd.json", bundle);
        if (access(icdb, R_OK) == 0) icd = icdb;
    }
    if (!getenv("VK_ICD_FILENAMES") && !getenv("VK_DRIVER_FILES") && access(icd, R_OK) == 0) setenv("VK_ICD_FILENAMES", icd, 0);
    const char *want = getenv("ME_PC_DXVK");
    void *lib = want ? dlopen(want, RTLD_NOW) : NULL;
    if (!lib) lib = dlopen("libdxvk_d3d9.dylib", RTLD_NOW);
    if (!lib) { rt_log("Direct3D 9: libdxvk_d3d9.dylib not available (%s); set ME_PC_DXVK to its path", dlerror()); return NULL; }
    dxvk_lib = lib;
    void (*set_hold)(void (*)(int)) = (void (*)(void (*)(int)))dlsym(lib, "dxvk_warm_set_hold_callback");
    if (set_hold) set_hold(host_stall_hook); /* absent in an ordinary DXVK */
    void *(*create)(uint32_t) = (void *(*)(uint32_t))dlsym(lib, "Direct3DCreate9");
    return create ? create(sdk_version) : NULL;
}

/* ---- windows and messages ---- */
typedef struct { void *hwnd; uint32_t msg, wp, lp, time; } HostMsg;
typedef struct MsgQueue {
    uint32_t tid;
    HostMsg q[512];
    int head, n;
    struct MsgQueue *next;
} MsgQueue;

static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static MsgQueue *queues;
static __thread MsgQueue *my_queue;

typedef struct { SDL_Window *win; uint32_t owner; int w, h; } HostWindow;
#define MAX_WINDOWS 16
static HostWindow windows[MAX_WINDOWS];
static atomic_int app_in_front = 1;

static MsgQueue *queue_of(uint32_t tid)
{
    for (MsgQueue *q = queues; q; q = q->next)
        if (q->tid == tid) return q;
    MsgQueue *q = calloc(1, sizeof *q);
    q->tid = tid;
    q->next = queues;
    queues = q;
    return q;
}

static int post_to(uint32_t tid, void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    pthread_mutex_lock(&mq_lock);
    MsgQueue *q = queue_of(tid);
    int ok = q->n < (int)(sizeof q->q / sizeof q->q[0]);
    if (ok) {
        HostMsg *m = &q->q[(q->head + q->n) % (int)(sizeof q->q / sizeof q->q[0])];
        m->hwnd = hwnd; m->msg = msg; m->wp = wp; m->lp = lp; m->time = (uint32_t)(host_ticks_ns() / 1000000u);
        q->n++;
    }
    pthread_mutex_unlock(&mq_lock);
    return ok;
}

static HostWindow *window_of(void *hwnd)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].win && windows[i].win == hwnd) return &windows[i];
    return NULL;
}

typedef struct { int index, w, h; const char *title; SDL_Window *out; } CreateReq;

static void create_on_main(void *p)
{
    CreateReq *r = p;
    /* the first window is the game; the others (helpers the engine makes) stay hidden */
    SDL_WindowFlags fl = SDL_WINDOW_VULKAN | (r->index == 0 ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_HIDDEN);
    r->out = SDL_CreateWindow(r->title && *r->title ? r->title : "Mirror's Edge", r->w, r->h, fl);
    if (!r->out) rt_log("SDL_CreateWindow failed: %s", SDL_GetError());
}

void *host_window_create(int w, int h, const char *title)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].win) {
            CreateReq r = {i, w > 0 ? w : 1280, h > 0 ? h : 720, title, NULL};
            if (!SDL_RunOnMainThread(create_on_main, &r, true) || !r.out) return NULL;
            windows[i].win = r.out;
            windows[i].owner = self_tid();
            windows[i].w = r.w;
            windows[i].h = r.h;
            rt_log("window %d: %dx%d (%s)", i, r.w, r.h, i == 0 ? "the game window" : "hidden");
            return r.out;
        }
    return NULL;
}

int host_peek_message(void **hwnd, uint32_t *msg, uint32_t *wp, uint32_t *lp, uint32_t *time, int32_t *x, int32_t *y,
                      int remove)
{
    pthread_mutex_lock(&mq_lock);
    if (!my_queue) my_queue = queue_of(self_tid());
    MsgQueue *q = my_queue;
    int have = q->n > 0;
    if (have) {
        HostMsg *m = &q->q[q->head];
        *hwnd = m->hwnd; *msg = m->msg; *wp = m->wp; *lp = m->lp; *time = m->time;
        *x = (int32_t)(int16_t)(m->lp & 0xFFFF);
        *y = (int32_t)(int16_t)(m->lp >> 16);
        if (remove) { q->head = (q->head + 1) % (int)(sizeof q->q / sizeof q->q[0]); q->n--; }
    }
    pthread_mutex_unlock(&mq_lock);
    return have;
}

void host_translate_last(void) {}
void host_dispatch_last(void) {}
void host_validate(void *hwnd) { (void)hwnd; }
uint32_t host_def_window_proc(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { (void)hwnd; (void)msg; (void)wp; (void)lp; return 0; }

static void show_on_main(void *p)
{
    SDL_Window *w = ((void **)p)[0];
    if (((intptr_t *)p)[1]) SDL_ShowWindow(w); else SDL_HideWindow(w);
}

void host_window_show(void *hwnd, int show)
{
    HostWindow *w = window_of(hwnd);
    if (!w || w != &windows[0]) return; /* only the game window is ever shown or hidden by the game */
    void *req[2] = {w->win, (void *)(intptr_t)show};
    SDL_RunOnMainThread(show_on_main, req, true);
}

/* The guest's client size. The SDL window may be any size (the user resizes or goes full screen; DXVK scales
   the backbuffer to it), so mouse positions are scaled back to the guest's size. */
static void set_size_on_main(void *p)
{
    SDL_Window *w = ((void **)p)[0];
    int sz[2] = {(int)(intptr_t)((void **)p)[1], (int)(intptr_t)((void **)p)[2]};
    /* ME_PC_RENDER_SCALE=<0.25..1>: the game renders at this fraction of the window (internal resolution); DXVK scales
       the backbuffer up to the window. 1 (default) = the window is the game's size. */
    static float scale = -1.0f;
    if (scale < 0) {
        const char *e = getenv("ME_PC_RENDER_SCALE");
        scale = e ? (float)atof(e) : 1.0f;
        if (scale < 0.25f || scale > 1.0f) scale = 1.0f;
        if (scale != 1.0f) rt_log("render scale %.2f: the game renders at %.0f%% of the window", scale, scale * 100);
    }
    if (scale != 1.0f) { sz[0] = (int)(sz[0] / scale + 0.5f); sz[1] = (int)(sz[1] / scale + 0.5f); }
    {   /* never ask for a window bigger than its display: 3024x1964 at scale 0.5 asked for 6048x3928, a drawable the GPU
           cannot back (the kernel panic in IOGPUFamily seen with it is probably this) */
        SDL_Rect r;
        if (SDL_GetDisplayBounds(SDL_GetDisplayForWindow(w), &r) && r.w > 0 && r.h > 0 && (sz[0] > r.w || sz[1] > r.h)) {
            float f = fminf((float)r.w / sz[0], (float)r.h / sz[1]);
            sz[0] = (int)(sz[0] * f); sz[1] = (int)(sz[1] * f);
        }
    }
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(w, sz[0], sz[1]);
    /* ME_PC_FULLSCREEN=1: borderless full-screen window on the first resize (Cmd+Return toggles it later) */
    static int fs_done;
    const char *fs = getenv("ME_PC_FULLSCREEN");
    if (!fs_done && fs && *fs == '1') { fs_done = 1; SDL_SetWindowFullscreen(w, true); }
}

void host_window_set_size(void *hwnd, int w, int h)
{
    HostWindow *hw = window_of(hwnd);
    if (!hw || hw != &windows[0] || w <= 0 || h <= 0) return;
    if (hw->w == w && hw->h == h) return;
    hw->w = w; hw->h = h;
    void *req[3] = {hw->win, (void *)(intptr_t)w, (void *)(intptr_t)h};
    SDL_RunOnMainThread(set_size_on_main, req, true);
    rt_log("window 0 resized to %dx%d", w, h);
}

static void scale_to_guest(float x, float y, int *ox, int *oy)
{
    int cw = 0, ch = 0;
    if (windows[0].win) SDL_GetWindowSize(windows[0].win, &cw, &ch);
    *ox = cw > 0 && windows[0].w > 0 ? (int)(x * (float)windows[0].w / (float)cw) : (int)x;
    *oy = ch > 0 && windows[0].h > 0 ? (int)(y * (float)windows[0].h / (float)ch) : (int)y;
}

int host_post_message(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    HostWindow *w = window_of(hwnd);
    return post_to(w ? w->owner : self_tid(), hwnd, msg, wp, lp);
}

int host_post_thread_message(uint32_t tid, uint32_t msg, uint32_t wp, uint32_t lp) { return post_to(tid, NULL, msg, wp, lp); }
void host_post_quit(int code) { post_to(self_tid(), NULL, 0x0012, (uint32_t)code, 0); }
void *host_focus_window(void) { return atomic_load(&app_in_front) && windows[0].win && windows[0].owner == self_tid() ? windows[0].win : NULL; }
void *host_foreground_window(void) { return atomic_load(&app_in_front) && windows[0].win ? windows[0].win : NULL; }

/* ---- keyboard: SDL scancode -> Windows virtual key, scan code (set 1), extended flag ---- */
typedef struct { SDL_Scancode sc; uint8_t vk, scan, ext; } KeyMap;
static const KeyMap keymap[] = {
    {SDL_SCANCODE_A, 'A', 0x1E, 0}, {SDL_SCANCODE_B, 'B', 0x30, 0}, {SDL_SCANCODE_C, 'C', 0x2E, 0}, {SDL_SCANCODE_D, 'D', 0x20, 0},
    {SDL_SCANCODE_E, 'E', 0x12, 0}, {SDL_SCANCODE_F, 'F', 0x21, 0}, {SDL_SCANCODE_G, 'G', 0x22, 0}, {SDL_SCANCODE_H, 'H', 0x23, 0},
    {SDL_SCANCODE_I, 'I', 0x17, 0}, {SDL_SCANCODE_J, 'J', 0x24, 0}, {SDL_SCANCODE_K, 'K', 0x25, 0}, {SDL_SCANCODE_L, 'L', 0x26, 0},
    {SDL_SCANCODE_M, 'M', 0x32, 0}, {SDL_SCANCODE_N, 'N', 0x31, 0}, {SDL_SCANCODE_O, 'O', 0x18, 0}, {SDL_SCANCODE_P, 'P', 0x19, 0},
    {SDL_SCANCODE_Q, 'Q', 0x10, 0}, {SDL_SCANCODE_R, 'R', 0x13, 0}, {SDL_SCANCODE_S, 'S', 0x1F, 0}, {SDL_SCANCODE_T, 'T', 0x14, 0},
    {SDL_SCANCODE_U, 'U', 0x16, 0}, {SDL_SCANCODE_V, 'V', 0x2F, 0}, {SDL_SCANCODE_W, 'W', 0x11, 0}, {SDL_SCANCODE_X, 'X', 0x2D, 0},
    {SDL_SCANCODE_Y, 'Y', 0x15, 0}, {SDL_SCANCODE_Z, 'Z', 0x2C, 0},
    {SDL_SCANCODE_1, '1', 0x02, 0}, {SDL_SCANCODE_2, '2', 0x03, 0}, {SDL_SCANCODE_3, '3', 0x04, 0}, {SDL_SCANCODE_4, '4', 0x05, 0},
    {SDL_SCANCODE_5, '5', 0x06, 0}, {SDL_SCANCODE_6, '6', 0x07, 0}, {SDL_SCANCODE_7, '7', 0x08, 0}, {SDL_SCANCODE_8, '8', 0x09, 0},
    {SDL_SCANCODE_9, '9', 0x0A, 0}, {SDL_SCANCODE_0, '0', 0x0B, 0},
    {SDL_SCANCODE_RETURN, 0x0D, 0x1C, 0}, {SDL_SCANCODE_ESCAPE, 0x1B, 0x01, 0}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E, 0},
    {SDL_SCANCODE_TAB, 0x09, 0x0F, 0}, {SDL_SCANCODE_SPACE, 0x20, 0x39, 0},
    {SDL_SCANCODE_MINUS, 0xBD, 0x0C, 0}, {SDL_SCANCODE_EQUALS, 0xBB, 0x0D, 0}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A, 0},
    {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B, 0}, {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B, 0}, {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27, 0},
    {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28, 0}, {SDL_SCANCODE_GRAVE, 0xC0, 0x29, 0}, {SDL_SCANCODE_COMMA, 0xBC, 0x33, 0},
    {SDL_SCANCODE_PERIOD, 0xBE, 0x34, 0}, {SDL_SCANCODE_SLASH, 0xBF, 0x35, 0},
    {SDL_SCANCODE_F1, 0x70, 0x3B, 0}, {SDL_SCANCODE_F2, 0x71, 0x3C, 0}, {SDL_SCANCODE_F3, 0x72, 0x3D, 0}, {SDL_SCANCODE_F4, 0x73, 0x3E, 0},
    {SDL_SCANCODE_F5, 0x74, 0x3F, 0}, {SDL_SCANCODE_F6, 0x75, 0x40, 0}, {SDL_SCANCODE_F7, 0x76, 0x41, 0}, {SDL_SCANCODE_F8, 0x77, 0x42, 0},
    {SDL_SCANCODE_F9, 0x78, 0x43, 0}, {SDL_SCANCODE_F10, 0x79, 0x44, 0}, {SDL_SCANCODE_F11, 0x7A, 0x57, 0}, {SDL_SCANCODE_F12, 0x7B, 0x58, 0},
    {SDL_SCANCODE_LSHIFT, 0xA0, 0x2A, 0}, {SDL_SCANCODE_RSHIFT, 0xA1, 0x36, 0}, {SDL_SCANCODE_LCTRL, 0xA2, 0x1D, 0},
    {SDL_SCANCODE_RCTRL, 0xA3, 0x1D, 1}, {SDL_SCANCODE_LALT, 0xA4, 0x38, 0}, {SDL_SCANCODE_RALT, 0xA5, 0x38, 1},
    {SDL_SCANCODE_UP, 0x26, 0x48, 1}, {SDL_SCANCODE_DOWN, 0x28, 0x50, 1}, {SDL_SCANCODE_LEFT, 0x25, 0x4B, 1}, {SDL_SCANCODE_RIGHT, 0x27, 0x4D, 1},
    {SDL_SCANCODE_INSERT, 0x2D, 0x52, 1}, {SDL_SCANCODE_DELETE, 0x2E, 0x53, 1}, {SDL_SCANCODE_HOME, 0x24, 0x47, 1},
    {SDL_SCANCODE_END, 0x23, 0x4F, 1}, {SDL_SCANCODE_PAGEUP, 0x21, 0x49, 1}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, 1},
    {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A, 0},
};

static void post_key(SDL_Scancode sc, int down, int repeat)
{
    if (!windows[0].win) return;
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (keymap[i].sc == sc) {
            uint32_t lp = 1u | (uint32_t)keymap[i].scan << 16 | (keymap[i].ext ? 1u << 24 : 0) | (repeat ? 1u << 30 : 0) | (down ? 0 : 0xC0000000u);
            post_to(windows[0].owner, windows[0].win, down ? 0x0100 : 0x0101, keymap[i].vk, lp);
            return;
        }
}

/* ---- mouse: window messages for menus, and relative motion for looking around ---- */
static atomic_int mouse_dx, mouse_dy;
static atomic_int mouse_down[3];
static int mouse_capture_wanted = 1;

int host_mouse_poll(int32_t *dx, int32_t *dy, uint8_t buttons[3])
{
    if (!mouse_capture_wanted || !atomic_load(&app_in_front)) return 0;
    *dx = atomic_exchange(&mouse_dx, 0);
    *dy = atomic_exchange(&mouse_dy, 0);
    for (int i = 0; i < 3; i++) buttons[i] = atomic_load(&mouse_down[i]) ? 0x80 : 0;
    return 1;
}

static void post_mouse_button(int button, int down, int x, int y)
{
    static const uint32_t msgs[3][2] = {{0x0202, 0x0201}, {0x0205, 0x0204}, {0x0208, 0x0207}};
    if (windows[0].win && button >= 0 && button < 3)
        post_to(windows[0].owner, windows[0].win, msgs[button][down ? 1 : 0], 0, (uint32_t)(x & 0xFFFF) | (uint32_t)y << 16);
}

/* ---- XInput pads from SDL gamepads ---- */
static SDL_Gamepad *pads[4];
static uint8_t pad_state[4][16];
static uint16_t pad_rumble[4][2];

static void pad_add(SDL_JoystickID id)
{
    for (int i = 0; i < 4; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == id) return;
    for (int i = 0; i < 4; i++)
        if (!pads[i]) { pads[i] = SDL_OpenGamepad(id); rt_log("gamepad %d: %s", i, pads[i] ? SDL_GetGamepadName(pads[i]) : "open failed"); return; }
}

static void pad_remove(SDL_JoystickID id)
{
    for (int i = 0; i < 4; i++)
        if (pads[i] && SDL_GetGamepadID(pads[i]) == id) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
}

static int16_t axis_flip(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; } /* SDL: +Y down; XInput: +Y up */

/* ME_PC_PAD_MAP="A=B,B=A,LB=RB": the XInput button a physical button is reported as (names: A B X Y LB RB LS RS START
   BACK UP DOWN LEFT RIGHT). ME_PC_PAD_INVERT_Y=1 flips the right stick's vertical axis (look). */
static const struct { const char *name; uint16_t bit; } xbtn[] = {
    {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010}, {"BACK", 0x0020},
    {"LS", 0x0040}, {"RS", 0x0080}, {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
};
static uint16_t pad_remap[16]; /* bit index -> reported bit; 0 = unmapped (identity) */
static int pad_invert_y;

static int xbtn_bit(const char *s, size_t n)
{
    for (size_t i = 0; i < sizeof xbtn / sizeof xbtn[0]; i++)
        if (strlen(xbtn[i].name) == n && !strncasecmp(xbtn[i].name, s, n)) return __builtin_ctz(xbtn[i].bit);
    return -1;
}

static void pad_config_init(void)
{
    const char *e = getenv("ME_PC_PAD_MAP");
    for (; e && *e; ) {
        const char *eq = strchr(e, '='), *comma = strchr(e, ',');
        const char *end = comma ? comma : e + strlen(e);
        if (eq && eq < end) {
            int from = xbtn_bit(e, (size_t)(eq - e)), to = xbtn_bit(eq + 1, (size_t)(end - eq - 1));
            if (from >= 0 && to >= 0) { pad_remap[from] = (uint16_t)(1u << to); rt_log("pad map: %.*s -> %.*s", (int)(eq - e), e, (int)(end - eq - 1), eq + 1); }
            else rt_log("ME_PC_PAD_MAP: cannot read '%.*s'", (int)(end - e), e);
        }
        e = comma ? comma + 1 : end;
    }
    const char *iy = getenv("ME_PC_PAD_INVERT_Y");
    pad_invert_y = iy && *iy == '1';
}

static void pads_refresh(void)
{
    static const struct { SDL_GamepadButton b; uint16_t bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008}, {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000}, {SDL_GAMEPAD_BUTTON_WEST, 0x4000},
        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    for (int i = 0; i < 4; i++) {
        if (!pads[i]) continue;
        uint8_t s[16];
        uint16_t buttons = 0;
        for (size_t k = 0; k < sizeof map / sizeof map[0]; k++)
            if (SDL_GetGamepadButton(pads[i], map[k].b)) {
                int bi = __builtin_ctz(map[k].bit);
                buttons |= pad_remap[bi] ? pad_remap[bi] : map[k].bit;
            }
        int lt = SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        int16_t lx = SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTX), ly = axis_flip(SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTY));
        int16_t rx = SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_RIGHTX), ry = axis_flip(SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_RIGHTY));
        if (pad_invert_y) ry = axis_flip(ry);
        memcpy(s + 4, &buttons, 2);
        s[6] = (uint8_t)(lt < 0 ? 0 : lt >> 7);
        s[7] = (uint8_t)(rt < 0 ? 0 : rt >> 7);
        memcpy(s + 8, &lx, 2); memcpy(s + 10, &ly, 2); memcpy(s + 12, &rx, 2); memcpy(s + 14, &ry, 2);
        if (memcmp(s + 4, pad_state[i] + 4, 12)) { /* the packet number moves when the state does */
            uint32_t packet;
            memcpy(&packet, pad_state[i], 4);
            packet++;
            memcpy(s, &packet, 4);
            memcpy(pad_state[i], s, 16);
        }
    }
}

uint32_t host_pad_get(uint32_t index, uint8_t state[16])
{
    if (index >= 4 || !pads[index]) return 1167u; /* ERROR_DEVICE_NOT_CONNECTED */
    memcpy(state, pad_state[index], 16);
    return 0;
}

uint32_t host_pad_rumble(uint32_t index, uint16_t left, uint16_t right)
{
    if (index >= 4 || !pads[index]) return 1167u;
    pad_rumble[index][0] = left;
    pad_rumble[index][1] = right;
    SDL_RumbleGamepad(pads[index], left, right, 1000);
    return 0;
}

/* ---- files: case-insensitive resolution of every component ---- */
/* Resolves path into out. Components that exist under another case are replaced; a missing
   component (and everything after it) is kept as given, so creating a new file still works.
   Returns 1 when the whole path exists. */
static int resolve_ci(const char *path, char *out, size_t n)
{
    struct stat st;
    if (stat(path, &st) == 0) { snprintf(out, n, "%s", path); return 1; }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    size_t o = 0;
    out[0] = 0;
    char *p = buf;
    if (*p == '/') { out[o++] = '/'; out[o] = 0; p++; }
    int exists = 1;
    while (*p) {
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        char cand[1024];
        snprintf(cand, sizeof cand, "%s%s", out, p);
        if (exists && stat(cand, &st) != 0) {
            exists = 0;
            DIR *d = opendir(o ? out : ".");
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)))
                    if (!strcasecmp(e->d_name, p)) { snprintf(cand, sizeof cand, "%s%s", out, e->d_name); exists = 1; break; }
                closedir(d);
            }
        }
        o = (size_t)snprintf(out, n, "%s", cand);
        if (!slash) break;
        if (o + 1 < n) { out[o++] = '/'; out[o] = 0; }
        p = slash + 1;
    }
    return exists;
}

int host_open(const char *path, int write, int create, int truncate)
{
    char r[1024];
    resolve_ci(path, r, sizeof r);
    int fl = (write ? O_RDWR : O_RDONLY) | (create ? O_CREAT : 0) | (truncate ? O_TRUNC : 0) | O_CLOEXEC;
    return open(r, fl, 0664);
}

int64_t host_read(int fd, void *buf, uint32_t n) { return read(fd, buf, n); }
int64_t host_write(int fd, const void *buf, uint32_t n) { return write(fd, buf, n); }
int64_t host_seek(int fd, int64_t off, int whence) { return lseek(fd, off, whence); }
int host_truncate(int fd, int64_t size) { return ftruncate(fd, size) == 0; }
void host_close(int fd) { close(fd); }

int host_stat(const char *path, int64_t *size, uint64_t *filetime)
{
    char r[1024];
    if (!resolve_ci(path, r, sizeof r)) return 0;
    struct stat st;
    if (stat(r, &st) != 0) return 0;
    if (size) *size = st.st_size;
    if (filetime) *filetime = ((uint64_t)st.st_mtimespec.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)st.st_mtimespec.tv_nsec / 100u;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

int host_mkdir(const char *path) { char r[1024]; resolve_ci(path, r, sizeof r); return mkdir(r, 0775) == 0; }
int host_rmdir(const char *path) { char r[1024]; resolve_ci(path, r, sizeof r); return rmdir(r) == 0; }
int host_unlink(const char *path) { char r[1024]; resolve_ci(path, r, sizeof r); return unlink(r) == 0; }

int host_rename(const char *from, const char *to)
{
    char a[1024], b[1024];
    resolve_ci(from, a, sizeof a);
    resolve_ci(to, b, sizeof b);
    return rename(a, b) == 0;
}

int host_list_dir(const char *path, void (*fn)(void *u, const char *name), void *u)
{
    char r[1024];
    if (!resolve_ci(path, r, sizeof r)) return 0;
    DIR *d = opendir(r);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) fn(u, e->d_name);
    closedir(d);
    return 1;
}

/* profiling: Instruments / `sample` work on the process; nothing built in yet */
void host_profile_start(void) {}

/* ---- audio: an SDL3 playback stream, the runtime's mixer as the source ---- */
static void (*audio_mix)(int16_t *out, uint32_t frames);

static void SDLCALL audio_cb(void *user, SDL_AudioStream *stream, int additional, int total)
{
    (void)user; (void)total;
    int16_t buf[2 * 2048];
    while (additional > 0) {
        uint32_t frames = (uint32_t)additional / 4u;
        if (frames > 2048) frames = 2048;
        if (!frames) break;
        if (host_stall_audio_muted()) memset(buf, 0, frames * 4); /* a compile stall: the game's audio waits too */
        else audio_mix(buf, frames);
        SDL_PutAudioStreamData(stream, buf, (int)(frames * 4));
        additional -= (int)(frames * 4);
    }
}

int host_audio_start(uint32_t rate, void (*mix)(int16_t *out, uint32_t frames))
{
    audio_mix = mix;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)rate};
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!s) { rt_log("audio: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError()); return 0; }
    return SDL_ResumeAudioStreamDevice(s);
}

/* ---- the process: the SDL event loop on the main thread, the game on a worker ---- */
int me_pc_main(int argc, char **argv); /* rt_main.c's main, renamed by the build */

typedef struct { int argc; char **argv; int code; } GameArgs;
static atomic_int game_done;

static void *game_thread(void *p)
{
    GameArgs *g = p;
    g->code = me_pc_main(g->argc, g->argv);
    atomic_store(&game_done, 1);
    return NULL;
}

static int auto_input_on; /* ME_PC_AUTO_INPUT, below */

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (windows[0].win) post_to(windows[0].owner, windows[0].win, 0x0010 /* WM_CLOSE */, 0, 0);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_GUI) && windows[0].win) { /* Cmd+Return */
            SDL_SetWindowFullscreen(windows[0].win, !(SDL_GetWindowFlags(windows[0].win) & SDL_WINDOW_FULLSCREEN));
            break;
        }
        post_key(e->key.scancode, 1, e->key.repeat);
        break;
    case SDL_EVENT_KEY_UP: post_key(e->key.scancode, 0, 0); break;
    case SDL_EVENT_MOUSE_MOTION:
        atomic_fetch_add(&mouse_dx, (int)e->motion.xrel);
        atomic_fetch_add(&mouse_dy, (int)e->motion.yrel);
        if (windows[0].win) {
            int gx, gy;
            scale_to_guest(e->motion.x, e->motion.y, &gx, &gy);
            post_to(windows[0].owner, windows[0].win, 0x0200, 0, (uint32_t)(gx & 0xFFFF) | (uint32_t)gy << 16);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b >= 0) {
            atomic_store(&mouse_down[b], e->button.down ? 1 : 0);
            int gx, gy;
            scale_to_guest(e->button.x, e->button.y, &gx, &gy);
            post_mouse_button(b, e->button.down, gx, gy);
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        atomic_store(&app_in_front, 1);
        if (mouse_capture_wanted && !auto_input_on && windows[0].win) SDL_SetWindowRelativeMouseMode(windows[0].win, true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (auto_input_on) break; /* scripted run: stays "in front" */
        atomic_store(&app_in_front, 0);
        if (windows[0].win) SDL_SetWindowRelativeMouseMode(windows[0].win, false);
        break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_add(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_remove(e->gdevice.which); break;
    default: break;
    }
}

/* ME_PC_AUTO_KEYS=40,45: taps Return that many seconds after start (up to 8 taps), so an unattended run can get past
   "Press Any Key" to the main menu (for frame dumps). Combine with ME_PC_ASSUME_FOCUS=1 when the window is not in front. */
static double auto_keys[8];
static int auto_key_count, auto_key_next;

static void auto_keys_init(void)
{
    const char *e = getenv("ME_PC_AUTO_KEYS");
    while (e && *e && auto_key_count < 8) {
        char *end;
        double v = strtod(e, &end);
        if (end == e) break;
        auto_keys[auto_key_count++] = v;
        e = *end == ',' ? end + 1 : end;
    }
}

static void auto_keys_poll(uint64_t start_ns)
{
    if (auto_key_next >= auto_key_count) return;
    if ((double)(SDL_GetTicksNS() - start_ns) / 1e9 < auto_keys[auto_key_next]) return;
    auto_key_next++;
    fprintf(stderr, "ME_PC_AUTO_KEYS: Return\n");
    post_key(SDL_SCANCODE_RETURN, 1, 0);
    post_key(SDL_SCANCODE_RETURN, 0, 0);
}

/* ME_PC_AUTO_INPUT: scripted input for unattended tests, keyed to the D3D9 frame counter. Entries separated by ';':
     <frame>:mouse=<dx>,<dy>,<frames>   relative mouse motion spread evenly over that many frames
     <frame>:down=<key> / <frame>:up=<key>   key by SDL name ("W", "Space", "Left Shift", "Return")
   Motion goes through handle_event like real SDL motion. While a script is set the window counts as in front but the
   real cursor is not captured, so the run can go on in the background. */
uint32_t d3d_frames_presented(void); /* rt_d3d9.c */
typedef struct { uint32_t frame; int kind; int dx, dy, frames; SDL_Scancode sc; } AutoInput;
static AutoInput auto_in[64];
static int auto_in_count, auto_in_next;
static struct { int dx, dy, left; double ax, ay; } auto_motion;
static uint32_t auto_last_frame;

static void auto_input_init(void)
{
    const char *e = getenv("ME_PC_AUTO_INPUT");
    if (!e || !*e) return;
    auto_input_on = 1;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, ";"); tok && auto_in_count < 64; tok = strtok(NULL, ";")) {
        AutoInput a = {0};
        char act[64] = {0}, arg[64] = {0};
        if (sscanf(tok, "%u:%63[^=]=%63[^;]", &a.frame, act, arg) != 3) continue;
        if (!strcmp(act, "mouse")) { a.kind = 0; if (sscanf(arg, "%d,%d,%d", &a.dx, &a.dy, &a.frames) < 2) continue; if (a.frames < 1) a.frames = 1; }
        else if (!strcmp(act, "down") || !strcmp(act, "up")) { a.kind = act[0] == 'd' ? 1 : 2; a.sc = SDL_GetScancodeFromName(arg); if (a.sc == SDL_SCANCODE_UNKNOWN) continue; }
        else continue;
        auto_in[auto_in_count++] = a;
    }
    fprintf(stderr, "ME_PC_AUTO_INPUT: %d actions\n", auto_in_count);
}

static void auto_input_poll(void)
{
    if (!auto_input_on) return;
    uint32_t f = d3d_frames_presented();
    if (f == auto_last_frame) return;
    auto_last_frame = f;
    while (auto_in_next < auto_in_count && auto_in[auto_in_next].frame <= f) {
        AutoInput *a = &auto_in[auto_in_next++];
        fprintf(stderr, "ME_PC_AUTO_INPUT: frame %u: %s\n", f, a->kind == 0 ? "mouse" : a->kind == 1 ? "down" : "up");
        if (a->kind == 0) { auto_motion.dx = a->dx; auto_motion.dy = a->dy; auto_motion.left = a->frames; auto_motion.ax = auto_motion.ay = 0; }
        else post_key(a->sc, a->kind == 1, 0);
    }
    if (auto_motion.left > 0) { /* one step per frame, rounding carried over so the total is exact */
        int total = auto_motion.left;
        double sx = (double)auto_motion.dx / total, sy = (double)auto_motion.dy / total;
        int ix = (int)(sx + auto_motion.ax), iy = (int)(sy + auto_motion.ay);
        auto_motion.ax += sx - ix; auto_motion.ay += sy - iy;
        auto_motion.dx -= ix; auto_motion.dy -= iy; auto_motion.left--;
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_EVENT_MOUSE_MOTION;
        ev.motion.xrel = (float)ix; ev.motion.yrel = (float)iy;
        int cw = 0, ch = 0;
        if (windows[0].win) SDL_GetWindowSize(windows[0].win, &cw, &ch);
        ev.motion.x = (float)cw / 2; ev.motion.y = (float)ch / 2;
        handle_event(&ev);
    }
}

int main(int argc, char **argv)
{
    /* SDL (which DXVK's SDL back end asks for the Vulkan instance extensions) has to find the Vulkan loader itself;
       a bare dlopen does not search Homebrew's /opt/homebrew/lib. Without this, Direct3DCreate9 throws a DxvkError
       ("SDL3 WSI: Failed to load Vulkan library"). It must be set before SDL_Init: SDL3 snapshots the environment. */
    char vkb[1024];
    const char *vkloader = "/opt/homebrew/lib/libvulkan.1.dylib";
    const char *bundle0 = getenv("ME_PC_BUNDLE");
    if (bundle0) {
        snprintf(vkb, sizeof vkb, "%s/lib/libvulkan.1.dylib", bundle0);
        if (access(vkb, R_OK) == 0) vkloader = vkb;
    }
    if (!getenv("SDL_VULKAN_LIBRARY") && access(vkloader, R_OK) == 0) setenv("SDL_VULKAN_LIBRARY", vkloader, 0);
    const char *cap = getenv("ME_PC_MOUSE_CAPTURE");
    if (cap && *cap == '0') mouse_capture_wanted = 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 3;
    }
    GameArgs g = {argc, argv, 0};
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20); /* the lifted code is deeply recursive in places */
    pthread_t t;
    if (pthread_create(&t, &a, game_thread, &g)) {
        fprintf(stderr, "cannot start the game thread\n");
        return 3;
    }
    pthread_attr_destroy(&a);
    SDL_Event e;
    auto_keys_init();
    auto_input_init();
    pad_config_init();
    uint64_t start_ns = SDL_GetTicksNS();
    while (!atomic_load(&game_done)) {
        auto_keys_poll(start_ns);
        auto_input_poll();
        if (SDL_WaitEventTimeout(&e, 4)) {
            do handle_event(&e); while (SDL_PollEvent(&e));
        }
        pads_refresh();
    }
    pthread_join(t, NULL);
    SDL_Quit();
    return g.code;
}
#endif
