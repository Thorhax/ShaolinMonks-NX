// The game in the port's folder: the extracted files in game/, or a disc image (.iso) beside them
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"
#include "file_ops.h"
#include "host.h"
#include "language_pack.h"
#include "launcher.h"
#include "mods/languages.h"
#include "mods/manager.h"
#include "mods/runtime.h"
#include "mods/sha256.h"
#include "runtime_paths.h"
#include "save_storage.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by tools/codegen.sh

namespace app {
namespace {
namespace fs = std::filesystem;

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool Matches(const fs::path& where, const std::vector<uint8_t>& xbe) {
    const std::string sha = Sha1(xbe);
    const bool match = sha == kImageSha1;
    LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, where.string().c_str(), sha.c_str(), match ? "" : " (another release, skipped)");
    return match;
}
// a file of the extracted game by its disc path, matched without regard to case as the disc does
fs::path OriginalPath(const fs::path& root, std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (!shaolin::files::SafeRelative(name)) return {};
    const auto lower = [](std::string v) {
        for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    std::error_code ec;
    fs::path path = root;
    for (const auto& part : shaolin::files::FromUtf8(name)) {
        fs::path next = path / part;
        if (!fs::exists(next, ec)) {
            const std::string key = lower(shaolin::files::Utf8(part));
            for (auto it = fs::directory_iterator(path, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (lower(shaolin::files::Utf8(it->path().filename())) == key) {
                    next = it->path();
                    break;
                }
        }
        if (fs::is_symlink(next, ec)) return {};
        path = next;
    }
    return path;
}

// upstream's language packs and mod routines (game_files.cpp there): the enabled mods in the mods folder, and
// a pack for the chosen language when it is not one of the disc's five
bool MountLanguage(const fs::path& folder, const GameFiles& game, std::string& error) {
    using namespace shaolin;
    language::Disable();
    mods::StageRoutines({});
    try {
        const std::string selected = launcher::Current().language;
        std::vector<fs::path> packs;
        std::vector<mods::RoutineScript> scripts;
        bool credits = false;
        mods::Recover(paths::Mods());
        static storage::DirectoryLock lock;
        if (!lock.Acquire(paths::Mods(), error)) return false;
        bool language_ready = mods::IsOriginalLanguage(selected);
        for (const auto& row : mods::ActiveMods(paths::Mods(), selected)) {
            const auto pack = paths::Mods() / files::FromUtf8(row.mod.install_dir);
            if (!files::Within(pack, paths::Mods()) || !fs::is_directory(pack))
                throw std::runtime_error("Installed mod folder is missing: " + row.mod.name);
            packs.push_back(pack);
            if (row.mod.type == "language") {
                language_ready = true;
                credits = selected == "pt-BR";
            }
            for (const auto& path : row.mod.routines) {
                const auto script = pack / files::FromUtf8(path);
                const auto file = std::find_if(row.mod.files.begin(), row.mod.files.end(), [&](auto& f) { return f.path == path; });
                if (file == row.mod.files.end() || mods::Sha256File(script) != file->sha256)
                    throw std::runtime_error("Installed routine script changed: " + row.mod.name);
                scripts.push_back({row.mod.id, script});
            }
        }
        if (!language_ready)
            throw std::runtime_error("No language pack for '" + selected + "' in " + paths::Mods().string() +
                                     "; install one there or set another language in settings.toml.");
        if (packs.empty()) return true;
        const auto* disc = kernel::MountedDisc();
        struct Readers {
            std::mutex mutex;
            std::map<std::string, language::ReadAt> files;
        };
        const auto readers = std::make_shared<Readers>();
        const language::SourceSize size = [disc, folder](std::string_view name) -> uint64_t {
            if (disc) {
                const auto* entry = disc->Find(name);
                return entry && !entry->dir ? entry->size : 0;
            }
            std::error_code ec;
            const auto length = fs::file_size(OriginalPath(folder, std::string(name)), ec);
            return ec ? 0 : length;
        };
        const language::SourceRead read = [disc, readers, folder](std::string_view name, uint64_t at, std::span<uint8_t> bytes) {
            if (disc) {
                const auto* entry = disc->Find(name);
                return entry && !entry->dir && bytes.size() <= UINT32_MAX &&
                       disc->Read(*entry, at, bytes.data(), uint32_t(bytes.size())) == bytes.size();
            }
            language::ReadAt reader;
            {
                std::lock_guard<std::mutex> guard(readers->mutex);
                auto found = readers->files.find(std::string(name));
                if (found == readers->files.end()) {
                    const auto file = OriginalPath(folder, std::string(name));
                    if (file.empty()) return false;
                    found = readers->files.emplace(name, language::PackFileReader(file)).first;
                }
                reader = found->second;
            }
            return reader(at, bytes);
        };
        if (!language::MountPacks(packs, game.xbe, read, size, credits, error)) return false;
        mods::StageRoutines(std::move(scripts));
        const auto& stats = language::Statistics();
        LOGR("Mods ready: packs=%zu resources=%u voices=%u occurrences=%u movies=%u", packs.size(), stats.archive.resources,
             stats.archive.voice_names, stats.archive.voice_occurrences, stats.movies);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        language::Disable();
        mods::StageRoutines({});
        return false;
    }
}

bool Mounted(const fs::path& folder, GameFiles& game) {
    std::string error;
    if (MountLanguage(folder, game, error)) return true;
    LOGE("Mods: %s", error.c_str());
    return false;
}
}  // namespace

bool FindGame(GameFiles& game) {
    const fs::path root = ExeDir();
    std::error_code ec;
    std::vector<uint8_t> xbe;
    // extracted files first: no image to read through
    for (const fs::path& dir : {root / "game", root}) {
        if (ReadHostFile(dir / IMAGE_XBE, xbe) && Matches(dir, xbe)) {
            kernel::MountDisc(nullptr);
            game = {dir, std::move(xbe)};
            return Mounted(dir, game);
        }
    }
    std::vector<fs::path> isos;
    for (const fs::path& dir : {root, root / "game"})
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if ((ext == ".iso" || ext == ".xiso") && it->is_regular_file(ec)) isos.push_back(it->path());
        }
    std::sort(isos.begin(), isos.end());
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        if (!disc->Load(IMAGE_XBE, xbe) || !Matches(iso, xbe)) continue;
        kernel::MountDisc(std::move(disc));
        // the disc is D:, the folder is only scratch space for the title's writes to its own drive
        game = {shaolin::paths::Cache() / "dvd-scratch", std::move(xbe)};
        fs::create_directories(game.dir, ec);
        return Mounted({}, game);
    }
    LOGE("Game: no USA/Europe copy of Mortal Kombat: Shaolin Monks in %s (put the .iso there, or the extracted files in game/)",
         root.string().c_str());
    return false;
}

}  // namespace app
