// The Switch host: finds the game on the SD card, boots it, then keeps the applet loop and the logs going
#include <switch.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "nx/guest_memory.h"
#include "nx/platform.h"
#include "runtime_paths.h"

namespace {

// a message on the console until the player presses +
[[noreturn]] void Fatal(const char* text) {
    consoleInit(nullptr);
    std::printf("\n  Shaolin Monks Recomp\n\n  %s\n\n  Log: %s\n\n  Press + to exit.\n", text, logx::Path().c_str());
    consoleUpdate(nullptr);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    logx::Flush();
    consoleExit(nullptr);
    std::_Exit(1);
}

}  // namespace

int main(int, char**) {
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
    std::string error;
    if (!shaolin::paths::Initialize(nx::kRoot, false, {}, error)) Fatal(("Cannot use the SD card folder: " + error).c_str());
    // the last few runs' logs stay: a short launch after a long session would otherwise wipe it
    {
        const auto dir = shaolin::paths::Logs();
        const std::string base = app::kGame.log;
        std::error_code ec;
        for (int n = 3; n >= 1; n--) {
            const auto from = n == 1 ? dir / (base + ".log") : dir / (base + "." + std::to_string(n - 1) + ".log");
            const auto to = dir / (base + "." + std::to_string(n) + ".log");
            std::filesystem::remove(to, ec);
            std::filesystem::rename(from, to, ec);
        }
    }
    logx::Init(shaolin::paths::Logs().string(), app::kGame.log);
    nx::InstallCrashLog();
    app::Load();
    app::LogSystem();
    const AppletType type = appletGetAppletType();
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    LOGR("Switch: applet type %d, memory %llu MB of %llu MB in use", int(type), (unsigned long long)(used >> 20),
         (unsigned long long)(total >> 20));
    if (type != AppletType_Application && type != AppletType_SystemApplication)
        LOGE("Running as an applet: memory is short. Start the game through a title override or a forwarder.");

    app::GameFiles game;
    if (!app::FindGame(game))
        Fatal("Copy your USA/Europe Xbox disc image (.iso) to /switch/shaolinmonks/,\n  "
              "or the extracted game files to /switch/shaolinmonks/game/.");
    LOGR("boot: input");
    input::Init();
    if (app::kGame.window) app::kGame.window(app::Window());
    LOGR("boot: kernel");
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string()))
        Fatal("The game failed to start.");

    LOGR("boot: guest started");
    // the guest runs on its own threads; this one keeps the applet responsive and flushes the log
    auto last = std::chrono::steady_clock::now();
    uint64_t last_frames = nx::PresentedFrames();
    while (appletMainLoop()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        if (seconds >= 5) {
            const uint64_t frames = nx::PresentedFrames();
            const auto gm = nx::gm::Query();
            LOGR("status: %.1f fps, guest memory %zu KB in %zu chunks", double(frames - last_frames) / seconds,
                 gm.committed >> 10, gm.chunks);
            last = now;
            last_frames = frames;
            logx::Flush();
        }
    }
    logx::Flush();
    kernel::Shutdown(0);
}
