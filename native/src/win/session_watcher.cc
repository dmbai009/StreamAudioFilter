// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Watches which applications are playing audio.
//
// Implementation notes:
//  * All audio sessions of all ACTIVE render endpoints are aggregated. An
//    application routed to a second device (Voicemeeter, USB headset, HDMI)
//    would be invisible if only the default endpoint was enumerated.
//  * Session creation is event driven (IAudioSessionNotification). State
//    changes and closed sessions are picked up by a periodic refresh, so the
//    plugin never scans the audio engine at a high rate.
//  * Everything runs on one dedicated MTA thread, because the audio session
//    callbacks are delivered on the COM apartment that registered them.

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../app_identity.h"
#include "audio_subformat.h"
#include "com_iids.h"
#include "com_ptr.h"
#include "win_internal.h"
#include "windows_backend.h"

namespace saf::win {

namespace {

constexpr int kRefreshIntervalMs = 1000;
constexpr int kPollIntervalMs = 250;

class SessionEnumerator : public IAppEnumerator {
public:
    ~SessionEnumerator() override { stop(); }

    bool start(std::string& error) override {
        if (running_.exchange(true)) return true;

        std::promise<std::string> ready;
        auto future = ready.get_future();
        const int generation = ++generation_;
        thread_ = std::thread([this, ready = std::move(ready), generation]() mutable {
            threadMain(std::move(ready), generation);
        });

        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            error = "timeout while initialising the audio session enumeration";
            stop();
            return false;
        }
        error = future.get();
        if (!error.empty()) {
            stop();
            return false;
        }
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) {
            join();
            return;
        }
        join();

        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        processes_.clear();
    }

    std::vector<SessionInfo> enumerate() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_;
    }

    std::vector<ProcessNode> processes() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return processes_;
    }

    bool consumeChangeFlag() override { return changed_.exchange(false); }

    std::string describe() const override {
        return "WASAPI (IAudioSessionManager2 over every active output device)";
    }

