// The title's Direct3D 8 calls drawn with Vulkan.
//
// This follows the SDK's Direct3D 12 renderer (runtime/gpu/d3d12.cpp, with ShaolinMonksRecomp's adaptations) step
// for step, so the two behave alike: the same targets, texture cache, shader keys, constants and read-backs, and
// the same HLSL, compiled to SPIR-V. What Vulkan does differently:
// - every image stays in VK_IMAGE_LAYOUT_GENERAL; D3D12's resource states become plain barriers between uses
// - draws record inside dynamic rendering instances, begun right before a draw or clear and ended by any copy
// - the viewport is flipped (negative height), which gives D3D's clip space and pixel centres, so the generated
//   shaders' raster conventions hold as they are
// - descriptors are pushed per draw, in the bindings the D3D12 registers map to (vk_shader.h)
// - a timeline semaphore stands in for the D3D12 fence
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dxgiformat.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "raster_compat.h"
#include "sfd_filter_compat.h"
#include "vk/vk_platform.h"
#include "vk/bc_decode.h"
#include "vk/button_icons.h"
#include "vk/vk_overlay.h"
#include "vk/vk_shader.h"

namespace gpu {

namespace {

using vkr::Stage;

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
#if defined(__SWITCH__)
constexpr uint64_t kUploadSize = 48ull << 20;
#else
constexpr uint64_t kUploadSize = 64ull << 20;
#endif
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;
constexpr uint32_t kSamplersKept = 2048;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;

#define VKCHECK(x)                                                              \
    do {                                                                        \
        VkResult _r = (x);                                                      \
        if (_r != VK_SUCCESS) LOGE("vulkan: %s failed (%d)", #x, int(_r));      \
    } while (0)

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) { return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count(); }
struct Perf {
    double draw = 0, assemble = 0, bind = 0, sig = 0, texup = 0, compile = 0, clear = 0, copy = 0, present = 0, wait = 0,
           swap = 0, sync = 0;
    uint64_t draws = 0, verts = 0, upload = 0, overflow = 0, sigs = 0, sig_bytes = 0, texups = 0, texup_bytes = 0,
             newtex = 0, targets = 0, rtups = 0, shrinks = 0, syncs = 0, psos = 0, shaders = 0, raw = 0, lates = 0;
};
Perf g_perf;
struct PerfScope {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    ~PerfScope() { acc += PerfMs(t); }
};

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kBlendDestColor: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: case 0x8003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

// D3DBLENDOP, GL values
VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 as D3D12 numbers them, from 1 (the shaders take these, minus 1)
uint32_t Cmp(uint32_t v) { return (v >= 0x200 && v <= 0x207 ? v - 0x200 : 7) + 1; }
// Vulkan's compare ops are in the same order, from 0
VkCompareOp VkCmp(uint32_t v) { return VkCompareOp(Cmp(v) - 1); }

// D3DSTENCILOP, GL values
VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFormat HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_R32_SFLOAT;
        default: return kBgra;
    }
}

// bytes per 4x4 block of a BC format, 0 for the others
uint32_t BlockBytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 8;
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: return 16;
        default: return 0;
    }
}

// the vertex formats Assemble names, as DXGI numbers them
VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// ---------------------------------------------------------------------------------------------------------------
// device memory: big blocks per memory type, carved first fit; buffers and images never share a block, so the
// device's buffer/image granularity never matters

struct Mem {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0, size = 0;
    uint8_t* map = nullptr;  // host visible memory, persistently mapped
    uint32_t block = ~0u;
};

class Allocator {
public:
    void Init(VkDevice dev, VkPhysicalDevice pd) {
        dev_ = dev;
        vkGetPhysicalDeviceMemoryProperties(pd, &props_);
    }

    Mem Alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, bool image) {
        std::lock_guard<std::mutex> lock(m_);
        const uint32_t type = Type(req.memoryTypeBits, want);
        if (type == ~0u) {
            LOGE("vulkan: no memory type %X for bits %X", want, req.memoryTypeBits);
            return {};
        }
        for (uint32_t b = 0; b < blocks_.size(); b++) {
            Block& bl = blocks_[b];
            if (!bl.mem || bl.type != type || bl.image != image) continue;
            if (auto m = Carve(bl, b, req)) return *m;
        }
        // a block of its own for what does not fit the usual size
        const VkDeviceSize size = std::max<VkDeviceSize>(kBlock, (req.size + 0xFFFF) & ~VkDeviceSize(0xFFFF));
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        Block bl;
        if (vkAllocateMemory(dev_, &ai, nullptr, &bl.mem) != VK_SUCCESS) {
            LOGE("vulkan: out of device memory (%llu MB more asked, %llu MB held)", (unsigned long long)(size >> 20),
                 (unsigned long long)(held_ >> 20));
            return {};
        }
        held_ += size;
        bl.size = size;
        bl.type = type;
        bl.image = image;
        bl.own = size > kBlock;
        if (props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            vkMapMemory(dev_, bl.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&bl.map));
        bl.free[0] = size;
        uint32_t b = 0;
        while (b < blocks_.size() && blocks_[b].mem) b++;
        if (b == blocks_.size()) blocks_.emplace_back();
        blocks_[b] = std::move(bl);
        return *Carve(blocks_[b], b, req);
    }

    void Free(const Mem& m) {
        if (m.block == ~0u) return;
        std::lock_guard<std::mutex> lock(m_);
        Block& bl = blocks_[m.block];
        auto n = bl.free.emplace(m.off, m.size).first;
        if (auto next = std::next(n); next != bl.free.end() && n->first + n->second == next->first) {
            n->second += next->second;
            bl.free.erase(next);
        }
        if (n != bl.free.begin()) {
            auto prev = std::prev(n);
            if (prev->first + prev->second == n->first) {
                prev->second += n->second;
                bl.free.erase(n);
            }
        }
        // an emptied block of its own goes back; the usual ones stay for reuse
        if (bl.own && bl.free.size() == 1 && bl.free.begin()->second == bl.size) {
            vkFreeMemory(dev_, bl.mem, nullptr);
            held_ -= bl.size;
            bl = Block{};
        }
    }

    bool HostCoherent(uint32_t bits) const { return Type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != ~0u; }
    VkDeviceSize Held() const { return held_; }

private:
    static constexpr VkDeviceSize kBlock = 64ull << 20;
    struct Block {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        uint8_t* map = nullptr;
        uint32_t type = 0;
        bool image = false, own = false;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
    };

    uint32_t Type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < props_.memoryTypeCount; i++)
            if ((bits >> i & 1) && (props_.memoryTypes[i].propertyFlags & want) == want) return i;
        return ~0u;
    }

    std::optional<Mem> Carve(Block& bl, uint32_t index, const VkMemoryRequirements& req) {
        const VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 256);
        for (auto it = bl.free.begin(); it != bl.free.end(); ++it) {
            const VkDeviceSize start = (it->first + align - 1) & ~(align - 1);
            const VkDeviceSize end = it->first + it->second;
            if (start + req.size > end) continue;
            const VkDeviceSize fs = it->first;
            bl.free.erase(it);
            if (start > fs) bl.free[fs] = start - fs;
            if (start + req.size < end) bl.free[start + req.size] = end - start - req.size;
            Mem m;
            m.mem = bl.mem;
            m.off = start;
            m.size = req.size;
            m.map = bl.map ? bl.map + start : nullptr;
            m.block = index;
            return m;
        }
        return std::nullopt;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    std::vector<Block> blocks_;
    VkDeviceSize held_ = 0;
    std::mutex m_;
};

// ---------------------------------------------------------------------------------------------------------------
// images and what the renderer last used them for: the D3D12 resource states, as barriers

enum class Use : uint8_t { Undefined, Color, Depth, Sampled, CopySrc, CopyDst };

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;    // what draws attach and sample (colour), or attach (depth)
    VkImageView sample = VK_NULL_HANDLE;  // a depth image's depth aspect, sampled
    Mem mem;
    VkFormat fmt = kBgra;
    uint32_t w = 0, h = 0, d = 1, levels = 1, layers = 1;
    bool depth = false;
    Use use = Use::Undefined;
    VkImageView Sampled() const { return sample ? sample : view; }
};

// a console surface the renderer draws into
struct Target : Image {
    uint32_t gw = 0, gh = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    Image fb;                     // copy read by draws that sample the target they render to
};

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex : Image {
    TextureDesc dsc;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint32_t icons = 0;    // the button icon setting it was uploaded with (button_icons.h)
    int icons_bc = 0;      // the BC its source has, once kept as BGRA for the icons
};

struct PsoKey {
    uint32_t atlas_guard;
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t strip;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;     // through the gamma lookup
    bool alpha = false;     // the source's alpha, else 1
    uint32_t taps = 1;      // n x n box filter per pixel
    float mul = 1;          // colour scale
    bool over = false;      // premultiplied over the target, which keeps its alpha
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    VkImageView view[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    Image* res[4] = {};
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k (no extension on this host: 1)
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// a draw's shaders: what they are made from, their cache keys and the hashes CompileHlsl returned
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule ps = VK_NULL_HANDLE;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

// a D3D12 style viewport rectangle (y down, top left)
struct Rect {
    float x, y, w, h;
};

// ---------------------------------------------------------------------------------------------------------------

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override { Submit(true); }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void ClearReport() override { counted_.clear(); }
    // done once the commands being recorded, the next timeline value, have run
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1}); }

