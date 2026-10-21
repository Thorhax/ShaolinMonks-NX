// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-game quick menu, adapted for Shaolin Monks from YabaSanshiro's NX overlay, itself from NaGa's Dolphin NX
// frontend (Source/Core/DolphinSwitch/RuntimeOverlay). Kept: the page and selection model, controller navigation,
// Minus + Plus to open it, and the ImGui drawing and style. The pages are this port's: cheats on the title's own
// variables and save slots (see cheats.h). Drawn by the renderer over each frame (vk_overlay.cpp).
#pragma once

#include <cstdint>

namespace port::menu {

// player 1's buttons, as the host reads them
enum Button : uint32_t {
    kUp = 1u << 0, kDown = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3,
    kA = 1u << 4, kB = 1u << 5, kX = 1u << 6, kY = 1u << 7,
    kMinus = 1u << 8, kPlus = 1u << 9,
};

// pressed since the last call, and held
void UpdateInput(uint32_t down, uint32_t held);
// while the menu is open, and after it closes until every button is up: the title sees no input from player 1
bool InputCaptured();
bool Visible();
bool ShowFps();

// the Switch's button icons in place of the Xbox's (the renderer draws them over the title's font atlas); a change
// counts the epoch up, and is saved through the host's hook
bool SwitchIcons();
void SetSwitchIcons(bool on);
uint32_t IconsEpoch();
void SetSaveHook(void (*save)());

// between ImGui::NewFrame and ImGui::Render: the menu when open, and status messages and the frame rate over play
void Draw(float fps);

}  // namespace port::menu
