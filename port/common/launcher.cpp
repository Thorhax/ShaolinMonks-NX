// The settings upstream's launcher keeps, without its Win32 dialog: settings.toml is edited by hand on this host
#include "launcher.h"
#include "launcher_extra.h"

#include <atomic>
#include <fstream>

#include "controller.h"
#include "core/log.h"
#include "runtime_paths.h"

namespace openshao::launcher {
namespace {
Settings current;
toml::table Disk() {
    try {
        return toml::parse_file(paths::Config().string());
    } catch (const std::exception&) {
        return {};
    }
}
}  // namespace

bool Show() { return true; }
bool ShouldPersist() { return true; }

void Load(const void* raw) {
    const auto& table = *static_cast<const toml::table*>(raw);
    current = Read(table);
    // the title's own 16:9 mode already lays out the picture, HUD and menus; upstream's ratios would scale them again
    current.aspect_ratio = "original";
    controller::Load(raw);
}

void Save(void* raw) {
    auto& table = *static_cast<toml::table*>(raw);
    table.insert_or_assign("language", current.language);
    table.insert_or_assign("voice_language", current.voice_language);
    table.insert_or_assign("master_volume", current.master_volume);
    table.insert_or_assign("bloom_effect", current.bloom_effect);
    toml::table enhancements;
    enhancements.insert_or_assign("scorpion_unfiltered", current.scorpion_unfiltered);
    table.insert_or_assign("enhancements", enhancements);
    controller::Save(raw);
    const auto extras = Merge({}, current);
    table.insert_or_assign("game", *extras["game"].as_table());
}

bool Persist(const toml::table& table, std::string& error) {
    try {
        return Write(paths::Config(), Merge(Disk(), Read(table)), error);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

unsigned Language() { return GameLanguage(current.language); }
unsigned VoiceLanguageIndex() { return GameLanguage(current.voice_language); }
float AudioGain() { return current.master_volume / 100.f; }
const Settings& Current() { return current; }
void SetBloomEffect(bool on) { current.bloom_effect = on; }

}  // namespace openshao::launcher
