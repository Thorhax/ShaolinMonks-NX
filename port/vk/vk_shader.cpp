#include "vk/vk_shader.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "gpu/shaders.h"

namespace vkr {
namespace {

std::filesystem::path g_dir;
std::mutex g_lock;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_spirv;
double g_compile_ms = 0;
uint64_t g_compiled = 0, g_loaded = 0;

EShLanguage Language(Stage s) {
    switch (s) {
        case Stage::Vertex: return EShLangVertex;
        case Stage::Fragment: return EShLangFragment;
        default: return EShLangGeometry;
    }
}

// what fxc takes and glslang does not
std::string Portable(std::string s) {
    for (size_t at; (at = s.find("1.#INF")) != std::string::npos;) s.replace(at, 6, "asfloat(0x7F800000u)");
    return s;
}

bool Compile(const std::string& src, const char* entry, Stage stage, std::vector<uint32_t>& out) {
    const EShLanguage lang = Language(stage);
    glslang::TShader sh(lang);
    const std::string text = Portable(src);
    const char* str = text.c_str();
    const int len = int(text.size());
    sh.setStringsWithLengths(&str, &len, 1);
    // the HLSL function is the entry, renamed main in the SPIR-V, so pipelines name every stage alike
    sh.setEntryPoint("main");
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setShiftBinding(glslang::EResUbo, kBindingVc);
    sh.setShiftBinding(glslang::EResTexture, kBindingTexture0);
    sh.setShiftBinding(glslang::EResSampler, kBindingSampler0);
    const auto msgs = EShMessages(EShMsgReadHlsl | EShMsgSpvRules | EShMsgVulkanRules | EShMsgHlslLegalization);
    if (!sh.parse(GetDefaultResources(), 100, false, msgs)) {
        LOGE("vulkan: %s shader '%s' does not compile:\n%s", stage == Stage::Vertex ? "vertex" : stage == Stage::Fragment ? "pixel" : "geometry",
             entry, sh.getInfoLog());
        LOGE("%s", text.c_str());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(msgs) || !program.mapIO()) {
        LOGE("vulkan: shader '%s' does not link:\n%s", entry, program.getInfoLog());
        LOGE("%s", text.c_str());
        return false;
    }
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;
    opt.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(lang), out, &opt);
    return !out.empty();
}

std::filesystem::path CacheFile(uint64_t hash) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.spv", (unsigned long long)hash);
    return g_dir / name;
}

bool Load(uint64_t hash, std::vector<uint32_t>& out) {
    if (g_dir.empty()) return false;
    FILE* f = std::fopen(CacheFile(hash).c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool ok = n > 20 && n % 4 == 0;
    if (ok) {
        out.resize(size_t(n) / 4);
        ok = std::fread(out.data(), 1, size_t(n), f) == size_t(n) && out[0] == 0x07230203;  // SPIR-V magic
    }
    std::fclose(f);
    return ok;
}

void Store(uint64_t hash, const std::vector<uint32_t>& code) {
    if (g_dir.empty()) return;
    const auto path = CacheFile(hash);
    auto tmp = path;
    tmp += ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(code.data(), 4, code.size(), f) == code.size();
    std::fclose(f);
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    if (!ok || ec) std::filesystem::remove(tmp, ec);
}

}  // namespace

void InitShaderCompiler(const std::filesystem::path& cache_dir) {
    glslang::InitializeProcess();
    g_dir = cache_dir;
    std::error_code ec;
    if (!g_dir.empty()) std::filesystem::create_directories(g_dir, ec);
    if (ec) {
        LOGW("vulkan: no shader cache in %s (%s)", g_dir.string().c_str(), ec.message().c_str());
        g_dir.clear();
    }
}

const std::vector<uint32_t>& CompileHlsl(const std::string& src, const char* entry, Stage stage, uint64_t& hash) {
    // bump the salt when the translation itself changes, so stale cache files are not used
    constexpr uint64_t kSalt = 0x5348414F4C494E01ull;
    const uint64_t stage_seed = gpu::Hash(&stage, sizeof(stage), kSalt);
    hash = gpu::Hash(src.data(), src.size(), gpu::Hash(entry, std::strlen(entry), stage_seed));
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_spirv.find(hash);
    if (it != g_spirv.end()) return it->second;
    std::vector<uint32_t> code;
    if (Load(hash, code)) {
        g_loaded++;
    } else {
        const auto t = std::chrono::steady_clock::now();
        if (Compile(src, entry, stage, code)) Store(hash, code);
        else code.clear();
        g_compile_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        if (++g_compiled % 50 == 0)
            LOGI("vulkan: %llu shaders compiled (%.0f ms in all), %llu from the cache", (unsigned long long)g_compiled,
                 g_compile_ms, (unsigned long long)g_loaded);
    }
    return g_spirv.emplace(hash, std::move(code)).first->second;
}

}  // namespace vkr
