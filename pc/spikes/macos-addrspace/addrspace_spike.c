#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#define GUEST_SIZE ((size_t)0x100000000ULL)
#define GUEST_PAGE 4096u
#define LOW_ADDR ((void *)(uintptr_t)0x100000000ULL)
static size_t host_page;
static volatile sig_atomic_t fault_seen, fault_signal;
static volatile uintptr_t fault_address, fault_pc;
static void *fault_page;

static void report(const char *name, int ok, const char *detail)
{ printf("%s: %s (%s)\n", ok ? "PASS" : "FAIL", name, detail); }

static void handler(int sig, siginfo_t *si, void *vuc)
{
    ucontext_t *uc = vuc;
    fault_signal = sig;
    fault_address = (uintptr_t)si->si_addr;
#if defined(__aarch64__)
    fault_pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
#else
    fault_pc = 0;
#endif
    fault_seen++;
    /* Make the entire host page accessible; this is required on 16 KB Apple Silicon. */
    mprotect(fault_page, host_page, PROT_READ | PROT_WRITE);
}

static int stack_touched;
static void *thread_fn(void *unused)
{
    (void)unused;
    volatile char buf[1024];
    memset((void *)buf, 0xa5, sizeof buf);
    stack_touched = buf[0] == (char)0xa5;
    return NULL;
}

int main(void)
{
    size_t n = sizeof(host_page);
    if (sysctlbyname("hw.pagesize", &host_page, &n, NULL, 0)) host_page = (size_t)sysconf(_SC_PAGESIZE);
    printf("host_page=%zu guest_page=%u\n", host_page, GUEST_PAGE);

    void *any = mmap(NULL, GUEST_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    int any_ok = any != MAP_FAILED;
    report("4 GiB PROT_NONE MAP_PRIVATE|MAP_ANON reservation", any_ok,
           any_ok ? "reserved 0x100000000 bytes" : strerror(errno));
    if (any_ok) munmap(any, GUEST_SIZE);

    /* No MAP_FIXED: test exact placement only if kernel honors the hint, without clobbering. */
    void *low = mmap(LOW_ADDR, GUEST_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    int low_exact = low != MAP_FAILED && low == LOW_ADDR;
    char low_detail[160];
    snprintf(low_detail, sizeof low_detail, "requested 0x%llx, got %p%s",
             (unsigned long long)(uintptr_t)LOW_ADDR, low,
             low == MAP_FAILED ? "; mmap failed" : (low_exact ? "; exact" : "; hint ignored"));
    report("4 GiB reservation at fixed low address", low_exact, low_detail);
    if (low != MAP_FAILED) munmap(low, GUEST_SIZE);

    void *p = mmap(NULL, host_page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    int sub_ok = p != MAP_FAILED;
    int sub_mprotect = sub_ok && mprotect((char *)p + GUEST_PAGE, GUEST_PAGE, PROT_READ | PROT_WRITE) == 0;
    if (sub_mprotect) {
        volatile unsigned char *q = (volatile unsigned char *)p;
        /* Verify neighboring 4 KB slices remain inaccessible in child processes in README
           discussion; mprotect accepts sub-page sizes but changes host-page granularity. */
        (void)q;
    }
    char sub_detail[180];
    snprintf(sub_detail, sizeof sub_detail, "mprotect 4 KB: %s; host page=%zu; kernel protection granularity is host page",
             sub_mprotect ? "accepted" : (sub_ok ? strerror(errno) : "mapping failed"), host_page);
    report("4 KB guest commit protection isolation", sub_mprotect && host_page == GUEST_PAGE, sub_detail);
    if (p != MAP_FAILED) munmap(p, host_page * 2);

    /* Decommit semantics: test both supported advice modes on writable pages. */
    p = mmap(NULL, host_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    int dontneed_zero = 0, free_zero = 0;
    if (p != MAP_FAILED) {
        memset(p, 0x5a, host_page);
        int a = madvise(p, host_page, MADV_DONTNEED);
        int b = mprotect(p, host_page, PROT_NONE);
        if (!a && !b && !mprotect(p, host_page, PROT_READ | PROT_WRITE)) dontneed_zero = ((unsigned char *)p)[0] == 0;
        memset(p, 0x5a, host_page);
        a = madvise(p, host_page, MADV_FREE);
        b = mprotect(p, host_page, PROT_NONE);
        if (!a && !b && !mprotect(p, host_page, PROT_READ | PROT_WRITE)) free_zero = ((unsigned char *)p)[0] == 0;
        munmap(p, host_page);
    }
    char decommit_detail[180];
    snprintf(decommit_detail, sizeof decommit_detail, "MADV_DONTNEED recommit-zero=%s; MADV_FREE immediate-zero=%s (lazy, not guaranteed)",
             dontneed_zero ? "yes" : "no", free_zero ? "yes" : "no");
    report("decommit zero-on-recommit semantics", dontneed_zero, decommit_detail);

    p = mmap(NULL, host_page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    fault_page = p;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    fault_seen = 0;
    if (p != MAP_FAILED) { volatile unsigned char v = *(volatile unsigned char *)p; (void)v; }
    int resumed = p != MAP_FAILED && fault_seen == 1 && *(volatile unsigned char *)p == 0;
    char fault_detail[200];
    snprintf(fault_detail, sizeof fault_detail, "signal=%d (%s), si_addr=%p, arm64 PC=0x%llx, resumed=%s",
             fault_signal, fault_signal == SIGSEGV ? "SIGSEGV" : fault_signal == SIGBUS ? "SIGBUS" : "none",
             (void *)fault_address, (unsigned long long)fault_pc, resumed ? "yes" : "no");
    report("SA_SIGINFO PROT_NONE fault, mprotect and resume", resumed, fault_detail);
    if (p != MAP_FAILED) munmap(p, host_page);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stack_size = 64u * 1024u * 1024u;
    int attr_ok = pthread_attr_setstacksize(&attr, stack_size) == 0;
    pthread_t th;
    int thread_ok = attr_ok && pthread_create(&th, &attr, thread_fn, NULL) == 0;
    if (thread_ok) pthread_join(th, NULL);
    pthread_attr_destroy(&attr);
    char thread_detail[128];
    snprintf(thread_detail, sizeof thread_detail, "pthread_attr_setstacksize(%zu)=%s; create/join=%s; worker touched stack=%s",
             stack_size, attr_ok ? "ok" : "failed", thread_ok ? "ok" : "failed", stack_touched ? "yes" : "no");
    report("64 MB pthread stack", thread_ok && stack_touched, thread_detail);
    printf("main_thread_stack_link_flag: build with -Wl,-stack_size,0x4000000 (64 MiB); see README\n");
    return 0;
}
