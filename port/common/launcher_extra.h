#pragma once
// The port's additions to upstream's launcher settings (launcher.h), for the quick menu
namespace openshao::launcher {
// the title's glow pass; saved with the other settings through the host's save hook
void SetBloomEffect(bool on);
}  // namespace openshao::launcher
