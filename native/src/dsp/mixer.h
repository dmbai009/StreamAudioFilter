// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mixing of N capture streams into one continuous output stream.
//
// Design notes (they matter for the "no clicks, no clipping" requirement):
//  * Every stream owns a FIFO. The mixer always consumes a fixed block size,
//    so the output timeline is perfectly regular even if a capture stalls.
//  * A stream that runs dry contributes silence and is counted as starved; it
//    never shifts the output timeline (that would break A/V sync).
//  * Streams fade in/out over 20 ms. A stream is only removed once its fade
//    out completed, so excluding an application never clicks.
//  * The sum of all streams goes through a peak limiter with instant attack
//    and slow release, followed by a hard clamp as a last resort guard.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ring_buffer.h"

namespace saf {

/** Per stream mixing state. Owned by the engine, referenced by the mixer. */
struct MixSource {
    FloatRingBuffer* buffer = nullptr;
    std::string key;

    /** Current and target gain, ramped per sample to avoid clicks. */
    float gain = 0.0f;
    float gainTarget = 0.0f;
    float rampStep = 1.0f;

    /** Peak level of the last block (0..1), for the UI activity meter. */
    float peak = 0.0f;

    /** Diagnostics. */
    uint64_t starvedFrames = 0;

    bool reachedTarget() const { return gain == gainTarget; }
    bool finishedFadeOut() const { return gainTarget <= 0.0f && gain <= 0.0f; }
};

/**
 * Mixes sources into 32 bit float interleaved stereo blocks.
 * Not thread safe: the engine serialises all calls.
 */
class Mixer {
public:
    Mixer(int sampleRate, int channels, int blockFrames, float fadeMs = 20.0f, float limiterCeiling = 0.891f);

    /** Mixes exactly blockFrames() frames into @p out (interleaved). */
    void mix(const std::vector<MixSource*>& sources, float* out);

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    int blockFrames() const { return blockFrames_; }

    /** Gain reduction currently applied by the limiter (1.0 == none). */
    float limiterGain() const { return limiterGain_; }
    /** Number of samples the final safety clamp had to touch. */
    uint64_t clampedSamples() const { return clampedSamples_; }
    /** Number of blocks that hit the limiter. */
    uint64_t limitedBlocks() const { return limitedBlocks_; }

private:
    int sampleRate_;
    int channels_;
    int blockFrames_;
    float fadeStep_;
    float ceiling_;
    float limiterGain_ = 1.0f;
    uint64_t clampedSamples_ = 0;
    uint64_t limitedBlocks_ = 0;
    std::vector<float> scratch_;
};

} // namespace saf
