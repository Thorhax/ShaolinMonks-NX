// The output hold the Thorhax intro (port/thorhax_intro) takes while it plays: the game boots meanwhile, its frames
// drawn offscreen, its sound mixed and dropped, its pads read as untouched; the window and audout are the intro's
#include <atomic>

#include "nx/platform.h"

namespace {
std::atomic<bool> g_held{false};
}  // namespace

extern "C" void RexSwitchHoldOutput(void) { g_held.store(true, std::memory_order_release); }
extern "C" void RexSwitchReleaseOutput(void) { g_held.store(false, std::memory_order_release); }
extern "C" bool RexSwitchOutputHeld(void) { return g_held.load(std::memory_order_acquire); }

bool nx::OutputHeld() { return RexSwitchOutputHeld(); }
