#include "port_compat.h"

#include <cstdio>
#include <system_error>

namespace port {

bool ReplaceFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::rename(src, dst, ec);
    if (!ec) return true;
    // FAT on the SD card does not replace an existing file by rename
    std::filesystem::remove(dst, ec);
    std::filesystem::rename(src, dst, ec);
    return !ec;
}

}  // namespace port
