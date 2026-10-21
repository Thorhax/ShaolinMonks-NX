// The Vulkan renderer on a Linux host without a window (lavapipe or a GPU): frames are drawn offscreen, and every
// VKDUMP-th presented frame is saved as cap/vk_<frame>.tga in the run folder
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <filesystem>

#include "app/app.h"
#include "gpu/backend.h"
#include "harness.h"
#include "vk/vk_platform.h"

namespace vkr {
namespace {

class Offscreen final : public Platform {
public:
    std::vector<const char*> InstanceExtensions() override { return {}; }
    VkSurfaceKHR CreateSurface(VkInstance) override { return VK_NULL_HANDLE; }
    void OutputSize(uint32_t& w, uint32_t& h) override {
        w = 1280;
        h = 720;
    }
    // the harness's pad script counts presented frames
    // and the frames come at 60 Hz, as a display's vertical blank would pace them (VKFPS=0: unpaced)
    void Presented(uint64_t frame) override {
        harness::g_frames = frame + 1;
        if (!fps_) return;
        next_ += std::chrono::nanoseconds(1000000000ull / fps_);
        const auto now = std::chrono::steady_clock::now();
        if (next_ < now) next_ = now;
        std::this_thread::sleep_until(next_);
    }
    bool WantFrame(uint64_t frame) override { return every_ && frame % every_ == 0; }
    void Frame(const uint8_t* px, uint32_t w, uint32_t h, uint64_t frame) override {
        const auto dir = app::ExeDir() / "cap";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        char name[32];
        std::snprintf(name, sizeof(name), "vk_%05llu.tga", (unsigned long long)frame);
        FILE* f = std::fopen((dir / name).c_str(), "wb");
        if (!f) return;
        // uncompressed true colour, top left origin: TGA stores BGRA as the image is
        uint8_t hd[18] = {0, 0, 2};
        hd[12] = uint8_t(w), hd[13] = uint8_t(w >> 8), hd[14] = uint8_t(h), hd[15] = uint8_t(h >> 8);
        hd[16] = 32;
        hd[17] = 0x28;
        std::fwrite(hd, 1, sizeof(hd), f);
        std::fwrite(px, 4, size_t(w) * h, f);
        std::fclose(f);
    }
    std::filesystem::path CacheDir() override { return app::ExeDir() / "cache"; }

private:
    std::chrono::steady_clock::time_point next_ = std::chrono::steady_clock::now();
    uint64_t fps_ = [] {
        const char* v = std::getenv("VKFPS");
        return v ? std::strtoull(v, nullptr, 10) : 60ull;
    }();
    uint64_t every_ = [] {
        const char* v = std::getenv("VKDUMP");
        return v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
};

}  // namespace

Platform& GetPlatform() {
    static Offscreen p;
    return p;
}

}  // namespace vkr

namespace gpu {
std::unique_ptr<Backend> CreateVulkanBackend();
std::unique_ptr<Backend> CreateBackend() { return CreateVulkanBackend(); }
}  // namespace gpu
