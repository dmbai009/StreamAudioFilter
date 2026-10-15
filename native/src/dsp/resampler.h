// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming windowed-sinc sample rate converter.
//
// The primary capture path asks WASAPI to resample for us
// (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY), which is both
// faster and higher quality. This converter exists for the fallback path and
// for unusual device formats. It is a 2*T tap Blackman windowed sinc
// interpolator with per-output-sample coefficient normalisation, which keeps
// the unity gain exactly and avoids the low frequency droop that a naive
// windowed sinc shows.

#pragma once

#include <cstddef>
#include <vector>

namespace saf {

class SincResampler {
public:
    /**
     * @param inRate   input sample rate in Hz
     * @param outRate  output sample rate in Hz
     * @param channels channel count (interleaved)
     * @param taps     half window length in input samples (default 16 -> 32 taps)
     */
    SincResampler(double inRate, double outRate, int channels, int taps = 16);

    /** Number of input frames the caller may hand over next. */
    size_t process(const float* in, size_t inFrames, float* out, size_t maxOutFrames);

    /**
     * Upper bound of output frames produced by process() for @p inFrames input
     * frames. Callers should size their output buffer with this value.
     */
    size_t maxOutputFor(size_t inFrames) const;

    /** Frames currently buffered internally (not yet consumed). */
    size_t pendingFrames() const { return pendingFrames_; }

    void reset();

    /** Input samples consumed per output sample. */
    double step() const { return step_; }
    bool passthrough() const { return passthrough_; }

private:
    int channels_;
    int taps_;
    double step_;
    bool passthrough_;
    double cutoff_;

    // Unconsumed input, interleaved: [taps_ history frames][pendingFrames_ live frames].
    std::vector<float> pending_;
    size_t pendingFrames_ = 0;
    // Fractional position of the next output sample inside the live region.
    double phase_ = 0.0;

    std::vector<float> weights_;

    float sampleAt(long frameIndex, int channel) const;
    /** Drops live frames that can no longer take part in interpolation. */
    void consumeUpTo(double position);
};

} // namespace saf
