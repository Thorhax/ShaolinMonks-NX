#pragma once
// Dear ImGui over the renderer's output: the quick menu, status messages and the frame rate (port/common/
// quick_menu.h). Drawn by the render thread in the pass that puts the title's frame on the output image, so
// ImGui is only ever used from that thread.
#include <vulkan/vulkan.h>

#include <cstdint>

namespace vkr::overlay {

struct Device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    uint32_t images;  // in the swap chain, at least 2
};

void Init(const Device& d);
void Shutdown();
// inside a dynamic rendering instance whose one colour attachment has this format and size
void Draw(VkCommandBuffer cmd, VkFormat format, uint32_t width, uint32_t height);

}  // namespace vkr::overlay
