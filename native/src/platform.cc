// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.h"

#include <algorithm>
#include <cctype>

#include "capture_group.h"
#include "mock/mock_backend.h"

#if defined(_WIN32)
#    include "win/windows_backend.h"
#endif

namespace saf {

BackendKind parseBackendKind(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char c : value) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lowered == "mock" || lowered == "test") return BackendKind::Mock;
    if (lowered == "wasapi") return BackendKind::Wasapi;
    return BackendKind::Auto;
}

std::unique_ptr<IAppEnumerator> createEnumerator(BackendKind kind, std::string& error) {
    if (kind == BackendKind::Mock) return std::make_unique<mock::MockEnumerator>();

#if defined(_WIN32)
    if (kind == BackendKind::Auto || kind == BackendKind::Wasapi) return win::createSessionEnumerator(error);
#else
    if (kind == BackendKind::Wasapi) {
        error = "WASAPI is only available on Windows";
        return nullptr;
    }
#endif

    error = "native capture is only available on Windows — test mode is on";
    return std::make_unique<mock::MockEnumerator>();
}

std::unique_ptr<ICaptureFactory> createCaptureFactory(BackendKind kind, std::string& error) {
    if (kind == BackendKind::Mock) {
        return std::make_unique<GroupingCaptureFactory>(std::make_unique<mock::MockCaptureFactory>());
    }

#if defined(_WIN32)
    if (kind == BackendKind::Auto || kind == BackendKind::Wasapi) {
        auto inner = win::createCaptureFactory(error);
        if (!inner) return nullptr;
        // One application can be started twice: the grouping factory captures
        // each of its process trees and mixes them into the one stream the
        // engine, the UI and the stream limit expect.
        return std::make_unique<GroupingCaptureFactory>(std::move(inner));
    }
#else
    if (kind == BackendKind::Wasapi) {
        error = "WASAPI is only available on Windows";
        return nullptr;
    }
#endif

    error = "native capture is only available on Windows — test mode is on";
    return std::make_unique<GroupingCaptureFactory>(std::make_unique<mock::MockCaptureFactory>());
}

#if defined(_WIN32)

SupportInfo probeSupport() { return win::probeSupport(); }

std::string describePlatform() { return win::describePlatform(); }

#else

SupportInfo probeSupport() {
    SupportInfo info;
    info.supported = false;
    info.os = describePlatform();
    info.reason = "Capturing the audio of individual applications only works on Windows (it needs WASAPI process loopback).";
    info.detail = "Test mode is active: the audio is produced by a signal generator.";
    info.checks.push_back("The platform is not Windows: WASAPI is unavailable");
    info.checks.push_back("Test mode is always available (it only exercises the interface)");
    return info;
}

std::string describePlatform() {
#    if defined(__APPLE__)
    return "macOS (test mode)";
#    elif defined(__linux__)
    return "Linux (test mode)";
#    else
    return "not Windows (test mode)";
#    endif
}

#endif

} // namespace saf
