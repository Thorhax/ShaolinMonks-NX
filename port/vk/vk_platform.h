#pragma once
// What the Vulkan renderer needs from the host it runs on
#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vkr {

class Platform {
public:
    virtual ~Platform() = default;
    virtual std::vector<const char*> InstanceExtensions() = 0;
    // the window's surface; VK_NULL_HANDLE draws offscreen and hands frames to Offscreen instead
    virtual VkSurfaceKHR CreateSurface(VkInstance instance) = 0;
    // while true, the window is someone else's (the Switch's startup video): frames are drawn offscreen, and the
    // surface is made once it turns false
    virtual bool SurfaceHeld() { return false; }
    // the size the output should have now; a change remakes the swap chain
    virtual void OutputSize(uint32_t& w, uint32_t& h) = 0;
    // after each present, counted from 0
    virtual void Presented(uint64_t) {}
    // offscreen: whether to read back this presented frame, then its BGRA pixels
    virtual bool WantFrame(uint64_t) { return false; }
    virtual void Frame(const uint8_t*, uint32_t, uint32_t, uint64_t) {}
    // where compiled shaders and the pipeline cache are kept
    virtual std::filesystem::path CacheDir() = 0;
};

Platform& GetPlatform();

}  // namespace vkr
