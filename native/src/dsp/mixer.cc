// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "convert.h"

namespace saf {

Mixer::Mixer(int sampleRate, int channels, int blockFrames, float fadeMs, float limiterCeiling)
    : sampleRate_(sampleRate)
    , channels_(channels)
    , blockFrames_(blockFrames)
    , ceiling_(limiterCeiling) {
    fadeStep_ = fadeMs > 0.0f ? 1.0f / (fadeMs * 0.001f * static_cast<float>(sampleRate)) : 1.0f;
    scratch_.assign(static_cast<size_t>(blockFrames_) * channels_, 0.0f);
}

void Mixer::mix(const std::vector<MixSource*>& sources, float* out) {
    const size_t sampleCount = static_cast<size_t>(blockFrames_) * channels_;
    std::memset(out, 0, sampleCount * sizeof(float));

    for (MixSource* source : sources) {
        if (!source || !source->buffer) continue;

        const size_t got = source->buffer->read(scratch_.data(), static_cast<size_t>(blockFrames_));
        if (got < static_cast<size_t>(blockFrames_)) {
            // Missing tail is silence: the output timeline must not shift.
            std::memset(scratch_.data() + got * channels_, 0, (static_cast<size_t>(blockFrames_) - got) * channels_ * sizeof(float));
            source->starvedFrames += static_cast<uint64_t>(blockFrames_) - got;
        }

        float blockPeak = 0.0f;
        for (int f = 0; f < blockFrames_; f++) {
            // Per sample gain ramp towards the target.
            if (source->gain < source->gainTarget) {
                source->gain = std::min(source->gainTarget, source->gain + source->rampStep);
            } else if (source->gain > source->gainTarget) {
                source->gain = std::max(source->gainTarget, source->gain - source->rampStep);
            }
            const float g = source->gain;

            for (int c = 0; c < channels_; c++) {
                const size_t index = static_cast<size_t>(f) * channels_ + c;
                const float v = sanitize(scratch_[index]) * g;
                out[index] += v;
                blockPeak = std::max(blockPeak, std::fabs(v));
            }
        }
        source->peak = blockPeak;
    }

    // Peak limiter: instant attack, slow release.
    float peak = 0.0f;
    for (size_t i = 0; i < sampleCount; i++) peak = std::max(peak, std::fabs(out[i]));

    const float targetGain = peak > ceiling_ ? ceiling_ / peak : 1.0f;
    if (targetGain < limiterGain_) {
        limiterGain_ = targetGain;  // instant attack
        limitedBlocks_++;
    } else if (targetGain > limiterGain_) {
        // ~300 ms release towards unity.
        const float releaseStep = static_cast<float>(blockFrames_) / (0.3f * static_cast<float>(sampleRate_));
        limiterGain_ = std::min(targetGain, limiterGain_ + releaseStep);
    }

    if (limiterGain_ < 1.0f) {
        for (size_t i = 0; i < sampleCount; i++) out[i] *= limiterGain_;
    }

    // Final safety net: never hand out samples above full scale.
    for (size_t i = 0; i < sampleCount; i++) {
        if (out[i] > 1.0f) {
            out[i] = 1.0f;
            clampedSamples_++;
        } else if (out[i] < -1.0f) {
            out[i] = -1.0f;
            clampedSamples_++;
        }
    }
}

} // namespace saf
