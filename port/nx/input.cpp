// The console's pads from the Switch's controllers: port 0 is player 1 (handheld or the first controller), ports 1 to
// 3 the next controllers. The face buttons work by position, as on the Xbox pad: the bottom button is the Xbox's A.
#include "core/input.h"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#include "app/app.h"
#include "cheats.h"
#include "launcher.h"
#include "quick_menu.h"

namespace input {
namespace {

constexpr int kPorts = 4;
std::mutex g_lock;
PadState g_pads[kPorts];
HidNpadIdType g_ids[kPorts] = {HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4};
// vibration handles depend on the controller's style: made again when it changes
HidVibrationDeviceHandle g_vibration[kPorts][2];
uint32_t g_vibration_style[kPorts];

int16_t Stick(int32_t v, float) {
    return int16_t(std::clamp(v, -32767, 32767));
}

// a radial dead zone, rescaled so the stick still reaches its full range
void DeadZone(int32_t x, int32_t y, float deadzone, int16_t& ox, int16_t& oy) {
    const float fx = float(x) / 32767.0f, fy = float(y) / 32767.0f;
    const float m = std::sqrt(fx * fx + fy * fy);
    if (m <= deadzone || m == 0.0f) {
        ox = oy = 0;
        return;
    }
    const float scale = std::min(1.0f, (m - deadzone) / (1.0f - deadzone)) / m;
    ox = Stick(int32_t(fx * scale * 32767.0f), 0);
    oy = Stick(int32_t(fy * scale * 32767.0f), 0);
}

// player 1's buttons, as the quick menu reads them
uint32_t MenuButtons(u64 b) {
    uint32_t m = 0;
    if (b & (HidNpadButton_Up | HidNpadButton_StickLUp)) m |= port::menu::kUp;
    if (b & (HidNpadButton_Down | HidNpadButton_StickLDown)) m |= port::menu::kDown;
    if (b & (HidNpadButton_Left | HidNpadButton_StickLLeft)) m |= port::menu::kLeft;
    if (b & (HidNpadButton_Right | HidNpadButton_StickLRight)) m |= port::menu::kRight;
    if (b & HidNpadButton_A) m |= port::menu::kA;
    if (b & HidNpadButton_B) m |= port::menu::kB;
    if (b & HidNpadButton_X) m |= port::menu::kX;
    if (b & HidNpadButton_Y) m |= port::menu::kY;
    if (b & HidNpadButton_Minus) m |= port::menu::kMinus;
    if (b & HidNpadButton_Plus) m |= port::menu::kPlus;
    return m;
}

}  // namespace

void Init() {
    padConfigureInput(kPorts, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pads[0]);  // player 1 and handheld
    static const HidNpadIdType ids[] = {HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4};
    for (int i = 1; i < kPorts; i++) padInitialize(&g_pads[i], ids[i - 1]);
}

Pad Poll(int port) {
    Pad p;
    if (port < 0 || port >= kPorts) return p;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        PadState& s = g_pads[port];
        padUpdate(&s);
        if (!padIsConnected(&s) && port != 0) return p;
        p.connected = port == 0 || padIsConnected(&s);
        const u64 b = padGetButtons(&s);
        // the quick menu (Minus + Plus) takes player 1's controller while it is open
        if (port == 0) {
            port::menu::UpdateInput(MenuButtons(padGetButtonsDown(&s)), MenuButtons(b));
            if (port::menu::InputCaptured()) return p;
        }
        auto on = [&](u64 mask) { return (b & mask) != 0; };
        p.buttons = uint16_t((on(HidNpadButton_Up) ? 0x01 : 0) | (on(HidNpadButton_Down) ? 0x02 : 0) |
                             (on(HidNpadButton_Left) ? 0x04 : 0) | (on(HidNpadButton_Right) ? 0x08 : 0) |
                             (on(HidNpadButton_Plus) ? 0x10 : 0) | (on(HidNpadButton_Minus) ? 0x20 : 0) |
                             (on(HidNpadButton_StickL) ? 0x40 : 0) | (on(HidNpadButton_StickR) ? 0x80 : 0));
        // A B X Y black white, then the triggers
        p.analog[0] = on(HidNpadButton_B) ? 255 : 0;
        if (port < 2) port::cheats::NoteJump(port, on(HidNpadButton_B));  // the Xbox's A, jump
        p.analog[1] = on(HidNpadButton_A) ? 255 : 0;
        p.analog[2] = on(HidNpadButton_Y) ? 255 : 0;
        p.analog[3] = on(HidNpadButton_X) ? 255 : 0;
        p.analog[4] = on(HidNpadButton_R) ? 255 : 0;
        p.analog[5] = on(HidNpadButton_L) ? 255 : 0;
        p.analog[6] = on(HidNpadButton_ZL) ? 255 : 0;
        p.analog[7] = on(HidNpadButton_ZR) ? 255 : 0;
        const float dz = float(shaolin::launcher::Current().deadzone);
        const HidAnalogStickState l = padGetStickPos(&s, 0), r = padGetStickPos(&s, 1);
        DeadZone(l.x, l.y, dz, p.lx, p.ly);
        DeadZone(r.x, r.y, dz, p.rx, p.ry);
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (port < 0 || port >= kPorts) return;
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard<std::mutex> lock(g_lock);
    // player 1 may be the handheld pair rather than controller 1
    const HidNpadIdType id = port == 0 && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld)
                                 ? HidNpadIdType_Handheld : g_ids[port];
    const uint32_t styles = hidGetNpadStyleSet(id);
    uint32_t style = 0;
    for (uint32_t s : {HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadJoyDual})
        if (styles & s) {
            style = s;
            break;
        }
    if (!style) return;
    if (g_vibration_style[port] != (style | uint32_t(id) << 16)) {
        if (R_FAILED(hidInitializeVibrationDevices(g_vibration[port], 2, id, HidNpadStyleTag(style)))) return;
        g_vibration_style[port] = style | uint32_t(id) << 16;
    }
    // the Xbox's left motor is the heavy, low one
    HidVibrationValue v[2];
    const float lo = left / 65535.0f, hi = right / 65535.0f;
    for (auto& x : v) {
        x.freq_low = 160.0f;
        x.freq_high = 320.0f;
        x.amp_low = lo;
        x.amp_high = hi;
    }
    hidSendVibrationValues(g_vibration[port], v, 2);
}

}  // namespace input
