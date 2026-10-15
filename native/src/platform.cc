// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.h"

#include <algorithm>
#include <cctype>

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
        error = "WASAPI доступен только в Windows";
        return nullptr;
    }
#endif

    error = "нативный захват доступен только в Windows — включён тестовый режим";
    return std::make_unique<mock::MockEnumerator>();
}

std::unique_ptr<ICaptureFactory> createCaptureFactory(BackendKind kind, std::string& error) {
    if (kind == BackendKind::Mock) return std::make_unique<mock::MockCaptureFactory>();

#if defined(_WIN32)
    if (kind == BackendKind::Auto || kind == BackendKind::Wasapi) return win::createCaptureFactory(error);
#else
    if (kind == BackendKind::Wasapi) {
        error = "WASAPI доступен только в Windows";
        return nullptr;
    }
#endif

    error = "нативный захват доступен только в Windows — включён тестовый режим";
    return std::make_unique<mock::MockCaptureFactory>();
}

#if defined(_WIN32)

SupportInfo probeSupport() { return win::probeSupport(); }

std::string describePlatform() { return win::describePlatform(); }

#else

SupportInfo probeSupport() {
    SupportInfo info;
    info.supported = false;
    info.os = describePlatform();
    info.reason = "Фильтрация по приложениям возможна только в Windows 10 (сборка 20348+) и Windows 11.";
    info.detail = "Сейчас работает тестовый режим: звук генерируется сигнал-генератором.";
    info.checks.push_back("Платформа не Windows: WASAPI недоступен");
    info.checks.push_back("Тестовый режим доступен всегда (нужен только для проверки интерфейса)");
    return info;
}

std::string describePlatform() {
#    if defined(__APPLE__)
    return "macOS (тестовый режим)";
#    elif defined(__linux__)
    return "Linux (тестовый режим)";
#    else
    return "не Windows (тестовый режим)";
#    endif
}

#endif

} // namespace saf
