// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plays the filtered mix into a muted session of the default output device.
//
// Why a muted session (see output_sink.h): Discord is pointed at this process
// as the source of the screen share audio and reads what this process hands to
// Windows before the session volume is applied. Muting the session keeps the
// user from hearing every transmitted application twice. If somebody unmutes
// the session in the volume mixer, it is muted again within half a second.
//
// The output device clocks the mix (Engine::pull), so there is no second clock
// to drift against. If the device disappears (unplugged, driver restart), the
// stream is opened again on the current default device.

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

#include "../engine.h"
#include "../output_sink.h"
#include "render_sink.h"
#include "audio_subformat.h"
#include "com_ptr.h"
#include "windows_backend.h"

namespace saf {

std::unique_ptr<IOutputSink> createNullSink();

bool discordHookLoaded() {
    // Discord injects DiscordHook64.dll (DiscordHook.dll for 32 bit targets)
    // into the process it takes the screen share audio from.
    return GetModuleHandleW(L"DiscordHook64.dll") != nullptr || GetModuleHandleW(L"DiscordHook.dll") != nullptr;
}

namespace {

using win::ComPtr;
using win::makeFloatFormat;
using win::formatHresult;
using win::toUtf8;

constexpr REFERENCE_TIME kBufferDuration = 300000;  // 30 ms
// PKEY_Device_FriendlyName, declared here instead of pulling in
// functiondiscoverykeys_devpkey.h (which does not build with WIN32_LEAN_AND_MEAN).
const PROPERTYKEY kDeviceFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
constexpr wchar_t kSessionName[] = L"StreamAudioFilter: share audio (muted here)";

} // namespace

RenderSink::~RenderSink() { stop(); }

bool RenderSink::start(Engine& engine, std::string& error) {
    return startWithSource([&engine](float* out, size_t frames) { engine.pull(out, frames); }, error);
}

bool RenderSink::startWithSource(Source source, std::string&) {
    if (running_.exchange(true)) return true;
    source_ = std::move(source);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

    void RenderSink::stop() {
        running_ = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (wake_) SetEvent(wake_);
        }
        if (thread_.joinable()) thread_.join();
    }

    OutputStatus RenderSink::status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        OutputStatus status = status_;
        status.running = running_;
        status.kind = "wasapi";
        status.discordAttached = discordHookLoaded();
        return status;
    }

    void RenderSink::setError(const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.error = error;
    }

    void RenderSink::threadMain() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comOwned = SUCCEEDED(com);
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wake_ = wake;
        }

        bool first = true;
        while (running_) {
            if (!first) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.restarts++;
            }
            first = false;
            const bool stoppedCleanly = run(wake);
            if (!running_ || stoppedCleanly) break;
            // The device went away or refused to open: retry on the (new)
            // default device after a short pause.
            WaitForSingleObject(wake, 1000);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            wake_ = nullptr;
            status_.sessionMuted = false;
        }
        if (wake) CloseHandle(wake);
        if (comOwned) CoUninitialize();
    }

    /** One device lifetime. True: stopped on request; false: retry. */
    bool RenderSink::run(HANDLE wake) {
        HRESULT hr = S_OK;
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())))) {
            setError(formatHresult(hr, "the audio device list is unavailable"));
            return false;
        }
        ComPtr<IMMDevice> device;
        if (FAILED(hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) {
            setError(formatHresult(hr, "there is no default output device to play the share audio on"));
            return false;
        }

        std::string deviceName;
        {
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR) {
                    deviceName = toUtf8(value.pwszVal);
                }
                PropVariantClear(&value);
            }
        }

        ComPtr<IAudioClient> client;
        if (FAILED(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()))) {
            setError(formatHresult(hr, "the output device could not be opened"));
            return false;
        }

        WAVEFORMATEXTENSIBLE format;
        makeFloatFormat(format, 48000, 2);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, reinterpret_cast<WAVEFORMATEX*>(&format), nullptr))) {
            setError(formatHresult(hr, "the output stream could not be initialised"));
            return false;
        }

        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ready) {
            setError("the output event could not be created");
            return false;
        }
        struct EventGuard {
            HANDLE handle;
            ~EventGuard() { CloseHandle(handle); }
        } readyGuard { ready };

        ComPtr<IAudioRenderClient> render;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioSessionControl> session;
        if (FAILED(hr = client->SetEventHandle(ready)) || FAILED(hr = client->GetService(IID_PPV_ARGS(render.put())))
            || FAILED(hr = client->GetService(IID_PPV_ARGS(volume.put())))) {
            setError(formatHresult(hr, "the output stream is incomplete"));
            return false;
        }
        // Muted *before* the first sample is played: nothing is ever audible.
        if (FAILED(hr = volume->SetMute(TRUE, nullptr))) {
            setError(formatHresult(hr, "the output session could not be muted, so it is not started (you would hear the audio twice)"));
            return false;
        }
        if (SUCCEEDED(client->GetService(IID_PPV_ARGS(session.put())))) session->SetDisplayName(kSessionName, nullptr);

        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);
        {
            BYTE* data = nullptr;
            if (SUCCEEDED(render->GetBuffer(bufferFrames, &data))) render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (FAILED(hr = client->Start())) {
            setError(formatHresult(hr, "the output stream could not be started"));
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.device = deviceName;
            status_.format = "48000 Hz / 2 ch / f32, " + std::to_string(bufferFrames * 1000 / 48000) + " ms buffer";
            status_.error.clear();
            status_.sessionMuted = true;
        }

        HANDLE waits[2] = { ready, wake };
        float windowPeak = 0.0f;
        uint64_t windowFrames = 0;
        ULONGLONG lastCheck = GetTickCount64();
        bool clean = true;

        while (running_) {
            const DWORD result = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (!running_) break;
            if (result == WAIT_FAILED) {
                setError("waiting for the output device failed");
                clean = false;
                break;
            }

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (SUCCEEDED(hr) && padding < bufferFrames) {
                const UINT32 frames = bufferFrames - padding;
                BYTE* data = nullptr;
                hr = render->GetBuffer(frames, &data);
                if (SUCCEEDED(hr)) {
                    float* samples = reinterpret_cast<float*>(data);
                    source_(samples, frames);
                    for (UINT32 i = 0; i < frames * 2; i++) windowPeak = std::max(windowPeak, std::fabs(samples[i]));
                    hr = render->ReleaseBuffer(frames, 0);
                    windowFrames += frames;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.renderedFrames += frames;
                    if (windowFrames >= 48000) {
                        status_.peak = windowPeak;
                        windowPeak = 0.0f;
                        windowFrames = 0;
                    }
                }
            }
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                setError(formatHresult(hr, "the output device went away; reopening on the default device"));
                clean = false;
                break;
            }

            const ULONGLONG now = GetTickCount64();
            if (now - lastCheck >= 500) {
                lastCheck = now;
                BOOL muted = FALSE;
                if (SUCCEEDED(volume->GetMute(&muted)) && !muted) {
                    volume->SetMute(TRUE, nullptr);
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.remutes++;
                }
            }
        }

        client->Stop();
        return clean;
    }


std::unique_ptr<IOutputSink> createRemoteSink();

std::unique_ptr<IOutputSink> createOutputSink(OutputKind kind, std::string&) {
    if (kind == OutputKind::Null) return createNullSink();
    if (kind == OutputKind::InProcess) return std::make_unique<RenderSink>();
    return createRemoteSink();
}

} // namespace saf
