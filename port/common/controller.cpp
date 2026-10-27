// Upstream's controller interface for a host whose pads are fixed per port: player n is controller n.
// The Windows host shares player one between keyboard and pad and moves pads between slots for Ko-op; here
// each Switch controller (or the handheld pair) is its own port, so Ko-op only needs a second controller.
#include "controller.h"

#include <atomic>
#include <toml++/toml.hpp>

#include "core/input.h"
#include "core/log.h"

namespace openshao::controller {
namespace {
std::atomic<bool> multiplayer{false};
std::atomic<bool> rumble_enabled{true};
}  // namespace

void Update() {}
void Shutdown() {}
void SetFocused(bool) {}
bool HasRumble(int port) { return rumble_enabled && input::Poll(port).connected; }
int KeyboardPort() { return 0; }
void SetMultiplayer(bool enabled) {
    if (multiplayer.exchange(enabled) != enabled) LOGI("input: %s", enabled ? "Ko-op or versus" : "solo");
}
void Window(void*) {}
bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }
void Load(const void* raw) {
    const auto c = (*static_cast<const toml::table*>(raw))["controller"];
    rumble_enabled = c["rumble"].value_or(true);
}
void Save(void* raw) {
    auto& table = *static_cast<toml::table*>(raw);
    toml::table c;
    c.insert_or_assign("rumble", rumble_enabled.load());
    table.insert_or_assign("controller", c);
}
bool Multiplayer() { return multiplayer; }
bool RumbleEnabled() { return rumble_enabled; }

}  // namespace openshao::controller
