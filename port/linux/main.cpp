// The Linux Vulkan test bed's entry: finds the game as the Switch host does (an .iso or game/ in the current
// folder, through port/common/game_files.cpp), so a disc image is exercised before it reaches hardware.
// The harness's pad script and offscreen platform drive and capture it as before.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "runtime_paths.h"

namespace port {
void StartTestHooks();
}

int main(int, char**) {
    std::string error;
    if (!shaolin::paths::Initialize(app::ExeDir(), false, {}, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    logx::Init(shaolin::paths::Logs().string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (app::kGame.window) app::kGame.window(nullptr);
    app::GameFiles game;
    if (!app::FindGame(game)) return 3;
    port::StartTestHooks();
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) return 1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        logx::Flush();
    }
}
