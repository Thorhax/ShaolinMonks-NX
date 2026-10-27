// [switch] perf = true: once a second, how busy each application core was, which threads used it, and how even the
// frames came; the renderer's own breakdown (vk_backend's "perf:" lines) is switched on with it.
// [switch] profile = true: every millisecond the title's threads and the renderer are paused for a moment and where
// they are noted (the pc and the return addresses of the x29 frame chain), as MK9 NX's sampler does; logs/profile.log
// has the counts by address in the image, rewritten every 10 seconds (tools/profile.py names them)
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "nx/platform.h"

namespace nx::perf {
namespace {

constexpr int kCores = 3;  // the application's; the system has the fourth

bool g_on = false;
std::mutex g_lock;
struct Named {
    std::string name;
    Handle handle;
    u64 last;
};
std::vector<Named> g_threads;

std::atomic<u64> g_frames{0}, g_late{0}, g_worst_ns{0}, g_pace_ns{0}, g_last_present{0};

u64 ThreadTicks(Handle h) {
    u64 t = 0;
    return R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, h, UINT64_MAX)) ? t : ~0ull;
}

// the idle ticks of a core, read from that core
u64 IdleTicks(int core) {
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    svcSleepThread(1000);
    if (svcGetCurrentProcessorNumber() != u32(core)) return ~0ull;
    u64 idle = 0;
    return R_SUCCEEDED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, UINT64_MAX)) ? idle : ~0ull;
}

void Sampler() {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    u64 idle[kCores];
    for (int c = 0; c < kCores; c++) idle[c] = IdleTicks(c);
    u64 last = armGetSystemTick();
    for (;;) {
        svcSleepThread(1'000'000'000);
        const u64 now = armGetSystemTick();
        const double span = double(now - last);
        last = now;
        char cores[96] = "";
        size_t at = 0;
        for (int c = 0; c < kCores; c++) {
            const u64 i = IdleTicks(c);
            const double busy = i == ~0ull || idle[c] == ~0ull ? -1 : 100.0 * (1.0 - double(i - idle[c]) / span);
            idle[c] = i;
            at += std::snprintf(cores + at, sizeof(cores) - at, "%s%d %.0f%%", c ? ", " : "", c, std::max(busy, 0.0));
        }
        std::vector<std::pair<double, std::string>> use;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            for (auto it = g_threads.begin(); it != g_threads.end();) {
                const u64 t = ThreadTicks(it->handle);
                if (t == ~0ull) {  // gone
                    it = g_threads.erase(it);
                    continue;
                }
                const double pct = 100.0 * double(t - it->last) / span;
                it->last = t;
                if (pct >= 1.0) use.emplace_back(pct, it->name);
                ++it;
            }
        }
        std::sort(use.rbegin(), use.rend());
        std::string threads;
        for (const auto& [pct, name] : use) threads += (threads.empty() ? "" : ", ") + name + " " + std::to_string(int(pct + 0.5)) + "%";
        const u64 frames = g_frames.exchange(0), late = g_late.exchange(0), worst = g_worst_ns.exchange(0),
                  pace = g_pace_ns.exchange(0);
        LOGR("perf: %llu frames, %llu late, slowest %.1f ms, pacing sleep %.2f ms/frame | busy: cores %s | threads: %s",
             (unsigned long long)frames, (unsigned long long)late, worst / 1e6, frames ? pace / 1e6 / frames : 0.0, cores,
             threads.c_str());
    }
}

extern "C" void _start();  // the image's first byte: addresses are logged from it, as the ELF has them

constexpr int kFrames = 8;
struct Profile {
    uint64_t samples = 0;
    std::unordered_map<uint64_t, uint32_t> self;       // pc
    std::unordered_map<uint64_t, uint32_t> inclusive;  // pc and each return address, once a sample
};

