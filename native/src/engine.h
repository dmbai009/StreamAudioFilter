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

    /**
     * true: no internal mix thread. The output device drives the mix through
     * pull(), so the mix follows the device clock (no drift, no growing delay).
     */
    bool externalClock = false;
    /** Pull mode: per application jitter cushion and backlog cap. */
    int primeMs = 20;
    int maxBacklogMs = 120;

    /** Audio sessions of this process are ignored (the host's own output). */
    uint32_t selfPid = 0;
    /** ... and of every process of this executable (the render process). */
    std::string selfExePath;
    /** Process trees that are never captured, whatever the rules say (Discord). */
    std::vector<uint32_t> protectedRoots;
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

    /**
     * Capture gate: while no screen share is running the engine only watches the
     * application list and captures nothing at all.
     */
    bool active = false;

    /** Resolves the effective decision for an application. */
    bool transmit(const std::string& appKey) const {
        if (!active) return false;
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

    /**
     * One of: transmitting, covered, excluded, blocked, silent, error,
     * unsupported.
     */
    std::string state;
    /** state == blocked: why this application can not be sent. */
    std::string blockedReason;
    /** state == covered: the application whose capture already contains it. */
    std::string coveredBy;
    /** Output devices the application plays on (friendly names). */
    std::vector<std::string> devices;
    /**
     * Windows reports that the application is playing, but its capture has
     * delivered nothing but silence for seconds. Seen with some virtual audio
     * devices, which process loopback does not tap: the viewers do not hear
     * this application, and the user has to be told so.
     */
    bool silentCapture = false;
    /** Diagnostics: the process trees captured, and the level at the capture input. */
    std::vector<uint32_t> captureRoots;
    float inputPeak = -1.0f;
    /** Session based: the application is running but has no active stream. */
    bool running = false;
    /**
     * Number of independent instances (process trees) of this application.
     * More than one means the application was started twice and both instances
     * are captured, summed exactly once.
     */
    size_t instances = 0;
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

    /** Final output gate; never changes application rules or capture levels. */
    void setMasterMuted(bool muted) { masterMuted_ = muted; }
    bool masterMuted() const { return masterMuted_; }

    /** Forces a re-enumeration + plan application (manual "refresh"). */
    void requestRefresh();

    /** Current application list (merged with the live stream state). */
    std::vector<AppView> apps();

    EngineStats stats();

    /** Pulls mixed audio from the output ring (internal clock mode). */
    size_t readFrames(float* destination, size_t frames);

    /**
     * External clock mode: mixes exactly @p frames frames into @p destination.
     * Must only be called from one thread (the output device thread), and
     * never concurrently with stop().
     */
    size_t pull(float* destination, size_t frames);

    std::string describeEnumerator() const;
    std::string describeCaptureBackend() const;

    /** Registers an externally supplied error/warning shown in the UI. */
    void addWarning(const std::string& warning);

private:
    struct StreamState {
        std::string appKey;
        std::string displayName;
        /** Process trees this capture is bound to; a change restarts it. */
        std::vector<uint32_t> rootPids;
        /** Number of process trees the capture currently aggregates. */
        size_t instances = 0;
        std::unique_ptr<ICaptureSource> source;
        /** Owned by the mix thread; never read elsewhere. */
        MixSource mix;
        /** Written under mutex_ by the watch thread; copied into mix by the mixer. */
        float wantedGain = 1.0f;
        /** Published by the mix thread under mutex_, for apps(). */
        float peak = 0.0f;
        uint64_t starved = 0;
        int64_t createdMs = 0;
        int64_t lastSignalMs = 0;
        bool enteredMix = false;
        bool retired = false;
        std::string startError;
    };

    void mixLoop();
    /** Mixes one block; called by the mix thread or by pull(). */
    void mixBlock(float* out);
    void watchLoop();
    void applyPlan();
    void collectRetired();
    void destroyStreams();

    bool isActiveInMixLocked(const std::string& appKey) const;

    EngineOptions options_;
    std::unique_ptr<IAppEnumerator> enumerator_;
    std::unique_ptr<ICaptureFactory> factory_;
    std::unique_ptr<Mixer> mixer_;
    std::atomic<bool> masterMuted_ { false };
    // Only touched by the audio thread. A 20 ms ramp avoids clicks.
    float masterGain_ = 1.0f;

    mutable std::mutex mutex_;
    RuleSet rules_;
    std::map<std::string, std::unique_ptr<StreamState>> streams_;
    std::vector<AppGroup> snapshot_;
    std::vector<std::unique_ptr<StreamState>> retired_;
    std::vector<std::string> warnings_;
    std::map<std::string, std::string> startErrors_;
    std::map<std::string, CapturePlan> plans_;

    std::vector<MixSource*> mixList_;
    std::vector<float> blockBuffer_;

    std::unique_ptr<FloatRingBuffer> output_;

    // pull() state (output device thread only).
    std::vector<float> pullBlock_;
    size_t pullOffset_ = 0;
    std::atomic<int64_t> lastPullMs_ { 0 };

    std::atomic<bool> running_ { false };
    std::atomic<bool> refreshRequested_ { true };
    std::thread mixThread_;
    std::thread watchThread_;

    std::atomic<uint64_t> blocksMixed_ { 0 };
    std::atomic<uint64_t> outputFrames_ { 0 };
    std::atomic<uint64_t> starvedFrames_ { 0 };
};

} // namespace saf
