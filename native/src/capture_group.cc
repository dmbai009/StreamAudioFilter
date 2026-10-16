// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "capture_group.h"

#include <algorithm>
#include <chrono>

namespace saf {

std::vector<uint32_t> uniqueRoots(const std::vector<uint32_t>& roots) {
    std::vector<uint32_t> result;
    result.reserve(roots.size());
    for (uint32_t pid : roots) {
        if (pid == 0) continue;
        if (std::find(result.begin(), result.end(), pid) != result.end()) continue;
        result.push_back(pid);
    }
    return result;
}

GroupedCaptureSource::GroupedCaptureSource(std::vector<std::unique_ptr<ICaptureSource>> parts, int sampleRate, int channels, std::string displayName)
    : parts_(std::move(parts))
    , sampleRate_(sampleRate > 0 ? sampleRate : 48000)
    , channels_(channels > 0 ? channels : 2)
    , blockFrames_(std::max(1, sampleRate_ / 100))  // 10 ms, same as the engine's block
    , displayName_(std::move(displayName))
    , output_(static_cast<size_t>(sampleRate_) / 2, static_cast<size_t>(channels_)) {
    mixer_ = std::make_unique<Mixer>(sampleRate_, channels_, blockFrames_);
    block_.assign(static_cast<size_t>(blockFrames_) * channels_, 0.0f);

    sources_.resize(parts_.size());
    sourcePointers_.reserve(parts_.size());
    for (size_t i = 0; i < parts_.size(); i++) {
        MixSource& source = sources_[i];
        source.buffer = parts_[i] ? parts_[i]->buffer() : nullptr;
        source.key = displayName_ + " #" + std::to_string(i + 1);
        // The parts are summed from the first block on: the fades of the engine
        // mixer and the aggregation of instances are independent stages, and a
        // second fade here would only delay the audio of a new instance.
        source.gain = 1.0f;
        source.gainTarget = 1.0f;
        source.rampStep = 1.0f;
        sourcePointers_.push_back(&source);
    }
}

GroupedCaptureSource::~GroupedCaptureSource() { stop(); }

bool GroupedCaptureSource::start(std::string& error) {
    if (parts_.empty()) {
        error = "no instance of the application was passed: " + displayName_;
        return false;
    }
    if (running_.exchange(true)) {
        error = "the capture is already running";
        return false;
    }

    startErrors_.assign(parts_.size(), std::string());
    size_t started = 0;
    std::string firstError;

    for (size_t i = 0; i < parts_.size(); i++) {
        std::string partError;
        if (parts_[i] && parts_[i]->start(partError)) {
            started++;
            continue;
        }
        startErrors_[i] = partError.empty() ? "could not start the capture of one instance" : partError;
        if (firstError.empty()) firstError = startErrors_[i];
    }

    if (started == 0) {
        running_ = false;
        error = "no instance could be captured: " + displayName_ + ": " + firstError;
        return false;
    }

    // Streaming with a subset is better than not streaming at all, but the
    // missing instance is reported instead of being hidden.
    if (started < parts_.size()) {
        error.clear();
        startErrors_.push_back("part of the application instances was not captured (" + std::to_string(parts_.size() - started)
            + " of " + std::to_string(parts_.size()) + ")");
    }

    thread_ = std::thread([this] { mixLoop(); });
    return true;
}

void GroupedCaptureSource::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    for (auto& part : parts_) {
        if (part) part->stop();
    }
}

void GroupedCaptureSource::mixLoop() {
    using namespace std::chrono;
    const auto blockDuration = microseconds(blockFrames_ * 1000000 / std::max(1, sampleRate_));
    auto next = steady_clock::now() + blockDuration;

    while (running_) {
        mixer_->mix(sourcePointers_, block_.data());
        output_.write(block_.data(), static_cast<size_t>(blockFrames_));

        next += blockDuration;
        const auto now = steady_clock::now();
        if (next > now) {
            std::this_thread::sleep_until(next);
        } else if (now - next > milliseconds(200)) {
            // Fell far behind (suspend, debugger): resynchronise instead of
            // trying to catch up in a burst.
            next = now + blockDuration;
        }
    }
}

std::string GroupedCaptureSource::summarise(const std::vector<std::string>& values, const std::string& fallback) const {
    std::vector<std::string> unique;
    for (const auto& value : values) {
        if (value.empty()) continue;
        if (std::find(unique.begin(), unique.end(), value) == unique.end()) unique.push_back(value);
    }
    if (unique.empty()) return fallback;

    std::string result;
    for (size_t i = 0; i < unique.size(); i++) {
        if (i) result += "; ";
        result += unique[i];
    }
    return result;
}

std::string GroupedCaptureSource::formatDescription() const {
    std::vector<std::string> formats;
    for (const auto& part : parts_) {
        if (part) formats.push_back(part->formatDescription());
    }

    const std::string summary = summarise(formats, "no data");
    return std::to_string(parts_.size()) + " instance(s), total: " + summary;
}

int GroupedCaptureSource::latencyMs() const {
    int latency = -1;
    for (const auto& part : parts_) {
        if (!part) continue;
        const int value = part->latencyMs();
        if (value > latency) latency = value;
    }
    return latency;
}

bool GroupedCaptureSource::alive() const {
    for (const auto& part : parts_) {
        if (part && !part->alive()) return false;
    }
    return true;
}

std::string GroupedCaptureSource::lastError() const {
    std::vector<std::string> errors = startErrors_;
    for (const auto& part : parts_) {
        if (part) errors.push_back(part->lastError());
    }
    return summarise(errors, "");
}

std::unique_ptr<ICaptureSource> GroupingCaptureFactory::create(const CaptureRequest& request) {
    const std::vector<uint32_t> roots = uniqueRoots(request.rootPids);

    if (roots.empty() || !inner_) return nullptr;

    // The common case: one process tree, no aggregation, no extra thread.
    if (roots.size() == 1) {
        CaptureRequest single = request;
        single.rootPids = roots;
        createdParts_++;
        return inner_->create(single);
    }

    std::vector<std::unique_ptr<ICaptureSource>> parts;
    parts.reserve(roots.size());
    for (uint32_t pid : roots) {
        CaptureRequest single = request;
        single.rootPids = { pid };
        createdParts_++;
        auto part = inner_->create(single);
        if (!part) continue;  // a root that can not be captured at all
        parts.push_back(std::move(part));
    }

    if (parts.empty()) return nullptr;
    return std::make_unique<GroupedCaptureSource>(std::move(parts), request.sampleRate, request.channels, request.displayName);
}

} // namespace saf