private:
    struct Garbage {
        VkImage img = VK_NULL_HANDLE;
        VkImageView views[2] = {};
        VkBuffer buf = VK_NULL_HANDLE;
        VkSampler smp = VK_NULL_HANDLE;
        Mem mem;
    };
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkBuffer upload = VK_NULL_HANDLE;
        Mem upload_mem;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<Garbage> garbage;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };
    struct Alloc {
        VkBuffer buf;
        uint64_t off;
        uint8_t* cpu;
    };

    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    VkCommandBuffer Cmd() { return CurrentFrame().cmd; }
    Alloc Upload(uint64_t size, uint64_t align = 256);
    void Release(Image& im);
    void Release(VkBuffer b, const Mem& m) { CurrentFrame().garbage.push_back({.buf = b, .mem = m}); }
    void Collect(Frame& f);
    // images
    bool CreateImage(Image& im, VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels,
                     uint32_t layers, VkImageUsageFlags usage, VkImageViewType view_type);
    void Transition(Image& im, Use want);
    // rendering instances
    void EndRendering();
    void BeginRendering(Image* rt, Image* ds);
    void EnsureRendering() { BeginRendering(bound_rt_, bound_ds_); }
    void SetViewport(const Rect& r);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(Image& im) {
        Transition(im, Use::Color);
        bound_rt_ = &im;
        bound_ds_ = nullptr;
        SetFullViewport(im.w, im.h);
    }
    // submission
    void Begin();
    void Submit(bool wait, VkSemaphore wait_sem = VK_NULL_HANDLE, VkSemaphore signal_sem = VK_NULL_HANDLE);
    void WaitFence(uint64_t value);
    uint64_t Completed();
    // shaders and pipelines
    VkShaderModule Module(const std::string& src, const char* entry, Stage stage, uint64_t& hash);
    VkPipeline FullScreenPipeline(uint64_t key, VkShaderModule vs, VkShaderModule ps, VkFormat color, VkFormat depth,
                                  const VkPipelineColorBlendAttachmentState* blend, bool depth_write);
    VkPipeline EnsureBlitPipeline(VkFormat rtv, bool over);
    VkPipeline EnsureDepthCopyPipeline();
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    // descriptors
    struct Bindings {
        VkBuffer vc = VK_NULL_HANDLE, dc = VK_NULL_HANDLE;
        VkDeviceSize vc_off = 0, dc_off = 0, vc_size = 0, dc_size = 0;
        VkImageView views[4] = {};
        VkSampler samplers[4] = {};
    };
    void Push(const Bindings& b);
    VkSampler Sampler(const VkSamplerCreateInfo& ci, const VkSamplerCustomBorderColorCreateInfoEXT* border);
    void BlitSamplers(VkSampler out[4]);
    void DrawFullScreen(VkPipeline pso, const Alloc& cb, const VkImageView views[4]);
    void Blit(Image& src, const float uvrect[4], const BlitOptions& o);
    Image* Feedback(Target& t);
    // Draw, in call order
    bool DepthCopy(const State& s);
    void DrawDepthCopy(Target& src, Target& dst, VkPipeline pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices, Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    Image* BindTexture(const State& s, int stage, DrawTextures& stages);
    Image* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box, DrawTextures& stages);
    Image* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours, uint64_t id);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    float PointScale() const;
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c, Bindings& b);
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages, Bindings& b);
    void StageSamplers(const State& s, const DrawTextures& stages, bool clamp, VkSampler out[4]);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    // Init, in order
    bool CreateInstance();
    bool CreateDevice();
    bool CreateLayouts();
    void CreateFrames();
    bool CreateSwapchain();
    void DestroySwapchain();
    void CreateFixedImages();
    // Present, in call order
    void ResizeSwap();
    void AttachSurface();
    void UploadGammaRamp();
    void DrawFrame(Target* t, Image& out, uint32_t w, uint32_t h);
    void ReleaseOldTextures();
    void ResetFrame();
    void ApplySettings();
    void SavePipelineCache();
    // targets
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t);
    void UploadTarget(Target& t, const Surface& s);
    bool CreatePixelTexture(Image& tex, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c);
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(Target& src_t, Target& dst_t, const CopyList& l);
    void CopyByRegion(Target& src_t, Target& dst_t, const CopyList& l);
    // read-backs
    Image* PrepareCopySource(Target& t, Image& down);
    bool Shrink(Image& src, uint32_t w, uint32_t h, Image& down);
    bool ReadBack(Image& res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CopyImageToBuffer(Image& res, uint32_t w, uint32_t h, VkBuffer buf);
    struct Late;
    void ResetLate(Late& l, const Target& t);

    vkr::Platform& platform_ = vkr::GetPlatform();
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t qfam_ = 0;
    VkPhysicalDeviceProperties props_{};
    Allocator alloc_;
    VkFormat depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool custom_border_ = false;
    float max_aniso_ = 1;
    PFN_vkCmdPushDescriptorSetKHR push_ = nullptr;
    // core in Vulkan 1.3, the KHR and EXT extensions before it
    PFN_vkCmdBeginRendering begin_rendering_ = nullptr;
    PFN_vkCmdEndRendering end_rendering_ = nullptr;
    PFN_vkCmdBindVertexBuffers2 bind_vertex_buffers2_ = nullptr;
    // presentation: a swap chain, or an image drawn offscreen
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    bool surface_later_ = false;  // the platform holds the window for now: offscreen until it gives it
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    std::vector<Image> swap_images_;
    std::vector<VkSemaphore> rendered_;  // per swap chain image
    VkFormat swap_fmt_ = kBgra;
    uint32_t bb_w_ = 0, bb_h_ = 0;
    Image offscreen_;
    // sync
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> fence_value_{0};
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    // pipelines
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache pcache_ = VK_NULL_HANDLE;
    uint64_t pcache_saved_ = 0;
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, VkPipeline> psos_;
    std::unordered_map<uint64_t, VkPipeline> blit_psos_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    // resources
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::unordered_map<uint64_t, Tex> textures_;          // by data address, a P8 texture's with its palette's above
    Image null_[4];                                       // a texture of each kind, for stages without one
    Image lut_;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    bool precise_ = false;  // occlusion counts exact, as the console reports them
    // targets the next draw renders to, and the ones the open rendering instance has
    Image* bound_rt_ = nullptr;
    Image* bound_ds_ = nullptr;
    Image* act_rt_ = nullptr;
    Image* act_ds_ = nullptr;
    bool rendering_ = false;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    VkPipeline bound_pso_ = VK_NULL_HANDLE;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    float scale_ = 1.0f;
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        VkBuffer rb[kFrames] = {};
        Mem rb_mem[kFrames];
        Image down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                    // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};  // presents_ for DeliverLate
    Assembled asm_;
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    VkQueryPool queries_ = VK_NULL_HANDLE;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;
    std::deque<Report> reports_;
    uint64_t presents_ = 0;
    // VKTRACE=N: every call of every Nth frame in the log, for following what a frame does
    const uint64_t trace_every_ = [] {
        const char* v = std::getenv("VKTRACE");
        return v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    bool Tracing() const { return trace_every_ && presents_ % trace_every_ == 0; }
    void TraceTextures(const State& s);
};

// ---------------------------------------------------------------------------------------------------------------
// Init

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    vkr::InitShaderCompiler(platform_.CacheDir() / "shaders");
    if (!CreateInstance() || !CreateDevice() || !CreateLayouts()) return false;
    alloc_.Init(dev_, pd_);
    CreateFrames();
    surface_later_ = platform_.SurfaceHeld();
    if (!surface_later_) surface_ = platform_.CreateSurface(instance_);
    if (!CreateSwapchain()) return false;
    // a swap chain made later has up to as many images as frames in flight
    vkr::overlay::Init({instance_, pd_, dev_, qfam_, queue_,
                        uint32_t(surface_later_ ? kFrames : std::max<size_t>(2, swap_images_.size()))});
    Begin();
    CreateFixedImages();
    scale_ = app::RenderScale(guest_h_);
    LOGR("Vulkan: %s, %ux%u guest, %ux%u output, render scale %.3f, depth %s", props_.deviceName, width, height, bb_w_,
         bb_h_, scale_, depth_fmt_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
    return true;
}

bool VulkanBackend::CreateInstance() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "Shaolin Monks NX";
    ai.apiVersion = VK_API_VERSION_1_3;  // the highest the renderer uses; 1.2 devices run with extensions
    auto ext = platform_.InstanceExtensions();
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(ext.size());
    ci.ppEnabledExtensionNames = ext.data();
    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: no instance (%d)", int(r));
        return false;
    }
    return true;
}

bool VulkanBackend::CreateDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    if (!n) {
        LOGE("vulkan: no device");
        return false;
    }
    pd_ = pds[0];
    vkGetPhysicalDeviceProperties(pd_, &props_);
    const bool v13 = props_.apiVersion >= VK_API_VERSION_1_3;
    if (props_.apiVersion < VK_API_VERSION_1_2) {
        LOGE("vulkan: %s has Vulkan %u.%u, 1.2 is needed", props_.deviceName, VK_API_VERSION_MAJOR(props_.apiVersion),
             VK_API_VERSION_MINOR(props_.apiVersion));
        return false;
    }
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd_, &qn, qf.data());
    qfam_ = ~0u;
    for (uint32_t i = 0; i < qn && qfam_ == ~0u; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) qfam_ = i;
    if (qfam_ == ~0u) return false;

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(pd_, nullptr, &en, exts.data());
    auto has = [&](const char* name) {
        return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
    };
    std::vector<const char*> want = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (!has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        LOGE("vulkan: %s lacks VK_KHR_push_descriptor", props_.deviceName);
        return false;
    }
    // the surface is made after the device; offscreen hosts may lack the extension
    if (has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) want.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (!v13) {
        for (const char* e : {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME}) {
            if (!has(e)) {
                LOGE("vulkan: %s has Vulkan 1.2 without %s", props_.deviceName, e);
                return false;
            }
            want.push_back(e);
        }
    }
    custom_border_ = has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (custom_border_) want.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);

    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f12;
    if (v13) {
        f12.pNext = &f13;
        if (custom_border_) f13.pNext = &border;
    } else {
        f12.pNext = &fdr;
        fdr.pNext = &feds;
        if (custom_border_) feds.pNext = &border;
    }
    vkGetPhysicalDeviceFeatures2(pd_, &f);
    const bool dynamic_rendering = v13 ? f13.dynamicRendering : fdr.dynamicRendering;
    if (!v13 && !feds.extendedDynamicState) {
        LOGE("vulkan: %s lacks extended dynamic state", props_.deviceName);
        return false;
    }
    if (!dynamic_rendering || !f12.timelineSemaphore) {
        LOGE("vulkan: %s lacks dynamic rendering or timeline semaphores", props_.deviceName);
        return false;
    }
    custom_border_ = custom_border_ && border.customBorderColors && border.customBorderColorWithoutFormat;
    VkPhysicalDeviceFeatures base{};
    base.geometryShader = f.features.geometryShader;
    base.samplerAnisotropy = f.features.samplerAnisotropy;
    base.textureCompressionBC = f.features.textureCompressionBC;
    base.fillModeNonSolid = f.features.fillModeNonSolid;
    base.depthBiasClamp = f.features.depthBiasClamp;
    base.occlusionQueryPrecise = precise_ = f.features.occlusionQueryPrecise;
    if (!precise_) LOGW("vulkan: occlusion queries are not exact on this device, pixel counts may read as 1");
    max_aniso_ = f.features.samplerAnisotropy ? props_.limits.maxSamplerAnisotropy : 1.0f;
    if (!base.geometryShader) LOGW("vulkan: no geometry shaders, points and atlas sprites draw without them");
    if (!base.textureCompressionBC) LOGE("vulkan: no BC textures, DXT textures will be missing");
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR edr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    edr.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eeds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    eeds.extendedDynamicState = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT eb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    eb.customBorderColors = VK_TRUE;
    eb.customBorderColorWithoutFormat = VK_TRUE;
    void** tail;
    if (v13) {
        e12.pNext = &e13;
        tail = &e13.pNext;
    } else {
        e12.pNext = &edr;
        edr.pNext = &eeds;
        tail = &eeds.pNext;
    }
    if (custom_border_) *tail = &eb;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(want.size());
    dci.ppEnabledExtensionNames = want.data();
    dci.pEnabledFeatures = &base;
    VkResult r = vkCreateDevice(pd_, &dci, nullptr, &dev_);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: no device (%d)", int(r));
        return false;
    }
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
    push_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(dev_, "vkCmdPushDescriptorSetKHR"));
    auto load = [&](const char* core, const char* ext) {
        PFN_vkVoidFunction f = v13 ? vkGetDeviceProcAddr(dev_, core) : nullptr;
        return f ? f : vkGetDeviceProcAddr(dev_, ext);
    };
    begin_rendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(load("vkCmdBeginRendering", "vkCmdBeginRenderingKHR"));
    end_rendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(load("vkCmdEndRendering", "vkCmdEndRenderingKHR"));
    bind_vertex_buffers2_ = reinterpret_cast<PFN_vkCmdBindVertexBuffers2>(load("vkCmdBindVertexBuffers2", "vkCmdBindVertexBuffers2EXT"));
    if (!push_ || !begin_rendering_ || !end_rendering_ || !bind_vertex_buffers2_) {
        LOGE("vulkan: %s lacks an entry point the renderer needs", props_.deviceName);
        return false;
    }

    // a 24 bit depth buffer as the console has, else 32 bit float
    for (VkFormat fmt : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd_, fmt, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) == need) {
            depth_fmt_ = fmt;
            break;
        }
    }
    LOGR("Vulkan device: %s, driver %u.%u.%u, API %u.%u", props_.deviceName, VK_API_VERSION_MAJOR(props_.driverVersion),
         VK_API_VERSION_MINOR(props_.driverVersion), VK_API_VERSION_PATCH(props_.driverVersion),
         VK_API_VERSION_MAJOR(props_.apiVersion), VK_API_VERSION_MINOR(props_.apiVersion));
    return true;
}

bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[vkr::kBindingCount] = {};
    for (uint32_t i = 0; i < vkr::kBindingCount; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        b[i].descriptorType = i < vkr::kBindingTexture0   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < vkr::kBindingSampler0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                          : VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sci.bindingCount = vkr::kBindingCount;
    sci.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev_, &sci, nullptr, &set_layout_) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &set_layout_;
    if (vkCreatePipelineLayout(dev_, &pci, nullptr, &layout_) != VK_SUCCESS) return false;

    // the pipeline cache of earlier runs
    std::vector<char> data;
    if (FILE* f = std::fopen((platform_.CacheDir() / "pipelines.bin").c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        data.resize(size_t(std::max(0L, std::ftell(f))));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        std::fclose(f);
    }
    VkPipelineCacheCreateInfo cci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cci.initialDataSize = data.size();
    cci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(dev_, &cci, nullptr, &pcache_) != VK_SUCCESS) {
        cci.initialDataSize = 0;
        cci.pInitialData = nullptr;
        vkCreatePipelineCache(dev_, &cci, nullptr, &pcache_);
    }
    return true;
}

void VulkanBackend::SavePipelineCache() {
    size_t n = 0;
    if (vkGetPipelineCacheData(dev_, pcache_, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(dev_, pcache_, &n, data.data()) != VK_SUCCESS) return;
    const auto path = platform_.CacheDir() / "pipelines.bin";
    auto tmp = path;
    tmp += ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        const bool ok = std::fwrite(data.data(), 1, n, f) == n;
        std::fclose(f);
        std::error_code ec;
        if (ok) std::filesystem::remove(path, ec), std::filesystem::rename(tmp, path, ec);
    }
}

void VulkanBackend::CreateFrames() {
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &tci;
    vkCreateSemaphore(dev_, &sci, nullptr, &timeline_);
    VkSemaphoreCreateInfo bci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = qfam_;
        vkCreateCommandPool(dev_, &pci, nullptr, &f.pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev_, &ai, &f.cmd);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kUploadSize;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(dev_, &bi, nullptr, &f.upload);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev_, f.upload, &req);
        f.upload_mem = alloc_.Alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
        vkBindBufferMemory(dev_, f.upload, f.upload_mem.mem, f.upload_mem.off);
        vkCreateSemaphore(dev_, &bci, nullptr, &f.acquired);
    }
}

bool VulkanBackend::CreateSwapchain() {
    platform_.OutputSize(bb_w_, bb_h_);
    if (!surface_) {
        // offscreen: one image the frame is drawn to, read back when the platform wants it
        return CreateImage(offscreen_, VK_IMAGE_TYPE_2D, kBgra, bb_w_, bb_h_, 1, 1, 1,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D);
    }
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd_, surface_, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, formats.data());
    VkSurfaceFormatKHR sf = formats.empty() ? VkSurfaceFormatKHR{kBgra, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            sf = f;
            if (f.format == kBgra) break;
        }
    swap_fmt_ = sf.format;
    if (caps.currentExtent.width != ~0u) {
        bb_w_ = caps.currentExtent.width;
        bb_h_ = caps.currentExtent.height;
    }
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!app::Get().vsync)
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, std::min(3u, caps.maxImageCount ? caps.maxImageCount : 3u));
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = {bb_w_, bb_h_};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(dev_, &ci, nullptr, &swap);
    if (swap_) DestroySwapchain();
    swap_ = swap;
    if (r != VK_SUCCESS) {
        LOGE("vulkan: no swap chain (%d)", int(r));
        return false;
    }
    uint32_t in = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &in, nullptr);
    std::vector<VkImage> imgs(in);
    vkGetSwapchainImagesKHR(dev_, swap_, &in, imgs.data());
    swap_images_.resize(in);
    rendered_.resize(in);
    for (uint32_t i = 0; i < in; i++) {
        Image& im = swap_images_[i];
        im = Image{};
        im.img = imgs[i];
        im.fmt = sf.format;
        im.w = bb_w_;
        im.h = bb_h_;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = im.img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sf.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev_, &vci, nullptr, &im.view);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev_, &sci, nullptr, &rendered_[i]);
    }
    LOGI("vulkan: swap chain %ux%u, %u images, format %d, present mode %d", bb_w_, bb_h_, in, int(sf.format), int(mode));
    return true;
}

void VulkanBackend::DestroySwapchain() {
    vkDeviceWaitIdle(dev_);
    for (auto& im : swap_images_) vkDestroyImageView(dev_, im.view, nullptr);
    for (auto s : rendered_) vkDestroySemaphore(dev_, s, nullptr);
    swap_images_.clear();
    rendered_.clear();
    vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
}

VulkanBackend::~VulkanBackend() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    vkr::overlay::Shutdown();
    SavePipelineCache();
}

void VulkanBackend::CreateFixedImages() {
    // a black texture of each kind for stages without one, and the gamma lookup
    const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CreateImage(null_[kTex2D], VK_IMAGE_TYPE_2D, kBgra, 1, 1, 1, 1, 1, u, VK_IMAGE_VIEW_TYPE_2D);
    CreateImage(null_[kTex3D], VK_IMAGE_TYPE_3D, kBgra, 1, 1, 1, 1, 1, u, VK_IMAGE_VIEW_TYPE_3D);
    CreateImage(null_[kTexCube], VK_IMAGE_TYPE_2D, kBgra, 1, 1, 1, 1, 6, u, VK_IMAGE_VIEW_TYPE_CUBE);
    null_[kTexNone] = null_[kTex2D];
    CreateImage(lut_, VK_IMAGE_TYPE_2D, kBgra, 256, 1, 1, 1, 1, u, VK_IMAGE_VIEW_TYPE_2D);
    VkClearColorValue black{};
    for (Image* im : {&null_[kTex2D], &null_[kTex3D], &null_[kTexCube], &lut_}) {
        Transition(*im, Use::CopyDst);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers};
        vkCmdClearColorImage(Cmd(), im->img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        Transition(*im, Use::Sampled);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// images, barriers, rendering instances

bool VulkanBackend::CreateImage(Image& im, VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d,
                                uint32_t levels, uint32_t layers, VkImageUsageFlags usage, VkImageViewType view_type) {
    im = Image{};
    im.fmt = fmt;
    im.w = w;
    im.h = h;
    im.d = d;
    im.levels = levels;
    im.layers = layers;
    im.depth = fmt == depth_fmt_;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = view_type == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = type;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &im.img) != VK_SUCCESS) {
        LOGE("vulkan: image %ux%ux%u format %d failed", w, h, d, int(fmt));
        im.img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, im.img, &req);
    im.mem = alloc_.Alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    if (!im.mem.mem) {
        vkDestroyImage(dev_, im.img, nullptr);
        im.img = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(dev_, im.img, im.mem.mem, im.mem.off);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = view_type;
    vci.format = fmt;
    vci.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                        : VK_IMAGE_ASPECT_COLOR_BIT),
                            0, levels, 0, layers};
    vkCreateImageView(dev_, &vci, nullptr, &im.view);
    if (im.depth) {
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCreateImageView(dev_, &vci, nullptr, &im.sample);
    }
    return true;
}

// once the GPU is past this frame
void VulkanBackend::Release(Image& im) {
    if (!im.img) return;
    if (bound_rt_ == &im || bound_ds_ == &im) bound_rt_ = bound_ds_ = nullptr;
    if (act_rt_ == &im || act_ds_ == &im) EndRendering();
    CurrentFrame().garbage.push_back({.img = im.img, .views = {im.view, im.sample}, .mem = im.mem});
    im.img = VK_NULL_HANDLE;
    im.view = im.sample = VK_NULL_HANDLE;
    im.mem = {};
}

void VulkanBackend::Collect(Frame& f) {
    for (auto& g : f.garbage) {
        for (VkImageView v : g.views)
            if (v) vkDestroyImageView(dev_, v, nullptr);
        if (g.img) vkDestroyImage(dev_, g.img, nullptr);
        if (g.buf) vkDestroyBuffer(dev_, g.buf, nullptr);
        if (g.smp) vkDestroySampler(dev_, g.smp, nullptr);
        alloc_.Free(g.mem);
    }
    f.garbage.clear();
}

struct UseInfo {
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};

UseInfo Info(Use u) {
    switch (u) {
        case Use::Color:
            return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case Use::Depth:
            return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case Use::Sampled:
            return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
        case Use::CopySrc: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        case Use::CopyDst: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        default: return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    }
}

