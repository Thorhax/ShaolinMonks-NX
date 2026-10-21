// From ShaolinMonksRecomp (src/linux/vk/bc_decode.cpp, GPL-2.0), unchanged apart from the include path.
#include "vk/bc_decode.h"

#include <cstring>

namespace vkbc {
namespace {

struct Rgba {
    uint8_t r, g, b, a;
};

Rgba Expand565(uint16_t c) {
    uint8_t r = uint8_t((c >> 11) & 31), g = uint8_t((c >> 5) & 63), b = uint8_t(c & 31);
    return {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)), uint8_t((b << 3) | (b >> 2)), 255};
}

uint8_t Mix(uint8_t a, uint8_t b, int wa, int wb, int div) { return uint8_t((a * wa + b * wb + div / 2) / div); }

// the colour half of a block: two 565 endpoints and 2 bit indices; alpha_mode allows the 3 colour, transparent black form
void ColourBlock(const uint8_t* p, bool alpha_mode, Rgba out[16]) {
    uint16_t c0 = uint16_t(p[0] | p[1] << 8), c1 = uint16_t(p[2] | p[3] << 8);
    Rgba pal[4];
    pal[0] = Expand565(c0);
    pal[1] = Expand565(c1);
    if (c0 > c1 || !alpha_mode) {
        pal[2] = {Mix(pal[0].r, pal[1].r, 2, 1, 3), Mix(pal[0].g, pal[1].g, 2, 1, 3), Mix(pal[0].b, pal[1].b, 2, 1, 3), 255};
        pal[3] = {Mix(pal[0].r, pal[1].r, 1, 2, 3), Mix(pal[0].g, pal[1].g, 1, 2, 3), Mix(pal[0].b, pal[1].b, 1, 2, 3), 255};
    } else {
        pal[2] = {Mix(pal[0].r, pal[1].r, 1, 1, 2), Mix(pal[0].g, pal[1].g, 1, 1, 2), Mix(pal[0].b, pal[1].b, 1, 1, 2), 255};
        pal[3] = {0, 0, 0, 0};
    }
    uint32_t idx = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
    for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (2 * i)) & 3];
}

void ExplicitAlpha(const uint8_t* p, Rgba out[16]) {
    for (int i = 0; i < 16; i++) {
        uint8_t nibble = (p[i / 2] >> ((i & 1) * 4)) & 15;
        out[i].a = uint8_t(nibble * 17);
    }
}

void InterpolatedAlpha(const uint8_t* p, Rgba out[16]) {
    uint8_t a[8];
    a[0] = p[0];
    a[1] = p[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; i++) a[i + 1] = Mix(a[0], a[1], 7 - i, i, 7);
    } else {
        for (int i = 1; i < 5; i++) a[i + 1] = Mix(a[0], a[1], 5 - i, i, 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(p[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i].a = a[(bits >> (3 * i)) & 7];
}

}  // namespace

void Decode(int bc, const uint8_t* blocks, uint32_t block_pitch, uint32_t width, uint32_t height, uint32_t slices,
            std::vector<uint8_t>& out, uint32_t& out_pitch) {
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    const uint32_t block_bytes = bc == 1 ? 8 : 16;
    out_pitch = bw * 4 * 4;
    out.assign(size_t(out_pitch) * bh * 4 * slices, 0);
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* p = blocks + (size_t(z) * bh + by) * block_pitch + size_t(bx) * block_bytes;
                Rgba px[16];
                if (bc == 1) {
                    ColourBlock(p, true, px);
                } else {
                    ColourBlock(p + 8, false, px);
                    if (bc == 2) ExplicitAlpha(p, px);
                    else InterpolatedAlpha(p, px);
                }
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + uint32_t(i & 3), y = by * 4 + uint32_t(i >> 2);
                    uint8_t* d = &out[(size_t(z) * bh * 4 + y) * out_pitch + size_t(x) * 4];
                    d[0] = px[i].b;
                    d[1] = px[i].g;
                    d[2] = px[i].r;
                    d[3] = px[i].a;
                }
            }
        }
    }
}

}  // namespace vkbc
