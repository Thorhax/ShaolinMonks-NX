// What upstream's game code calls in parts of its PC host this port leaves out. Its main menu rows are built in
// (recomp_settings.cpp): Game Mode leads to Survival Mode, Enhancements edits and saves the enhancement settings, and
// Konfigure Recompilation opens the quick menu in place of the PC settings pages. Those pages' language change, which
// restarts the game, never runs here.
#include <filesystem>
#include <string>

#include "performance_overlay.h"
#include "quick_menu.h"
#include "restart.h"
#include "save_pages.h"

namespace openshao::restart {
// the saves' folder is the data folder (paths::SaveRoot)
const std::filesystem::path& ActiveSaveDirectory() {
    static const std::filesystem::path none;
    return none;
}
bool Prepare(const launcher::Settings&, std::string& error) {
    error = "The language changes on the next start on this host.";
    return false;
}
bool Commit(std::string& error) {
    error = "The language changes on the next start on this host.";
    return false;
}
void Cancel() {}
// the game is read from its disc image (kernel::MountedDisc), which the Scorpion previews read first
std::filesystem::path CurrentGameSource() { return {}; }
}  // namespace openshao::restart

namespace openshao::performance {
bool Visible() { return port::menu::ShowFps(); }
}  // namespace openshao::performance

// Upstream's ten pages of save slots stay out of this port: the title's own nine slots are page 1
namespace openshao::save_pages {
uint32_t ShownPage() { return 1; }
void BeforeCopyIn(X86Context&, uint8_t*, uint32_t) {}
void AfterCopyIn(uint32_t) {}
void BeforeCopyOut(X86Context&, uint8_t*) {}
void AfterCopyOut() {}
}  // namespace openshao::save_pages
