// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace saf {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double sinc(double x) {
    if (std::fabs(x) < 1e-9) return 1.0;
    const double pix = kPi * x;
    return std::sin(pix) / pix;
}

/** Blackman window over the interval [-1, 1]. */
inline double blackman(double normalised) {
    return 0.42 + 0.5 * std::cos(kPi * normalised) + 0.08 * std::cos(2.0 * kPi * normalised);
}

} // namespace

// Buffer layout:
//   [ taps_ history frames ][ pendingFrames_ live frames ]
// The history region always exists (silence at stream start), which is what
// makes interpolation across block borders trivial and allocation free.
SincResampler::SincResampler(double inRate, double outRate, int channels, int taps)
    : channels_(std::max(1, channels))
    , taps_(std::max(2, taps))
    // Input samples consumed per produced output sample.
    , step_(inRate > 0 ? inRate / outRate : 1.0)
    , passthrough_(std::fabs(inRate - outRate) < 1e-9)
    , cutoff_(inRate > 0 ? 0.95 * std::min(1.0, outRate / inRate) : 0.95) {
    pending_.assign(static_cast<size_t>(taps_) * channels_, 0.0f);
    weights_.assign(static_cast<size_t>(2 * taps_ + 1), 0.0f);
}

void SincResampler::reset() {
    pending_.assign(static_cast<size_t>(taps_) * channels_, 0.0f);
    pendingFrames_ = 0;
    phase_ = 0.0;
}

float SincResampler::sampleAt(long frameIndex, int channel) const {
    const long absolute = frameIndex + taps_;
    if (absolute < 0 || absolute >= static_cast<long>(taps_ + pendingFrames_)) return 0.0f;
    return pending_[static_cast<size_t>(absolute) * channels_ + channel];
}

size_t SincResampler::maxOutputFor(size_t inFrames) const {
    return static_cast<size_t>(std::ceil(static_cast<double>(inFrames) / step_)) + 2 * static_cast<size_t>(taps_) + 4;
}

void SincResampler::consumeUpTo(double position) {
    // Drop everything that no longer takes part in interpolation, shifting the
    // history window along with it.
    const long keepFrom = static_cast<long>(std::floor(position)) - taps_ + 1;
    if (keepFrom <= 0) return;

    const size_t drop = std::min<size_t>(static_cast<size_t>(keepFrom), pendingFrames_);
    if (drop == 0) return;

    const size_t remaining = pendingFrames_ - drop;
    if (remaining > 0) {
        std::memmove(pending_.data(), pending_.data() + drop * channels_, (taps_ + remaining) * channels_ * sizeof(float));
    }
    pending_.resize((taps_ + remaining) * channels_);
    pendingFrames_ = remaining;
    phase_ -= static_cast<double>(drop);
}

size_t SincResampler::process(const float* in, size_t inFrames, float* out, size_t maxOutFrames) {
    if (in && inFrames > 0) {
        const size_t historyFloats = static_cast<size_t>(taps_) * channels_;
        pending_.resize(historyFloats + (pendingFrames_ + inFrames) * channels_);
        std::memcpy(pending_.data() + historyFloats + pendingFrames_ * channels_, in, inFrames * channels_ * sizeof(float));
        pendingFrames_ += inFrames;
    }

    size_t outCount = 0;

    if (passthrough_) {
        outCount = std::min(inFrames, maxOutFrames);
        if (outCount > 0) {
            std::memcpy(out, pending_.data() + static_cast<size_t>(taps_) * channels_, outCount * channels_ * sizeof(float));
            consumeUpTo(static_cast<double>(outCount));
            phase_ = 0.0;
        }
        return outCount;
    }

    while (outCount < maxOutFrames) {
        const long i0 = static_cast<long>(std::floor(phase_));
        if (i0 + taps_ >= static_cast<long>(pendingFrames_)) break;

        const double frac = phase_ - static_cast<double>(i0);

        double weightSum = 0.0;
        for (int j = -taps_; j <= taps_; j++) {
            const double distance = frac - static_cast<double>(j);
            const double w = cutoff_ * sinc(cutoff_ * distance) * blackman(distance / taps_);
            weights_[static_cast<size_t>(j + taps_)] = w;
            weightSum += w;
        }
        if (std::fabs(weightSum) < 1e-12) weightSum = 1.0;
        const double normalise = 1.0 / weightSum;

        for (int c = 0; c < channels_; c++) {
            double acc = 0.0;
            for (int j = -taps_; j <= taps_; j++) {
                acc += sampleAt(i0 + j, c) * weights_[static_cast<size_t>(j + taps_)];
            }
            out[outCount * channels_ + c] = static_cast<float>(acc * normalise);
        }

        outCount++;
        phase_ += step_;
    }

    if (outCount > 0) consumeUpTo(phase_);

    return outCount;
}

} // namespace saf
