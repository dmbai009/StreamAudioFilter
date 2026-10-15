// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Backends without any OS dependency.
//
//  1. They let the whole engine be unit tested on Linux/macOS/CI.
//  2. They power the plugin's "тестовый режим" (signal generator) so the
//     renderer side (stream injection) can be validated without WASAPI.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../capture.h"

namespace saf {
namespace mock {

/** A configurable, scriptable application list. */
class MockEnumerator : public IAppEnumerator {
public:
    struct App {
        std::string appKey;
        std::string displayName;
        std::string exePath;
        std::string exeName;
        uint32_t pid = 0;
        uint32_t parentPid = 0;
        bool playing = true;
        bool capturable = true;
        std::string notCapturableReason;
        float peak = 0.5f;
    };

    MockEnumerator();
    ~MockEnumerator() override { destroy(); }

    bool start(std::string& error) override;
    void stop() override;
    std::vector<SessionInfo> enumerate() override;
    std::vector<ProcessNode> processes() override;
    bool consumeChangeFlag() override;
    std::string describe() const override { return "mock (тестовый список приложений)"; }

    /** Replaces the whole scripted state. Marks a change for the engine. */
    void setApps(std::vector<App> apps);

private:
    void destroy();

    std::mutex mutex_;
    std::vector<App> apps_;
    std::atomic<bool> changed_ { false };
    bool running_ = false;
};

/**
 * Produces a sine tone per captured process tree, so tests can check which
 * instances really end up in the mix.
 *
 * The tone is derived from the root process id, not from the creation order:
 * two instances of one application must be distinguishable regardless of when
 * their capture was created.
 */
class MockCaptureSource : public ICaptureSource {
public:
    MockCaptureSource(std::string appKey, int sampleRate, int channels, double toneHz, uint32_t rootPid = 0);
    ~MockCaptureSource() override;

    bool start(std::string& error) override;
    void stop() override;

    FloatRingBuffer* buffer() override { return &buffer_; }
    std::string formatDescription() const override;
    int latencyMs() const override { return 10; }
    std::string lastError() const override { return {}; }

    /** Feeds @p frames frames synchronously (used by tests). */
    void pushFrames(size_t frames);

private:
    void threadMain();

    std::string appKey_;
    uint32_t rootPid_ = 0;
    int sampleRate_;
    int channels_;
    double toneHz_;
    double phase_ = 0.0;
    FloatRingBuffer buffer_;
    std::thread thread_;
    std::atomic<bool> running_ { false };
    std::vector<float> scratch_;
};

/**
 * Adds/updates the scripted application list of every live MockEnumerator.
 * The engine owns its enumerator, so the addon reaches it through this
 * registry instead of keeping a dangling pointer.
 */
void setAppsOnAllInstances(std::vector<MockEnumerator::App> apps);

class MockCaptureFactory : public ICaptureFactory {
public:
    explicit MockCaptureFactory(double baseToneHz = 440.0) : baseToneHz_(baseToneHz) {}

    /**
     * Creates one source per request, i.e. per process tree. Multi root
     * requests are split by GroupingCaptureFactory, so an application with
     * several instances is captured instance by instance and mixed once.
     */
    std::unique_ptr<ICaptureSource> create(const CaptureRequest& request) override;
    std::string describe() const override { return "mock (сигнал-генератор)"; }

    /** Number of captures handed out (diagnostics and tests). */
    size_t createdSources() const { return created_.load(); }

    /** Frequency of the tone used for a root process (tests). */
    double toneForPid(uint32_t pid) const;

private:
    std::atomic<size_t> created_ { 0 };
    double baseToneHz_;
};

} // namespace mock
} // namespace saf
