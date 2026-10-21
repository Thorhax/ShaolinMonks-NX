#include "vk/button_icons.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "quick_menu.h"

namespace vkr::icons {
namespace {

constexpr uint64_t kAtlas = 0xd4068777901abbaaull;
constexpr uint64_t kHud = 0x613d44d9f6026755ull;
constexpr uint64_t kSans = 0xcefaed4c0aa6bda2ull;

// 5 x 7 letters, a row per byte, the high bit of the five on the left
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kLetters[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
};

// 3 x 5 letters for two-letter labels on the smallest keys, the high bit of the three on the left
constexpr Glyph kSmall[] = {
    {'L', {0x4, 0x4, 0x4, 0x4, 0x7}},
    {'R', {0x6, 0x5, 0x6, 0x5, 0x5}},
    {'Z', {0x7, 0x1, 0x2, 0x4, 0x7}},
};

const Glyph* Find(char c, bool small = false) {
    if (small) {
        for (const Glyph& g : kSmall)
            if (g.c == c) return &g;
        return nullptr;
    }
    for (const Glyph& g : kLetters)
        if (g.c == c) return &g;
    return nullptr;
}

struct Canvas {
    uint8_t* px;
    uint32_t w, h, pitch;
    // blends a colour over a pixel by coverage 0..1
    void Blend(int x, int y, const uint8_t bgra[4], float cover) {
        if (x < 0 || y < 0 || uint32_t(x) >= w || uint32_t(y) >= h || cover <= 0) return;
        uint8_t* p = px + size_t(y) * pitch + size_t(x) * 4;
        cover = std::min(cover, 1.0f) * bgra[3] / 255.0f;
        const float a = p[3] / 255.0f, out = cover + a * (1 - cover);
        for (int i = 0; i < 3; i++)
            p[i] = uint8_t(out > 0 ? std::lround((bgra[i] * cover + p[i] * a * (1 - cover)) / out) : 0);
        p[3] = uint8_t(std::lround(out * 255));
    }
    void Clear(int x0, int y0, int x1, int y1) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                if (x >= 0 && y >= 0 && uint32_t(x) < w && uint32_t(y) < h) std::memset(px + size_t(y) * pitch + size_t(x) * 4, 0, 4);
    }
};

constexpr uint8_t kFill[4] = {48, 48, 48, 255};      // BGRA: the Switch's dark grey buttons
constexpr uint8_t kRing[4] = {205, 205, 205, 255};
constexpr uint8_t kInk[4] = {255, 255, 255, 255};

// coverage of a pixel by a shape, 4 x 4 samples
template <class Inside>
float Cover(int x, int y, Inside inside) {
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) n += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
    return n / 16.0f;
}

// text centred on (cx, cy), letters 5 x 7 scaled to the height given. Under 7 pixels a scaled letter blurs, so
// it is drawn pixel for pixel instead, and a label of two letters in the 3 x 5 ones
void Text(Canvas& c, const char* s, float cx, float cy, float height) {
    const size_t n = std::strlen(s);
    const bool tiny = height < 7, small = tiny && n > 1;
    const int gw = small ? 3 : 5, gh = small ? 5 : 7;
    const float scale = tiny ? 1 : height / 7.0f, advance = (gw + 1) * scale;
    float x0 = cx - (n * advance - scale) / 2, y0 = cy - gh * scale / 2;
    if (tiny) x0 = std::round(x0), y0 = std::round(y0);
    for (size_t i = 0; i < n; i++) {
        const Glyph* g = Find(s[i], small);
        if (!g) continue;
        const float gx = x0 + i * advance;
        auto inside = [&](float x, float y) {
            const int col = int(std::floor((x - gx) / scale)), row = int(std::floor((y - y0) / scale));
            return col >= 0 && col < gw && row >= 0 && row < gh && (g->rows[row] >> (gw - 1 - col) & 1);
        };
        for (int y = int(y0) - 1; y <= int(y0 + gh * scale) + 1; y++)
            for (int x = int(gx) - 1; x <= int(gx + gw * scale) + 1; x++) c.Blend(x, y, kInk, Cover(x, y, inside));
    }
}

