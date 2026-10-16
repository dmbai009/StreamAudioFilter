// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WASAPI process loopback capture — the heart of the plugin.
//
// One capture client per transmitted application, created with
// ActivateAudioInterfaceAsync() + AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
// in PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE mode, which captures the
// given process and all of its child processes (Chrome, Chromium games with a
// separate audio process, ...).
//
// Requirements this file fulfils:
//  * no virtual audio device is created and no device is changed,
//  * the capture is endpoint independent (works with Voicemeeter, USB, HDMI),
//  * no administrator rights are required,
//  * the format negotiation asks the audio engine to convert for us
//    (AUTOCONVERTPCM + SRC_DEFAULT_QUALITY); the built in resampler is only
//    used if the engine refuses the requested format.
//
// The whole capture runs on its own MTA thread: process loopback activation is
// asynchronous and calls back on a COM thread, which is exactly why we control
// that thread ourselves instead of relying on the host's apartment.

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../dsp/convert.h"
#include "../hr_advice.h"
#include "../dsp/resampler.h"
#include "audio_subformat.h"
#include "com_iids.h"
#include "com_ptr.h"
#include "win_internal.h"
#include "windows_backend.h"

namespace saf::win {

namespace {

constexpr REFERENCE_TIME kBufferDuration = 200000;  // 20 ms
constexpr int kStartTimeoutMs = 8000;

/** Signals the manual reset event used to wait for the async activation. */
class ActivationHandler : public ComCallback<IActivateAudioInterfaceCompletionHandler, &IID_IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ActivationHandler(HANDLE completed) : completed_(completed) {}

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(completed_);
        return S_OK;
    }

private:
    HANDLE completed_;
};

AudioFormat audioFormatFrom(const WAVEFORMATEX* format) {
    AudioFormat result;
    result.sampleRate = format->nSamplesPerSec;
    result.channels = format->nChannels;
    result.validBits = format->wBitsPerSample;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (sameGuid(extended->SubFormat, kSubFormatIeeeFloat)) {
            result.sample = SampleFormat::IeeeFloat32;
            return result;
        }
        if (sameGuid(extended->SubFormat, kSubFormatPcm)) {
            if (format->wBitsPerSample == 16) result.sample = SampleFormat::PcmS16;
            else if (format->wBitsPerSample == 24) result.sample = SampleFormat::PcmS24;
            else if (format->wBitsPerSample == 32) result.sample = SampleFormat::PcmS32;
            else result.sample = SampleFormat::PcmU8;
            return result;
        }
        result.sample = SampleFormat::Unknown;
        return result;
    }

    switch (format->wFormatTag) {
        case WAVE_FORMAT_IEEE_FLOAT: result.sample = SampleFormat::IeeeFloat32; break;
        case WAVE_FORMAT_PCM:
            if (format->wBitsPerSample == 16) result.sample = SampleFormat::PcmS16;
            else if (format->wBitsPerSample == 24) result.sample = SampleFormat::PcmS24;
            else if (format->wBitsPerSample == 32) result.sample = SampleFormat::PcmS32;
            else result.sample = SampleFormat::PcmU8;
            break;
        default: result.sample = SampleFormat::Unknown; break;
    }
    return result;
}

std::string describeFormat(const WAVEFORMATEX* format) {
    if (!format) return "unknown format";
    const AudioFormat parsed = audioFormatFrom(format);
    return std::to_string(parsed.sampleRate) + " Hz / " + std::to_string(parsed.channels) + " ch / "
        + std::string(toString(parsed.sample));
}

/**
 * Captures exactly one process tree and delivers interleaved float32 stereo at
 * the requested sample rate.
 *
 * Windows' process loopback activation takes a single TargetProcessId, so this
 * class handles one root process: request.rootPids.front(). An application
 * started twice has several roots; GroupingCaptureFactory (portable, see
 * capture_group.h) splits the request and sums the resulting captures, so no
 * tree is captured twice and no instance is left out.
 */
class ProcessLoopbackCapture : public ICaptureSource {
public:
    explicit ProcessLoopbackCapture(CaptureRequest request, std::string note = {})
        : request_(std::move(request))
        , buffer_(static_cast<size_t>(request_.sampleRate) / 2, static_cast<size_t>(request_.channels))
        , note_(std::move(note)) {}

    ~ProcessLoopbackCapture() override { stop(); }

