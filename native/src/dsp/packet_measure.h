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
#include <vector>

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
 * The level of an amplitude in the -1..1 range, in dBFS (0 dBFS = full scale).
 * Exact zero is reported as a floor of -200 dB instead of negative infinity, so
 * that every sample window has a comparable number.
 */
inline double dbfs(double amplitude) {
    if (amplitude <= 0.0) return -200.0;
    const double value = 20.0 * std::log10(amplitude);
    return value < -200.0 ? -200.0 : value;
}

/**
 * The level below which a window is reported as a noise floor rather than as audio.
 *
 * -60 dBFS is a reporting convention, not a statement about any machine, and it
 * exists because a count of non-zero samples alone cannot tell dither from music:
 * the real run of the probe on Windows 10 19045 that prompted this line measured
 * 383520 frames with a peak of -90.2 dBFS and 580972 samples that were not exactly
 * zero. Reporting that as "the stream carries audio" would have been wrong.
 */
inline constexpr double kNoiseFloorDbfs = -60.0;

/** What a measured window contains: nothing, dither, or audio. */
enum class PacketContent {
    /** No samples at all, or every one of them exactly zero. */
    Silent,
    /** Samples are there, but the peak is below kNoiseFloorDbfs. */
    NoiseFloor,
    /** The peak is above kNoiseFloorDbfs. */
    Audio,
};

/** Classification of a window, using the same threshold the reports quote. */
inline PacketContent classifyContent(const SampleStats& stats, double noiseFloorDbfs = kNoiseFloorDbfs) {
    if (stats.samples == 0 || stats.peak <= 0.0) return PacketContent::Silent;
    return dbfs(stats.peak) < noiseFloorDbfs ? PacketContent::NoiseFloor : PacketContent::Audio;
}

/**
 * Goertzel: how much of @p frequency is inside the interleaved samples.
 *
 * @p count counts interleaved samples, @p channels says how many of them form one
 * frame, and the search runs on the mono downmix at @p sampleRate. The channel
 * count is not an optimisation, it is the difference between a working search and
 * a broken one: reading interleaved stereo as if it were mono doubles every sample,
 * which moves a 1 kHz tone to 500 Hz, so a search at 1 kHz finds nothing at all
 * (measured with this very function: 0.1999 at 500 Hz, 0.0000 at 1 kHz). The first
 * real probe runs on Windows 10 19045 did exactly that and reported 0.000025 for a
 * tone that was in the captured audio - a measurement bug that looked like a
 * capture problem.
 *
 * Normalised so that the result is the amplitude of that frequency: a pure sine
 * of amplitude 0.2 reads about 0.2 (about 0.1 when it occupies one channel of
 * two), and an unrelated signal (or silence) reads near zero. The standalone probe
 * uses 0.05 as "the tone is there", which is a quarter of the amplitude it plays.
 */
inline double toneMagnitude(const float* samples, size_t count, int sampleRate, double frequency,
    int channels = 1) {
    if (!samples || count == 0 || sampleRate <= 0) return 0.0;
    if (channels < 1) channels = 1;

    const size_t mono = count / static_cast<size_t>(channels);
    if (mono == 0) return 0.0;

    std::vector<float> downmix;
    const float* search = samples;
    if (channels > 1) {
        downmix.resize(mono);
        for (size_t frame = 0; frame < mono; frame++) {
            double sum = 0.0;
            for (int channel = 0; channel < channels; channel++) {
                sum += static_cast<double>(samples[frame * static_cast<size_t>(channels)
                    + static_cast<size_t>(channel)]);
            }
            downmix[frame] = static_cast<float>(sum / static_cast<double>(channels));
        }
        search = downmix.data();
    }

    const double omega = 2.0 * 3.14159265358979323846 * frequency / static_cast<double>(sampleRate);
    const double coefficient = 2.0 * std::cos(omega);

    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = 0; i < mono; i++) {
        const double s0 = static_cast<double>(search[i]) + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(power < 0.0 ? 0.0 : power) / static_cast<double>(mono);
}

} // namespace saf::dsp
