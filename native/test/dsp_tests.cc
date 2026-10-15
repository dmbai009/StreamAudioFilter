// StreamAudioFilter — DSP unit tests
// SPDX-License-Identifier: GPL-3.0-or-later
//
// These tests are completely OS independent and run on any platform with a
// C++17 compiler (they are the reason the DSP layer never includes Windows
// headers):
//
//     ./scripts/run-native-tests.sh        (any OS, g++/clang++)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/dsp/convert.h"
#include "../src/dsp/mixer.h"
#include "../src/dsp/resampler.h"
#include "../src/dsp/ring_buffer.h"

using namespace saf;

static int failures = 0;
static int checks = 0;

#define CHECK(condition, ...)                                                        \
    do {                                                                             \
        checks++;                                                                    \
        if (!(condition)) {                                                          \
            failures++;                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            std::printf("       " __VA_ARGS__);                                      \
            std::printf("\n");                                                       \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b, tolerance, ...)                                                    \
    do {                                                                                   \
        checks++;                                                                          \
        const double _a = (a), _b = (b);                                                    \
        if (std::fabs(_a - _b) > (tolerance)) {                                            \
            failures++;                                                                    \
            std::printf("  FAIL %s:%d: |%.9f - %.9f| > %g\n", __FILE__, __LINE__, _a, _b,  \
                        static_cast<double>(tolerance));                                   \
            std::printf("       " __VA_ARGS__);                                            \
            std::printf("\n");                                                             \
        }                                                                                  \
    } while (0)

static void section(const char* name) { std::printf("== %s\n", name); }

// ---------------------------------------------------------------- ring buffer

static void testRingBuffer() {
    section("ring buffer");

    FloatRingBuffer buffer(8, 2);
    CHECK(buffer.capacityFrames() == 8, "capacity");
    CHECK(buffer.availableFrames() == 0, "starts empty");

    const float data[6] = { 1, 2, 3, 4, 5, 6 };  // 3 frames of stereo
    CHECK(buffer.write(data, 3) == 0, "no drops");
    CHECK(buffer.availableFrames() == 3, "3 frames available");

    float out[16] = {};
    CHECK(buffer.read(out, 2) == 2, "reads 2");
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4, "FIFO order");
    CHECK(buffer.read(out, 8) == 1, "reads remaining");
    CHECK(out[0] == 5 && out[1] == 6, "last frame");
    CHECK(buffer.read(out, 1) == 0, "empty read returns 0");

    // Wrap around
    for (int i = 0; i < 6; i++) {
        const float f[2] = { static_cast<float>(i), static_cast<float>(-i) };
        buffer.write(f, 1);
    }
    CHECK(buffer.availableFrames() == 6, "wrapped size");
    for (int i = 0; i < 6; i++) {
        buffer.read(out, 1);
        CHECK(out[0] == static_cast<float>(i), "wrapped order %d", i);
    }

    // Overflow drops the oldest frames
    float block[20];
    for (int i = 0; i < 20; i++) block[i] = static_cast<float>(i);
    const size_t dropped = buffer.write(block, 10);
    CHECK(dropped == 2, "dropped == 2 (capacity 8), got %zu", dropped);
    CHECK(buffer.availableFrames() == 8, "filled to capacity");
    CHECK(buffer.read(out, 8) == 8, "reads all");
    // An oversized write keeps its newest frames (oldest input is dropped).
    CHECK(out[0] == 4.0f, "newest frames kept, got %f", out[0]);
    CHECK(buffer.droppedFrames() == 2, "drop counter");

    // Oversized single write keeps the newest data
    FloatRingBuffer small(4, 1);
    float big[10];
    for (int i = 0; i < 10; i++) big[i] = static_cast<float>(i);
    CHECK(small.write(big, 10) == 6, "oversized write reports dropped frames");
    small.read(out, 4);
    CHECK(out[0] == 6.0f && out[3] == 9.0f, "keeps newest of oversized write (%f..%f)", out[0], out[3]);

    // Concurrency smoke test: producer and consumer never corrupt state.
    FloatRingBuffer shared(1024, 1);
    std::thread producer([&] {
        for (int i = 0; i < 200000; i++) {
            const float v = static_cast<float>(i);
            shared.write(&v, 1);
        }
    });
    float sink[64];
    size_t totalRead = 0;
    for (int i = 0; i < 40000; i++) totalRead += shared.read(sink, 64);
    producer.join();
    totalRead += shared.read(sink, 64);
    CHECK(totalRead > 0, "concurrent read produced data");
    CHECK(shared.availableFrames() <= 1024, "never exceeds capacity");
}