    bool start(std::string& error) override {
        if (running_.exchange(true)) {
            error = "the capture is already running";
            return false;
        }

        std::promise<std::string> ready;
        auto future = ready.get_future();
        thread_ = std::thread([this, ready = std::move(ready)]() mutable { threadMain(std::move(ready)); });

        if (future.wait_for(std::chrono::milliseconds(kStartTimeoutMs)) != std::future_status::ready) {
            error = "timeout while starting the capture of " + request_.displayName;
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
        running_ = false;
        {
            // The handles are created by the capture thread: read them under
            // the same lock it uses.
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopEvent_) SetEvent(stopEvent_);
            if (captureEvent_) SetEvent(captureEvent_);
        }
        if (thread_.joinable()) thread_.join();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopEvent_) {
                CloseHandle(stopEvent_);
                stopEvent_ = nullptr;
            }
            if (captureEvent_) {
                CloseHandle(captureEvent_);
                captureEvent_ = nullptr;
            }
        }
    }

    FloatRingBuffer* buffer() override { return &buffer_; }

    std::string formatDescription() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }

    int latencyMs() const override { return latencyMs_; }

    bool alive() const override { return !died_; }

    float inputPeak() const override { return inputPeak_.load(); }

    std::string lastError() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_.empty()) return note_;
        return note_.empty() ? error_ : error_ + " (" + note_ + ")";
    }

