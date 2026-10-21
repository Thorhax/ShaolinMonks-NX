#include "vk/vk_overlay.h"

#include <algorithm>
#include <chrono>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "backends/imgui_impl_vulkan.h"
#include "core/log.h"
#include "imgui.h"
#include "quick_menu.h"

namespace vkr::overlay {
namespace {

Device g_device{};
VkDescriptorPool g_pool = VK_NULL_HANDLE;
VkFormat g_format = VK_FORMAT_UNDEFINED;  // the backend's pipeline is made for one output format
bool g_context = false, g_backend = false, g_failed = false;
std::chrono::steady_clock::time_point g_last{}, g_fps_start{};
int g_fps_frames = 0;
float g_fps = 0;

void Check(VkResult r) {
    if (r != VK_SUCCESS) LOGE("overlay: Vulkan error %d", int(r));
}

void CreateContext() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
#ifdef __SWITCH__
    // the system font, as the Home menu's
    PlFontData font;
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) && R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) &&
        font.address) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;  // shared memory the pl service owns
        io.Fonts->AddFontFromMemoryTTF(font.address, int(font.size), 26.0f, &config);
    }
#endif
    if (io.Fonts->Fonts.empty()) {
        ImFontConfig config;
        config.SizePixels = 26.0f;
        io.Fonts->AddFontDefault(&config);
    }
    g_context = true;
}

bool CreateBackend(VkFormat format) {
    if (!g_pool) {
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ci.maxSets = 8;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = &size;
        if (vkCreateDescriptorPool(g_device.device, &ci, nullptr, &g_pool) != VK_SUCCESS) return false;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = g_device.instance;
    info.PhysicalDevice = g_device.physical;
    info.Device = g_device.device;
    info.QueueFamily = g_device.queue_family;
    info.Queue = g_device.queue;
    info.DescriptorPool = g_pool;
    info.MinImageCount = std::max(2u, g_device.images);
    info.ImageCount = std::max(2u, g_device.images);
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
    info.CheckVkResultFn = Check;
    if (!ImGui_ImplVulkan_Init(&info)) return false;
    g_format = format;
    return true;
}

}  // namespace

void Init(const Device& d) { g_device = d; }

void Shutdown() {
    if (g_backend) {
        vkDeviceWaitIdle(g_device.device);
        ImGui_ImplVulkan_Shutdown();
    }
    if (g_pool) vkDestroyDescriptorPool(g_device.device, g_pool, nullptr);
    if (g_context) ImGui::DestroyContext();
    g_pool = VK_NULL_HANDLE;
    g_backend = g_context = false;
}

void Draw(VkCommandBuffer cmd, VkFormat format, uint32_t width, uint32_t height) {
    const auto now = std::chrono::steady_clock::now();
    if (g_fps_start == std::chrono::steady_clock::time_point{}) g_fps_start = now;
    g_fps_frames++;
    const double span = std::chrono::duration<double>(now - g_fps_start).count();
    if (span >= 0.5) {
        g_fps = float(g_fps_frames / span);
        g_fps_frames = 0;
        g_fps_start = now;
    }
    // nothing on screen: no ImGui frame at all, so play costs nothing
    if (g_failed || !g_device.device || (!port::menu::Visible() && !port::menu::ShowFps() && !g_backend)) return;
    if (!g_context) CreateContext();
    if (g_backend && format != g_format) {
        ImGui_ImplVulkan_Shutdown();
        g_backend = false;
    }
    if (!g_backend) {
        g_backend = CreateBackend(format);
        if (!g_backend) {
            LOGE("overlay: the ImGui Vulkan backend did not start; no quick menu");
            g_failed = true;
            return;
        }
        LOGR("overlay: quick menu ready (Minus + Plus)");
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime = g_last == std::chrono::steady_clock::time_point{}
                       ? 1.0f / 60.0f
                       : std::max(1e-4f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    port::menu::Draw(g_fps);
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

}  // namespace vkr::overlay
