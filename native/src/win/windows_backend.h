// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows backend.
//
//  * Session watching: IAudioSessionManager2 over ALL active render endpoints,
//    event driven (OnSessionCreated) plus a periodic reconcile.
//  * Capture: WASAPI process loopback
//    (ActivateAudioInterfaceAsync + AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
//    with PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE).
//  * Capability probe: actually performs an activation so an unsupported OS
//    build is demonstrated with the real HRESULT instead of a guess.
//
// Everything is endpoint independent: the capture follows the process, not the
// device, which is why it also works when the user routes audio through
// Voicemeeter or a USB headset.

#pragma once

#include <string>
#include <vector>

#include "../capture.h"
#include "../platform.h"

namespace saf::win {

std::string toUtf8(const std::wstring& value);
std::wstring toWide(const std::string& value);
std::string hresultName(HRESULT hr);
std::string hresultMessage(HRESULT hr);
std::string formatHresult(HRESULT hr, const std::string& operation);

struct OsVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
    std::wstring productName;
};

OsVersion osVersion();

/** Full process table (pid + parent pid) via a single Toolhelp snapshot. */
std::vector<ProcessNode> processTable();

std::unique_ptr<IAppEnumerator> createSessionEnumerator(std::string& error);
std::unique_ptr<ICaptureFactory> createCaptureFactory(std::string& error);

/**
 * Tries a real process loopback activation on our own process tree and returns
 * a human readable result. This is the empirical proof whether the OS build
 * supports what the plugin needs.
 */
SupportInfo probeSupport();

std::string describePlatform();

} // namespace saf::win
