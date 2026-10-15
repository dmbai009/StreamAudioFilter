// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Portable (OS independent) audio format description. This header must never
// include Windows headers so the DSP layer can be unit tested anywhere.

#pragma once

#include <cstdint>

namespace saf {

enum class SampleFormat : uint32_t {
    Unknown = 0,
    PcmU8,
    PcmS16,
    PcmS24,     // packed 3 bytes per sample
    PcmS32,
    IeeeFloat32,
};

/** Description of a raw interleaved audio stream. */
struct AudioFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t validBits = 0;
    SampleFormat sample = SampleFormat::Unknown;

    bool isFullySpecified() const {
        return sampleRate > 0 && channels > 0 && sample != SampleFormat::Unknown;
    }

    bool operator==(const AudioFormat& other) const {
        return sampleRate == other.sampleRate && channels == other.channels && sample == other.sample;
    }
    bool operator!=(const AudioFormat& other) const { return !(*this == other); }

    size_t bytesPerSample() const {
        switch (sample) {
            case SampleFormat::PcmU8: return 1;
            case SampleFormat::PcmS16: return 2;
            case SampleFormat::PcmS24: return 3;
            case SampleFormat::PcmS32: return 4;
            case SampleFormat::IeeeFloat32: return 4;
            default: return 0;
        }
    }

    size_t frameBytes() const { return bytesPerSample() * channels; }
};

/** Converts a format to a human readable string, for diagnostics. */
const char* toString(SampleFormat format);

} // namespace saf
