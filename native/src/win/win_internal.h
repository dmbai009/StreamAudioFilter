// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal helpers shared by the Windows backend translation units.

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "../capture.h"

namespace saf::win {

/** Converts between UTF-16 and UTF-8. */
std::string toUtf8(const std::wstring& value);
std::wstring toWide(const std::string& value);

/** "Google Chrome" from the version resource, empty when unavailable. */
std::string fileDescription(const std::string& path);

/** Full path of a process image, empty when not readable. */
std::string processImagePath(uint32_t pid);

/** Application user model id (UWP), empty for classic Win32 apps. */
std::string applicationUserModelId(uint32_t pid);

std::string fileNameOf(const std::string& path);

/**
 * Mix format of the default render endpoint. Used as the fallback capture
 * format when the audio engine refuses our preferred 48 kHz float format.
 */
bool defaultMixFormat(WAVEFORMATEX** format);

/**
 * Performs a real process loopback activation of our own process tree.
 *
 * @param detail human readable outcome, including the HRESULT on failure
 * @returns true when this OS build supports per application capture
 */
bool probeProcessLoopback(std::string& detail);

} // namespace saf::win
