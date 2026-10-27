// Where the port keeps its data: one folder holding settings, logs, cache, crash reports and saves
#include "runtime_paths.h"

#include <cstdio>

#include "core/log.h"

namespace fs = std::filesystem;

namespace openshao::paths {
namespace {
fs::path root;
}

bool Initialize(const fs::path& executable, bool, const fs::path& requested, std::string& error, bool read_only) {
    root = requested.empty() ? executable : requested;
    if (read_only) return true;
    std::error_code ec;
    for (const auto& dir : {root, Logs(), Cache(), Crashes()}) {
        fs::create_directories(dir, ec);
        if (ec) {
            error = dir.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

const fs::path& Root() { return root; }
fs::path Config() { return root / "settings.toml"; }
fs::path Logs() { return root / "logs"; }
fs::path Cache() { return root / "cache"; }
fs::path Crashes() { return root / "crashes"; }
fs::path Captures() { return Cache(); }
fs::path Selection() { return root / "game-source.toml"; }
fs::path Mods() { return root / "mods"; }
const fs::path& SaveRoot() { return root; }
const std::string& MoveWarning() {
    static const std::string none;
    return none;
}
// the launcher's folder choices; this host keeps everything in its one folder
bool ChangeFolders(const fs::path&, const fs::path&, std::string& error) {
    error = "The data folder is fixed on this host.";
    return false;
}
bool DataFolderOverridden() { return false; }
std::string Description() { return "{\"root\":\"" + root.string() + "\"}"; }

std::wstring ErrorText(const std::wstring& reason, const fs::path&) { return reason; }

void ReportError(const std::wstring& reason, const fs::path& dump) {
    std::string text(reason.begin(), reason.end());  // the messages are ASCII
    LOGE("%s%s%s", text.c_str(), dump.empty() ? "" : " Report: ", dump.empty() ? "" : dump.string().c_str());
    logx::Flush();
}

}  // namespace openshao::paths
