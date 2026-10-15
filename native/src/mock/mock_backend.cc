// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mock_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace saf {
namespace mock {

namespace {
constexpr double kPi = 3.14159265358979323846;

std::mutex g_instancesMutex;
std::vector<MockEnumerator*> g_instances;
}

// ------------------------------------------------------------- enumerator

MockEnumerator::MockEnumerator() {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    g_instances.push_back(this);
}

void MockEnumerator::destroy() {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    g_instances.erase(std::remove(g_instances.begin(), g_instances.end(), this), g_instances.end());
}

bool MockEnumerator::start(std::string&) {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    changed_ = true;
    return true;
}

void MockEnumerator::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

std::vector<SessionInfo> MockEnumerator::enumerate() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionInfo> sessions;
    for (const auto& app : apps_) {
        SessionInfo info;
        info.sessionId = "mock:" + app.appKey + ":" + std::to_string(app.pid);
        info.pid = app.pid;
        info.parentPid = app.parentPid;
        info.exePath = app.exePath;
        info.exeName = app.exeName;
        info.displayName = app.displayName;
        info.appKey = app.appKey;
        info.playing = app.playing;
        info.capturable = app.capturable;
        info.notCapturableReason = app.notCapturableReason;
        info.peak = app.playing ? app.peak : 0.0f;
        sessions.push_back(std::move(info));
    }
    return sessions;
}

std::vector<ProcessNode> MockEnumerator::processes() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProcessNode> nodes;
    for (const auto& app : apps_) {
        nodes.push_back(ProcessNode { app.pid, app.parentPid });
    }
    return nodes;
}

bool MockEnumerator::consumeChangeFlag() {
    return changed_.exchange(false);
}

void MockEnumerator::setApps(std::vector<App> apps) {
    std::lock_guard<std::mutex> lock(mutex_);
    apps_ = std::move(apps);
    changed_ = true;
}

void setAppsOnAllInstances(std::vector<MockEnumerator::App> apps) {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    for (MockEnumerator* instance : g_instances) {
        if (instance) instance->setApps(apps);
    }
}

// ------------------------------------------------------------ capture source

MockCaptureSource::MockCaptureSource(std::string appKey, int sampleRate, int channels, double toneHz, uint32_t rootPid)
    : appKey_(std::move(appKey))
    , rootPid_(rootPid)
    , sampleRate_(sampleRate)
    , channels_(channels)
    , toneHz_(toneHz)
    , buffer_(static_cast<size_t>(sampleRate) / 2, static_cast<size_t>(channels)) {
    scratch_.assign(static_cast<size_t>(channels) * 480, 0.0f);
}

MockCaptureSource::~MockCaptureSource() { stop(); }

bool MockCaptureSource::start(std::string&) {
    running_ = true;
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void MockCaptureSource::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void MockCaptureSource::pushFrames(size_t frames) {
    if (scratch_.size() < frames * static_cast<size_t>(channels_)) scratch_.resize(frames * static_cast<size_t>(channels_));
    for (size_t f = 0; f < frames; f++) {
        const float value = 0.25f * static_cast<float>(std::sin(phase_));
        phase_ += 2.0 * kPi * toneHz_ / sampleRate_;
        if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
        for (int c = 0; c < channels_; c++) scratch_[f * static_cast<size_t>(channels_) + c] = value;
    }
    buffer_.write(scratch_.data(), frames);
}

void MockCaptureSource::threadMain() {
    const int blockMs = 10;
    const size_t frames = static_cast<size_t>(sampleRate_) * blockMs / 1000;
    auto next = std::chrono::steady_clock::now();
    while (running_) {
        pushFrames(frames);
        next += std::chrono::milliseconds(blockMs);
        std::this_thread::sleep_until(next);
    }
}

std::string MockCaptureSource::formatDescription() const {
    const std::string tree = rootPid_ ? (" (pid " + std::to_string(rootPid_) + ")") : std::string();
    return "mock " + std::to_string(sampleRate_) + " Hz / " + std::to_string(channels_) + " ch / f32" + tree;
}

// ----------------------------------------------------------------- factory

double MockCaptureFactory::toneForPid(uint32_t pid) const {
    // Eight semitone steps: reproducible, and far enough apart to be told
    // apart by a tone detector. Only the ratio between the tones matters, so
    // the base is not assumed to be audible.
    return baseToneHz_ * std::pow(2.0, static_cast<double>(pid % 8) / 12.0);
}

std::unique_ptr<ICaptureSource> MockCaptureFactory::create(const CaptureRequest& request) {
    created_++;
    const uint32_t rootPid = request.rootPids.empty() ? 0 : request.rootPids.front();
    return std::make_unique<MockCaptureSource>(
        request.appKey, request.sampleRate, request.channels, toneForPid(rootPid), rootPid);
}

} // namespace mock
} // namespace saf
