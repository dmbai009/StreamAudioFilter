// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace saf {

using namespace std::chrono;

Engine::Engine(EngineOptions options, std::unique_ptr<IAppEnumerator> enumerator, std::unique_ptr<ICaptureFactory> factory)
    : options_(options)
    , enumerator_(std::move(enumerator))
    , factory_(std::move(factory)) {}

Engine::~Engine() {
    stop();
}

bool Engine::start(std::string& error) {
    if (running_) {
        error = "engine already running";
        return false;
    }

    const int blockFrames = std::max(1, options_.sampleRate * options_.blockMs / 1000);
    mixer_ = std::make_unique<Mixer>(options_.sampleRate, options_.channels, blockFrames);
    const size_t outputCapacity = static_cast<size_t>(options_.sampleRate) * options_.outputBufferMs / 1000;
    output_ = std::make_unique<FloatRingBuffer>(std::max<size_t>(outputCapacity, static_cast<size_t>(blockFrames) * 4), options_.channels);
    blockBuffer_.assign(static_cast<size_t>(blockFrames) * options_.channels, 0.0f);

    if (!enumerator_->start(error)) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        warnings_.clear();
        startErrors_.clear();
    }

    running_ = true;
    refreshRequested_ = true;
    mixThread_ = std::thread([this] { mixLoop(); });
    watchThread_ = std::thread([this] { watchLoop(); });
    return true;
}

void Engine::stop() {
    if (!running_.exchange(false)) {
        destroyStreams();
        enumerator_->stop();
        return;
    }

    if (mixThread_.joinable()) mixThread_.join();
    if (watchThread_.joinable()) watchThread_.join();

    destroyStreams();
    enumerator_->stop();

    std::lock_guard<std::mutex> lock(mutex_);
    streams_.clear();
    snapshot_.clear();
    output_.reset();
    mixer_.reset();
}

void Engine::setRules(const RuleSet& rules) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_ = rules;
    }
    refreshRequested_ = true;
}

void Engine::requestRefresh() {
    refreshRequested_ = true;
}

void Engine::addWarning(const std::string& warning) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(warnings_.begin(), warnings_.end(), warning) == warnings_.end()) warnings_.push_back(warning);
}

// --------------------------------------------------------------- mix thread

void Engine::mixLoop() {
    const int blockFrames = mixer_->blockFrames();
    const auto blockDuration = microseconds(options_.blockMs * 1000);
    auto next = steady_clock::now() + blockDuration;

    auto* blockOut = blockBuffer_.data();

    while (running_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mixList_.clear();
            for (auto& [key, stream] : streams_) {
                if (stream->retired) continue;
                mixList_.push_back(&stream->mix);
            }
        }

        mixer_->mix(mixList_, blockOut);
        output_->write(blockOut, static_cast<size_t>(blockFrames));

        blocksMixed_++;
        outputFrames_ += static_cast<uint64_t>(blockFrames);

        {
            // Streams whose fade out completed are handed to the watch thread,
            // which owns the (potentially blocking) destruction.
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [key, stream] : streams_) {
                if (!stream->retired && stream->mix.finishedFadeOut() && !isActiveInMixLocked(key)) {
                    stream->retired = true;
                    starvedFrames_ += stream->mix.starvedFrames;
                }
            }
        }

        next += blockDuration;
        const auto now = steady_clock::now();
        if (next > now) {
            std::this_thread::sleep_until(next);
        } else if (now - next > milliseconds(200)) {
            // We fell far behind (system suspend, debugger): resynchronise
            // instead of trying to catch up in a burst.
            next = now + blockDuration;
        }
    }
}

bool Engine::isActiveInMixLocked(const std::string& appKey) const {
    const auto it = streams_.find(appKey);
    if (it == streams_.end()) return false;
    if (it->second->retired) return false;
    // A stream stays in the mix while it fades; it only counts as active when
    // its gain target is non zero.
    return it->second->mix.gainTarget > 0.0f;
}

// ------------------------------------------------------------- watch thread