private:
    void setError(const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error;
    }

    /**
     * ActivateAudioInterfaceAsync with the process loopback activation params.
     *
     * The four ways this can fail are kept apart on purpose, because they mean
     * completely different things (and only two of them are about the OS):
     *   1. [activation-call]     the call itself was refused. E_ILLEGAL_METHOD_CALL
     *                            (0x8000000E) belongs here and means the completion
     *                            handler was not usable from the calling apartment
     *                            — it is not a statement about the Windows build.
     *   2. [activation-wait]     the call was accepted but the callback never came.
     *   3. [activate-result]     GetActivateResult failed, or the activation it
     *                            reports failed (two different HRESULTs).
     *   4. [interface]           the activated object has no IAudioClient.
     * Every one of them lands in `error` with its stage, its HRESULT name, the
     * numeric value and what the code means.
     */
    HRESULT activate(ComPtr<IAudioClient>& client, std::string& error) {
        AUDIOCLIENT_ACTIVATION_PARAMS params = {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = targetPid_;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activateParams = {};
        activateParams.vt = VT_BLOB;
        activateParams.blob.cbSize = sizeof(params);
        activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!completed) {
            error = "[activation-call] the synchronisation event of the activation could not be created";
            return E_FAIL;
        }

        // Declaration order is destruction order in reverse: the operation is
        // released before the handler. Windows keeps a reference to the handler
        // until the operation is gone, and the callback must not run into a
        // destroyed object.
        ComPtr<ActivationHandler> handler(new ActivationHandler(completed));
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;

        const HRESULT callHr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, handler.get(), operation.put());

        if (FAILED(callHr)) {
            error = "[activation-call] " + formatHresult(callHr, "ActivateAudioInterfaceAsync was refused") + "\n"
                + "  the request was rejected before anything was activated";
            if (callHr == hr::IllegalMethodCall) {
                error += handler->isAgile()
                    ? "\n  this handler is an agile object, so the calling apartment is the remaining candidate"
                    : "\n  this handler is not an agile object (IAgileObject) — that is the documented cause";
            }
            CloseHandle(completed);
            return callHr;
        }

        if (WaitForSingleObject(completed, kStartTimeoutMs) != WAIT_OBJECT_0) {
            error = "[activation-wait] the activation callback did not fire within " + std::to_string(kStartTimeoutMs)
                + " ms (the call itself was accepted)";
            CloseHandle(completed);
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        HRESULT activateResult = E_FAIL;
        ComPtr<IUnknown> unknown;
        const HRESULT resultHr = operation->GetActivateResult(&activateResult, unknown.put());
        if (FAILED(resultHr)) {
            error = "[activate-result] "
                + formatHresult(resultHr, "GetActivateResult failed") + "\n"
                + "  the asynchronous operation never produced a result";
            CloseHandle(completed);
            return resultHr;
        }

        if (FAILED(activateResult)) {
            error = "[activate-result] "
                + formatHresult(activateResult, "the activation of process " + std::to_string(targetPid_) + " was refused")
                + "\n  the call succeeded and the callback fired: the audio engine itself rejected the activation";
            if (activateResult == E_INVALIDARG || activateResult == E_NOTIMPL) {
                error += "\n  (rejected activation parameters look exactly like this; Windows 10 version 2004 / build "
                         "19041 and newer do implement per process capture in practice, so the exact code matters "
                         "more than the build number)";
            }
            CloseHandle(completed);
            return activateResult;
        }

        if (!unknown) {
            error = "[activate-result] the activation reported success but returned no object";
            CloseHandle(completed);
            return E_UNEXPECTED;
        }

        const HRESULT qiHr = unknown->QueryInterface(IID_PPV_ARGS(client.put()));
        if (FAILED(qiHr) || !client) {
            error = "[interface] " + formatHresult(qiHr, "QueryInterface(IAudioClient) failed") + "\n"
                + "  the activated object is not an audio client";
            CloseHandle(completed);
            return qiHr;
        }

        CloseHandle(completed);
        return S_OK;
    }

    /** Initializes the client, negotiating the format the engine will deliver. */
    HRESULT initialize(IAudioClient* client, ComPtr<IAudioCaptureClient>& captureClient, std::string& error) {
        // 1. Preferred: let the audio engine convert to 48 kHz float stereo.
        WAVEFORMATEXTENSIBLE preferred;
        makeFloatFormat(preferred, request_.sampleRate, request_.channels);

        // Process loopback clients refuse AUDCLNT_STREAMFLAGS_NOPERSIST; the
        // level the capture is scaled with is reset by resetOwnSessionLevels().
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0,
            reinterpret_cast<WAVEFORMATEX*>(&preferred), nullptr);

        if (FAILED(hr)) {
            // 2. Fallback: whatever the engine uses natively, converted by us.
            WAVEFORMATEX* mixFormat = nullptr;
            if (defaultMixFormat(&mixFormat) && mixFormat) {
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, mixFormat, nullptr);
                if (SUCCEEDED(hr)) {
                    copyMixFormat(mixFormat);
                }
                CoTaskMemFree(mixFormat);
            }
        }

        if (FAILED(hr)) {
            error = "[client-init] " + formatHresult(hr, "the audio client refused to initialise for " + request_.displayName)
                + "\n  neither the requested format nor the endpoint mix format was accepted";
            return hr;
        }

        if (FAILED(hr = client->GetService(IID_PPV_ARGS(captureClient.put())))) {
            error = "[capture-client] " + formatHresult(hr, "IAudioCaptureClient is not available");
            return hr;
        }


        UINT32 bufferFrames = 0;
        // Process loopback clients report nonsense buffer sizes (seen: billions
        // of frames); only a plausible value is shown as a latency.
        if (SUCCEEDED(client->GetBufferSize(&bufferFrames)) && request_.sampleRate > 0 && bufferFrames <= static_cast<UINT32>(request_.sampleRate)) {
            latencyMs_ = static_cast<int>(bufferFrames * 1000 / static_cast<unsigned>(request_.sampleRate));
        }
        return S_OK;
    }

    void copyMixFormat(const WAVEFORMATEX* format) {
        captured = audioFormatFrom(format);
        // AudioFormat stores unsigned counts, the capture request uses ints.
        const uint32_t requestRate = static_cast<uint32_t>(request_.sampleRate);
        const uint16_t requestChannels = static_cast<uint16_t>(request_.channels);

        needsConversion_ = !(captured.channels == requestChannels && captured.sampleRate == requestRate
            && captured.sample == SampleFormat::IeeeFloat32);

        if (captured.sampleRate != requestRate) {
            resampler_ = std::make_unique<SincResampler>(static_cast<int>(captured.sampleRate), request_.sampleRate, request_.channels);
        }

        // Buffers for the conversion path.
        const size_t frames = 4096;
        decodeBuffer_.assign(frames * captured.channels, 0.0f);
        stereoBuffer_.assign(resampler_ ? resampler_->maxOutputFor(frames) * request_.channels : frames * request_.channels, 0.0f);

        std::lock_guard<std::mutex> lock(mutex_);
        format_ = describeFormat(format) + (needsConversion_ ? " (converted by the plugin)" : " (no conversion)");
    }

    void setFormatDirect() {
        std::lock_guard<std::mutex> lock(mutex_);
        format_ = std::to_string(request_.sampleRate) + " Hz / " + std::to_string(request_.channels)
            + " ch / f32 (straight from the audio engine)";
    }

    /** Converts an engine buffer and appends it to the ring buffer. */
    void push(const BYTE* data, UINT32 frames, bool silent) {
        if (!silent && data && !needsConversion_) {
            const float* samples = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames * static_cast<UINT32>(request_.channels); i++) windowPeak_ = std::max(windowPeak_, std::fabs(samples[i]));
        }
        windowFrames_ += frames;
        if (windowFrames_ >= static_cast<uint32_t>(request_.sampleRate / 2)) {
            inputPeak_ = windowPeak_;
            windowPeak_ = 0.0f;
            windowFrames_ = 0;
        }
        if (silent || !data) {
            pushSilence(frames);
            return;
        }

        if (!needsConversion_) {
            buffer_.write(reinterpret_cast<const float*>(data), frames);
            return;
        }

        const auto* source = reinterpret_cast<const uint8_t*>(data);
        size_t remaining = frames;
        while (remaining > 0) {
            const size_t chunk = std::min(remaining, decodeBuffer_.size() / captured.channels);
            decodeToFloat(source, chunk, captured, decodeBuffer_.data());
            source += chunk * captured.frameBytes();

            if (resampler_) {
                const size_t produced = resampler_->process(decodeBuffer_.data(), chunk, stereoBuffer_.data(),
                    stereoBuffer_.size() / request_.channels);
                buffer_.write(stereoBuffer_.data(), produced);
            } else {
                // Same rate, different format or channel count.
                if (captured.channels == static_cast<uint16_t>(request_.channels)) {
                    buffer_.write(decodeBuffer_.data(), chunk);
                } else {
                    mapChannels(decodeBuffer_.data(), chunk, captured.channels, stereoBuffer_.data(), request_.channels);
                    buffer_.write(stereoBuffer_.data(), chunk);
                }
            }
            remaining -= chunk;
        }
    }

    void pushSilence(UINT32 frames) {
        silence_.assign(static_cast<size_t>(frames) * request_.channels, 0.0f);
        buffer_.write(silence_.data(), frames);
    }

    void threadMain(std::promise<std::string> ready);

    CaptureRequest request_;
    uint32_t targetPid_ = 0;
    FloatRingBuffer buffer_;
    std::thread thread_;
    std::atomic<bool> running_ { false };
    /** The capture thread ended on its own (error), not through stop(). */
    std::atomic<bool> died_ { false };
    /** Peak of the last ~0.5 s of captured data, before any conversion. */
    std::atomic<float> inputPeak_ { 0.0f };
    float windowPeak_ = 0.0f;
    uint32_t windowFrames_ = 0;
    HANDLE stopEvent_ = nullptr;
    HANDLE captureEvent_ = nullptr;

    mutable std::mutex mutex_;
    /** Non fatal remark about the request itself, shown in the diagnostics. */
    std::string note_;
    std::string error_;
    std::string format_ = "not running";
    int latencyMs_ = -1;

    // Conversion state (only used on the capture thread).
    AudioFormat captured;
    bool needsConversion_ = false;
    std::unique_ptr<SincResampler> resampler_;
    std::vector<float> decodeBuffer_;
    std::vector<float> stereoBuffer_;
    std::vector<float> silence_;
};

