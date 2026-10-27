// Upstream's in-game settings (its Konfigure Recompilation pages) stay out of this port: settings.toml and the quick
// menu stand in for them. What upstream's game code still calls into them with, here doing nothing.
#include "recomp_settings.h"
#include "scorpion_preview_audio.h"

namespace openshao::recomp {
// the main menu's input goes to the title (controller_mode.inl asks first)
bool MainInput(X86Context&, uint8_t*) { return false; }
}  // namespace openshao::recomp

// Scorpion Unfiltered's voice previews, played only from those pages
namespace openshao::scorpion::audio_preview {
bool Play(unsigned) { return false; }
void Stop() {}
void Mix(float*, unsigned) {}
}  // namespace openshao::scorpion::audio_preview
