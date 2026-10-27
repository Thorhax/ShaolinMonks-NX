#include "app/app.h"
#include "brightness.h"
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
    .load = openshao::launcher::Load, .save = openshao::launcher::Save, .init = nullptr, .loaded = openshao::brightness::Initialize,
    .window = nullptr, .message = nullptr, .ui = nullptr,
    .pad = nullptr, .rumble = nullptr,
};
