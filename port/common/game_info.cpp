#include "app/app.h"
#include "launcher.h"

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
    .load = shaolin::launcher::Load, .save = shaolin::launcher::Save, .init = nullptr, .loaded = nullptr,
    .window = nullptr, .message = nullptr, .ui = nullptr,
    .pad = nullptr, .rumble = nullptr,
};