void Engine::watchLoop() {
    const auto changeCheck = milliseconds(options_.changeCheckMs);
    const auto reconcile = milliseconds(options_.reconcileMs);
    auto lastReconcile = steady_clock::now() - reconcile;

    while (running_) {
        bool refresh = refreshRequested_.exchange(false);
        if (!refresh && enumerator_->consumeChangeFlag()) refresh = true;
        if (!refresh && steady_clock::now() - lastReconcile >= reconcile) refresh = true;

        if (refresh) {
            lastReconcile = steady_clock::now();
            applyPlan();
        }

        collectRetired();
        std::this_thread::sleep_for(std::min(changeCheck, milliseconds(100)));
    }
}

void Engine::applyPlan() {
    std::vector<SessionInfo> sessions = enumerator_->enumerate();
    std::vector<ProcessNode> tree = enumerator_->processes();
    std::vector<AppGroup> groups = groupSessions(sessions, tree);

    // Decide what should be captured. Starting a capture can take tens of
    // milliseconds, so it happens outside of the lock.
    struct Pending {
        std::string appKey;
        std::string displayName;
        std::vector<uint32_t> roots;
        std::unique_ptr<ICaptureSource> source;
        std::string error;
    };
    std::vector<Pending> pending;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& group : groups) {
            const bool transmit = rules_.transmit(group.appKey) && group.capturable && !group.roots.empty();
            if (!transmit) continue;
            if (streams_.count(group.appKey)) continue;
            if (std::any_of(pending.begin(), pending.end(), [&](const Pending& p) { return p.appKey == group.appKey; })) continue;
            if (pending.size() + streams_.size() >= options_.maxStreams) {
                const std::string warning = "Достигнут предел одновременного захвата (" + std::to_string(options_.maxStreams)
                    + " приложений). Часть звука не передаётся.";
                if (std::find(warnings_.begin(), warnings_.end(), warning) == warnings_.end()) warnings_.push_back(warning);
                break;
            }
            pending.push_back(Pending { group.appKey, group.displayName, group.roots, nullptr, {} });
        }
    }

    for (auto& item : pending) {
        CaptureRequest request;
        request.appKey = item.appKey;
        request.displayName = item.displayName;
        request.rootPids = item.roots;
        request.sampleRate = options_.sampleRate;
        request.channels = options_.channels;

        std::string error;
        auto source = factory_->create(request);
        if (!source || !source->start(error)) {
            item.error = error.empty() ? "не удалось запустить захват" : error;
            continue;
        }
        item.source = std::move(source);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_ = groups;

        // 1. Retire streams that must not be transmitted any more.
        for (auto& [key, stream] : streams_) {
            if (stream->retired) continue;
            bool wanted = false;
            for (const auto& group : groups) {
                if (group.appKey == key && rules_.transmit(key) && group.capturable && !group.roots.empty()) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted) stream->mix.gainTarget = 0.0f;
            // If the application is gone completely, fade out too.
            const bool stillThere = std::any_of(groups.begin(), groups.end(), [&](const AppGroup& g) { return g.appKey == key; });
            if (!stillThere) stream->mix.gainTarget = 0.0f;
        }

        // 2. Add newly started captures.
        for (auto& item : pending) {
            if (!item.source) {
                startErrors_[item.appKey] = item.error;
                continue;
            }
            if (streams_.count(item.appKey)) {
                // Raced with another apply: drop the duplicate capture. Its
                // destructor stops it.
                item.source.reset();
                continue;
            }

            auto stream = std::make_unique<StreamState>();
            stream->appKey = item.appKey;
            stream->displayName = item.displayName;
            stream->rootPids = item.roots;
            stream->source = std::move(item.source);
            stream->mix.buffer = stream->source->buffer();
            stream->mix.key = item.appKey;
            stream->mix.gain = 0.0f;
            stream->mix.gainTarget = 1.0f;
            // 20 ms fade: audible clicks are impossible, the delay is not.
            stream->mix.rampStep = 1.0f / (0.02f * static_cast<float>(options_.sampleRate));
            streams_[item.appKey] = std::move(stream);
            startErrors_.erase(item.appKey);
        }
    }
}

