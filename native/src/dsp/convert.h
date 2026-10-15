// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sample format decoding and channel mapping. Portable: no OS headers.

#pragma once

#include <cstddef>
#include <cstdint>

#include "audio_format.h"

namespace saf {

/** Replaces NaN/Inf/denormals with 0. Audio engines occasionally emit them. */
inline float sanitize(float v) {
    if (!(v == v)) return 0.0f;               // NaN
    if (v > 1.0e9f || v < -1.0e9f) return 0.0f;
    return v;
}

/**
 * Decodes @p frames frames of interleaved audio in @p format into float32,
 * preserving the channel count. @p dst must hold frames * fmt.channels floats.
 */
void decodeToFloat(const void* src, size_t frames, const AudioFormat& format, float* dst);

/**
 * Maps an arbitrary channel count down to stereo.
 * 1 channel  -> duplicated to both channels.
 * 2 channels -> copied.
 * > 2        -> front left/right are kept, the center channel (index 2) is
 *               folded in at -3 dB, everything else (surround, LFE) is
 *               discarded. Surround content is intentionally not folded to
 *               avoid unpredictable loudness on unusual channel layouts.
 */
void downmixToStereo(const float* src, size_t frames, int srcChannels, float* dst);

/**
 * Maps an arbitrary channel count to stereo, keeping exactly @p dstChannels
 * channels (currently 1 or 2 supported; anything else falls back to stereo).
 */
void mapChannels(const float* src, size_t frames, int srcChannels, float* dst, int dstChannels);

} // namespace saf