void ProcessLoopbackCapture::threadMain(std::promise<std::string> ready) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comResult);
    const HRESULT comExpected = comResult == RPC_E_CHANGED_MODE ? S_OK : comResult;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (!stopEvent_ || !captureEvent_) {
        ready.set_value("could not create the synchronisation events of the capture");
        if (comOwned) CoUninitialize();
        return;
    }

    targetPid_ = request_.rootPids.empty() ? 0 : request_.rootPids.front();
    if (targetPid_ == 0) {
        ready.set_value("no process id was passed for the capture");
        if (comOwned) CoUninitialize();
        return;
    }

    std::string error;
    if (FAILED(comExpected)) {
        error = "[com] " + formatHresult(comExpected, "COM could not be initialised in the capture thread");
        ready.set_value(error);
        return;
    }

    // Every COM object of this thread lives in this scope, so all of them are
    // released before CoUninitialize below.
    [&] {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> captureClient;
    HRESULT hr = activate(client, error);

    if (SUCCEEDED(hr)) {
        needsConversion_ = false;
        hr = initialize(client.get(), captureClient, error);
        if (SUCCEEDED(hr) && !needsConversion_) setFormatDirect();
    }

    if (SUCCEEDED(hr)) {
        hr = client->SetEventHandle(captureEvent_);
        if (FAILED(hr)) error = "[capture-start] " + formatHresult(hr, "SetEventHandle failed");
    }

    if (SUCCEEDED(hr)) {
        hr = client->Start();
        if (FAILED(hr)) error = "[capture-start] " + formatHresult(hr, "Start failed");
    }

    if (FAILED(hr)) {
        if (error.empty()) error = "[capture-start] " + formatHresult(hr, "the capture could not be started");
        setError(error);
        ready.set_value(error);
        return;
    }

    // Nothing failed: the engine may start streaming.
    ready.set_value("");

    std::vector<float> scratch;
    while (running_) {
        const DWORD wait = WaitForSingleObject(captureEvent_, 200);
        if (!running_) break;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            setError("[wait] the capture event could not be waited on");
            died_ = true;
            break;
        }

        UINT32 packetFrames = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);

            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
                    setError("[device-changed] " + formatHresult(hr, "the audio device changed during the capture"));
                    died_ = true;
                    running_ = false;
                } else if (hr != AUDCLNT_S_BUFFER_EMPTY) {
                    setError("[data] " + formatHresult(hr, "the capture buffer could not be read"));
                }
                break;
            }

            push(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            captureClient->ReleaseBuffer(frames);
        }
    }

    if (client) client->Stop();
    }();

    if (comOwned) CoUninitialize();
}

