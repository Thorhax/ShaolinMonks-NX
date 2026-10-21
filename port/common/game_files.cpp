// The game in the port's folder: the extracted files in game/, or a disc image (.iso) beside them
#include <algorithm>
#include <cctype>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"
#include "runtime_paths.h"

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
            return true;
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
        return true;
    }
    LOGE("Game: no USA/Europe copy of Mortal Kombat: Shaolin Monks in %s (put the .iso there, or the extracted files in game/)",
         root.string().c_str());
    return false;
}

}  // namespace app
