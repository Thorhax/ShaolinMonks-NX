#include "app/app.h"
#include "brightness.h"
#include "launcher.h"
#include "launcher_settings.h"
#include "photo_mode.h"

// controller.cpp: player 1's pad, for Enhanced Camera's recenter
namespace openshao::controller {
void NotePad(int port, const input::Pad& pad);
}

const app::GameInfo app::kGame = {
    .title = L"Shaolin Monks NX",
    .short_title = L"Shaolin Monks NX",
    .name = L"ShaolinMonksRecomp",
    .log = "shaolin-recomp",
    .settings = "settings.toml",
    .splash = L"shaolin-recomp",
    .missing = L"Copy your Xbox game files to the SD card.",
    .images = {{"default.xbe", "ShaolinMonksRecomp"}},
    .options = {},
    .load = openshao::launcher::Load, .save = openshao::launcher::Save, .init = nullptr,
    .loaded = [] { openshao::brightness::Initialize(openshao::launcher::Current().brightness); },
    .window = nullptr, .message = nullptr, .ui = nullptr,
    // Photo Mode takes the pads while it is open
    .pad = [](int port, input::Pad& pad) {
        openshao::controller::NotePad(port, pad);
        openshao::photo_mode::Poll(port, pad);
    },
    .rumble = nullptr,
};
