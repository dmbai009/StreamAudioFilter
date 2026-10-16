// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output sink without an audio device: pulls the mix in real time and keeps
// the level, so the whole host can be tested on any OS (CI) exactly like the
// real one, minus the speaker.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "engine.h"
#include "output_sink.h"

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace saf {

namespace {

class NullSink : public IOutputSink {
public:
    ~NullSink() override { stop(); }

    bool start(Engine& engine, std::string&) override {
        if (running_.exchange(true)) return true;
        engine_ = &engine;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        engine_ = nullptr;
    }

    OutputStatus status() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        OutputStatus status = status_;
        status.running = running_;
        status.kind = "null";
        status.device = "none (test output)";
#if defined(_WIN32)
        status.pid = GetCurrentProcessId();
#endif
        status.discordAttached = discordHookLoaded();
        return status;
    }

private:
    void loop() {
        using namespace std::chrono;
        constexpr size_t frames = 480;
        std::vector<float> buffer(frames * 2);
        auto next = steady_clock::now();
        float windowPeak = 0.0f;
        int blocks = 0;
        while (running_) {
            engine_->pull(buffer.data(), frames);
            for (float v : buffer) windowPeak = std::max(windowPeak, std::fabs(v));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.renderedFrames += frames;
                status_.format = "48000 Hz / 2 ch / f32";
                if (++blocks >= 100) {
                    status_.peak = windowPeak;
                    windowPeak = 0.0f;
                    blocks = 0;
                }
            }
            next += milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    }

    Engine* engine_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_ { false };
    mutable std::mutex mutex_;
    OutputStatus status_;
};

} // namespace

std::unique_ptr<IOutputSink> createNullSink() { return std::make_unique<NullSink>(); }

#if !defined(_WIN32)
std::unique_ptr<IOutputSink> createOutputSink(OutputKind kind, std::string& error) {
    if (kind == OutputKind::Wasapi) error = "WASAPI output is only available on Windows; using the test output";
    return createNullSink();
}

bool discordHookLoaded() { return false; }
#endif

} // namespace saf
