#pragma once
// What the portable host supplies in place of the Win32 calls of the upstream adaptations
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <pthread.h>

namespace port {

inline uint64_t TickMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// replaces dst with src in one step where the file system allows it
bool ReplaceFile(const std::filesystem::path& src, const std::filesystem::path& dst);

// host stack of a guest thread: the recompiled code nests a host frame per guest call
#if defined(__SWITCH__)
inline constexpr size_t kGuestThreadStack = 8u << 20;
#else
inline constexpr size_t kGuestThreadStack = 64u << 20;
#endif
// on a guest thread's creation, then first thing on the new thread
void BeforeGuestThread(pthread_attr_t* attr);
void OnGuestThread();
// a host thread's name for debuggers, where the platform keeps one
void NameThread(const char* name);
// first thing on the renderer's own thread
void OnRenderThread();

}  // namespace port

// the few Win32 calls upstream's portable files keep (its platform_compat.h on Linux)
inline unsigned long GetCurrentProcessId() { return 1; }
inline uint64_t GetTickCount64() { return port::TickMs(); }