private:
    /** Session creation is the one event we must not miss: mode A depends on it. */
    class SessionNotification : public ComCallback<IAudioSessionNotification, &IID_IAudioSessionNotification> {
    public:
        explicit SessionNotification(std::atomic<bool>* changed) : changed_(changed) {}

        HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
            changed_->store(true);
            return S_OK;
        }

    private:
        std::atomic<bool>* changed_;
    };

    void join() {
        if (thread_.joinable()) thread_.join();
    }

    void threadMain(std::promise<std::string> ready, int generation) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comOwned = SUCCEEDED(comResult);
        const HRESULT expected = comResult == RPC_E_CHANGED_MODE ? S_OK : comResult;

        std::string error;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(deviceEnumerator_.put()));
        if (FAILED(expected)) {
            error = formatHresult(expected, "could not initialise COM");
        } else if (FAILED(hr)) {
            error = formatHresult(hr, "could not create MMDeviceEnumerator");
        } else {
            notification_ = ComPtr<SessionNotification>(new SessionNotification(&changed_));
            refresh(error);
        }

        ready.set_value(error);
        if (!error.empty()) {
            if (comOwned) CoUninitialize();
            return;
        }

        auto lastRefresh = std::chrono::steady_clock::now();
        while (running_ && generation == generation_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
            if (std::chrono::steady_clock::now() - lastRefresh >= std::chrono::milliseconds(kRefreshIntervalMs)) {
                std::string refreshError;
                refresh(refreshError);
                lastRefresh = std::chrono::steady_clock::now();
            }
        }

        if (defaultManager_) defaultManager_->UnregisterSessionNotification(notification_.get());
        defaultManager_.reset();
        deviceEnumerator_.reset();
        if (comOwned) CoUninitialize();
    }

    void collectEndpoint(IAudioSessionManager2* manager, std::vector<SessionInfo>& out) const {
        ComPtr<IAudioSessionEnumerator> enumerator;
        if (FAILED(manager->GetSessionEnumerator(enumerator.put())) || !enumerator) return;

        int count = 0;
        if (FAILED(enumerator->GetCount(&count))) return;

        for (int i = 0; i < count; i++) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(enumerator->GetSession(i, control.put())) || !control) continue;

            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control->QueryInterface(IID_PPV_ARGS(control2.put()))) || !control2) continue;

            SessionInfo info;
            DWORD pid = 0;
            control2->GetProcessId(&pid);
            info.pid = pid;
            info.isSystemSounds = control2->IsSystemSoundsSession() == S_OK;

            AudioSessionState state = AudioSessionStateInactive;
            control2->GetState(&state);
            info.playing = state == AudioSessionStateActive;

            // Per session peak meter, when the session exposes one. Optional:
            // captured applications report their real level anyway, and the UI
            // falls back to the session state.
            ComPtr<IAudioMeterInformation> meter;
            if (SUCCEEDED(control2->QueryInterface(IID_PPV_ARGS(meter.put()))) && meter) {
                float peak = 0.0f;
                if (SUCCEEDED(meter->GetPeakValue(&peak))) info.peak = peak;
            }

            LPWSTR sessionId = nullptr;
            if (SUCCEEDED(control2->GetSessionIdentifier(&sessionId)) && sessionId) {
                info.sessionId = toUtf8(sessionId);
                CoTaskMemFree(sessionId);
            }
            if (info.sessionId.empty()) info.sessionId = std::to_string(pid) + ":" + std::to_string(i);

            if (info.isSystemSounds) {
                info.displayName = "System sounds";
                info.exeName = "system";
            } else {
                info.exePath = processImagePath(pid);
                info.exeName = fileNameOf(info.exePath);
                if (info.exeName.empty()) info.exeName = "pid " + std::to_string(pid);
                info.displayName = fileDescription(info.exePath);
                if (info.displayName.empty()) {
                    info.displayName = info.exeName;
                    const size_t dot = info.displayName.find_last_of('.');
                    if (dot != std::string::npos) info.displayName = info.displayName.substr(0, dot);
                }
            }

            const std::string aumid = info.isSystemSounds ? std::string() : applicationUserModelId(pid);
            info.isUwp = aumid.find('!') != std::string::npos;
            info.appKey = makeAppKey(info.exePath, info.exeName, aumid, info.isSystemSounds, pid);

            if (info.exePath.empty() && aumid.empty() && !info.isSystemSounds) {
                info.notCapturableReason = "the executable of the process could not be determined";
            }

            out.push_back(std::move(info));
        }
    }

    void refresh(std::string& error) {
        // Notifications are registered on the default endpoint: that is where
        // applications start playing in practice, and one registration is enough
        // to learn about new sessions immediately.
        std::string defaultId;
        {
            ComPtr<IMMDevice> defaultDevice;
            if (SUCCEEDED(deviceEnumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, defaultDevice.put())) && defaultDevice) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
                    defaultId = toUtf8(id);
                    CoTaskMemFree(id);
                }
                if (defaultId != defaultEndpointId_) {
                    ComPtr<IAudioSessionManager2> manager;
                    if (SUCCEEDED(defaultDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid())) && manager) {
                        if (defaultManager_ && notification_) defaultManager_->UnregisterSessionNotification(notification_.get());
                        defaultManager_ = std::move(manager);
                        defaultEndpointId_ = defaultId;
                        if (notification_) defaultManager_->RegisterSessionNotification(notification_.get());
                    }
                }
            }
        }

        ComPtr<IMMDeviceCollection> devices;
        HRESULT hr = deviceEnumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put());
        if (FAILED(hr)) {
            error = formatHresult(hr, "could not enumerate the output devices");
            return;
        }

        UINT deviceCount = 0;
        if (FAILED(devices->GetCount(&deviceCount))) return;

        std::vector<SessionInfo> collected;
        for (UINT i = 0; i < deviceCount; i++) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(i, device.put())) || !device) continue;

            ComPtr<IAudioSessionManager2> manager;
            if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid())) || !manager) continue;

            collectEndpoint(manager.get(), collected);
        }

        // The same session can be enumerated on several endpoints (for example
        // when an application is mirrored to two devices): keep one entry per
        // session id and merge the pids.
        std::vector<SessionInfo> unique;
        std::set<std::string> seen;
        for (auto& session : collected) {
            if (!seen.insert(session.sessionId).second) continue;
            unique.push_back(std::move(session));
        }

        const std::vector<ProcessNode> processes = processTable();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::set<std::string> before, after;
            for (const auto& session : sessions_) before.insert(session.sessionId);
            for (const auto& session : unique) after.insert(session.sessionId);
            if (before != after) changed_ = true;
            sessions_ = std::move(unique);
            processes_ = processes;
        }
    }

    std::atomic<bool> running_ { false };
    std::atomic<bool> changed_ { false };
    std::atomic<int> generation_ { 0 };
    std::thread thread_;

    mutable std::mutex mutex_;
    std::vector<SessionInfo> sessions_;
    std::vector<ProcessNode> processes_;

    // Only touched from the worker thread.
    ComPtr<IMMDeviceEnumerator> deviceEnumerator_;
    ComPtr<IAudioSessionManager2> defaultManager_;
    ComPtr<SessionNotification> notification_;
    std::string defaultEndpointId_;
};

} // namespace

std::unique_ptr<IAppEnumerator> createSessionEnumerator(std::string& error) {
    auto enumerator = std::make_unique<SessionEnumerator>();
    if (!enumerator->start(error)) return nullptr;
    return enumerator;
}

} // namespace saf::win
