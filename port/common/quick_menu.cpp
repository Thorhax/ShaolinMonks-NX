// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// See quick_menu.h. The model and drawing follow YabaSanshiro's RuntimeOverlay.cpp (from Dolphin NX); the pages and
// actions are this port's.
#include "quick_menu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

#include "cheats.h"
#include "imgui.h"

namespace port::menu {
namespace {

enum class Page { Main, Cheats };

constexpr uint32_t kChord = kMinus | kPlus;
constexpr int kMainItems = 4;
constexpr int kMainShowFps = 2;
constexpr int kMainIcons = 3;

std::mutex g_lock;
bool g_visible = false;
bool g_capture_until_release = false;
bool g_show_fps = false;
Page g_page = Page::Main;
int g_selection = 0;
std::atomic<bool> g_switch_icons{true};
std::atomic<uint32_t> g_icons_epoch{1};
void (*g_save)() = nullptr;
int g_cheat_player = 0;  // whose cheats the page shows

void SetPage(Page page) {
    g_page = page;
    g_selection = 0;
}

void Open() {
    SetPage(Page::Main);
    g_visible = true;
}

void Close() {
    g_visible = false;
    g_capture_until_release = true;
}

int ItemCount() {
    switch (g_page) {
        case Page::Main: return kMainItems;
        case Page::Cheats: return cheats::kCount + 2;  // the player, the cheats, Back
    }
    return 1;
}

void Back() {
    if (g_page == Page::Main) Close();
    else SetPage(Page::Main);
}

void Activate() {
    switch (g_page) {
        case Page::Main:
            switch (g_selection) {
                case 0: Close(); break;
                case 1: SetPage(Page::Cheats); break;
                case kMainShowFps: g_show_fps = !g_show_fps; break;
                case kMainIcons:
                    SetSwitchIcons(!g_switch_icons);
                    if (g_save) g_save();
                    break;
            }
            break;
        case Page::Cheats:
            if (g_selection == 0) g_cheat_player = 1 - g_cheat_player;
            else if (g_selection == cheats::kCount + 1) SetPage(Page::Main);
            else cheats::Toggle(g_selection - 1, g_cheat_player);
            break;
    }
}

void CenteredText(std::string_view text) {
    const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - width) * 0.5f));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void Row(const std::string& label, int index, float height) {
    const bool selected = g_selection == index;
    ImGui::Selectable((label + "##row" + std::to_string(index)).c_str(), selected, ImGuiSelectableFlags_None, {0.0f, height});
    if (selected) ImGui::SetScrollHereY(0.5f);
}

void MainPage(float row) {
    Row("Resume game", 0, row);
    Row("Cheats                                           >", 1, row);
    Row(std::string("Show FPS                           <  ") + (g_show_fps ? "Enabled" : "Disabled") + "  >", kMainShowFps, row);
    Row(std::string("Button icons                       <  ") + (g_switch_icons ? "Switch" : "Xbox") + "  >", kMainIcons, row);
    ImGui::Separator();
    CenteredText("D-Pad  Navigate     A  Select     B  Close     Minus + Plus  Toggle");
}

void CheatPage(float row) {
    Row("Player                                     <  " + std::to_string(g_cheat_player + 1) + "  >", 0, row);
    for (int c = 0; c < cheats::kCount; c++) {
        const bool on = cheats::Enabled(c, g_cheat_player);
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 235, 150, 255));
        Row(std::string(on ? "[ ON  ]  " : "[ off ]  ") + cheats::Name(c) + (cheats::PerPlayer(c) ? "" : "  (both players)"),
            c + 1, row);
        if (on) ImGui::PopStyleColor();
    }
    Row("Back", cheats::kCount + 1, row);
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 20.0f);
    ImGui::TextDisabled("The unlocks are saved with your progress the next time the game saves; fatalities show in the "
                        "Moves tab of the pause menu.");
    ImGui::PopTextWrapPos();
    CenteredText("A  Toggle     Left / Right  Player 1 or 2     B  Back");
}

}  // namespace

void UpdateInput(uint32_t down, uint32_t held) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_capture_until_release) {
        if (held) return;
        g_capture_until_release = false;
    }
    if ((held & kChord) == kChord && (down & kChord)) {
        if (g_visible) Close();
        else Open();
        return;
    }
    if (!g_visible) return;
    const int count = std::max(1, ItemCount());
    if (down & kUp) g_selection = (g_selection + count - 1) % count;
    else if (down & kDown) g_selection = (g_selection + 1) % count;
    // left and right flip a setting in place, as on NaGa's menu
    if ((down & (kLeft | kRight)) && ((g_page == Page::Main && (g_selection == kMainShowFps || g_selection == kMainIcons)) ||
                                      (g_page == Page::Cheats && g_selection <= cheats::kCount))) {
        Activate();
        return;
    }
    if (down & kB) Back();
    else if (down & kA) Activate();
}

bool SwitchIcons() { return g_switch_icons.load(); }
void SetSwitchIcons(bool on) {
    if (g_switch_icons.exchange(on) != on) g_icons_epoch++;
}
uint32_t IconsEpoch() { return g_icons_epoch.load(); }
void SetSaveHook(void (*save)()) { g_save = save; }

bool InputCaptured() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_visible || g_capture_until_release;
}

bool Visible() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_visible;
}

bool ShowFps() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_show_fps;
}

void Draw(float fps) {
    std::lock_guard<std::mutex> lock(g_lock);
    const ImGuiIO& io = ImGui::GetIO();
    const float scale = std::clamp(io.DisplaySize.y / 1080.0f, 0.66f, 1.0f);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float text_size = 26.0f * scale;
    auto badge = [&](const char* text, ImVec2 origin, ImU32 bg) {
        const ImVec2 extent = font->CalcTextSizeA(text_size, 1e9f, 0.0f, text);
        fg->AddRectFilled({origin.x - 8 * scale, origin.y - 4 * scale},
                          {origin.x + extent.x + 8 * scale, origin.y + extent.y + 4 * scale}, bg, 6.0f * scale);
        fg->AddText(font, text_size, origin, IM_COL32(140, 235, 255, 255), text);
        return extent;
    };
    if (g_show_fps && !g_visible) {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f FPS", fps);
        badge(text, {16.0f * scale, 12.0f * scale}, IM_COL32(0, 0, 0, 150));
    }
    if (!g_visible) return;

    ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(0, 0, 0, 132));
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({880.0f * scale, 640.0f * scale}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {30.0f * scale, 22.0f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f * scale, 8.0f * scale});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(7, 28, 44, 248));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(71, 190, 235, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(15, 89, 126, 245));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(15, 89, 126, 245));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(15, 89, 126, 245));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("Shaolin Monks quick menu", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale);  // the font is made at 26 px, for 1080p
        const float row = 44.0f * scale;
        switch (g_page) {
            case Page::Main: CenteredText("Quick Menu"); break;
            case Page::Cheats: CenteredText("Cheats"); break;
        }
        ImGui::Separator();
        switch (g_page) {
            case Page::Main: MainPage(row); break;
            case Page::Cheats: CheatPage(row); break;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
}

}  // namespace port::menu
