// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Measurement of PCM packets: how much is in them, how loud they are, and
// whether a known tone is present.
//
// Why this is portable and tested. The first version of the capture diagnostics
// decided "audio arrived" from the AUDCLNT_BUFFERFLAGS_SILENT packet flag. A
// process loopback stream does not set that flag (the plugin's own probe printed
// "0 silent frames" for every stream it ever saw, including silent ones), so the
// diagnostics reported samples for a stream that carried nothing but zeros. The
// measurement below is the replacement: it counts real samples, so "the stream
// exists" and "the stream carries audio" stop being the same claim.
//
// It deliberately includes no Windows header, so the portable test suite can
// hold it to its numbers on any machine (native/test/dsp_tests.cc). Nothing here
// stores audio: caller supplied buffers are read, counters and a peak are
// updated, and the buffers are never written to.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace saf::dsp {

/** Peak, RMS and the number of non-zero samples over interleaved samples. */
struct SampleStats {
    /** Samples looked at, across all channels. */
    uint64_t samples = 0;
    /** How many of them were not exactly zero. */
    uint64_t nonzero = 0;
    /** Largest absolute value seen, the cheapest "is there anything here" test. */
    double peak = 0.0;
    /** Running sum of squares, for the RMS. */
    double sumSquares = 0.0;

    void add(double sample) {
        if (sample != 0.0) nonzero++;
        const double magnitude = sample < 0.0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
        sumSquares += sample * sample;
        samples++;
    }

    double rms() const {
        return samples == 0 ? 0.0 : std::sqrt(sumSquares / static_cast<double>(samples));
    }

    /** True when audio, not merely a correctly paced stream of zeros, arrived. */
    bool carriesAudio() const { return nonzero > 0; }

    void reset() { *this = SampleStats {}; }
};

/** Interleaved 32 bit float samples, as WASAPI delivers them. */
inline void measureFloats(const float* values, size_t count, SampleStats& stats) {
    if (!values) return;
    for (size_t i = 0; i < count; i++) stats.add(static_cast<double>(values[i]));
}

/** Interleaved 16 bit PCM, scaled to the same -1..1 range as the float path. */
inline void measureInt16(const int16_t* values, size_t count, SampleStats& stats) {
    if (!values) return;
    for (size_t i = 0; i < count; i++) stats.add(static_cast<double>(values[i]) / 32768.0);
}

/**
 * Goertzel: how much of @p frequency is inside @p count interleaved samples.
 *
 * Normalised so that the result is the amplitude of that frequency: a pure sine
 * of amplitude 0.2 reads about 0.2, and an unrelated signal (or silence) reads
 * near zero. The standalone probe uses 0.05 as "the tone is there", which is a
 * quarter of the amplitude it plays.
 */
inline double toneMagnitude(const float* samples, size_t count, int sampleRate, double frequency) {
    if (!samples || count == 0 || sampleRate <= 0) return 0.0;

    const double omega = 2.0 * 3.14159265358979323846 * frequency / static_cast<double>(sampleRate);
    const double coefficient = 2.0 * std::cos(omega);

    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = 0; i < count; i++) {
        const double s0 = static_cast<double>(samples[i]) + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(power < 0.0 ? 0.0 : power) / static_cast<double>(count);
}

} // namespace saf::dsp
