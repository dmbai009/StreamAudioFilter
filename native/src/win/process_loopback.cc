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

/** Builds the interleaved float format we ask the audio engine to deliver. */
void makeFloatFormat(WAVEFORMATEXTENSIBLE& format, int sampleRate, int channels) {
    std::memset(&format, 0, sizeof(format));
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    format.Format.nAvgBytesPerSec = static_cast<DWORD>(sampleRate * channels * 4);
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channels >= 2 ? kSpeakerStereo : kSpeakerMono;
    format.SubFormat = kSubFormatIeeeFloat;
}

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
        if (stopEvent_) SetEvent(stopEvent_);
        if (captureEvent_) SetEvent(captureEvent_);
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

    /** ActivateAudioInterfaceAsync with the process loopback activation params. */
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
            error = "could not create the activation event";
            return E_FAIL;
        }

        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        ComPtr<ActivationHandler> handler(new ActivationHandler(completed));

        HRESULT hr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, handler.get(), operation.put());
        if (SUCCEEDED(hr)) {
            if (WaitForSingleObject(completed, kStartTimeoutMs) != WAIT_OBJECT_0) {
                error = "timeout while activating the process capture";
                hr = E_FAIL;
            } else {
                HRESULT activateResult = E_FAIL;
                ComPtr<IUnknown> unknown;
                hr = operation->GetActivateResult(&activateResult, unknown.put());
                if (SUCCEEDED(hr)) hr = activateResult;
                if (SUCCEEDED(hr) && unknown) hr = unknown->QueryInterface(IID_PPV_ARGS(client.put()));
            }
        }

        if (FAILED(hr) && error.empty()) {
            error = formatHresult(hr, "could not activate the capture of process " + std::to_string(targetPid_));
            if (hr == E_INVALIDARG || hr == E_NOTIMPL) {
                error += " (this Windows build rejected the activation of a per process capture; Windows 10 "
                         "build 19041/2004 and newer do support it in practice, so the exact "
                         "error code matters more than the build number)";
            }
        }

        CloseHandle(completed);
        return hr;
    }

    /** Initializes the client, negotiating the format the engine will deliver. */
    HRESULT initialize(IAudioClient* client, ComPtr<IAudioCaptureClient>& captureClient, std::string& error) {
        // 1. Preferred: let the audio engine convert to 48 kHz float stereo.
        WAVEFORMATEXTENSIBLE preferred;
        makeFloatFormat(preferred, request_.sampleRate, request_.channels);

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
            error = formatHresult(hr, "could not initialise the capture of " + request_.displayName);
            return hr;
        }

        if (FAILED(hr = client->GetService(IID_PPV_ARGS(captureClient.put())))) {
            error = formatHresult(hr, "could not obtain IAudioCaptureClient");
            return hr;
        }

        UINT32 bufferFrames = 0;
        if (SUCCEEDED(client->GetBufferSize(&bufferFrames)) && request_.sampleRate > 0) {
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
        error = formatHresult(comExpected, "could not initialise COM in the capture thread");
        ready.set_value(error);
        return;
    }

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
        if (FAILED(hr)) error = formatHresult(hr, "could not bind the capture event");
    }

    if (SUCCEEDED(hr)) {
        hr = client->Start();
        if (FAILED(hr)) error = formatHresult(hr, "could not start the capture");
    }

    if (FAILED(hr)) {
        if (error.empty()) error = formatHresult(hr, "could not start the capture");
        setError(error);
        ready.set_value(error);
        if (comOwned) CoUninitialize();
        return;
    }

    // Nothing failed: the engine may start streaming.
    ready.set_value("");

    std::vector<float> scratch;
    while (running_) {
        const DWORD wait = WaitForSingleObject(captureEvent_, 200);
        if (!running_) break;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;

        UINT32 packetFrames = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);

            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
                    setError(formatHresult(hr, "the output device changed during the capture"));
                    running_ = false;
                } else if (hr != AUDCLNT_S_BUFFER_EMPTY) {
                    setError(formatHresult(hr, "the capture buffer could not be read"));
                }
                break;
            }

            push(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            captureClient->ReleaseBuffer(frames);
        }
    }

    if (client) client->Stop();
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
class WasapiCaptureFactory : public ICaptureFactory {
public:
    std::unique_ptr<ICaptureSource> create(const CaptureRequest& request) override {
        if (request.rootPids.empty()) return nullptr;
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
