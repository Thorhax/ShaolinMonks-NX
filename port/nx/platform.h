#pragma once
// The Switch side of the host: settings of its own, the display, and where its threads run
#include <cstdint>

struct NWindow;

namespace nx {

// [switch] in settings.toml
struct ThreadingConfig {
    int guest_core = 1;       // every guest thread on this core; -1 lets them float over cores 0 to 2
    int guest_priority = 59;  // the priority Horizon time slices on cores 0 to 2
    int render_core = 2;
    bool renderer_status = false;  // renderer = "status": no graphics, the bring-up console
};
const ThreadingConfig& Threading();
void LoadPlatform(const void* table);  // toml::table
void SavePlatform(void* table);

// the output follows the console: 1920x1080 docked, 1280x720 handheld
bool Docked();
void OutputSize(uint32_t& w, uint32_t& h);
NWindow* Window();

// frames the renderer has presented so far
uint64_t PresentedFrames();

// CPU exceptions write logs/crash.log from here on
void InstallCrashLog();

// the port's folder on the SD card (the default device: rooted paths stay plain for std::filesystem)
inline constexpr const char* kRoot = "/switch/shaolinmonks";

}  // namespace nx