// the D3D12 Transition: a barrier between two uses, the layout always GENERAL; no barrier inside a rendering instance
void VulkanBackend::Transition(Image& im, Use want) {
    if (im.use == want || !im.img) return;
    EndRendering();
    const UseInfo from = Info(im.use), to = Info(want);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = from.access;
    b.dstAccessMask = to.access;
    b.oldLayout = im.use == Use::Undefined ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.img;
    b.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                      : VK_IMAGE_ASPECT_COLOR_BIT),
                          0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(Cmd(), from.stage, to.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.use = want;
}

void VulkanBackend::EndRendering() {
    if (!rendering_) return;
    end_rendering_(Cmd());
    rendering_ = false;
    act_rt_ = act_ds_ = nullptr;
}

void VulkanBackend::BeginRendering(Image* rt, Image* ds) {
    if (rendering_ && act_rt_ == rt && act_ds_ == ds) return;
    EndRendering();
    if (!rt && !ds) return;
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    for (auto* a : {&ca, &da}) {
        a->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        a->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (rt) ca.imageView = rt->view;
    if (ds) da.imageView = ds->view;
    const Image* any = rt ? rt : ds;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = rt ? 1 : 0;
    ri.pColorAttachments = rt ? &ca : nullptr;
    ri.pDepthAttachment = ds ? &da : nullptr;
    ri.pStencilAttachment = ds ? &da : nullptr;
    begin_rendering_(Cmd(), &ri);
    rendering_ = true;
    act_rt_ = rt;
    act_ds_ = ds;
}

// a D3D style rectangle (top left, y down) as the flipped Vulkan viewport that gives D3D's clip space
void VulkanBackend::SetViewport(const Rect& r) {
    VkViewport vp{r.x, r.y + r.h, r.w, -r.h, 0.0f, 1.0f};
    vkCmdSetViewport(Cmd(), 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    VkRect2D sc{{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
    vkCmdSetScissor(Cmd(), 0, 1, &sc);
}

void VulkanBackend::SetFullViewport(uint32_t w, uint32_t h) {
    SetViewport({0, 0, float(w), float(h)});
    SetScissor(0, 0, int32_t(w), int32_t(h));
}

// ---------------------------------------------------------------------------------------------------------------
// submission

void VulkanBackend::Begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(Cmd(), &bi);
    bound_rt_ = bound_ds_ = nullptr;
    rendering_ = false;
    act_rt_ = act_ds_ = nullptr;
    bound_pso_ = VK_NULL_HANDLE;
}

void VulkanBackend::Submit(bool wait, VkSemaphore wait_sem, VkSemaphore signal_sem) {
    EndRendering();
    Frame& f = CurrentFrame();
    vkEndCommandBuffer(f.cmd);
    const uint64_t value = fence_value_ + 1;
    VkSemaphore signals[2] = {timeline_, signal_sem};
    uint64_t values[2] = {value, 0};
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.signalSemaphoreValueCount = signal_sem ? 2 : 1;
    ti.pSignalSemaphoreValues = values;
    uint64_t wait_value = 0;
    ti.waitSemaphoreValueCount = wait_sem ? 1 : 0;
    ti.pWaitSemaphoreValues = &wait_value;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.waitSemaphoreCount = wait_sem ? 1 : 0;
    si.pWaitSemaphores = &wait_sem;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = signal_sem ? 2 : 1;
    si.pSignalSemaphores = signals;
    VKCHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
    fence_value_ = value;
    f.fence = value;
    if (wait) {
        WaitFence(value);
        vkResetCommandPool(dev_, f.pool, 0);
        Collect(f);
        f.used = 0;
        Begin();
    }
}

uint64_t VulkanBackend::Completed() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev_, timeline_, &v);
    return v;
}

void VulkanBackend::WaitFence(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    VKCHECK(vkWaitSemaphores(dev_, &wi, UINT64_MAX));
}

VulkanBackend::Alloc VulkanBackend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    align = std::max<uint64_t>(align, props_.limits.minUniformBufferOffsetAlignment);
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload, o, f.upload_mem.map + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<uint64_t>(size, 256);
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer b;
    vkCreateBuffer(dev_, &bi, nullptr, &b);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b, &req);
    Mem m = alloc_.Alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
    vkBindBufferMemory(dev_, b, m.mem, m.off);
    Release(b, m);
    return {b, 0, m.map};
}

// ---------------------------------------------------------------------------------------------------------------
// shaders, pipelines, descriptors

