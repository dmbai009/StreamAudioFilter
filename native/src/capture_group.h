// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One application can own several *unrelated* process trees: a program that
// was started twice from the same path (two independent instances) has two root
// processes, and a capture backend attaches to one process tree per capture
// client. Windows' process loopback takes exactly one TargetProcessId, so
// "capture the application" has to mean "capture every root of it".
//
// This file turns a multi root request into one request per root and mixes the
// results into a single capture source:
//
//   CaptureRequest{rootPids: {100, 300}}
//        -> capture(100) + capture(300) --> GroupedCaptureSource --> engine
//
// Why mixing inside the capture and not as several engine streams:
//  * the engine keys streams by application, and one application is one entry
//    with one transmit switch — splitting it into several streams would make the
//    UI lie about what is transmitted;
//  * the engine's stream limit counts applications, not instances;
//  * audio is summed exactly once. Capturing a root twice, or capturing a child
//    process whose parent tree is already captured, would double that audio's
//    level — which is why the roots come from computeCaptureRoots() (a root is
//    by construction not a descendant of another root) and why duplicates are
//    filtered out again here.
//
// Cost: aggregation only exists when an application really has more than one
// root. A request with a single root is handed to the inner factory unchanged,
// so the common case keeps the zero overhead path (no extra thread, no extra
// buffer, no extra mixing step).

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "dsp/mixer.h"

namespace saf {

/** Removes exact duplicates, keeping the first occurrence order. */
std::vector<uint32_t> uniqueRoots(const std::vector<uint32_t>& roots);

/**
 * Mixes the audio of several captures of one application into one continuous
 * stream at the requested rate.
 *
 * Created by GroupingCaptureFactory, which decides whether an application needs
 * more than one capture at all.
 */
class GroupedCaptureSource : public ICaptureSource {
public:
    GroupedCaptureSource(std::vector<std::unique_ptr<ICaptureSource>> parts, int sampleRate, int channels, std::string displayName);
    ~GroupedCaptureSource() override;

    bool start(std::string& error) override;
    void stop() override;

    FloatRingBuffer* buffer() override { return &output_; }

    std::string formatDescription() const override;
    int latencyMs() const override;
    std::string lastError() const override;

    /** Number of process trees that are captured for this application. */
    size_t partCount() const { return parts_.size(); }

private:
    void mixLoop();
    /** Joins the non empty values of every part into one line. */
    std::string summarise(const std::vector<std::string>& values, const std::string& fallback) const;

    std::vector<std::unique_ptr<ICaptureSource>> parts_;
    std::vector<MixSource> sources_;
    std::vector<MixSource*> sourcePointers_;

    int sampleRate_ = 48000;
    int channels_ = 2;
    int blockFrames_ = 480;
    std::string displayName_;

    FloatRingBuffer output_;
    std::unique_ptr<Mixer> mixer_;
    std::vector<float> block_;
    std::vector<std::string> startErrors_;

    std::thread thread_;
    std::atomic<bool> running_ { false };
};

/**
 * Splits multi root requests and aggregates their results.
 *
 * @param inner factory that captures exactly one process tree per request
 */
class GroupingCaptureFactory : public ICaptureFactory {
public:
    explicit GroupingCaptureFactory(std::unique_ptr<ICaptureFactory> inner) : inner_(std::move(inner)) {}

    std::unique_ptr<ICaptureSource> create(const CaptureRequest& request) override;
    std::string describe() const override { return inner_ ? inner_->describe() : std::string(); }

    /** Number of single tree captures created through this factory. */
    size_t createdParts() const { return createdParts_; }

private:
    std::unique_ptr<ICaptureFactory> inner_;
    size_t createdParts_ = 0;
};

} // namespace saf
