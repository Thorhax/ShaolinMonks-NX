// See guest_memory.h. A second 4 GB reservation, in code memory space, mirrors the window: the shadow of window
// offset x is shadow + x, so a chunk needs no bookkeeping beyond its heap backing.
#include "nx/guest_memory.h"

#include <malloc.h>
#include <switch.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"

namespace nx::gm {
namespace {

// commit granularity: the guest commits page by page, and each chunk costs two kernel mappings
constexpr uint64_t kChunk = 0x20000;

std::mutex g_lock;
uint8_t* g_window = nullptr;
uint8_t* g_shadow = nullptr;
std::unordered_map<uint32_t, void*> g_chunks;  // window offset of a mapped chunk -> its heap backing
size_t g_committed = 0;

Handle Proc() { return envGetOwnProcessHandle(); }

// heap backing, as code memory at its shadow, mapped into the window at off
bool MapRange(uint64_t off, void* backing, uint64_t size) {
    const u64 shadow = reinterpret_cast<u64>(g_shadow + off);
    Result rc = svcMapProcessCodeMemory(Proc(), shadow, reinterpret_cast<u64>(backing), size);
    if (R_FAILED(rc)) {
        LOGE("guest memory: code mapping of %08llX+%llX failed (%X)", (unsigned long long)off, (unsigned long long)size, rc);
        return false;
    }
    rc = svcSetProcessMemoryPermission(Proc(), shadow, size, Perm_Rw);
    if (R_SUCCEEDED(rc)) rc = svcMapProcessMemory(g_window + off, Proc(), shadow, size);
    if (R_FAILED(rc)) {
        LOGE("guest memory: window mapping of %08llX+%llX failed (%X)", (unsigned long long)off, (unsigned long long)size, rc);
        svcUnmapProcessCodeMemory(Proc(), shadow, reinterpret_cast<u64>(backing), size);
        return false;
    }
    return true;
}

void UnmapRange(uint64_t off, void* backing, uint64_t size) {
    const u64 shadow = reinterpret_cast<u64>(g_shadow + off);
    svcUnmapProcessMemory(g_window + off, Proc(), shadow, size);
    svcUnmapProcessCodeMemory(Proc(), shadow, reinterpret_cast<u64>(backing), size);
}

}  // namespace

uint8_t* Init(uint64_t size, uint32_t contiguous_base, uint32_t write_combine_base, uint32_t physical_size) {
    std::lock_guard<std::mutex> lock(g_lock);
    LOGR("guest memory: reserving %llu GB", (unsigned long long)(size >> 30));
    virtmemLock();
    g_window = static_cast<uint8_t*>(virtmemFindAslr(size, 0));
    VirtmemReservation* window = g_window ? virtmemAddReservation(g_window, size) : nullptr;
    g_shadow = static_cast<uint8_t*>(virtmemFindCodeMemory(size, 0));
    VirtmemReservation* shadow = g_shadow ? virtmemAddReservation(g_shadow, size) : nullptr;
    virtmemUnlock();
    if (!window || !shadow) {
        LOGE("guest memory: no room for the 4 GB window (window %p, shadow %p). Run as an application, not an applet",
             g_window, g_shadow);
        return nullptr;
    }
    // the physical view's shadow is the first one; the second view maps the same shadow again
    void* ram = memalign(0x200000, physical_size);
    if (!ram) {
        LOGE("guest memory: no heap for %u MB of console RAM", physical_size >> 20);
        return nullptr;
    }
    std::memset(ram, 0, physical_size);
    LOGR("guest memory: mapping %u MB of console RAM", physical_size >> 20);
    if (!MapRange(contiguous_base, ram, physical_size)) return nullptr;
    Result rc = svcMapProcessMemory(g_window + write_combine_base, Proc(),
                                    reinterpret_cast<u64>(g_shadow + contiguous_base), physical_size);
    if (R_FAILED(rc)) {
        LOGE("guest memory: second physical view failed (%X)", rc);
        return nullptr;
    }
    LOGR("guest memory: window %p, shadow %p", g_window, g_shadow);
    return g_window;
}

bool Commit(uint32_t va, size_t size) {
    std::lock_guard<std::mutex> lock(g_lock);
    const uint64_t first = va & ~(kChunk - 1), end = (uint64_t(va) + size + kChunk - 1) & ~(kChunk - 1);
    for (uint64_t c = first; c < end; c += kChunk) {
        if (g_chunks.count(uint32_t(c))) continue;
        void* backing = memalign(0x1000, kChunk);
        if (!backing) {
            LOGE("guest memory: out of heap committing %08X (%zu KB committed)", uint32_t(c), g_committed >> 10);
            return false;
        }
        // NtAllocateVirtualMemory hands out zeroed pages
        std::memset(backing, 0, kChunk);
        if (!MapRange(c, backing, kChunk)) {
            free(backing);
            return false;
        }
        g_chunks.emplace(uint32_t(c), backing);
        g_committed += kChunk;
    }
    return true;
}

void Decommit(uint32_t va, size_t size) {
    std::lock_guard<std::mutex> lock(g_lock);
    const uint64_t end = uint64_t(va) + size;
    for (uint64_t c = va & ~(kChunk - 1); c < end; c += kChunk) {
        auto it = g_chunks.find(uint32_t(c));
        if (it == g_chunks.end()) continue;
        if (c >= va && c + kChunk <= end) {
            UnmapRange(c, it->second, kChunk);
            free(it->second);
            g_chunks.erase(it);
            g_committed -= kChunk;
        } else {
            // part of a chunk: its pages read as zero when committed again, as fresh pages do
            const uint64_t lo = c > va ? c : va, hi = c + kChunk < end ? c + kChunk : end;
            std::memset(g_window + lo, 0, size_t(hi - lo));
        }
    }
}

Stats Query() {
    std::lock_guard<std::mutex> lock(g_lock);
    return {g_committed, g_chunks.size()};
}

}  // namespace nx::gm