// Pauses the thread, takes its pc and frame chain, resumes it. Nothing here may take a lock (an allocation
// included): the paused thread could hold it.
bool Take(Handle h, uint64_t base, uint64_t out[kFrames + 1], int& n) {
    if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) return false;
    ThreadContext ctx;
    n = 0;
    if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h))) {
        out[n++] = ctx.pc.x - base;
        MemoryInfo mi;
        u32 page = 0;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &page, ctx.sp)) && (mi.perm & Perm_R)) {
            const uint64_t lo = mi.addr, hi = mi.addr + mi.size;
            uint64_t fp = ctx.fp;
            out[n++] = ctx.lr - base;
            while (n <= kFrames && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
                const uint64_t* frame = reinterpret_cast<const uint64_t*>(fp);
                out[n++] = frame[1] - base;
                if (frame[0] <= fp) break;
                fp = frame[0];
            }
        }
    }
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    return n > 0;
}

void WriteProfile(const std::unordered_map<std::string, Profile>& profiles) {
    char path[128];
    std::snprintf(path, sizeof(path), "%s/logs/profile.log", kRoot);
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    auto top = [f](const char* kind, const std::unordered_map<uint64_t, uint32_t>& m, size_t count) {
        std::vector<std::pair<uint32_t, uint64_t>> v;
        v.reserve(m.size());
        for (const auto& [a, c] : m) v.emplace_back(c, a);
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < count; i++) std::fprintf(f, "%s %llx %u\n", kind, (unsigned long long)v[i].second, v[i].first);
    };
    for (const auto& [name, p] : profiles) {
        std::fprintf(f, "thread %s %llu\n", name.c_str(), (unsigned long long)p.samples);
        top("self", p.self, 400);
        top("incl", p.inclusive, 400);
    }
    std::fclose(f);
}

void Profiler() {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1u << 0);  // beside the system's light work, off the title's and renderer's cores
    const uint64_t base = reinterpret_cast<uint64_t>(&_start);
    std::unordered_map<std::string, Profile> profiles;
    std::vector<std::pair<std::string, Handle>> targets;
    uint64_t written = armGetSystemTick(), listed = 0;
    for (uint64_t tick = 0;; tick++) {
        svcSleepThread(1'000'000);
        if (tick - listed >= 1000 || targets.empty()) {  // the threads, once a second
            listed = tick;
            targets.clear();
            std::lock_guard<std::mutex> lock(g_lock);
            for (const auto& t : g_threads)
                if (t.name.rfind("guest", 0) == 0 || t.name == "render") targets.emplace_back(t.name, t.handle);
        }
        for (const auto& [name, h] : targets) {
            uint64_t at[kFrames + 1];
            int n = 0;
            if (!Take(h, base, at, n)) continue;
            Profile& p = profiles[name];
            p.samples++;
            p.self[at[0]]++;
            for (int i = 0; i < n; i++) {
                bool seen = false;
                for (int j = 0; j < i; j++) seen |= at[j] == at[i];
                if (!seen) p.inclusive[at[i]]++;
            }
        }
        if (armTicksToNs(armGetSystemTick() - written) >= 10'000'000'000ull) {
            written = armGetSystemTick();
            WriteProfile(profiles);
        }
    }
}

}  // namespace

void Init(bool on, bool profile) {
    g_on = on || profile;
    if (!g_on) return;
    if (on) {
        setenv("XBE_PERF", "1", 1);  // the renderer's breakdown (diag::Perf)
        std::thread(Sampler).detach();
        LOGR("perf: sampling once a second");
    }
    if (profile) {
        std::thread(Profiler).detach();
        LOGR("perf: profiling the title's threads and the renderer every millisecond (logs/profile.log)");
    }
}

void NameThread(const char* name) {
    if (!g_on) return;
    const Handle h = threadGetCurHandle();
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& t : g_threads)
        if (t.handle == h) {
            t.name = name;
            return;
        }
    g_threads.push_back({name, h, ThreadTicks(h)});
}

void Presented() {
    if (!g_on) return;
    const u64 now = armTicksToNs(armGetSystemTick()), prev = g_last_present.exchange(now);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (!prev) return;
    const u64 dt = now - prev;
    if (dt > 20'000'000) g_late.fetch_add(1, std::memory_order_relaxed);  // a vsync or more missed
    u64 worst = g_worst_ns.load(std::memory_order_relaxed);
    while (dt > worst && !g_worst_ns.compare_exchange_weak(worst, dt)) {}
}

void Paced(u64 ns) {
    if (g_on) g_pace_ns.fetch_add(ns, std::memory_order_relaxed);
}

}  // namespace nx::perf
