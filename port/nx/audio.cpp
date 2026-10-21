// The mixer's output through audout: 48 kHz stereo, a thread pulling the mixer for each buffer the console plays
#include "audio/audio.h"

#include <malloc.h>
#include <switch.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "core/log.h"
#include "launcher.h"

namespace audio {
namespace {
constexpr uint32_t kFrames = 768;  // 16 ms per buffer
constexpr int kBuffers = 4;
}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    Result rc = audoutInitialize();
    if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        LOGE("audio: audout failed (%X), no sound", rc);
        return;
    }
    LOGI("audio: audout %u Hz, %u channels", audoutGetSampleRate(), audoutGetChannelCount());
    std::thread([render = std::move(render)] {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1);
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
        const size_t bytes = (kFrames * 2 * sizeof(int16_t) + 0xFFF) & ~size_t(0xFFF);
        AudioOutBuffer buffers[kBuffers] = {};
        for (auto& b : buffers) {
            b.buffer = memalign(0x1000, bytes);
            std::memset(b.buffer, 0, bytes);
            b.buffer_size = bytes;
            b.data_size = kFrames * 2 * sizeof(int16_t);
            audoutAppendAudioOutBuffer(&b);
        }
        std::vector<float> mix(kFrames * 2);
        for (;;) {
            AudioOutBuffer* done = nullptr;
            u32 count = 0;
            if (R_FAILED(audoutWaitPlayFinish(&done, &count, UINT64_MAX)) || !done) continue;
            render(mix.data(), kFrames);
            const float gain = shaolin::launcher::AudioGain();
            auto* out = static_cast<int16_t*>(done->buffer);
            for (uint32_t i = 0; i < kFrames * 2; i++) out[i] = int16_t(std::clamp(mix[i] * gain, -1.0f, 1.0f) * 32767.0f);
            audoutAppendAudioOutBuffer(done);
        }
    }).detach();
}

}  // namespace audio