/**
 * Creates a capture for one process tree.
 *
 * Requests with several roots are split by GroupingCaptureFactory before they
 * reach this factory (see capture_group.h), so one call always means one tree.
 * The process tree filter covers the children of that root, which is why a
 * single activation captures e.g. every process of Chrome.
 */
/**
 * Process loopback delivers an application's audio scaled by the volume and
 * mute of the *capturing* process' own session on the device that application
 * plays on. Windows remembers that session state per executable and device,
 * so a mute that was ever set for saf-audio-host.exe (versions before 0.2
 * muted their output as the default session) silences every capture of
 * applications on that device. Measured: the installed host captured only
 * silence from Chrome on a device where its session was remembered as muted,
 * the same binary in another folder captured Chrome at its exact level.
 *
 * So before captures are made, this process' default session is set to
 * unmuted, full volume on every output device. The host's output uses a
 * session of its own (render_sink.cc), so this never touches it.
 */
void resetOwnSessionLevels() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> devices;
        UINT count = 0;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())))
            && SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put())) && SUCCEEDED(devices->GetCount(&count))) {
            for (UINT i = 0; i < count; i++) {
                ComPtr<IMMDevice> device;
                ComPtr<IAudioSessionManager> manager;
                ComPtr<ISimpleAudioVolume> volume;
                if (FAILED(devices->Item(i, device.put()))) continue;
                if (FAILED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, manager.putVoid()))) continue;
                if (FAILED(manager->GetSimpleAudioVolume(nullptr, 0, volume.put())) || !volume) continue;
                // Set unconditionally: a session that is not active yet can
                // report "not muted" and still carry the remembered mute.
                volume->SetMute(FALSE, nullptr);
                volume->SetMasterVolume(1.0f, nullptr);
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

class WasapiCaptureFactory : public ICaptureFactory {
public:
    std::unique_ptr<ICaptureSource> create(const CaptureRequest& request) override {
        if (request.rootPids.empty()) return nullptr;
        resetOwnSessionLevels();
        if (request.rootPids.size() > 1) {
            // Should not happen: the grouping factory splits first. Capturing
            // only the first tree would silently lose the other instances, so
            // say it in the diagnostics instead of hiding it.
            return std::make_unique<ProcessLoopbackCapture>(request, "a request with several root processes was not split");
        }
        return std::make_unique<ProcessLoopbackCapture>(request);
    }

    std::string describe() const override {
        return "WASAPI Process Loopback (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK)";
    }
};

} // namespace

std::unique_ptr<ICaptureFactory> createCaptureFactory(std::string& error) {
    error.clear();
    return std::make_unique<WasapiCaptureFactory>();
}

} // namespace saf::win
