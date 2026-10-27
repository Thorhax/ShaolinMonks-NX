#pragma once
// Upstream's mod routine bridge (src/mods/routine_bridge.h), which every generated game function passes through, with
// a cheaper check. Upstream loads the routine table with acquire ordering on every game function call; the table is
// published once in boot (StartRoutines), before the title's first thread is created, and only cleared before boot,
// so thread creation already orders it and a plain load sees it. On the Switch that check cost about 10% of the
// title thread's time; without mod routines the call now goes straight to the original.
#include "../../third_party/ShaolinMonksRecomp/src/mods/routine_bridge.h"

namespace openshao::mods {
inline void RunRoutineAfterBoot(uint32_t address, X86Context& ctx, uint8_t* base, X86Func* original) {
    const auto* route = CurrentRoutines.load(std::memory_order_relaxed);
    if (__builtin_expect(route != nullptr, 0) && address < route->limit && route->flags[address])
        InvokeRouted(address, ctx, base, original);
    else
        original(ctx, base);
}
}  // namespace openshao::mods

#undef X86_WEAK_FUNC
#define X86_WEAK_FUNC(name, impl)                                                                              \
    extern "C" __attribute__((weak, noinline)) void name(X86Context& __restrict ctx, uint8_t* base) {          \
        ::openshao::mods::RunRoutineAfterBoot(::openshao::mods::FunctionAddress(#name), ctx, base, impl);     \
    }
#undef X86_TRACED_FUNC
#define X86_TRACED_FUNC(name, impl, va)                                                                        \
    extern "C" __attribute__((weak, noinline)) void name(X86Context& __restrict ctx, uint8_t* base) {          \
        const auto caller = x86_trace(ctx, va, 0);                                                             \
        ::openshao::mods::RunRoutineAfterBoot(va, ctx, base, impl);                                            \
        x86_trace(ctx, va, caller);                                                                            \
    }
