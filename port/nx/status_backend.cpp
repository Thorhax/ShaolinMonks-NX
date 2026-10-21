// A renderer that draws nothing: the console shows how far the game got. For bring-up on hardware before the
// Vulkan renderer, and as a fallback with [switch] renderer = "status".
#include <switch.h>

#include <chrono>
#include <cstdio>

#include "core/log.h"
#include "gpu/backend.h"
#include "nx/guest_memory.h"

namespace gpu {
namespace {

class StatusBackend final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        consoleInit(nullptr);
        std::printf("\x1b[2J\n  Shaolin Monks Recomp: bring-up build, no graphics yet\n  Title back buffer %ux%u\n", w, h);
        consoleUpdate(nullptr);
        start_ = last_ = std::chrono::steady_clock::now();
        return true;
    }
    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override { draws_++; }
    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t) override {}
    void Present(const Surface&) override {
        frames_++;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_).count();
        if (dt < 1.0) return;
        const auto gm = nx::gm::Query();
        std::printf("\x1b[6;1H  %6.1f s   frames %8llu   %5.1f fps   draws/frame %6.0f   guest memory %6zu KB   \n",
                    std::chrono::duration<double>(now - start_).count(), (unsigned long long)frames_,
                    (frames_ - frames_at_) / dt, double(draws_ - draws_at_) / double(frames_ - frames_at_ ? frames_ - frames_at_ : 1),
                    gm.committed >> 10);
        consoleUpdate(nullptr);
        if (frames_ % 300 < (frames_ - frames_at_))
            LOGI("status: %llu frames, %.1f fps", (unsigned long long)frames_, (frames_ - frames_at_) / dt);
        last_ = now;
        frames_at_ = frames_;
        draws_at_ = draws_;
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    std::chrono::steady_clock::time_point start_, last_;
    uint64_t frames_ = 0, frames_at_ = 0, draws_ = 0, draws_at_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateStatusBackend() { return std::make_unique<StatusBackend>(); }

}  // namespace gpu
