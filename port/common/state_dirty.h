#pragma once
// The draw state marked where the title's D3D writes it, in 64 byte pieces. The render thread's hand-off
// (render_thread.cpp SendState) then compares only the pieces written since the last draw instead of all it sends
// on every draw, which was most of the hand-off's time: on the Switch, reading about 3 KB of each of the two copies
// per draw missed the cache every time. XBE_STATECHECK=1 compares the unmarked pieces too and names any that changed
// unmarked.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "gpu/state.h"

namespace port::state {

inline constexpr size_t kPieces = (sizeof(gpu::State) + 63) / 64;
inline constexpr size_t kWords = (kPieces + 63) / 64;

// all marked at start: the first draw sends everything
inline std::array<uint64_t, kWords> g_dirty = [] {
    std::array<uint64_t, kWords> a;
    a.fill(~0ull);
    return a;
}();

inline void MarkPieces(size_t first, size_t last) {
    for (size_t p = first; p <= last; p++) g_dirty[p >> 6] |= 1ull << (p & 63);
}
inline void Mark(const gpu::State& s, const void* field, size_t n) {
    const size_t off = size_t(static_cast<const uint8_t*>(field) - reinterpret_cast<const uint8_t*>(&s));
    MarkPieces(off >> 6, (off + n - 1) >> 6);
}
// a whole member (or element) of State, after a write to it
template <typename T>
inline void Mark(const gpu::State& s, const T& field) {
    Mark(s, &field, sizeof(T));
}

inline uint32_t& Simple(gpu::State& s, uint32_t i) {
    Mark(s, s.simple[i]);
    return s.simple[i];
}
inline std::array<float, 4>& Vsc(gpu::State& s, uint32_t reg) {
    Mark(s, s.vsc[reg]);
    return s.vsc[reg];
}
// the pixel shader constants ApplyPixelConstants packs into simple states (the first member of State: a method's byte
// offset is the method)
inline void PixelConstants() {
    for (uint32_t m : {gpu::kMPsConstant0, gpu::kMPsConstant1, gpu::kMPsFinalConstant0, gpu::kMPsFinalConstant1})
        MarkPieces(m >> 6, (m + 28) >> 6);
}
inline void All() { g_dirty.fill(~0ull); }

// whether bytes off to off + n of State may differ from what was sent
inline bool Marked(size_t off, size_t n) {
    for (size_t p = off >> 6, last = (off + n - 1) >> 6; p <= last; p++)
        if (g_dirty[p >> 6] >> (p & 63) & 1) return true;
    return false;
}
// the first byte at or after off whose 64 pieces' word has a mark: a hand-off skips whole unmarked words at once
inline size_t NextMarkedWord(size_t off) {
    size_t w = off >> 12;
    while (w < kWords && !g_dirty[w]) w++;
    return w == (off >> 12) ? off : w << 12;
}
inline void Sent() { g_dirty.fill(0); }

// The render thread's side: the threaded backend counts the changes to the vertex shader constants it applies, so the
// renderer knows without comparing them whether the last draw's upload still holds
inline bool g_vsc_counted = false;
inline uint64_t g_vsc_changes = 0;
inline void Applied(size_t off, size_t n) {
    static const gpu::State s;
    static const size_t vsc = size_t(reinterpret_cast<const uint8_t*>(&s.vsc) - reinterpret_cast<const uint8_t*>(&s));
    if (off < vsc + sizeof(gpu::State::vsc) && off + n > vsc) g_vsc_changes++;
}

inline const bool g_check = std::getenv("XBE_STATECHECK") != nullptr;
inline bool Check() { return g_check; }

}  // namespace port::state
