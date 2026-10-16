// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace saf {

using namespace std::chrono;

namespace {
int64_t nowMs() { return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); }
constexpr int64_t kSilentCaptureMs = 3000;
} // namespace

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

    pullBlock_.assign(static_cast<size_t>(blockFrames) * options_.channels, 0.0f);
    pullOffset_ = static_cast<size_t>(blockFrames);  // empty: the first pull mixes
    lastPullMs_ = 0;

    running_ = true;
    refreshRequested_ = true;
    if (!options_.externalClock) mixThread_ = std::thread([this] { mixLoop(); });
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

void Engine::mixBlock(float* out) {
    const int blockFrames = mixer_->blockFrames();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mixList_.clear();
        for (auto& [key, stream] : streams_) {
            if (stream->retired) continue;
            // The only place the mixer's copy of the target is written: the
            // watch thread never touches MixSource, so no field of it is
            // shared between threads without the lock.
            stream->mix.gainTarget = stream->wantedGain;
            mixList_.push_back(&stream->mix);
        }
    }

    mixer_->mix(mixList_, out);

    blocksMixed_++;
    outputFrames_ += static_cast<uint64_t>(blockFrames);

    {
        // Publish the meters, and hand streams whose fade out completed to the
        // watch thread, which owns the (potentially blocking) destruction.
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t now = nowMs();
        for (auto& [key, stream] : streams_) {
            if (stream->retired) continue;
            stream->peak = stream->mix.peak;
            if (stream->mix.peak > 1e-4f) stream->lastSignalMs = now;
            stream->starved = stream->mix.starvedFrames;
            if (stream->mix.finishedFadeOut() && stream->wantedGain <= 0.0f) {
                stream->retired = true;
                starvedFrames_ += stream->mix.starvedFrames;
            }
        }
    }
}

void Engine::mixLoop() {
    const int blockFrames = mixer_->blockFrames();
    const auto blockDuration = microseconds(options_.blockMs * 1000);
    auto next = steady_clock::now() + blockDuration;

    auto* blockOut = blockBuffer_.data();

    while (running_) {
        mixBlock(blockOut);
        output_->write(blockOut, static_cast<size_t>(blockFrames));

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

size_t Engine::pull(float* destination, size_t frames) {
    if (!destination) return 0;
    const size_t channels = static_cast<size_t>(options_.channels);
    if (!running_ || !mixer_) {
        std::memset(destination, 0, frames * channels * sizeof(float));
        return 0;
    }

    lastPullMs_ = nowMs();
    const size_t blockFrames = static_cast<size_t>(mixer_->blockFrames());
    size_t written = 0;
    while (written < frames) {
        if (pullOffset_ >= blockFrames) {
            mixBlock(pullBlock_.data());
            pullOffset_ = 0;
        }
        const size_t n = std::min(blockFrames - pullOffset_, frames - written);
        std::memcpy(destination + written * channels, pullBlock_.data() + pullOffset_ * channels, n * channels * sizeof(float));
        pullOffset_ += n;
        written += n;
    }
    return written;
}

bool Engine::isActiveInMixLocked(const std::string& appKey) const {
    const auto it = streams_.find(appKey);
    if (it == streams_.end()) return false;
    if (it->second->retired) return false;
    // A stream stays in the mix while it fades; it only counts as active when
    // its gain target is non zero.
    return it->second->wantedGain > 0.0f;
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

        if (options_.externalClock) {
            // Nobody pulls (the output is off): a fade can not run, so streams
            // that are no longer wanted are retired directly. Nothing is heard
            // in that state, so no click is possible either.
            if (nowMs() - lastPullMs_.load() > 250) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& [key, stream] : streams_) {
                    if (!stream->retired && stream->wantedGain <= 0.0f) stream->retired = true;
                }
            }
        }

        collectRetired();
        std::this_thread::sleep_for(std::min(changeCheck, milliseconds(100)));
    }
}

