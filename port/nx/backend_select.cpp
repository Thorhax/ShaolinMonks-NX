// gpu::CreateBackend for the Switch: the Vulkan renderer, or the status console when [switch] renderer = "status"
// or when Vulkan does not start, so the game still runs and its log tells why
#include <memory>

#include "core/log.h"
#include "gpu/backend.h"
#include "nx/platform.h"

namespace gpu {

std::unique_ptr<Backend> CreateStatusBackend();
std::unique_ptr<Backend> CreateVulkanBackend();

namespace {

class Fallback final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        if (!nx::Threading().renderer_status) {
            b_ = CreateVulkanBackend();
            if (b_->Init(w, h)) return true;
            LOGE("vulkan: the renderer did not start; running without graphics (see the lines above)");
        }
        b_ = CreateStatusBackend();
        return b_->Init(w, h);
    }
    void Clear(const State& s, uint32_t n, const uint8_t* r, uint32_t f, uint32_t c, float z, uint32_t st) override { b_->Clear(s, n, r, f, c, z, st); }
    void Draw(const State& s, uint32_t p, uint32_t st, uint32_t n, const uint8_t* i) override { b_->Draw(s, p, st, n, i); }
    void Copy(const Surface& s, const Surface& d, uint32_t n, uint32_t r, uint32_t p) override { b_->Copy(s, d, n, r, p); }
    void Present(const Surface& f) override { b_->Present(f); }
    void Sync(uint32_t d, uint32_t n) override { b_->Sync(d, n); }
    void SyncLate(uint32_t d) override { b_->SyncLate(d); }
    bool DeliverLate(uint32_t d) override { return b_->DeliverLate(d); }
    void RecordLate(uint32_t d) override { b_->RecordLate(d); }
    void Flush() override { b_->Flush(); }
    void SetGamma(const uint8_t* r) override { b_->SetGamma(r); }
    void ClearReport() override { b_->ClearReport(); }
    void GetReport(uint32_t r) override { b_->GetReport(r); }
    uint32_t Fence() override { return b_->Fence(); }
    bool FencePending(uint32_t f) override { return b_->FencePending(f); }
    void WaitFence(uint32_t f) override { b_->WaitFence(f); }
    bool Busy(uint32_t r) override { return b_->Busy(r); }
    void WaitResource(uint32_t r) override { b_->WaitResource(r); }

private:
    std::unique_ptr<Backend> b_;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<Fallback>(); }

}  // namespace gpu
