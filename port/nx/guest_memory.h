#pragma once
// The guest's 4 GB address space on Horizon, for the SDK's kernel/memory.cpp.
//
// Horizon has no mmap: memory is the heap, and a range can only be aliased elsewhere once it is code memory. So a
// committed range is heap backing mapped as code memory (its "shadow", read/write), then mapped into the window
// from there. Physical RAM is one such range, mapped into the window twice, as the console shows it at
// 0x80000000 and again write-combined at 0xF0000000.
#include <cstddef>
#include <cstdint>

namespace nx::gm {

// reserves the window and maps physical RAM into both views; null on failure
uint8_t* Init(uint64_t size, uint32_t contiguous_base, uint32_t write_combine_base, uint32_t physical_size);
// guest range, page aligned, outside the physical views
bool Commit(uint32_t va, size_t size);
void Decommit(uint32_t va, size_t size);

struct Stats {
    size_t committed, chunks;
};
Stats Query();

}  // namespace nx::gm