// a round button in the cell (x0, y0)..(x1, y1) inclusive, the old glyph cleared
void Button(Canvas& c, int x0, int y0, int x1, int y1, const char* letter) {
    c.Clear(x0, y0, x1, y1);
    const float cx = (x0 + x1 + 1) / 2.0f, cy = (y0 + y1 + 1) / 2.0f, r = (x1 - x0 + 1) / 2.0f;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            c.Blend(x, y, kRing, Cover(x, y, [&](float px, float py) { return std::hypot(px - cx, py - cy) <= r; }));
            c.Blend(x, y, kFill, Cover(x, y, [&](float px, float py) { return std::hypot(px - cx, py - cy) <= r - 1.4f; }));
        }
    Text(c, letter, cx, cy, (y1 - y0 + 1) * 0.5f);
}

// a rounded key (the triggers) in the cell
void Key(Canvas& c, int x0, int y0, int x1, int y1, const char* label) {
    c.Clear(x0, y0, x1, y1);
    const float r = (y1 - y0 + 1) / 2.0f;
    auto rounded = [&](float inset) {
        return [=](float px, float py) {
            const float ax = std::clamp(px, x0 + r, x1 + 1 - r), ay = (y0 + y1 + 1) / 2.0f;
            return std::hypot(px - ax, py - ay) <= r - inset;
        };
    };
    const auto ring = rounded(0), fill = rounded(1.4f);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            c.Blend(x, y, kRing, Cover(x, y, ring));
            c.Blend(x, y, kFill, Cover(x, y, fill));
        }
    Text(c, label, (x0 + x1 + 1) / 2.0f, (y0 + y1 + 1) / 2.0f, (y1 - y0 + 1) * 0.45f);
}

// the font atlas: the menus' and prompts' icons
void PaintAtlas(Canvas& c) {
    // the face buttons, by position: Xbox X (left) Y (top) A (bottom) B (right) are the Switch's Y X B A
    Button(c, 147, 231, 164, 248, "Y");
    Button(c, 168, 231, 185, 248, "X");
    Button(c, 189, 231, 206, 248, "B");
    Button(c, 210, 231, 227, 248, "A");
    // White and Black are the shoulder buttons here, L and R
    Button(c, 168, 210, 185, 227, "L");
    Button(c, 231, 210, 248, 227, "R");
    // the triggers' "L | R" key: ZL and ZR
    Key(c, 189, 210, 207, 227, "ZL");
    Key(c, 210, 210, 228, 227, "ZR");
}

// the HUD sheet: the bigger coloured buttons of the save and load screens and the move list, and the left
// trigger's key
void PaintHud(Canvas& c) {
    Button(c, 57, 114, 87, 144, "X");
    Button(c, 89, 114, 119, 144, "Y");
    Button(c, 57, 146, 87, 176, "B");
    Button(c, 89, 146, 119, 176, "A");
    Key(c, 191, 178, 229, 205, "ZL");
}

// the small font (128x128): the save and load screens' and the move list's prompts. Its White and Black buttons
// are balls of their own, and the triggers are the "L" and "R" keys
void PaintSans(Canvas& c) {
    Button(c, 71, 112, 82, 123, "Y");
    Button(c, 83, 112, 94, 123, "X");
    Button(c, 95, 112, 106, 123, "B");
    Button(c, 107, 112, 118, 123, "A");
    Button(c, 95, 100, 105, 110, "L");
    Button(c, 12, 113, 22, 123, "R");
    Key(c, 107, 100, 118, 110, "ZL");
    Key(c, 0, 113, 10, 123, "ZR");
}

}  // namespace

bool IsButtonAtlas(uint64_t id) { return id == kAtlas || id == kHud || id == kSans; }

void Paint(uint64_t id, uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) {
    Canvas c{bgra, width, height, pitch};
    if (id == kAtlas && width == 256 && height == 512) PaintAtlas(c);
    if (id == kHud && width == 256 && height == 256) PaintHud(c);
    if (id == kSans && width == 128 && height == 128) PaintSans(c);
}

bool SwitchIcons() { return port::menu::SwitchIcons(); }
uint32_t Epoch() { return port::menu::IconsEpoch(); }

}  // namespace vkr::icons
