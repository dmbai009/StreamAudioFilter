// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Backend selection. On Windows the real WASAPI backend is used, everywhere
// else (and in test mode) the mock backends keep the plugin usable for
// validating the UI and the audio pipeline.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "capture.h"

namespace saf {

enum class BackendKind {
    Auto = 0,
    Wasapi,
    Mock,
};

BackendKind parseBackendKind(const std::string& value);

std::unique_ptr<IAppEnumerator> createEnumerator(BackendKind kind, std::string& error);
std::unique_ptr<ICaptureFactory> createCaptureFactory(BackendKind kind, std::string& error);

/** Capability probe: is per application capture possible on this machine? */
struct SupportInfo {
    bool supported = false;
    std::string os;
    uint32_t build = 0;
    std::string reason;
    std::string detail;
    std::vector<std::string> checks;
};

SupportInfo probeSupport();

/** Describes the OS in one line, for diagnostics. */
std::string describePlatform();

} // namespace saf
