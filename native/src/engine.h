// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio engine:
//   * owns one capture per transmitted application (union of process trees),
//   * mixes all captures into ONE continuous output stream,
//   * reacts to applications starting/stopping without disturbing the output,
//   * applies rule changes live, fading applications in and out.
//
// The engine is platform independent: capture and enumeration are injected, so
// the whole plan/apply logic runs (and is tested) with a mock backend.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_identity.h"
#include "capture.h"
#include "dsp/mixer.h"

namespace saf {

struct EngineOptions {
    int sampleRate = 48000;
    int channels = 2;
    int blockMs = 10;
    size_t maxStreams = 16;
    size_t outputBufferMs = 400;
    /** How often the watch thread reconciles the plan at the latest. */
    int reconcileMs = 2000;
    /** How often the watch thread checks the change flag. */
    int changeCheckMs = 250;
};

/** The user's intent, as sent by the UI. */
struct RuleSet {
    enum class Mode {
        /** Mode A: everything is transmitted unless it is switched off. */
        AllExcept = 0,
        /** Mode B: nothing is transmitted unless it is explicitly allowed. */
        OnlySelected = 1,
    };

    Mode mode = Mode::AllExcept;
    std::map<std::string, bool> overrides;

    /** Resolves the effective decision for an application. */
    bool transmit(const std::string& appKey) const {
        const auto it = overrides.find(appKey);
        if (it != overrides.end()) return it->second;
        return mode == Mode::AllExcept;
    }
};

/** State of one application, as shown in the UI. */
struct AppView {
    std::string appKey;
    std::string displayName;
    std::string exeName;
    std::string exePath;

    bool playing = false;
    bool capturable = true;
    bool ephemeral = false;
    /** The rule in effect after mode + overrides have been resolved. */
    bool transmitting = false;
    /** A capture stream currently exists for this application. */
    bool captured = false;

    float peak = 0.0f;
    int latencyMs = -1;
    std::string format;
    std::string error;
    std::string notCapturableReason;
    uint64_t starvedFrames = 0;

    /** One of: transmitting, excluded, silent, error, unsupported, closed. */
    std::string state;
    /** Session based: the application is running but has no active stream. */
    bool running = false;
};

struct EngineStats {
    bool running = false;
    std::string backend;
    std::string captureBackend;
    int sampleRate = 0;
    int channels = 0;
    int blockMs = 0;

    uint64_t blocksMixed = 0;
    uint64_t outputFrames = 0;
    uint64_t outputDroppedFrames = 0;
    uint64_t starvedFrames = 0;
    uint64_t limitedBlocks = 0;
    uint64_t clampedSamples = 0;
    float limiterGain = 1.0f;
    size_t activeStreams = 0;
    /** Audio the JS bridge has not picked up yet. */
    size_t outputBufferedFrames = 0;

    std::vector<std::string> warnings;
};

class Engine {
public:
    Engine(EngineOptions options, std::unique_ptr<IAppEnumerator> enumerator, std::unique_ptr<ICaptureFactory> factory);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const { return running_; }

    /** Replaces the rule set and applies it as soon as possible. */
    void setRules(const RuleSet& rules);

    /** Forces a re-enumeration + plan application (manual "refresh"). */
    void requestRefresh();

    /** Current application list (merged with the live stream state). */
    std::vector<AppView> apps();

    EngineStats stats();

    /** Pulls mixed audio for the JS bridge. Returns frames written. */
    size_t readFrames(float* destination, size_t frames);

    std::string describeEnumerator() const;
    std::string describeCaptureBackend() const;

    /** Registers an externally supplied error/warning shown in the UI. */
    void addWarning(const std::string& warning);

private:
    struct StreamState {
        std::string appKey;
        std::string displayName;
        std::vector<uint32_t> rootPids;
        std::unique_ptr<ICaptureSource> source;
        MixSource mix;
        bool enteredMix = false;
        bool retired = false;
        std::string startError;
    };

    void mixLoop();
    void watchLoop();
    void applyPlan();
    void collectRetired();
    void destroyStreams();

    bool isActiveInMixLocked(const std::string& appKey) const;

    EngineOptions options_;
    std::unique_ptr<IAppEnumerator> enumerator_;
    std::unique_ptr<ICaptureFactory> factory_;
    std::unique_ptr<Mixer> mixer_;

    mutable std::mutex mutex_;
    RuleSet rules_;
    std::map<std::string, std::unique_ptr<StreamState>> streams_;
    std::vector<AppGroup> snapshot_;
    std::vector<std::unique_ptr<StreamState>> retired_;
    std::vector<std::string> warnings_;
    std::map<std::string, std::string> startErrors_;

    std::vector<MixSource*> mixList_;
    std::vector<float> blockBuffer_;

    std::unique_ptr<FloatRingBuffer> output_;

    std::atomic<bool> running_ { false };
    std::atomic<bool> refreshRequested_ { true };
    std::thread mixThread_;
    std::thread watchThread_;

    std::atomic<uint64_t> blocksMixed_ { 0 };
    std::atomic<uint64_t> outputFrames_ { 0 };
    std::atomic<uint64_t> starvedFrames_ { 0 };
};

} // namespace saf