// ------------------------------------------------------------------ conversion

static void testConversion() {
    section("format conversion");

    // s16 -> float
    const int16_t s16[4] = { 0, 32767, -32768, 16384 };
    float out[16] = {};
    AudioFormat fmt { 48000, 2, 16, SampleFormat::PcmS16 };
    decodeToFloat(s16, 2, fmt, out);
    CHECK(out[0] == 0.0f, "s16 zero");
    CHECK_NEAR(out[1], 0.99997, 1e-4, "s16 max");
    CHECK(out[2] == -1.0f, "s16 min");
    CHECK_NEAR(out[3], 0.5, 1e-5, "s16 half, got %f", out[3]);

    // s24 packed -> float (little endian, sign extended)
    const uint8_t s24[6] = { 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F };
    AudioFormat fmt24 { 48000, 2, 24, SampleFormat::PcmS24 };
    decodeToFloat(s24, 2, fmt24, out);
    CHECK(out[0] == 0.0f, "s24 zero");
    CHECK(out[1] > 0.9999f, "s24 max, got %f", out[1]);
    const uint8_t s24neg[3] = { 0x00, 0x00, 0x80 };
    decodeToFloat(s24neg, 1, AudioFormat { 48000, 1, 24, SampleFormat::PcmS24 }, out);
    CHECK(out[0] == -1.0f, "s24 min, got %f", out[0]);

    // s32 -> float
    const int32_t s32[2] = { 1073741824, -2147483647 };
    decodeToFloat(s32, 2, AudioFormat { 48000, 2, 32, SampleFormat::PcmS32 }, out);
    CHECK_NEAR(out[0], 0.5, 1e-6, "s32 half");
    CHECK(out[1] < -0.999f, "s32 negative");

    // NaN / Inf guard
    const float bad[2] = { std::nanf(""), 1e30f };
    decodeToFloat(bad, 2, AudioFormat { 48000, 2, 32, SampleFormat::IeeeFloat32 }, out);
    CHECK(out[0] == 0.0f && out[1] == 0.0f, "NaN/Inf sanitised");

    // mono -> stereo
    const float mono[3] = { 0.25f, -0.5f, 1.0f };
    downmixToStereo(mono, 3, 1, out);
    CHECK(out[0] == 0.25f && out[1] == 0.25f, "mono duplicated");
    CHECK(out[4] == 1.0f && out[5] == 1.0f, "mono last frame");

    // 5.1 -> stereo keeps FL/FR and folds center at -3 dB, drops LFE/surround
    const float surround[6] = { 1.0f, 1.0f, 1.0f, 0.0f, 0.5f, 0.5f };
    downmixToStereo(surround, 1, 6, out);
    CHECK_NEAR(out[0], 1.0 + 0.70710678, 1e-6, "center folded, got %f", out[0]);
    CHECK_NEAR(out[1], 1.0 + 0.70710678, 1e-6, "center folded R");
}

// ------------------------------------------------------------------ resampler

/**
 * Measures the SNR of a resampled sine against the ideal resampled sine and
 * reports the measured (unity) gain via @p gainOut.
 */
