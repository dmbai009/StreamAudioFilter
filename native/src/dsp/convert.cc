// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "convert.h"

#include <cstring>

namespace saf {

const char* toString(SampleFormat format) {
    switch (format) {
        case SampleFormat::PcmU8: return "u8";
        case SampleFormat::PcmS16: return "s16";
        case SampleFormat::PcmS24: return "s24";
        case SampleFormat::PcmS32: return "s32";
        case SampleFormat::IeeeFloat32: return "f32";
        default: return "unknown";
    }
}

namespace {

template <typename T>
void decodeTyped(const void* src, size_t frames, int channels, float scale, float* dst) {
    const T* in = static_cast<const T*>(src);
    const size_t count = frames * static_cast<size_t>(channels);
    for (size_t i = 0; i < count; i++) {
        dst[i] = sanitize(static_cast<float>(in[i]) * scale);
    }
}

} // namespace

void decodeToFloat(const void* src, size_t frames, const AudioFormat& format, float* dst) {
    if (!src || !dst || frames == 0) return;

    const int channels = format.channels;
    switch (format.sample) {
        case SampleFormat::IeeeFloat32:
            decodeTyped<float>(src, frames, channels, 1.0f, dst);
            break;
        case SampleFormat::PcmS16:
            decodeTyped<int16_t>(src, frames, channels, 1.0f / 32768.0f, dst);
            break;
        case SampleFormat::PcmS32:
            decodeTyped<int32_t>(src, frames, channels, 1.0f / 2147483648.0f, dst);
            break;
        case SampleFormat::PcmU8:
            // Unsigned 8 bit is centered around 128.
            for (size_t i = 0; i < frames * static_cast<size_t>(channels); i++)
                dst[i] = sanitize((static_cast<const uint8_t*>(src)[i] - 128) / 128.0f);
            break;
        case SampleFormat::PcmS24: {
            const uint8_t* in = static_cast<const uint8_t*>(src);
            const size_t count = frames * static_cast<size_t>(channels);
            for (size_t i = 0; i < count; i++) {
                const uint8_t* p = in + i * 3;
                int32_t value = (static_cast<int32_t>(p[2]) << 24) | (static_cast<int32_t>(p[1]) << 16) | (static_cast<int32_t>(p[0]) << 8);
                // Arithmetic shift keeps the sign.
                value >>= 8;
                dst[i] = sanitize(static_cast<float>(value) / 8388608.0f);
            }
            break;
        }
        default:
            std::memset(dst, 0, frames * static_cast<size_t>(channels) * sizeof(float));
            break;
    }
}

void downmixToStereo(const float* src, size_t frames, int srcChannels, float* dst) {
    if (!src || !dst || frames == 0 || srcChannels <= 0) return;

    switch (srcChannels) {
        case 1:
            for (size_t f = 0; f < frames; f++) {
                const float v = src[f];
                dst[f * 2] = v;
                dst[f * 2 + 1] = v;
            }
            break;
        case 2:
            std::memcpy(dst, src, frames * 2 * sizeof(float));
            break;
        default: {
            // 0.7071 == -3 dB
            constexpr float centerGain = 0.70710678f;
            for (size_t f = 0; f < frames; f++) {
                const float* frame = src + f * static_cast<size_t>(srcChannels);
                const float center = srcChannels > 2 ? frame[2] * centerGain : 0.0f;
                dst[f * 2] = frame[0] + center;
                dst[f * 2 + 1] = frame[1] + center;
            }
            break;
        }
    }
}

void mapChannels(const float* src, size_t frames, int srcChannels, float* dst, int dstChannels) {
    if (dstChannels == 2 || dstChannels <= 0) {
        downmixToStereo(src, frames, srcChannels, dst);
        return;
    }

    // Mono output: average of the two front channels.
    for (size_t f = 0; f < frames; f++) {
        if (srcChannels == 1) {
            dst[f] = src[f];
        } else if (srcChannels == 2) {
            dst[f] = (src[f * 2] + src[f * 2 + 1]) * 0.5f;
        } else {
            const float* frame = src + f * static_cast<size_t>(srcChannels);
            dst[f] = (frame[0] + frame[1]) * 0.5f;
        }
    }
}

} // namespace saf
