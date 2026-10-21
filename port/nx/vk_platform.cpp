// The Vulkan renderer on the Switch: NVK, linked in, drawing to the default NWindow
#include <switch.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_vi.h>

#include <atomic>

#include "app/app.h"
#include "core/log.h"
#include "nx/platform.h"
#include "vk/vk_platform.h"

namespace {
std::atomic<uint64_t> g_presented{0};
}  // namespace

uint64_t nx::PresentedFrames() { return g_presented.load(std::memory_order_relaxed); }

namespace vkr {
namespace {

class Horizon final : public Platform {
public:
    std::vector<const char*> InstanceExtensions() override {
        return {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
    }
    VkSurfaceKHR CreateSurface(VkInstance instance) override {
        NWindow* win = nx::Window();
        uint32_t w, h;
        OutputSize(w, h);
        Result rc = nwindowSetDimensions(win, w, h);
        if (R_FAILED(rc)) LOGW("vulkan: window size %ux%u refused (%X)", w, h, rc);
        VkViSurfaceCreateInfoNN ci{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
        ci.window = win;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        auto create = reinterpret_cast<PFN_vkCreateViSurfaceNN>(vkGetInstanceProcAddr(instance, "vkCreateViSurfaceNN"));
        VkResult r = create ? create(instance, &ci, nullptr, &surface) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (r != VK_SUCCESS) LOGE("vulkan: no surface on the window (%d)", int(r));
        return surface;
    }
    // 720p in both modes: the console scales it to the TV docked
    void OutputSize(uint32_t& w, uint32_t& h) override {
        w = 1280;
        h = 720;
    }
    void Presented(uint64_t) override { g_presented.fetch_add(1, std::memory_order_relaxed); }
    std::filesystem::path CacheDir() override { return app::ExeDir() / "cache"; }
};

}  // namespace

Platform& GetPlatform() {
    static Horizon p;
    return p;
}

}  // namespace vkr