void Engine::applyPlan() {
    std::vector<SessionInfo> sessions = enumerator_->enumerate();
    std::vector<ProcessNode> tree = enumerator_->processes();

    // The host's own output is a session too: it must never be listed, and
    // never be captured (it carries the mix itself).
    // ... and so are the render processes of the host (any copy of it).
    const auto fileName = [](const std::string& path) {
        const std::string normalised = normalisePath(path);
        const size_t slash = normalised.find_last_of('\\');
        return slash == std::string::npos ? normalised : normalised.substr(slash + 1);
    };
    const std::string selfExe = fileName(options_.selfExePath);
    if (options_.selfPid != 0 || !selfExe.empty()) {
        sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [&](const SessionInfo& session) {
            return session.pid == options_.selfPid || (!selfExe.empty() && fileName(session.exePath) == selfExe);
        }), sessions.end());
    }

    std::vector<AppGroup> groups = groupSessions(sessions, tree);
    protectProcessTrees(groups, tree, options_.protectedRoots,
        "Discord's own audio (voice chat, notification sounds) is never sent to the share.");

    RuleSet rules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rules = rules_;
    }
    const std::map<std::string, CapturePlan> plans = planCaptures(groups, tree, [&](const std::string& key) { return rules.transmit(key); });

    auto wantedRoots = [&](const std::string& key) -> const std::vector<uint32_t>* {
        const auto it = plans.find(key);
        if (it == plans.end() || !it->second.blockedReason.empty() || it->second.roots.empty()) return nullptr;
        return &it->second.roots;
    };

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
    bool replan = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& group : groups) {
            const auto* roots = wantedRoots(group.appKey);
            if (!roots) continue;

            const auto existing = streams_.find(group.appKey);
            if (existing != streams_.end()) {
                // The capture is being restarted (or is still fading out):
                // run the plan again once it is gone instead of waiting for
                // the next periodic reconciliation.
                if (existing->second->retired || existing->second->wantedGain <= 0.0f) replan = true;
                continue;
            }
            if (pending.size() + streams_.size() >= options_.maxStreams) {
                const std::string warning = "The limit of simultaneous captures has been reached (" + std::to_string(options_.maxStreams)
                    + " applications). Part of the audio is not sent.";
                if (std::find(warnings_.begin(), warnings_.end(), warning) == warnings_.end()) warnings_.push_back(warning);
                break;
            }
            pending.push_back(Pending { group.appKey, group.displayName, *roots, nullptr, {} });
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
            item.error = error.empty() ? "could not start the capture" : error;
            continue;
        }
        item.source = std::move(source);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_ = groups;
        plans_ = plans;
        for (auto it = startErrors_.begin(); it != startErrors_.end();) {
            if (wantedRoots(it->first)) ++it;
            else it = startErrors_.erase(it);
        }

        // 1. Retire streams that must not be transmitted any more.
        for (auto& [key, stream] : streams_) {
            if (stream->retired) continue;
            const auto* roots = wantedRoots(key);

            // One capture is bound to one set of process trees. When an
            // application is started a second time (or one of its instances
            // closes), that set changes: the capture is restarted so the new
            // instance becomes audible and no instance is captured twice.
            // A capture that died (device change) is replaced the same way.
            // Only this application is affected - the share, the other
            // applications and the output stream keep running.
            if (roots && (*roots != stream->rootPids || (stream->source && !stream->source->alive()))) {
                stream->wantedGain = 0.0f;
                replan = true;
                continue;
            }

            if (!roots) stream->wantedGain = 0.0f;
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
            stream->instances = item.roots.size();
            stream->source = std::move(item.source);
            stream->mix.buffer = stream->source->buffer();
            stream->mix.key = item.appKey;
            stream->mix.gain = 0.0f;
            stream->mix.gainTarget = 1.0f;
            stream->wantedGain = 1.0f;
            stream->createdMs = nowMs();
            stream->lastSignalMs = stream->createdMs;
            // 20 ms fade: audible clicks are impossible, the delay is not.
            stream->mix.rampStep = 1.0f / (0.02f * static_cast<float>(options_.sampleRate));
            if (options_.externalClock) {
                stream->mix.primeFrames = static_cast<size_t>(options_.sampleRate) * static_cast<size_t>(std::max(0, options_.primeMs)) / 1000;
                stream->mix.maxBacklogFrames = static_cast<size_t>(options_.sampleRate) * static_cast<size_t>(std::max(0, options_.maxBacklogMs)) / 1000;
            }
            streams_[item.appKey] = std::move(stream);
            startErrors_.erase(item.appKey);
        }
    }

    if (replan) {
        // A capture is fading out to be restarted with the new root set: run
        // the plan again as soon as it is gone instead of waiting for the next
        // regular reconciliation.
        refreshRequested_ = true;
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
    // Everything that belongs to a stream is copied while the lock is held:
    // a retired stream is destroyed by the watch thread right after the lock
    // is released, so no pointer to it may outlive this block.
    struct Live {
        bool captured = false;
        uint64_t starved = 0;
        size_t instances = 0;
        std::string format;
        int latencyMs = -1;
        std::string error;
        float peak = 0.0f;
        int64_t createdMs = 0;
        int64_t lastSignalMs = 0;
        std::vector<uint32_t> roots;
        float inputPeak = -1.0f;
    };

    std::vector<AppGroup> snapshot;
    std::map<std::string, Live> live;
    std::map<std::string, std::string> errors;
    std::map<std::string, CapturePlan> plans;
    RuleSet rules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = snapshot_;
        plans = plans_;
        for (auto& [key, stream] : streams_) {
            Live info;
            info.captured = !stream->retired && stream->wantedGain > 0.0f;
            info.starved = stream->starved;
            info.instances = stream->instances;
            if (stream->source) {
                info.format = stream->source->formatDescription();
                info.latencyMs = stream->source->latencyMs();
                info.error = stream->source->lastError();
                info.inputPeak = stream->source->inputPeak();
            }
            info.roots = stream->rootPids;
            info.peak = stream->peak;
            info.createdMs = stream->createdMs;
            info.lastSignalMs = stream->lastSignalMs;
            live[key] = std::move(info);
        }
        errors = startErrors_;
        rules = rules_;
    }

    const int64_t now = nowMs();
    // Silence only means something while the mix is actually running.
    const bool mixing = !options_.externalClock || now - lastPullMs_.load() < 500;

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
        view.instances = group.roots.size();
        view.devices = group.devices;

        const auto planIt = plans.find(group.appKey);
        if (planIt != plans.end()) {
            view.blockedReason = planIt->second.blockedReason;
            view.coveredBy = planIt->second.coveredBy;
        }

        const auto streamIt = live.find(group.appKey);
        if (streamIt != live.end()) {
            const Live& stream = streamIt->second;
            view.captured = stream.captured;
            view.starvedFrames = stream.starved;
            if (stream.instances) view.instances = stream.instances;
            view.format = stream.format;
            view.latencyMs = stream.latencyMs;
            view.error = stream.error;
            if (stream.peak > view.peak) view.peak = stream.peak;
            view.captureRoots = stream.roots;
            view.inputPeak = stream.inputPeak;
            view.silentCapture = mixing && stream.captured && group.playing && group.peak > 0.01f
                && now - stream.createdMs > kSilentCaptureMs && now - stream.lastSignalMs > kSilentCaptureMs;
        }
        const auto errorIt = errors.find(group.appKey);
        if (errorIt != errors.end() && view.error.empty()) view.error = errorIt->second;

        if (!view.capturable) {
            view.state = "unsupported";
        } else if (view.transmitting && !view.blockedReason.empty()) {
            view.state = "blocked";
        } else if (!view.error.empty()) {
            view.state = "error";
        } else if (!view.transmitting) {
            view.state = "excluded";
        } else if (view.captured) {
            view.state = "transmitting";
        } else if (!view.coveredBy.empty()) {
            view.state = "covered";
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
            if (!stream->retired && stream->wantedGain > 0.0f) stats.activeStreams++;
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
