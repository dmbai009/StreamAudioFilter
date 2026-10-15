// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two sub formats we care about, defined here instead of pulling in
// <ksmedia.h>.
//
// <ksmedia.h> is a kernel streaming header: it refuses to compile unless
// <ks.h> was included first ("KS.H must be included before KSMEDIA.H"), and it
// drags driver types into a user mode audio client. The GUIDs themselves are
// part of the public WAVE_FORMAT_EXTENSIBLE specification, so they are simply
// written out. WAVEFORMATEXTENSIBLE and the rest come from <mmreg.h>.

#pragma once

#include <windows.h>

#include <mmreg.h>

namespace saf::win {

/**
 * The string that activates a process loopback client. The SDK defines it as a
 * macro in one of the audio headers; the fallback keeps the build independent of
 * which one, and the value is part of the documented activation contract.
 */
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#    define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

/**
 * Channel masks for the extensible capture format. KSAUDIO_SPEAKER_* lives in
 * the kernel streaming header we deliberately avoid, but the values are the
 * documented SPEAKER_* combinations from mmreg.h.
 */
constexpr DWORD kSpeakerMono = SPEAKER_FRONT_CENTER;
constexpr DWORD kSpeakerStereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

/** KSDATAFORMAT_SUBTYPE_PCM — 00000001-0000-0010-8000-00AA00389B71. */
constexpr GUID kSubFormatPcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/** KSDATAFORMAT_SUBTYPE_IEEE_FLOAT — 00000003-0000-0010-8000-00AA00389B71. */
constexpr GUID kSubFormatIeeeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/** Bit-exact comparison; IsEqualGUID is a function call for no good reason here. */
inline bool sameGuid(const GUID& left, const GUID& right) {
    return left.Data1 == right.Data1 && left.Data2 == right.Data2 && left.Data3 == right.Data3
        && left.Data4[0] == right.Data4[0] && left.Data4[1] == right.Data4[1]
        && left.Data4[2] == right.Data4[2] && left.Data4[3] == right.Data4[3]
        && left.Data4[4] == right.Data4[4] && left.Data4[5] == right.Data4[5]
        && left.Data4[6] == right.Data4[6] && left.Data4[7] == right.Data4[7];
}

/**
 * Builds the interleaved float format the plugin asks the audio engine for:
 * WAVE_FORMAT_EXTENSIBLE, 32 bit float, the requested channel count.
 *
 * 32 bit float is what the DSP core works in, and with AUTOCONVERTPCM the
 * engine converts it from whatever the endpoint uses, so the plugin does not
 * have to guess the device format.
 */
inline void makeFloatFormat(WAVEFORMATEXTENSIBLE& format, int sampleRate, int channels) {
    format = {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    format.Format.nAvgBytesPerSec = static_cast<DWORD>(sampleRate * channels * 4);
    format.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channels >= 2 ? kSpeakerStereo : kSpeakerMono;
    format.SubFormat = kSubFormatIeeeFloat;
}

} // namespace saf::win
