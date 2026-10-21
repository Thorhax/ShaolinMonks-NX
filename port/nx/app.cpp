// app:: on the Switch: one full screen output, settings in settings.toml beside the NRO
#include "app/app.h"

#include <switch.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <toml++/toml.hpp>

#include "core/log.h"
#include "quick_menu.h"
#include "nx/platform.h"
#include "runtime_paths.h"

namespace nx {
namespace {
ThreadingConfig g_threading;
}

const ThreadingConfig& Threading() { return g_threading; }

void LoadPlatform(const void* raw) {
    const auto s = (*static_cast<const toml::table*>(raw))["switch"];
    g_threading.guest_core = int(s["guest_core"].value_or(int64_t(g_threading.guest_core)));
    if (g_threading.guest_core < -1 || g_threading.guest_core > 2) g_threading.guest_core = 1;
    g_threading.guest_priority = int(s["guest_priority"].value_or(int64_t(g_threading.guest_priority)));
    if (g_threading.guest_priority < 0x1C || g_threading.guest_priority > 0x3B) g_threading.guest_priority = 59;
    g_threading.render_core = int(s["render_core"].value_or(int64_t(g_threading.render_core)));
    if (g_threading.render_core < 0 || g_threading.render_core > 2) g_threading.render_core = 2;
    g_threading.renderer_status = s["renderer"].value_or(std::string("vulkan")) == "status";
    port::menu::SetSwitchIcons(s["button_icons"].value_or(std::string("switch")) != "xbox");
    port::menu::SetSaveHook(app::Save);
}

void SavePlatform(void* raw) {
    toml::table s;
    s.insert_or_assign("guest_core", g_threading.guest_core);
    s.insert_or_assign("guest_priority", g_threading.guest_priority);
    s.insert_or_assign("render_core", g_threading.render_core);
    s.insert_or_assign("renderer", g_threading.renderer_status ? "status" : "vulkan");
    s.insert_or_assign("button_icons", port::menu::SwitchIcons() ? "switch" : "xbox");
    static_cast<toml::table*>(raw)->insert_or_assign("switch", s);
}

bool Docked() { return appletGetOperationMode() == AppletOperationMode_Console; }

void OutputSize(uint32_t& w, uint32_t& h) {
    if (Docked()) w = 1920, h = 1080;
    else w = 1280, h = 720;
}

NWindow* Window() { return nwindowGetDefault(); }

}  // namespace nx

namespace app {

Settings& Get() {
    static Settings s;
    return s;
}

namespace {
toml::table g_table;
}

void Load() {
    try {
        g_table = toml::parse_file(shaolin::paths::Config().string());
    } catch (const toml::parse_error& e) {
        LOGW("settings: %s, using the defaults", e.what());
        g_table = {};
    }
    auto& s = Get();
    const auto d = g_table["display"];
    s.display_mode = DisplayMode::Fullscreen;
    s.render_percent = uint32_t(d["render_percent"].value_or(int64_t(100)));
    if (s.render_percent < 25 || s.render_percent > 200) s.render_percent = 100;
    s.fps_cap = uint32_t(d["fps_cap"].value_or(int64_t(60)));
    s.vsync = d["vsync"].value_or(true);
    s.aniso = uint32_t(d["aniso"].value_or(int64_t(0)));
    nx::LoadPlatform(&g_table);
    if (kGame.load) kGame.load(&g_table);
    Save();  // a first run writes the file with every setting, for editing on a PC
}

void Save() {
    auto& s = Get();
    toml::table d;
    d.insert_or_assign("render_percent", int64_t(s.render_percent));
    d.insert_or_assign("fps_cap", int64_t(s.fps_cap));
    d.insert_or_assign("vsync", s.vsync);
    d.insert_or_assign("aniso", int64_t(s.aniso));
    g_table.insert_or_assign("display", d);
    nx::SavePlatform(&g_table);
    if (kGame.save) kGame.save(&g_table);
    std::ofstream(shaolin::paths::Config()) << g_table << "\n";
}

void LogSettings() { LOGR("settings: %s", (std::ostringstream() << g_table).str().c_str()); }

void LogSystem() {
    uint32_t w, h;
    nx::OutputSize(w, h);
    LOGR("Switch: %s, output %ux%u, guest core %d priority %d, render core %d", nx::Docked() ? "docked" : "handheld",
         w, h, nx::Threading().guest_core, nx::Threading().guest_priority, nx::Threading().render_core);
    LogSettings();
}

void ApplyDisplay() {}

Mode Desktop() {
    Mode m;
    nx::OutputSize(m.width, m.height);
    return m;
}
Mode Output() { return Desktop(); }

float RenderScale(uint32_t guest_height) {
    if (!guest_height) return 1.0f;
    return float(Output().height) * float(Get().render_percent) / 100.0f / float(guest_height);
}

bool Widescreen() { return true; }

std::filesystem::path ExeDir() { return nx::kRoot; }
int Instance() { return 0; }
void* Window() { return nx::Window(); }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    const uint32_t cap = Get().fps_cap;
    const auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    const auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    if (next > clock::now()) std::this_thread::sleep_until(next);
}

}  // namespace app
