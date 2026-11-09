#pragma once
// Sampling CPU profiler that needs neither root nor simpleperf (the tablet refuses
// perf_event_open from apps unless security.perf_harden is changed, which we do not touch).
// A file `me_profile_request` in the watched directory (ME_PROFILE_DIR, the app's internal
// files dir in the APK: `adb shell run-as <package> sh -c 'echo 10 > files/me_profile_request'`;
// the app cannot read files adb puts in its external dir) starts a run; its content is the
// duration in seconds (default 10, at most 60). Every thread of the process gets a timer on
// its own CPU-time clock firing SIGPROF each millisecond of CPU it burns, so samples are
// proportional to CPU use. The handler stores tid, pc and lr. The result `me_profile.txt`:
//   map <start> <end> <file offset> <path>   executable mappings
//   thread <tid> <cpu ms during the run> <comm>
//   sample <tid> <pc> <lr> <count>           aggregated, most frequent first
// Offsets are symbolized on the host against the unstripped libraries.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <tuple>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

struct MeCpuSampler {
    struct Sample { int32_t tid; uint64_t pc, lr; };
    static constexpr size_t kCapacity = 1u << 18;
    static inline Sample* samples = nullptr;
    static inline std::atomic<size_t> count{0};
    static inline std::atomic<bool> active{false};

    static void handler(int, siginfo_t*, void* raw) {
        if (!active.load(std::memory_order_relaxed) || !samples || !raw) return;
        const size_t i = count.fetch_add(1, std::memory_order_relaxed);
        if (i >= kCapacity) return;
        const auto* context = static_cast<const ucontext_t*>(raw);
        samples[i] = {int32_t(syscall(SYS_gettid)), context->uc_mcontext.pc, context->uc_mcontext.regs[30]};
    }
    // Linux MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED): per-thread scheduler CPU time.
    static clockid_t thread_clock(int tid) { return clockid_t((~unsigned(tid)) << 3) | 6; }
    static uint64_t thread_cpu_ns(int tid) {
        timespec t{};
        return clock_gettime(thread_clock(tid), &t) ? 0 : uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
    }
    static std::vector<int> threads() {
        std::vector<int> result;
        if (DIR* dir = opendir("/proc/self/task")) {
            while (dirent* entry = readdir(dir)) if (entry->d_name[0] != '.') result.push_back(std::atoi(entry->d_name));
            closedir(dir);
        }
        return result;
    }
    static std::string comm(int tid) {
        char path[64], name[32] = {};
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        if (FILE* f = std::fopen(path, "r")) { if (!std::fgets(name, sizeof(name), f)) name[0] = 0; std::fclose(f); }
        for (char& c : name) if (c == '\n' || c == ' ') c = c == ' ' ? '_' : 0;
        return name[0] ? name : "?";
    }
    // Returns the number of samples taken, or -1 when the run could not start.
    static long run(unsigned seconds, const std::string& output) {
        if (!samples) {
            void* memory = mmap(nullptr, kCapacity * sizeof(Sample), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory == MAP_FAILED) return -1;
            samples = static_cast<Sample*>(memory);
        }
        struct sigaction action{};
        action.sa_sigaction = handler; action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGPROF, &action, nullptr)) return -1;
        count.store(0); active.store(true);
        struct Armed { int tid; timer_t timer; uint64_t cpu_begin; };
        std::vector<Armed> armed;
        const int self = int(syscall(SYS_gettid));
        for (int tid : threads()) {
            if (tid == self) continue;
            sigevent event{};
            event.sigev_notify = SIGEV_THREAD_ID; event.sigev_signo = SIGPROF;
            event.sigev_notify_thread_id = tid;
            timer_t timer{};
            if (timer_create(thread_clock(tid), &event, &timer)) continue;
            itimerspec period{{0, 1000000}, {0, 1000000}};
            if (timer_settime(timer, 0, &period, nullptr)) { timer_delete(timer); continue; }
            armed.push_back({tid, timer, thread_cpu_ns(tid)});
        }
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::vector<std::pair<int, uint64_t>> cpu;
        for (auto& a : armed) { timer_delete(a.timer); const uint64_t now = thread_cpu_ns(a.tid); cpu.push_back({a.tid, now > a.cpu_begin ? now - a.cpu_begin : 0}); }
        // Signals already queued may still land: let them drain before reading.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        active.store(false);
        const size_t taken = std::min(count.load(), kCapacity);
        std::map<std::tuple<int, uint64_t, uint64_t>, uint32_t> histogram;
        for (size_t i = 0; i < taken; ++i) ++histogram[{samples[i].tid, samples[i].pc, samples[i].lr}];
        std::vector<std::pair<uint32_t, std::tuple<int, uint64_t, uint64_t>>> sorted;
        for (auto& [key, n] : histogram) sorted.push_back({n, key});
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });
        const std::string temporary = output + ".tmp";
        FILE* out = std::fopen(temporary.c_str(), "w");
        if (!out) return -1;
        if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), maps)) {
                unsigned long long start, end, offset; char perms[8], path[768] = {};
                if (std::sscanf(line, "%llx-%llx %7s %llx %*s %*s %767[^\n]", &start, &end, perms, &offset, path) >= 4 && perms[2] == 'x')
                    std::fprintf(out, "map %llx %llx %llx %s\n", start, end, offset, path[0] ? path : "[anon]");
            }
            std::fclose(maps);
        }
        for (auto& [tid, ns] : cpu) std::fprintf(out, "thread %d %.1f %s\n", tid, ns / 1e6, comm(tid).c_str());
        for (auto& [n, key] : sorted)
            std::fprintf(out, "sample %d %llx %llx %u\n", std::get<0>(key), (unsigned long long)std::get<1>(key), (unsigned long long)std::get<2>(key), n);
        if (std::fclose(out) || std::rename(temporary.c_str(), output.c_str())) return -1;
        return long(taken);
    }
    // Watches `directory` for the request file once a second.
    static void start_watcher(std::string directory) {
        if (directory.empty()) return;
        std::thread([directory] {
            const std::string request = directory + "/me_profile_request", output = directory + "/me_profile.txt";
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                FILE* f = std::fopen(request.c_str(), "r");
                if (!f) continue;
                unsigned seconds = 0;
                if (std::fscanf(f, "%u", &seconds) != 1) seconds = 10;
                std::fclose(f);
                std::remove(request.c_str());
                seconds = std::clamp(seconds, 1u, 60u);
                std::printf("{\"event\":\"cpu_profile_begin\",\"seconds\":%u}\n", seconds);
                const long taken = run(seconds, output);
                std::printf("{\"event\":\"cpu_profile_done\",\"samples\":%ld,\"file\":\"me_profile.txt\"}\n", taken);
            }
        }).detach();
    }
};
