#pragma once
// The renderer's HLSL (the SDK's generated shaders and the passes) to SPIR-V, with glslang's HLSL front end.
//
// Registers keep the meaning the D3D12 root signature gave them, as fixed bindings of descriptor set 0:
// b0 b1 -> 0 1, t0..t3 -> 2..5, s0..s3 -> 6..9. Compiled shaders are cached in memory and in a folder on disk,
// keyed by their source, entry point and stage, so a second run does not compile what it has seen.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vkr {

enum class Stage : uint8_t { Vertex, Fragment, Geometry };

enum : uint32_t {
    kBindingVc = 0,        // b0
    kBindingDc = 1,        // b1
    kBindingTexture0 = 2,  // t0..t3
    kBindingSampler0 = 6,  // s0..s3
    kBindingCount = 10,
};

void InitShaderCompiler(const std::filesystem::path& cache_dir);
// empty on failure, after logging the error and the source
const std::vector<uint32_t>& CompileHlsl(const std::string& src, const char* entry, Stage stage, uint64_t& hash);

}  // namespace vkr