static double measureSineSnr(double inRate, double outRate, double toneHz, int taps, double* gainOut = nullptr) {
    const int inFrames = static_cast<int>(inRate);  // 1 second of input
    std::vector<float> input(static_cast<size_t>(inFrames) * 2);
    for (int i = 0; i < inFrames; i++) {
        const float v = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * toneHz * i / inRate));
        input[static_cast<size_t>(i) * 2] = v;
        input[static_cast<size_t>(i) * 2 + 1] = v;
    }

    SincResampler resampler(inRate, outRate, 2, taps);
    std::vector<float> output;
    output.reserve(static_cast<size_t>(inFrames) * 2);

    // Feed in irregular chunks: verifies that the streaming state is correct.
    const size_t chunkSizes[] = { 441, 1024, 137, 4800, 32 };
    size_t position = 0;
    size_t chunkIndex = 0;
    std::vector<float> block(static_cast<size_t>(2 * (4800 + 64)));
    while (position < static_cast<size_t>(inFrames)) {
        const size_t chunk = std::min(chunkSizes[chunkIndex++ % 5], static_cast<size_t>(inFrames) - position);
        const size_t produced = resampler.process(&input[position * 2], chunk, block.data(), block.size() / 2);
        position += chunk;
        for (size_t i = 0; i < produced * 2; i++) output.push_back(block[i]);
    }

    // Compare against the ideal sine, ignoring the initial transient.
    const size_t skip = 4000;
    double sumIdealSq = 0, sumErrorSq = 0;
    double dotSin = 0, dotCos = 0;
    std::vector<double> ideal;
    for (size_t n = skip; n < output.size() / 2; n++) {
        const double value = std::sin(2.0 * M_PI * toneHz * static_cast<double>(n) / outRate);
        ideal.push_back(value);
        dotSin += output[n * 2] * value;
        dotCos += output[n * 2] * std::cos(2.0 * M_PI * toneHz * static_cast<double>(n) / outRate);
    }

    // Least squares gain (the resampler must be unity gain, so also check it).
    const double referenceEnergy = 0.5 * static_cast<double>(ideal.size());
    const double gain = std::sqrt(dotSin * dotSin + dotCos * dotCos) / referenceEnergy / 0.5;

    for (size_t i = 0; i < ideal.size(); i++) {
        const double reference = 0.5 * gain * ideal[i];
        const double error = output[(skip + i) * 2] - reference;
        sumIdealSq += reference * reference;
        sumErrorSq += error * error;
    }

    if (gainOut) *gainOut = gain;
    return 10.0 * std::log10(sumIdealSq / std::max(sumErrorSq, 1e-30));
}

static void testResampler() {
    section("resampler");

    // Same rate must be a bit exact passthrough.
    {
        SincResampler resampler(48000, 48000, 2, 16);
        CHECK(resampler.passthrough(), "same rate uses passthrough");
        float in[8] = { 1, 1, 2, 2, 3, 3, 4, 4 };
        float out[8] = {};
        const size_t produced = resampler.process(in, 4, out, 4);
        CHECK(produced == 4, "passthrough frame count");
        CHECK(std::memcmp(in, out, sizeof(in)) == 0, "passthrough identical");
    }

    // Unity gain and quality for the common conversions.
    double gain44to48 = 0, gain48to44 = 0, gain48to96 = 0;
    const double snr44to48 = measureSineSnr(44100, 48000, 1000, 16, &gain44to48);
    const double snr48to44 = measureSineSnr(48000, 44100, 1000, 16, &gain48to44);
    const double snr48to96 = measureSineSnr(48000, 96000, 1000, 16, &gain48to96);
    std::printf("  SNR 44.1k->48k: %.1f dB, 48k->44.1k: %.1f dB, 48k->96k: %.1f dB\n", snr44to48, snr48to44, snr48to96);
    CHECK_NEAR(gain44to48, 1.0, 0.01, "unity gain 44.1k->48k, got %.4f", gain44to48);
    CHECK_NEAR(gain48to44, 1.0, 0.01, "unity gain 48k->44.1k, got %.4f", gain48to44);
    CHECK_NEAR(gain48to96, 1.0, 0.01, "unity gain 48k->96k, got %.4f", gain48to96);
    CHECK(snr44to48 > 70.0, "44.1k->48k SNR %.1f dB", snr44to48);
    CHECK(snr48to44 > 70.0, "48k->44.1k SNR %.1f dB", snr48to44);
    CHECK(snr48to96 > 70.0, "48k->96k SNR %.1f dB", snr48to96);

    // Band limiting: a 15 kHz tone must not alias when downsampling 48k -> 24k.
    {
        const int inFrames = 48000;
        std::vector<float> input(static_cast<size_t>(inFrames));
        for (int i = 0; i < inFrames; i++) input[i] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 15000.0 * i / 48000.0));
        SincResampler resampler(48000, 24000, 1, 16);
        std::vector<float> block(4096);
        std::vector<float> output;
        for (size_t i = 0; i < input.size(); i += 1000) {
            const size_t chunk = std::min<size_t>(1000, input.size() - i);
            const size_t produced = resampler.process(&input[i], chunk, block.data(), block.size());
            for (size_t k = 0; k < produced; k++) output.push_back(block[k]);
        }
        // 15 kHz at 24 kHz output is above the new Nyquist -> must be attenuated.
        double peak = 0;
        for (size_t i = output.size() / 2; i < output.size(); i++) peak = std::max(peak, std::fabs(static_cast<double>(output[i])));
        CHECK(peak < 0.05, "aliasing suppressed, peak %.4f", peak);
    }

    // Output size bound must hold.
    {
        SincResampler resampler(44100, 48000, 2, 16);
        // 441 input frames at 44.1 kHz are ~405 output frames at 48 kHz.
        CHECK(resampler.maxOutputFor(441) >= 405, "maxOutputFor covers the ratio, got %zu", resampler.maxOutputFor(441));
        CHECK_NEAR(resampler.maxOutputFor(48000), 48000.0 * 48000.0 / 44100.0 + 36, 36, "maxOutputFor upsampling");
        CHECK(SincResampler(48000, 96000, 2, 16).maxOutputFor(48000) >= 96000, "maxOutputFor 2x upsampling");
        CHECK(SincResampler(48000, 48000, 2, 16).maxOutputFor(0) >= 4, "maxOutputFor tolerates zero input");
    }
}