VkShaderModule VulkanBackend::Module(const std::string& src, const char* entry, Stage stage, uint64_t& hash) {
    const auto t = PerfClock::now();
    const std::vector<uint32_t>& code = vkr::CompileHlsl(src, entry, stage, hash);
    auto it = modules_.find(hash);
    if (it != modules_.end()) return it->second;
    g_perf.shaders++;
    VkShaderModule m = VK_NULL_HANDLE;
    if (!code.empty()) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        if (vkCreateShaderModule(dev_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    g_perf.compile += PerfMs(t);
    modules_[hash] = m;
    return m;
}

struct PipelineParts {
    VkPipelineShaderStageCreateInfo stages[3];
    uint32_t stage_count = 0;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[5] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat color_fmt = VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

    PipelineParts(VkPipelineLayout layout, VkFormat color, VkFormat depth, bool vertex_strides) {
        vp.viewportCount = vp.scissorCount = 1;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        att.colorWriteMask = 0xF;
        cb.attachmentCount = color ? 1 : 0;
        cb.pAttachments = &att;
        dy.dynamicStateCount = vertex_strides ? 5 : 4;
        dy.pDynamicStates = dyn;
        color_fmt = color;
        ri.colorAttachmentCount = color ? 1 : 0;
        ri.pColorAttachmentFormats = &color_fmt;
        ri.depthAttachmentFormat = depth;
        ri.stencilAttachmentFormat = depth;
        pci.pNext = &ri;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &dy;
        pci.layout = layout;
    }
    void Stage(VkShaderStageFlagBits stage, VkShaderModule m, const char* entry) {
        stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, stage, m, entry, nullptr};
    }
    VkPipeline Create(VkDevice dev, VkPipelineCache cache) {
        pci.stageCount = stage_count;
        pci.pStages = stages;
        VkPipeline p = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(dev, cache, 1, &pci, nullptr, &p) != VK_SUCCESS) {
            LOGE("vulkan: pipeline creation failed");
            return VK_NULL_HANDLE;
        }
        return p;
    }
};

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
VkPipeline VulkanBackend::FullScreenPipeline(uint64_t key, VkShaderModule vs, VkShaderModule ps, VkFormat color,
                                             VkFormat depth, const VkPipelineColorBlendAttachmentState* blend,
                                             bool depth_write) {
    auto& pso = blit_psos_[key];
    if (pso) return pso;
    if (!vs || !ps) return VK_NULL_HANDLE;
    PipelineParts p(layout_, color, depth, false);
    p.Stage(VK_SHADER_STAGE_VERTEX_BIT, vs, "main");
    p.Stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main");
    if (blend) p.att = *blend;
    if (depth_write) {
        p.ds.depthTestEnable = VK_TRUE;
        p.ds.depthWriteEnable = VK_TRUE;
        p.ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    pso = p.Create(dev_, pcache_);
    return pso;
}

VkPipeline VulkanBackend::EnsureBlitPipeline(VkFormat rtv, bool over) {
    const uint64_t key = uint64_t(rtv) | uint64_t(over) << 32 | 1ull << 40;
    if (auto it = blit_psos_.find(key); it != blit_psos_.end()) return it->second;
    uint64_t h;
    VkShaderModule vs = Module(kBlitHlsl, "vs", Stage::Vertex, h);
    VkShaderModule ps = Module(kBlitHlsl, "psmain", Stage::Fragment, h);
    VkPipelineColorBlendAttachmentState b{};
    b.colorWriteMask = 0xF;
    if (over) {
        // premultiplied colour, the target keeps its alpha
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    return FullScreenPipeline(key, vs, ps, rtv, VK_FORMAT_UNDEFINED, &b, false);
}

// DepthCopy's pipeline: writes depth only, always passing
VkPipeline VulkanBackend::EnsureDepthCopyPipeline() {
    const uint64_t key = ~0ull;
    if (auto it = blit_psos_.find(key); it != blit_psos_.end()) return it->second;
    uint64_t h;
    VkShaderModule vs = Module(kDepthHlsl, "vs", Stage::Vertex, h);
    VkShaderModule ps = Module(kDepthHlsl, "psmain", Stage::Fragment, h);
    return FullScreenPipeline(key, vs, ps, VK_FORMAT_UNDEFINED, depth_fmt_, nullptr, true);
}

VkPrimitiveTopology Topo(uint32_t topo, bool strip) {
    switch (topo) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        default: return strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    const uint64_t h = Hash(&k, sizeof(k));
    if (auto it = psos_.find(h); it != psos_.end()) return it->second;
    g_perf.psos++;
    PipelineParts p(layout_, k.has_rt ? VkFormat(k.rtfmt) : VK_FORMAT_UNDEFINED, k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED, true);
    p.Stage(VK_SHADER_STAGE_VERTEX_BIT, vs, "main");
    p.Stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main");
    // the key's vertex attributes, in register order: glslang numbers the inputs as declared
    VkVertexInputAttributeDescription attrs[16];
    VkVertexInputBindingDescription binds[16];
    uint32_t na = 0, nb = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        const uint32_t e = k.elements[r];
        attrs[na] = {na, (e >> 8) & 0xFF, VertexFormat(e & 0xFF), e >> 16};
        nb = std::max(nb, ((e >> 8) & 0xFF) + 1);
        na++;
    }
    for (uint32_t b = 0; b < nb; b++) binds[b] = {b, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    p.vi.vertexAttributeDescriptionCount = na;
    p.vi.pVertexAttributeDescriptions = attrs;
    p.vi.vertexBindingDescriptionCount = nb;
    p.vi.pVertexBindingDescriptions = binds;
    p.ia.topology = Topo(k.topo, k.strip);
    auto& b = p.att;
    b.blendEnable = k.blend;
    b.srcColorBlendFactor = Blend(k.src, false);
    b.dstColorBlendFactor = Blend(k.dst, false);
    b.colorBlendOp = BlendOp(k.op);
    b.srcAlphaBlendFactor = Blend(k.src, true);
    b.dstAlphaBlendFactor = Blend(k.dst, true);
    b.alphaBlendOp = BlendOp(k.op);
    b.colorWriteMask = k.wmask;  // D3D12's RGBA bits are Vulkan's
    p.rs.polygonMode = k.fill == kFillWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    p.rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // the flipped viewport keeps D3D's orientation on the target, so the windings mean what they do in D3D12
    p.rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    p.rs.depthBiasEnable = k.zbias || k.zslope;
    p.rs.depthBiasConstantFactor = float(k.zbias);
    p.rs.depthBiasSlopeFactor = k.zslope;
    p.ds.depthTestEnable = k.zen;
    p.ds.depthWriteEnable = k.zwrite;
    p.ds.depthCompareOp = k.zen ? VkCmp(k.zfunc) : VK_COMPARE_OP_ALWAYS;
    p.ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), VkCmp(k.sfunc), k.smask, k.swmask, 0};
    p.ds.front = p.ds.back = so;
    VkShaderModule gs = VK_NULL_HANDLE;
    uint64_t gh;
    // a point is drawn as its whole console pixel by a geometry shader, solid, front facing and never culled
    if (k.topo == uint32_t(Topology::Points)) {
        gs = Module(PointHlsl(), "main", Stage::Geometry, gh);
        p.rs.polygonMode = VK_POLYGON_MODE_FILL;
        p.rs.cullMode = VK_CULL_MODE_NONE;
        p.rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    }
    if (k.atlas_guard) gs = Module(shaolin::AtlasBoundsHlsl(), "main", Stage::Geometry, gh);
    if ((k.topo == uint32_t(Topology::Points) || k.atlas_guard) && gs) p.Stage(VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main");
    VkPipeline pso = p.Create(dev_, pcache_);
    psos_[h] = pso;
    return pso;
}

void VulkanBackend::Push(const Bindings& b) {
    VkDescriptorBufferInfo bufs[2] = {{b.vc ? b.vc : b.dc, b.vc ? b.vc_off : b.dc_off, b.vc ? b.vc_size : b.dc_size},
                                      {b.dc, b.dc_off, b.dc_size}};
    VkDescriptorImageInfo imgs[4], smps[4];
    VkWriteDescriptorSet w[vkr::kBindingCount];
    for (uint32_t i = 0; i < vkr::kBindingCount; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    for (int i = 0; i < 2; i++) {
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pBufferInfo = &bufs[i];
    }
    for (int i = 0; i < 4; i++) {
        imgs[i] = {VK_NULL_HANDLE, b.views[i] ? b.views[i] : null_[kTex2D].view, VK_IMAGE_LAYOUT_GENERAL};
        smps[i] = {b.samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[vkr::kBindingTexture0 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[vkr::kBindingTexture0 + i].pImageInfo = &imgs[i];
        w[vkr::kBindingSampler0 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[vkr::kBindingSampler0 + i].pImageInfo = &smps[i];
    }
    push_(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, vkr::kBindingCount, w);
}

VkSampler VulkanBackend::Sampler(const VkSamplerCreateInfo& ci, const VkSamplerCustomBorderColorCreateInfoEXT* border) {
    uint64_t key = Hash(&ci.magFilter, offsetof(VkSamplerCreateInfo, unnormalizedCoordinates) - offsetof(VkSamplerCreateInfo, magFilter));
    if (border) key = Hash(&border->customBorderColor, sizeof(border->customBorderColor), key);
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    if (samplers_.size() >= kSamplersKept) {
        for (auto& [k, s] : samplers_) CurrentFrame().garbage.push_back({.smp = s});
        samplers_.clear();
    }
    VkSamplerCreateInfo c = ci;
    c.pNext = border;
    VkSampler s = VK_NULL_HANDLE;
    VKCHECK(vkCreateSampler(dev_, &c, nullptr, &s));
    samplers_[key] = s;
    return s;
}

// four clamp samplers: linear at s0, point at s1 to s3
void VulkanBackend::BlitSamplers(VkSampler out[4]) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    out[0] = Sampler(ci, nullptr);
    ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    out[1] = out[2] = out[3] = Sampler(ci, nullptr);
}

// the full screen triangle: cb at b0 and b1, views at t0 to t3, the blit samplers
void VulkanBackend::DrawFullScreen(VkPipeline pso, const Alloc& cb, const VkImageView views[4]) {
    if (!pso) return;
    Bindings b;
    b.vc = b.dc = cb.buf;
    b.vc_off = b.dc_off = cb.off;
    b.vc_size = b.dc_size = 256;
    for (int i = 0; i < 4; i++) b.views[i] = views[i];
    BlitSamplers(b.samplers);
    EnsureRendering();
    vkCmdBindPipeline(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    bound_pso_ = pso;
    Push(b);
    vkCmdDraw(Cmd(), 3, 1, 0, 0);
}

// src over the bound target and viewport
void VulkanBackend::Blit(Image& src, const float uvrect[4], const BlitOptions& o) {
    Transition(src, Use::Sampled);
    Transition(lut_, Use::Sampled);
    Alloc cb = Upload(256, 256);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    const VkImageView views[4] = {src.view, lut_.view, null_[kTex2D].view, null_[kTex2D].view};
    DrawFullScreen(EnsureBlitPipeline(bound_rt_ ? bound_rt_->fmt : kBgra, o.over), cb, views);
}

// a copy of the target to read while drawing to it
Image* VulkanBackend::Feedback(Target& t) {
    if (!t.fb.img && !CreateImage(t.fb, VK_IMAGE_TYPE_2D, t.fmt, t.w, t.h, 1, 1, 1,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D))
        return nullptr;
    Transition(t, Use::CopySrc);
    Transition(t.fb, Use::CopyDst);
    VkImageCopy r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {t.w, t.h, 1};
    vkCmdCopyImage(Cmd(), t.img, VK_IMAGE_LAYOUT_GENERAL, t.fb.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    Transition(t.fb, Use::Sampled);
    return &t.fb;
}

// ---------------------------------------------------------------------------------------------------------------
// targets

// a resized target gives back its resources
void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    Release(it->second.fb);
    Release(it->second);
    targets_.erase(it);
}

// the host image of a new target, at render scale, cleared
bool VulkanBackend::CreateTargetResource(Target& t) {
    const uint32_t gw = t.gw, gh = t.gh;
    const bool depth = t.depth;
    const uint32_t w = std::max(1u, uint32_t(std::lround(gw * scale_)));
    const uint32_t h = std::max(1u, uint32_t(std::lround(gh * scale_)));
    const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(t, VK_IMAGE_TYPE_2D, depth ? depth_fmt_ : kBgra, w, h, 1, 1, 1, u, VK_IMAGE_VIEW_TYPE_2D)) return false;
    t.gw = gw;
    t.gh = gh;
    Transition(t, Use::CopyDst);
    if (depth) {
        VkClearDepthStencilValue one{1.0f, 0};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(Cmd(), t.img, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
    } else {
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(Cmd(), t.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t VulkanBackend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

VulkanBackend::TargetSlot VulkanBackend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    if (!slot.sized) return nullptr;
    target_used_[slot.data] = frame_;
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    target_used_[slot.data] = frame_;
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target& t = targets_[slot.key];
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t)) {
        targets_.erase(slot.key);
        return nullptr;
    }
    g_perf.targets++;
    t.data = slot.data;
    t.bytes = depth ? slot.gw * slot.gh * 4 : GuestPitch(slot.gw) * slot.gh;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    return &t;
}

bool VulkanBackend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(*rt, Use::Color);
        rt->drawn = true;
    }
    if (ds) {
        Transition(*ds, Use::Depth);
        ds->drawn = true;
    }
    bound_rt_ = rt;
    bound_ds_ = ds;
    return true;
}

// src's depth drawn into dst's depth buffer
void VulkanBackend::DrawDepthCopy(Target& src, Target& dst, VkPipeline pso) {
    Transition(src, Use::Sampled);
    Transition(dst, Use::Depth);
    bound_rt_ = nullptr;
    bound_ds_ = &dst;
    SetFullViewport(dst.w, dst.h);
    Alloc cb = Upload(256, 256);
    const VkImageView views[4] = {src.sample, null_[kTex2D].view, null_[kTex2D].view, null_[kTex2D].view};
    DrawFullScreen(pso, cb, views);
    dst.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube || d.depth != 1)
        return false;
    uint32_t gw, gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    VkPipeline pso = EnsureDepthCopyPipeline();
    if (!pso) return false;
    DrawDepthCopy(si->second, di->second, pso);
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left sampled
bool VulkanBackend::CreatePixelTexture(Image& tex, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!CreateImage(tex, VK_IMAGE_TYPE_2D, kBgra, w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_VIEW_TYPE_2D))
        return false;
    Alloc a = Upload(uint64_t(w) * h * 4, 16);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    Transition(tex, Use::CopyDst);
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(Cmd(), a.buf, tex.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    Transition(tex, Use::Sampled);
    return true;
}

// CPU pixels of a colour surface into its host target, scaled
void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "vulkan: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Image tex;
    if (!CreatePixelTexture(tex, px, pitch, d.width, d.height)) return;
    SetBlitTarget(t);
    float uv[4] = {0, 0, 1, 1};
    Blit(tex, uv, {.alpha = true, .over = over});
    Release(tex);
    // as after any blit, the draw binds its targets again
    bound_rt_ = bound_ds_ = nullptr;
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// ---------------------------------------------------------------------------------------------------------------
// textures

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

Image* VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                        DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) {
        Image* fb = Feedback(t);
        if (fb) stages.view[stage] = fb->view;
        return fb;
    }
    Transition(t, Use::Sampled);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    stages.view[stage] = t.Sampled();
    return &t;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img && tx.dsc.format == d.format && tx.dsc.width == d.width && tx.dsc.height == d.height &&
           tx.dsc.depth == d.depth && tx.dsc.levels == d.levels && tx.dsc.cube == d.cube && tx.dsc.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    Release(tx);
    const VkFormat fmt = HostFormat(fi.host);
    uint32_t w = d.width, h = d.height, levels = d.levels;
    if (fi.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        levels = 1;
    }
    Tex fresh;
    if (!CreateImage(fresh, d.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, fmt, w, h, d.depth, levels, d.cube ? 6 : 1,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D)) {
        textures_.erase(key);
        return false;
    }
    static_cast<Image&>(tx) = fresh;
    tx.dsc = d;
    tx.sig = tx.frame = tx.palette = 0;
    return true;
}

void DumpTexture(const TextureDesc& d, VkFormat fmt, uint32_t w, uint32_t h, const std::vector<uint8_t>& px, uint32_t pitch,
                 uint64_t id);

// which BC a host format is, 0 for none
int BcOf(VkFormat f) {
    return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 1 : f == VK_FORMAT_BC2_UNORM_BLOCK ? 2 : f == VK_FORMAT_BC3_UNORM_BLOCK ? 3 : 0;
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours, uint64_t id) {
    // The textures with the button icons are drawn on: it is kept as BGRA from then on, and any BC source decoded
    const int source_bc = tx.dsc.format == d.format && tx.icons_bc ? tx.icons_bc : BcOf(tx.fmt);
    const bool icons = vkr::icons::SwitchIcons() && vkr::icons::IsButtonAtlas(id) && !d.cube && d.depth <= 1;
    if (icons && tx.fmt != kBgra && source_bc) {
        Tex fresh;
        if (CreateImage(fresh, VK_IMAGE_TYPE_2D, kBgra, tx.w, tx.h, 1, tx.levels, 1,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D)) {
            const TextureDesc dsc = tx.dsc;
            Release(tx);
            static_cast<Image&>(tx) = fresh;
            tx.dsc = dsc;
            tx.icons_bc = source_bc;
            LOGR("vulkan: Switch button icons drawn over texture %016llx", (unsigned long long)id);
        }
    }
    Transition(tx, Use::CopyDst);
    const uint32_t faces = d.cube ? 6 : 1;
    const uint32_t block = BlockBytes(tx.fmt);
    const uint32_t texel = tx.fmt == VK_FORMAT_R32_SFLOAT || tx.fmt == kBgra ? 4 : 0;
    std::vector<uint8_t> px, drawn;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            const uint32_t lw = std::max(1u, tx.w >> l), lh = std::max(1u, tx.h >> l), ld = std::max(1u, tx.d >> l);
            if (tx.fmt == kBgra && (source_bc || icons)) {
                // a BC source kept as BGRA, or a BGRA one; with the icons, the smaller levels are made from the
                // drawn-on first one
                std::vector<uint8_t> bgra;
                uint32_t bgra_pitch = 0;
                if (icons && l > 0 && !drawn.empty()) {
                    bgra.resize(size_t(lw) * lh * 4);
                    const uint32_t pw = std::max(1u, lw * 2), ph = std::max(1u, lh * 2);
                    for (uint32_t y = 0; y < lh; y++)
                        for (uint32_t x = 0; x < lw; x++)
                            for (int ch = 0; ch < 4; ch++) {
                                uint32_t sum = 0;
                                for (uint32_t k = 0; k < 4; k++)
                                    sum += drawn[(size_t(std::min(y * 2 + k / 2, ph - 1)) * pw + std::min(x * 2 + k % 2, pw - 1)) * 4 + ch];
                                bgra[(size_t(y) * lw + x) * 4 + ch] = uint8_t((sum + 2) / 4);
                            }
                    bgra_pitch = lw * 4;
                } else {
                    if (source_bc) {
                        vkbc::Decode(source_bc, px.data(), pitch, lw, lh, ld, bgra, bgra_pitch);
                    } else {
                        // tightly packed, so the smaller levels can be made from it
                        bgra.resize(size_t(lw) * lh * 4);
                        for (uint32_t y = 0; y < lh; y++) std::memcpy(&bgra[size_t(y) * lw * 4], &px[size_t(y) * pitch], size_t(lw) * 4);
                        bgra_pitch = lw * 4;
                    }
                    if (icons && l == 0) {
                        vkr::icons::Paint(id, bgra.data(), lw, lh, bgra_pitch);
                        if (!source_bc && f == 0) LOGR("vulkan: Switch button icons drawn over texture %016llx", (unsigned long long)id);
                    }
                }
                if (icons) drawn = bgra;
                px.swap(bgra);
                pitch = bgra_pitch;
            }
            if (l == 0 && f == 0) DumpTexture(d, tx.fmt, tx.w, tx.h, px, pitch, id);
            // rows of the level as Vulkan reads them tightly packed: blocks of four rows for BC
            const uint32_t row = block ? std::max(1u, (lw + 3) / 4) * block : lw * texel;
            const uint32_t rows = block ? std::max(1u, (lh + 3) / 4) : lh;
            Alloc a = Upload(uint64_t(row) * rows * ld, 16);
            const size_t src_rows = pitch ? px.size() / pitch / ld : 0;
            for (uint32_t z = 0; z < ld; z++)
                for (uint32_t y = 0; y < rows; y++) {
                    uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row;
                    if (y < src_rows) std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row));
                    else std::memset(dst, 0, row);
                }
            VkBufferImageCopy r{};
            r.bufferOffset = a.off;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
            r.imageExtent = {lw, lh, ld};
            vkCmdCopyBufferToImage(Cmd(), a.buf, tx.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
    }
    Transition(tx, Use::Sampled);
}