void Engine::collectRetired() {
    std::vector<std::unique_ptr<StreamState>> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (it->second->retired) {
                retired.push_back(std::move(it->second));
                it = streams_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Destruction happens outside of the lock: stopping a capture joins its
    // thread and must never block the mixer.
    retired.clear();
}

void Engine::destroyStreams() {
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streams.swap(streams_);
    }
    for (auto& [key, stream] : streams) {
        if (stream->source) stream->source->stop();
    }
}

// ------------------------------------------------------------------ queries

std::vector<AppView> Engine::apps() {
    std::vector<AppGroup> snapshot;
    std::map<std::string, StreamState*> live;
    std::map<std::string, std::string> errors;
    std::vector<std::string> warnings;
    RuleSet rules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = snapshot_;
        for (auto& [key, stream] : streams_) live[key] = stream.get();
        errors = startErrors_;
        warnings = warnings_;
        rules = rules_;
    }

    std::vector<AppView> result;
    result.reserve(snapshot.size());

    for (const auto& group : snapshot) {
        AppView view;
        view.appKey = group.appKey;
        view.displayName = group.displayName;
        view.exeName = group.exeName;
        view.exePath = group.exePath;
        view.playing = group.playing;
        view.capturable = group.capturable;
        view.notCapturableReason = group.notCapturableReason;
        view.ephemeral = isEphemeralKey(group.appKey);
        view.running = true;
        view.transmitting = rules.active && rules.transmit(group.appKey) && group.capturable;
        view.peak = std::max(group.peak, 0.0f);

        const auto streamIt = live.find(group.appKey);
        if (streamIt != live.end()) {
            StreamState* stream = streamIt->second;
            view.captured = !stream->retired && stream->mix.gainTarget > 0.0f;
            view.starvedFrames = stream->mix.starvedFrames;
            view.format = stream->source ? stream->source->formatDescription() : "";
            view.latencyMs = stream->source ? stream->source->latencyMs() : -1;
            if (stream->source) view.error = stream->source->lastError();
            if (stream->mix.peak > view.peak) view.peak = stream->mix.peak;
        }
        const auto errorIt = errors.find(group.appKey);
        if (errorIt != errors.end() && view.error.empty()) view.error = errorIt->second;

        if (!view.capturable) {
            view.state = "unsupported";
        } else if (!view.error.empty()) {
            view.state = "error";
        } else if (!view.transmitting) {
            view.state = "excluded";
        } else if (view.captured) {
            view.state = "transmitting";
        } else if (group.playing) {
            view.state = "silent";
        } else {
            view.state = "silent";
        }

        result.push_back(std::move(view));
    }

    // Applications with stored rules that are not running are added by the
    // renderer (it owns the persisted rules), the engine only reports live data.
    return result;
}

EngineStats Engine::stats() {
    EngineStats stats;
    stats.running = running_;
    stats.backend = enumerator_->describe();
    stats.captureBackend = factory_->describe();
    stats.sampleRate = options_.sampleRate;
    stats.channels = options_.channels;
    stats.blockMs = options_.blockMs;
    stats.blocksMixed = blocksMixed_.load();
    stats.outputFrames = outputFrames_.load();
    stats.starvedFrames = starvedFrames_.load();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats.warnings = warnings_;
        stats.activeStreams = 0;
        for (const auto& [key, stream] : streams_) {
            if (!stream->retired && stream->mix.gainTarget > 0.0f) stats.activeStreams++;
        }
        stats.limiterGain = mixer_ ? mixer_->limiterGain() : 1.0f;
        stats.limitedBlocks = mixer_ ? mixer_->limitedBlocks() : 0;
        stats.clampedSamples = mixer_ ? mixer_->clampedSamples() : 0;
    }

    if (output_) {
        const size_t buffered = output_->availableFrames();
        stats.outputBufferedFrames = buffered;
        stats.outputDroppedFrames = output_->droppedFrames();
    }
    return stats;
}

size_t Engine::readFrames(float* destination, size_t frames) {
    if (!output_ || !destination) return 0;
    return output_->read(destination, frames);
}

std::string Engine::describeEnumerator() const { return enumerator_->describe(); }
std::string Engine::describeCaptureBackend() const { return factory_->describe(); }

} // namespace saf