// ---------------------------------------------------------------------- mixer

static void testMixer() {
    section("mixer");

    const int rate = 48000;
    const int blockFrames = 480;  // 10 ms

    Mixer mixer(rate, 2, blockFrames);
    FloatRingBuffer a(rate / 2, 2);
    FloatRingBuffer b(rate / 2, 2);

    MixSource sourceA { &a, "a" };
    MixSource sourceB { &b, "b" };
    sourceA.rampStep = sourceB.rampStep = 1.0f / (0.02f * rate);

    std::vector<MixSource*> sources { &sourceA, &sourceB };
    std::vector<float> out(static_cast<size_t>(blockFrames) * 2);

    // --- fade in must not click
    std::vector<float> constant(static_cast<size_t>(blockFrames) * 2, 0.5f);
    a.write(constant.data(), blockFrames);
    sourceA.gainTarget = 1.0f;
    mixer.mix(sources, out.data());
    float maxDelta = 0;
    float previous = 0;
    for (int f = 0; f < blockFrames; f++) {
        maxDelta = std::max(maxDelta, std::fabs(out[f * 2] - previous));
        previous = out[f * 2];
    }
    CHECK(maxDelta < 0.5f / (0.02f * rate) + 1e-6f, "fade in is gradual, max delta %f", maxDelta);
    // A 20 ms fade is half finished after one 10 ms block.
    CHECK_NEAR(out[(blockFrames - 2) * 2], 0.25f, 0.01f, "fade reaches half gain in one block, got %f", out[(blockFrames - 2) * 2]);

    // --- mixing two streams, no clipping
    sourceA.gain = 1.0f;
    sourceB.gain = 1.0f;
    std::fill(constant.begin(), constant.end(), 1.0f);
    a.write(constant.data(), blockFrames);
    b.write(constant.data(), blockFrames);
    mixer.mix(sources, out.data());
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= 1.0f, "limiter keeps output <= 1.0, peak %f", peak);
    CHECK(peak > 0.85f, "limiter does not over-attenuate, peak %f", peak);

    // --- starvation produces silence, output timeline stays intact
    MixSource sourceC { &b, "c" };
    std::vector<MixSource*> single { &sourceC };
    sourceC.gain = sourceC.gainTarget = 1.0f;
    b.clear();
    mixer.mix(single, out.data());
    float energy = 0;
    for (float v : out) energy += std::fabs(v);
    CHECK(energy == 0.0f, "empty stream is silence");
    CHECK(sourceC.starvedFrames == static_cast<uint64_t>(blockFrames), "starvation counted, got %llu",
          static_cast<unsigned long long>(sourceC.starvedFrames));

    // --- fade out finishes and marks the stream removable
    MixSource sourceD { &a, "d" };
    sourceD.gain = 1.0f;
    sourceD.gainTarget = 0.0f;
    std::vector<MixSource*> fading { &sourceD };
    int iterations = 0;
    while (!sourceD.finishedFadeOut() && iterations < 100) {
        a.write(constant.data(), blockFrames);
        mixer.mix(fading, out.data());
        iterations++;
    }
    CHECK(sourceD.finishedFadeOut(), "fade out completes");
    CHECK(iterations <= 2, "fade out takes ~1 block (20 ms), took %d", iterations);
    CHECK(std::fabs(out[blockFrames * 2 - 1]) < 1e-6f, "silent after fade out, got %f", out[blockFrames * 2 - 1]);
}

// ------------------------------------------------------------------- main

int main() {
    std::printf("StreamAudioFilter native DSP tests\n\n");
    testRingBuffer();
    testConversion();
    testResampler();
    testMixer();

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