// VKTEXDUMP=folder: each texture up to 512 x 512, its first level as BGRA in a TGA named by the hash of its guest
// bytes, once. For finding a texture to replace (the button icons).
void DumpTexture(const TextureDesc& d, VkFormat fmt, uint32_t w, uint32_t h, const std::vector<uint8_t>& px, uint32_t pitch,
                 uint64_t id) {
    static const char* dir = std::getenv("VKTEXDUMP");
    static std::unordered_map<uint64_t, bool> done;
    static std::mutex lock;
    if (!dir || w > 512 || h > 512 || d.cube || d.depth > 1) return;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (done[id]) return;
        done[id] = true;
    }
    std::vector<uint8_t> bgra;
    uint32_t bgra_pitch = w * 4;
    if (fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || fmt == VK_FORMAT_BC2_UNORM_BLOCK || fmt == VK_FORMAT_BC3_UNORM_BLOCK) {
        const int bc = fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 1 : fmt == VK_FORMAT_BC2_UNORM_BLOCK ? 2 : 3;
        vkbc::Decode(bc, px.data(), pitch, w, h, 1, bgra, bgra_pitch);
    } else if (fmt == kBgra) {
        bgra.assign(px.begin(), px.end());
        bgra_pitch = pitch;
    } else {
        return;
    }
    char name[512];
    std::snprintf(name, sizeof name, "%s/tex_%016llx_%ux%u_f%02X.tga", dir, (unsigned long long)id, w, h, d.format);
    FILE* f = std::fopen(name, "wb");
    if (!f) return;
    const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 0x28};
    std::fwrite(header, 1, sizeof header, f);
    for (uint32_t y = 0; y < h; y++) std::fwrite(&bgra[size_t(y) * bgra_pitch], 1, w * 4, f);
    std::fclose(f);
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;
    constexpr uint32_t kPaletteCommon = 0;
    constexpr uint32_t kPaletteData = 4;
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
Image* VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                       DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same || tx.icons != vkr::icons::Epoch()) {
            tx.icons = vkr::icons::Epoch();
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, p, palette.va ? palette.colours : nullptr, Hash(p, total, palette.hash));
        }
    }
    Transition(tx, Use::Sampled);
    stages.view[stage] = tx.view;
    stages.levels[stage] = tx.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    return &tx;
}

