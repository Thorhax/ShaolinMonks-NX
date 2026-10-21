// Save storage on a POSIX host: a backup of each save before the game overwrites it, as upstream keeps on Windows
#include "save_storage.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <mutex>
#include <vector>
#include <unistd.h>

#include "port_compat.h"

namespace fs = std::filesystem;

namespace shaolin::storage {
namespace {
fs::path g_root;
std::mutex g_backups;
// absolute and normal, or empty for an empty path or one the host cannot resolve (never throws: the disc's
// files have no host path when it is an image)
fs::path Normal(const fs::path& p) {
    std::error_code ec;
    fs::path a = p.empty() ? fs::path() : fs::absolute(p, ec);
    return ec ? fs::path() : a.lexically_normal();
}
std::string Upper(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool SameBytes(const fs::path& a, const fs::path& b) {
    FILE* fa = std::fopen(a.c_str(), "rb");
    FILE* fb = std::fopen(b.c_str(), "rb");
    bool same = fa && fb;
    char x[4096], y[4096];
    while (same) {
        size_t n = std::fread(x, 1, sizeof x, fa), m = std::fread(y, 1, sizeof y, fb);
        same = n == m && std::equal(x, x + n, y);
        if (!n) break;
    }
    if (fa) std::fclose(fa);
    if (fb) std::fclose(fb);
    return same;
}
// the bytes only: std::filesystem::copy_file also copies the permissions, which Horizon's SD card does not
// implement (ENOSYS), and the backup failed every save the title made after its first
uint32_t CopyBytes(const fs::path& from, const fs::path& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return kIoError;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return kIoError;
    }
    uint32_t status = 0;
    char buf[16384];
    for (size_t n; !status && (n = std::fread(buf, 1, sizeof buf, in)) != 0;)
        if (std::fwrite(buf, 1, n, out) != n) status = errno == ENOSPC ? kDiskFull : kIoError;
    if (!status && std::ferror(in)) status = kIoError;
    if (!status) status = Flush(out);
    std::fclose(in);
    if (std::fclose(out) && !status) status = kIoError;
    return status;
}
}  // namespace

uint32_t Flush(FILE* file) {
    if (!file) return 0xC0000008;
    if (std::fflush(file) != 0) return errno == ENOSPC ? kDiskFull : kIoError;
    if (fsync(fileno(file)) != 0 && errno != EINVAL && errno != ENOSYS) return errno == ENOSPC ? kDiskFull : kIoError;
    return 0;
}

void SetDataRoot(const fs::path& root) { g_root = Normal(root); }

bool IsSave(const fs::path& file) {
    const fs::path normal = Normal(file);
    if (g_root.empty() || normal.empty()) return false;
    const auto relative = normal.lexically_relative(g_root);
    auto p = relative.begin();
    if (p == relative.end() || Upper(p->string()) != "E") return false;
    ++p;
    return p != relative.end() && Upper(p->string()) == "UDATA";
}

uint32_t BeforeWrite(const fs::path& file) {
    if (!IsSave(file)) return 0;
    std::lock_guard<std::mutex> guard(g_backups);
    std::error_code ec;
    if (!fs::exists(file, ec) || ec) return ec ? kIoError : 0;
    const auto size = fs::file_size(file, ec);
    if (ec) return kIoError;
    if (!size) return 0;
    const auto relative = Normal(file).lexically_relative(g_root);
    const auto folder = g_root / ".backups" / relative;
    fs::create_directories(folder, ec);
    if (ec) return ec == std::errc::no_space_on_device ? kDiskFull : kIoError;
    const auto id = std::to_string(port::TickMs());
    const auto temp = folder / (id + ".tmp"), final = folder / (id + ".bak");
    uint32_t status = CopyBytes(file, temp);
    if (!status && !SameBytes(file, temp)) status = kIoError;
    if (!status) {
        fs::rename(temp, final, ec);
        if (ec) status = kIoError;
    }
    if (status) {
        fs::remove(temp, ec);
        return status;
    }
    std::vector<fs::path> versions;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".bak") versions.push_back(it->path());
    std::sort(versions.begin(), versions.end());
    for (size_t i = 0; i + 3 < versions.size(); ++i) fs::remove(versions[i], ec);
    return 0;
}

bool PrepareDirectory(const fs::path&, const fs::path& destination, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) error = ec.message();
    return !ec;
}

fs::path SavedGamesRoot(std::string&) { return {}; }

DirectoryLock::~DirectoryLock() = default;
bool DirectoryLock::Acquire(const fs::path&, std::string&) { return true; }

}  // namespace shaolin::storage