Image* VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.view[stage] = VK_NULL_HANDLE;
    stages.levels[stage] = 1;
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) return BindTargetTexture(s, stage, d, tit->second, true, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

// ---------------------------------------------------------------------------------------------------------------
// samplers, keys, constants

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

void VulkanBackend::StageSamplers(const State& s, const DrawTextures& stages, bool clamp, VkSampler out[4]) {
    const uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        const uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
        auto lin = [](uint32_t f) { return f >= kTexfLinear; };
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.minFilter = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.magFilter = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
        uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
        // forced only where the title filters across mip levels and the texture has them
        if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
        if (an > 1 && max_aniso_ > 1) {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = std::min(float(std::min<uint32_t>(an, 16)), max_aniso_);
            ci.minFilter = ci.magFilter = VK_FILTER_LINEAR;
            ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
        // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
        ci.addressModeU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
        ci.addressModeV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
        ci.addressModeW = Address(t[kTssAddressW]);
        ci.mipLodBias = Bits(t[kTssMipLodBias]);
        ci.minLod = float(t[kTssMaxMipLevel]);
        ci.maxLod = mip ? VK_LOD_CLAMP_NONE : ci.minLod;
        VkSamplerCustomBorderColorCreateInfoEXT border{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
        const bool uses_border = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                 ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                 ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        float bc[4];
        Color(t[kTssBorderColor], bc);
        if (uses_border && custom_border_) {
            ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            std::memcpy(border.customBorderColor.float32, bc, sizeof(bc));
        } else {
            // the nearest of the fixed border colours
            ci.borderColor = bc[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                             : bc[0] + bc[1] + bc[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                            : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        }
        if (stages.info[i].shadow) {
            // the console tests texel OP r, Vulkan tests r OP texel
            static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
            ci.minFilter = ci.magFilter = lin(min) || lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            ci.compareEnable = VK_TRUE;
            ci.compareOp = VkCompareOp(swap[Cmp(s.rs[kRsShadowFunc]) - 1]);
            ci.anisotropyEnable = VK_FALSE;
        }
        out[i] = Sampler(ci, uses_border && custom_border_ ? &border : nullptr);
    }
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are.
    // Immediate XYZRHW sprites stay 2D when the title orders its menu layers with Z on (ShaolinMonksRecomp).
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && (!s.rs[kRsZEnable] || (!sh.prog && a.pretransformed)) && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kQuadSalt = 0xC3C30000;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (auto vit = shader_src_.find(sh.vkey); vit != shader_src_.end()) {
        sh.vs = modules_[vit->second];
        sh.vh = vit->second;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    sh.vs = Module(sh.quad ? QuadHlsl(std::move(src)) : src, "main", Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool VulkanBackend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    auto perf_t = PerfClock::now();
    draw_rt_ = static_cast<Target*>(bound_rt_);
    draw_ds_ = static_cast<Target*>(bound_ds_);
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    g_perf.bind += PerfMs(perf_t);
    // textures may have recorded copies that unbound the targets
    return BindTargets(s);
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    if (auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt); pit != shader_src_.end()) {
        sh.ps = modules_[pit->second];
        sh.ph = pit->second;
        return;
    }
    sh.ps = Module(PixelHlsl(pk), "main", Stage::Fragment, sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        const Target* ds = static_cast<const Target*>(bound_ds_);
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (ds ? float(ds->w) / ds->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.strip = a.strip;
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? uint32_t(bound_rt_->fmt) : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = 7.0f;

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void VulkanBackend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, Vulkan half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void VulkanBackend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] + x[r * 4 + 3] * y[12 + col];
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w, gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw, sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

float VulkanBackend::PointScale() const {
    const Target* t = static_cast<const Target*>(bound_rt_ ? bound_rt_ : bound_ds_);
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

void VulkanBackend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                    GuestExtent size, DrawConstants& c, Bindings& b) {
    Alloc vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = PointScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
    b.vc = vc.buf;
    b.vc_off = vc.off;
    b.vc_size = sizeof(s.vsc);
    b.dc = dc.buf;
    b.dc_off = dc.off;
    b.dc_size = sizeof(DrawConstants);
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a, const DrawTextures& stages) {
    // ShaolinMonksRecomp: immediate sprites with FLOAT4 coordinates on plain 2D textures
    if (shaolin::UnitSpriteTexcoords(a)) {
        bool guarded = true;
        for (int i = 0; i < 4; ++i)
            if (stages.res[i]) guarded &= !stages.rt[i] && stages.info[i].kind == kTex2D && !stages.info[i].rect;
        if (guarded) return true;
    }
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// a stage without a texture gets a black texture of its kind
void VulkanBackend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages, Bindings& b) {
    for (int i = 0; i < 4; i++) b.views[i] = stages.view[i] ? stages.view[i] : null_[stages.info[i].kind].view;
    StageSamplers(s, stages, UnitTexcoords(a, stages), b.samplers);
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void VulkanBackend::SetViewportScissor(const State& s, uint32_t gw) {
    const Image* t = bound_rt_ ? bound_rt_ : bound_ds_;
    const float W = float(t->w), H = float(t->h);
    SetViewport({0, 0, W, H});
    const float sc = W / gw;
    int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(int32_t(s.scissors[0]) * sc));
        y0 = std::max(y0, int32_t(int32_t(s.scissors[1]) * sc));
        x1 = std::min(x1, int32_t(int32_t(s.scissors[2]) * sc));
        y1 = std::min(y1, int32_t(int32_t(s.scissors[3]) * sc));
    }
    SetScissor(std::clamp(x0, 0, int32_t(W)), std::clamp(y0, 0, int32_t(H)), std::clamp(x1, 0, int32_t(W)),
               std::clamp(y1, 0, int32_t(H)));
}

void VulkanBackend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(Cmd(), bf);
    vkCmdSetStencilReference(Cmd(), VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
}

void VulkanBackend::DrawGeometry(const Assembled& a) {
    VkBuffer bufs[16];
    VkDeviceSize offs[16], sizes[16], strides[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(std::max<uint32_t>(b.bytes, 4), 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        bufs[i] = vb.buf;
        offs[i] = vb.off;
        sizes[i] = std::max<uint32_t>(b.bytes, 4);
        strides[i] = b.stride;
    }
    if (a.buffer_count) bind_vertex_buffers2_(Cmd(), 0, a.buffer_count, bufs, offs, sizes, strides);
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(Cmd(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(Cmd(), a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(Cmd(), a.draw_count, 1, 0, 0);
    }
}

bool VulkanBackend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                                 Assembled& a) {
    auto perf_t = PerfClock::now();
    bool assembled = Assemble(s, prim, start, count, indices, a);
    g_perf.assemble += PerfMs(perf_t);
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

// ---------------------------------------------------------------------------------------------------------------
// visibility tests

bool VulkanBackend::EnsureQueries() {
    if (queries_) return true;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_OCCLUSION;
    ci.queryCount = kQueries;
    if (vkCreateQueryPool(dev_, &ci, nullptr, &queries_) != VK_SUCCESS) {
        LOG_FIRST(1, "vulkan: no occlusion queries, visibility tests stay pending");
        queries_ = VK_NULL_HANDLE;
        return false;
    }
    EndRendering();
    vkCmdResetQueryPool(Cmd(), queries_, 0, kQueries);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t VulkanBackend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    const uint32_t query = query_next_++ % kQueries;
    // a query is reset outside rendering before it is used again
    EndRendering();
    vkCmdResetQueryPool(Cmd(), queries_, query, 1);
    EnsureRendering();
    vkCmdBeginQuery(Cmd(), queries_, query, precise_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    return query;
}

void VulkanBackend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    vkCmdEndQuery(Cmd(), queries_, query);
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale comes out
float VulkanBackend::SamplesPerPixel() const {
    const Target* t = static_cast<const Target*>(bound_rt_ ? bound_rt_ : bound_ds_);
    if (!t || !t->gw || !t->gh) return 1;
    return float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void VulkanBackend::DeliverReports() {
    constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
    const uint64_t done = Completed();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) {
            uint64_t n = 0;
            vkGetQueryPoolResults(dev_, queries_, q.index, 1, sizeof(n), &n, sizeof(n), VK_QUERY_RESULT_64_BIT);
            pixels += double(n) / q.samples_per_pixel;
        }
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 20)
            LOGR("vulkan: occlusion report %08X, %zu draws, %.0f pixels", r.va, r.queries.size(), pixels);
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Draw

void VulkanBackend::TraceTextures(const State& s) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        const bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        LOGI("trace:   t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, d.linear ? " linear" : "",
             tg ? " target" : "");
    }
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (Tracing()) {
        LOGI("trace: %llu draw rt %08X zs %08X prim %u count %u vs %08X ps %08X zen %u blend %u %X %X cw %X", (unsigned long long)presents_,
             s.rt.data, s.zs.data, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, s.rs[kRsZEnable],
             SimpleState(s, kMBlendEnable), SimpleState(s, kMSrcBlend), SimpleState(s, kMDestBlend),
             SimpleState(s, kMColorWriteEnable));
        TraceTextures(s);
    }
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return;

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs || !sh.ps) return;

    // pipeline; ShaolinMonksRecomp's guard keeps atlas sprites inside their cell
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    bool atlas = false, target_texture = false;
    for (int i = 0; i < 4; ++i)
        if (stages.res[i]) {
            target_texture |= stages.rt[i] || stages.info[i].kind != kTex2D || stages.info[i].rect;
            atlas |= !stages.rt[i] && stages.info[i].kind == kTex2D;
        }
    k.atlas_guard = sh.quad && a.topology == Topology::Triangles && atlas && !target_texture;
    VkPipeline pso = EnsurePipeline(k, sh.vs, sh.ps);
    if (!pso) return;

    // constants, views and samplers, viewport and scissor, blend factor and stencil reference
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    Bindings b;
    UploadConstants(s, sh, k, stages, size, c, b);
    SetTextureViewsAndSamplers(s, a, stages, b);
    EnsureRendering();
    if (pso != bound_pso_) {
        vkCmdBindPipeline(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        bound_pso_ = pso;
    }
    Push(b);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);

    // geometry, counted while the title runs a visibility test
    uint32_t query = BeginCountedDraw(s);
    DrawGeometry(a);
    EndCountedDraw(query);
}

// ---------------------------------------------------------------------------------------------------------------
// Clear, Copy

// a guest rect scaled to the target and clamped to it
VkRect2D ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    int32_t l = std::clamp(int32_t(x0 * sc), 0, int32_t(t.w)), tp = std::clamp(int32_t(y0 * sc), 0, int32_t(t.h));
    int32_t r = std::clamp(int32_t(x1 * sc), 0, int32_t(t.w)), b = std::clamp(int32_t(y1 * sc), 0, int32_t(t.h));
    return {{l, tp}, {uint32_t(std::max(0, r - l)), uint32_t(std::max(0, b - tp))}};
}

void VulkanBackend::ClearTarget(Target& t, const State& s, const ClearArgs& c) {
    std::vector<VkClearRect> rs;
    auto add = [&](VkRect2D r) {
        if (r.extent.width && r.extent.height) rs.push_back({r, 0, 1});
    };
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            add(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        add(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    VkClearAttachment ca{};
    if (t.depth) {
        Transition(t, Use::Depth);
        ca.aspectMask = ((c.flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((c.flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue.depthStencil = {std::clamp(c.z, 0.0f, 1.0f), c.stencil & 0xFF};
        if (!ca.aspectMask || rs.empty()) return;
        BeginRendering(nullptr, &t);
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t, Use::Color);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Color(c.color, ca.clearValue.color.float32);
        if (rs.empty()) return;
        BeginRendering(&t, nullptr);
    }
    vkCmdClearAttachments(Cmd(), 1, &ca, uint32_t(rs.size()), rs.data());
    t.drawn = true;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (Tracing())
        LOGI("trace: %llu clear rt %08X zs %08X flags %X color %08X z %.3f rects %u", (unsigned long long)presents_, s.rt.data,
             s.zs.data, flags, color, z, count);
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, c);
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
struct GuestRect {
    int32_t left, top, right, bottom;
};
struct GuestPoint {
    int32_t x, y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)), int32_t(kernel::R32(r + 12))};
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * 8;
    return {int32_t(kernel::R32(p)), int32_t(kernel::R32(p + 4))};
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 0, dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r{float(g.left), float(g.top), float(g.right), float(g.bottom), float(g.left), float(g.top)};
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void VulkanBackend::CopyByDraw(Target& src_t, Target& dst_t, const CopyList& l) {
    Transition(src_t, Use::Sampled);
    SetBlitTarget(dst_t);
    const float src_scale = float(src_t.w) / src_t.gw, dst_scale = float(dst_t.w) / dst_t.gw;
    const bool listed = l.count && l.src_rects;
    const uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        DrawnRect r{0, 0, float(std::min(src_t.gw, dst_t.gw)), float(std::min(src_t.gh, dst_t.gh)), 0, 0};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        Rect vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale};
        int32_t right = std::min(int32_t(vp.x + vp.w), int32_t(dst_t.w));
        int32_t bottom = std::min(int32_t(vp.y + vp.h), int32_t(dst_t.h));
        if (right <= int32_t(vp.x) || bottom <= int32_t(vp.y)) continue;
        SetViewport(vp);
        SetScissor(int32_t(vp.x), int32_t(vp.y), right, bottom);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        Blit(src_t, uv, {.alpha = true, .taps = taps});
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one image copy per rect, clamped to both targets
void VulkanBackend::CopyByRegion(Target& src_t, Target& dst_t, const CopyList& l) {
    Transition(src_t, Use::CopySrc);
    Transition(dst_t, Use::CopyDst);
    const float dst_scale = float(dst_t.w) / dst_t.gw;
    const bool listed = l.count && l.src_rects;
    const uint32_t n = listed ? l.count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t left = 0, top = 0, right = std::min(src_t.w, dst_t.w), bottom = std::min(src_t.h, dst_t.h), dx = 0, dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            left = ScaleUnsigned(r.left, dst_scale);
            top = ScaleUnsigned(r.top, dst_scale);
            right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = left;
            dy = top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (right <= left || bottom <= top || dx >= dst_t.w || dy >= dst_t.h) continue;
        right = std::min(right, left + dst_t.w - dx);
        bottom = std::min(bottom, top + dst_t.h - dy);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.srcOffset = {int32_t(left), int32_t(top), 0};
        c.dstOffset = {int32_t(dx), int32_t(dy), 0};
        c.extent = {right - left, bottom - top, 1};
        regions.push_back(c);
    }
    if (!regions.empty())
        vkCmdCopyImage(Cmd(), src_t.img, VK_IMAGE_LAYOUT_GENERAL, dst_t.img, VK_IMAGE_LAYOUT_GENERAL, uint32_t(regions.size()), regions.data());
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (Tracing())
        LOGI("trace: %llu copy %08X (%s) to %08X, %u rects", (unsigned long long)presents_, src.data,
             src_t ? (src_t->drawn ? "target" : "idle target") : "memory", dst.data, count);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    // sizes that differ are scaled across by a draw; same sized targets copy directly
    if (src_t->fmt != dst_t->fmt) CopyByDraw(*src_t, *dst_t, l);
    else CopyByRegion(*src_t, *dst_t, l);
}

// ---------------------------------------------------------------------------------------------------------------
// Present

void VulkanBackend::ResizeSwap() {
    uint32_t w, h;
    platform_.OutputSize(w, h);
    if (w == bb_w_ && h == bb_h_) return;
    Flush();
    vkDeviceWaitIdle(dev_);
    if (!surface_) {
        Release(offscreen_);
        Flush();
    }
    CreateSwapchain();
}

// the platform has given the window back: from now on the frames go to a swap chain on it
void VulkanBackend::AttachSurface() {
    surface_later_ = false;
    Flush();
    vkDeviceWaitIdle(dev_);
    Release(offscreen_);
    Flush();
    surface_ = platform_.CreateSurface(instance_);
    if (surface_ && !CreateSwapchain()) {
        LOGE("vulkan: no swap chain on the window, drawing offscreen");
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (!surface_) CreateSwapchain();
    LOGR("vulkan: the window is the renderer's from frame %llu", (unsigned long long)presents_);
}

// ramp_ holds the red, green and blue ramps, 256 entries each
void VulkanBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    Alloc a = Upload(256 * 4, 16);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[512 + i];
        p[1] = ramp_[256 + i];
        p[2] = ramp_[i];
        p[3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_, Use::CopyDst);
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {256, 1, 1};
    vkCmdCopyBufferToImage(Cmd(), a.buf, lut_.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    Transition(lut_, Use::Sampled);
}

// the title's front buffer letterboxed onto the output: 16:9 when the console reports a widescreen display, else 4:3
void VulkanBackend::DrawFrame(Target* t, Image& out, uint32_t w, uint32_t h) {
    bound_rt_ = &out;
    bound_ds_ = nullptr;
    BeginRendering(&out, nullptr);
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    ca.clearValue.color.float32[3] = 1.0f;
    VkClearRect cr{{{0, 0}, {w, h}}, 0, 1};
    vkCmdClearAttachments(Cmd(), 1, &ca, 1, &cr);
    if (t) {
        const float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float vw = std::min(float(w), float(h) * aspect), vh = vw / aspect;
        // the target's barrier ends this rendering instance; the blit begins another
        Transition(*t, Use::Sampled);
        SetViewport({(float(w) - vw) / 2, (float(h) - vh) / 2, vw, vh});
        SetScissor(0, 0, int32_t(w), int32_t(h));
        float uv[4] = {0, 0, 1, 1};
        Blit(*t, uv, {.gamma = gamma_, .taps = shaolin::sfd::PresentationTaps()});
    }
    // the quick menu and status over the frame; ImGui binds its own pipeline and state
    EnsureRendering();
    vkr::overlay::Draw(Cmd(), out.fmt, w, h);
    bound_pso_ = VK_NULL_HANDLE;
    EndRendering();
    bound_rt_ = nullptr;
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Release(it->second);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && Completed() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    vkResetCommandPool(dev_, f.pool, 0);
    Collect(f);
    f.used = 0;
    Begin();
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el < 1000.0) return;
    Perf& p = g_perf;
    double n = double(perf_frames);
    double host = p.draw + p.clear + p.copy + p.present + p.sync;
    LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
         "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
         n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n, p.compile / n,
         p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n, (el - host) / n);
    LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits)", g_perf_capture_ns.exchange(0) / n / 1e6,
         g_perf_wait_ns.exchange(0) / n / 1e6, g_perf_waits.exchange(0) / n);
    LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.1f tex uploads %.2f MB, "
         "%.1f new tex, %.1f new targets, %.1f target uploads, %.1f read-backs %.1f late | %llu shaders %llu pipelines",
         p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.texups / n,
         p.texup_bytes / n / 1048576.0, p.newtex / n, p.targets / n, p.rtups / n, p.syncs / n, p.lates / n,
         (unsigned long long)p.shaders, (unsigned long long)p.psos);
    g_perf = Perf{};
    perf_start = PerfClock::now();
    perf_frames = 0;
}

void VulkanBackend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (surface_later_ && !platform_.SurfaceHeld()) AttachSurface();
    ResizeSwap();
    const bool known = FindTarget(front, false) != nullptr;
    Target* t = EnsureTarget(front, false);
    const bool cpu = t && LatchCpuWrite(*t);
    if (cpu) UploadTarget(*t, front);
    if (Tracing())
        LOGI("trace: %llu present %08X%s%s%s", (unsigned long long)presents_, front.data, known ? "" : " (new target)",
             cpu ? " with CPU pixels" : "", t && t->drawn ? " drawn" : "");
    if (ramp_dirty_) UploadGammaRamp();

    Frame& f = CurrentFrame();
    if (surface_) {
        uint32_t index = 0;
        auto perf_swap = PerfClock::now();
        VkResult r = vkAcquireNextImageKHR(dev_, swap_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
        g_perf.swap += PerfMs(perf_swap);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            Image& out = swap_images_[index];
            // the swap chain's image comes undefined each time and leaves in the present layout
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = out.img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            EndRendering();
            vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            out.use = Use::Color;
            DrawFrame(t, out, bb_w_, bb_h_);
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            Submit(false, f.acquired, rendered_[index]);
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &rendered_[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swap_;
            pi.pImageIndices = &index;
            perf_swap = PerfClock::now();
            r = vkQueuePresentKHR(queue_, &pi);
            g_perf.swap += PerfMs(perf_swap);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) bb_w_ = 0;  // made again next frame
        } else {
            LOG_FIRST(8, "vulkan: acquire failed (%d), frame dropped", int(r));
            if (r == VK_ERROR_OUT_OF_DATE_KHR) bb_w_ = 0;
            Submit(false);
        }
    } else {
        Transition(offscreen_, Use::Color);
        DrawFrame(t, offscreen_, bb_w_, bb_h_);
        if (platform_.WantFrame(presents_)) {
            std::vector<uint8_t> px;
            if (ReadBack(offscreen_, bb_w_, bb_h_, px)) platform_.Frame(px.data(), bb_w_, bb_h_, presents_);
        }
        Submit(false);
    }
    DeliverReports();

    platform_.Presented(presents_);
    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    if (diag::Perf()) LogPerf();
    ApplySettings();
    // the pipeline cache goes to the card a while after new pipelines stop appearing
    if (presents_ % 1800 == 0 && psos_.size() + blit_psos_.size() != pcache_saved_) {
        pcache_saved_ = psos_.size() + blit_psos_.size();
        SavePipelineCache();
    }
}

void VulkanBackend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    if (std::fabs(want - scale_) > 1e-4f) {
        Flush();
        for (auto it = targets_.begin(); it != targets_.end();) {
            Release(it->second.fb);
            Release(it->second);
            it = targets_.erase(it);
        }
        scale_ = want;
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// read-backs

// long unused: the title freed it and other data is at this address now
bool VulkanBackend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// filters src into a w x h BGRA image (made unless down has one of that size), left as a copy source
bool VulkanBackend::Shrink(Image& src, uint32_t w, uint32_t h, Image& down) {
    g_perf.shrinks++;
    if (down.img && (down.w != w || down.h != h)) Release(down);
    if (!down.img && !CreateImage(down, VK_IMAGE_TYPE_2D, kBgra, w, h, 1, 1, 1,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D))
        return false;
    Transition(src, Use::Sampled);
    SetBlitTarget(down);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, uv, {.alpha = true});
    bound_rt_ = bound_ds_ = nullptr;
    Transition(down, Use::CopySrc);
    return true;
}

// the target as a guest sized BGRA copy source, shrunk into down when it is not one already; null on failure
Image* VulkanBackend::PrepareCopySource(Target& t, Image& down) {
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) return Shrink(t, t.gw, t.gh, down) ? &down : nullptr;
    Transition(t, Use::CopySrc);
    return &t;
}

void VulkanBackend::CopyImageToBuffer(Image& res, uint32_t w, uint32_t h, VkBuffer buf) {
    Transition(res, Use::CopySrc);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(Cmd(), res.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// a BGRA image to the CPU, rows packed; waits for the GPU
bool VulkanBackend::ReadBack(Image& res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = uint64_t(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (vkCreateBuffer(dev_, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, buf, &req);
    Mem m = alloc_.Alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
    if (!m.map) {
        vkDestroyBuffer(dev_, buf, nullptr);
        alloc_.Free(m);
        return false;
    }
    vkBindBufferMemory(dev_, buf, m.mem, m.off);
    CopyImageToBuffer(res, w, h, buf);
    Flush();
    out.assign(m.map, m.map + size_t(w) * h * 4);
    vkDestroyBuffer(dev_, buf, nullptr);
    alloc_.Free(m);
    return true;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    Target& t = it->second;
    Image down;
    Image* src = PrepareCopySource(t, down);
    std::vector<uint8_t> px;
    const bool ok = src && ReadBack(*src, t.gw, t.gh, px);
    Release(down);
    if (!ok) return;
    // colour targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    const uint64_t done = Completed();
    const uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) return false;
    if (l.fence[best] > l.delivered) {
        const uint8_t* p = l.rb_mem[best].map;
        if (!p) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void VulkanBackend::ResetLate(Late& l, const Target& t) {
    for (int i = 0; i < kFrames; i++)
        if (l.rb[i]) Release(l.rb[i], l.rb_mem[i]);
    Release(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

void VulkanBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) ResetLate(l, t);
    // a copy for a later lock, unless the GPU is a whole ring behind
    const uint32_t slot = l.next;
    if (l.fence[slot] > Completed()) return;
    Image* src = PrepareCopySource(t, l.down);
    if (!src) return;
    if (!l.rb[slot]) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = uint64_t(t.gw) * t.gh * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev_, &bi, nullptr, &l.rb[slot]) != VK_SUCCESS) {
            l.rb[slot] = VK_NULL_HANDLE;
            return;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev_, l.rb[slot], &req);
        l.rb_mem[slot] = alloc_.Alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
        vkBindBufferMemory(dev_, l.rb[slot], l.rb_mem[slot].mem, l.rb_mem[slot].off);
    }
    CopyImageToBuffer(*src, t.gw, t.gh, l.rb[slot]);
    l.fence[slot] = fence_value_ + 1;  // the next submission carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
