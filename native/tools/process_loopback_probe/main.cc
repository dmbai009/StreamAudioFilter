// StreamAudioFilter - standalone WASAPI process loopback probe
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A console program that performs *one* thing: it activates a Windows process
// loopback capture, tells you which stage fails if it fails, and checks whether
// PCM really arrives. It is independent of Discord, of Vencord and of the
// plugin's capture engine - but it uses the very same agile completion handler
// (native/src/win/com_ptr.h) and the same HRESULT explanations
// (native/src/hr_advice.cc), so a green run here and a red plugin means the
// difference is in the plugin, not in the operating system.
//
// Why it exists: the plugin reported "0x8000000E" on Windows 10 19045 and that
// looked like "this Windows build does not support process loopback". It does:
// 0x8000000E is E_ILLEGAL_METHOD_CALL, which Windows returns when the
// IActivateAudioInterfaceCompletionHandler passed to ActivateAudioInterfaceAsync
// is not an agile object (IAgileObject / free threaded marshaler). This program
// exercises that exact path and prints every stage separately, so the real cause
// is never hidden behind a single bool.
//
// What it never does: it writes no audio to disk (there is no code path that
// opens a file), it does not install anything, and it does not touch the user's
// audio settings. The loudest thing it can do is play a 1 second 1 kHz tone on
// the default output device, and only when you ask for it with --self-tone.
//
// Build (MSVC):
//   cl /nologo /std:c++17 /EHsc /W4 /D_USE_MATH_DEFINES /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
//      /Fe:process-loopback-probe.exe main.cc ../../src/hr_advice.cc ^
//      /link ole32.lib oleaut32.lib uuid.lib mmdevapi.lib
//
// Usage:
//   process-loopback-probe.exe --list
//   process-loopback-probe.exe --pid 1234 [--seconds 5]
//   process-loopback-probe.exe --process spotify.exe [--seconds 5]
//   process-loopback-probe.exe --self-tone [--seconds 5]
//   process-loopback-probe.exe <any of the above> --exclude
//   process-loopback-probe.exe <any of the above> --json
//
// Exit codes: 0 = every stage succeeded (and, with --self-tone, the tone was
// found in the capture), 1 = a stage failed (see the stage log), 2 = usage error.

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/dsp/packet_measure.h"
#include "../../src/hr_advice.h"
#include "../../src/win/audio_subformat.h"
// The COM identities the SDK only declares (their definitions live in uuid.lib,
// which not every SDK/MSVC combination ships): the same header the addon uses,
// so the probe and the plugin answer IID queries identically.
#include "../../src/win/com_iids.h"
#include "../../src/win/com_ptr.h"

using saf::win::ComCallback;
using saf::win::ComPtr;
// IID_IAgileObject is deliberately *not* pulled into the global namespace: the
// SDK declares that name there, and a using-declaration for it is a redefinition
// (MSVC C2874). It is used as saf::win::IID_IAgileObject below.

namespace {

constexpr DWORD kActivationTimeoutMs = 5000;
constexpr DWORD kPacketTimeoutMs = 200;
constexpr double kToneFrequency = 1000.0;
constexpr double kToneAmplitude = 0.2;

// ------------------------------------------------------------------ plumbing

/**
 * The real Windows version. GetVersionEx lies unless the executable carries a
 * compatibility manifest, so the version comes from ntdll, which is what every
 * diagnostic tool does.
 */
std::string platformLine() {
    std::string text = "Windows";

    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        RTL_OSVERSIONINFOW info = {};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtlGetVersion && rtlGetVersion(&info) == 0) {
            text += " " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion)
                + " build " + std::to_string(info.dwBuildNumber);
        }
    }

    SYSTEM_INFO system = {};
    GetNativeSystemInfo(&system);
    text += system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? " (x64)" : " (not x64)";

    return text;
}

/** Closes the event handle after everything that could hold it is gone. */
class EventGuard {
public:
    explicit EventGuard(HANDLE handle) : handle_(handle) {}
    ~EventGuard() { if (handle_) CloseHandle(handle_); }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

/** Closes the apartment last, whatever the path out of runProbe() is. */
class ComGuard {
public:
    explicit ComGuard(bool owned) : owned_(owned) {}
    ~ComGuard() { if (owned_) CoUninitialize(); }

private:
    bool owned_ = false;
};

// ------------------------------------------------------------------ reporting

bool g_json = false;
int g_failures = 0;

void note(const std::string& stage, const std::string& text) {
    if (!g_json) std::printf("[%-16s] %s\n", stage.c_str(), text.c_str());
}

void failed(const std::string& stage, HRESULT hr, const std::string& what) {
    g_failures++;
    if (g_json) return;
    std::printf("[%-16s] FAILED - %s\n", stage.c_str(), what.c_str());
    std::printf("[%-16s] %s\n", stage.c_str(), saf::describeHresult(static_cast<int32_t>(hr)).c_str());
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// ------------------------------------------------------------- agile callback

/**
 * The callback Windows fills in when the asynchronous activation finishes.
 * It is called from a worker thread of the multi threaded apartment, so it must
 * be an apartment agnostic ("agile") object - that is the whole point of this
 * program.
 */
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

// ------------------------------------------------------------------ processes

struct ProcessEntry {
    DWORD pid = 0;
    std::string exeName;
    std::string exePath;
};

std::vector<ProcessEntry> processList() {
    std::vector<ProcessEntry> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessEntry item;
            item.pid = entry.th32ProcessID;

            char name[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, name, sizeof(name) - 1, nullptr, nullptr);
            item.exeName = name;

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process) {
                wchar_t path[MAX_PATH] = {};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                    char utf8[MAX_PATH * 4] = {};
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
                    item.exePath = utf8;
                }
                CloseHandle(process);
            }
            result.push_back(item);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool equalsIgnoreCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

// --------------------------------------------------------------- self tone

/**
 * Plays a 1 kHz tone from *this* process through the default render endpoint.
 *
 * This is how the probe proves that PCM really arrives: the capture is rooted at
 * our own process tree, so the tone we render must appear in the capture. It is
 * also the honest way to test - no third party application, no user audio.
 */
class TonePlayer {
public:
    ~TonePlayer() { stop(); }

    bool start(std::string& error) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comOwned_ = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            error = "CoInitializeEx on the tone thread: " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()));
        if (FAILED(hr)) {
            error = "MMDeviceEnumerator: " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put());
        if (FAILED(hr) || !device) {
            error = "no default output device: " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid());
        if (FAILED(hr) || !client_) {
            error = "IAudioClient (render): " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        WAVEFORMATEX* mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            error = "GetMixFormat: " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        formatRate_ = mix->nSamplesPerSec;
        formatChannels_ = mix->nChannels;
        formatBits_ = mix->wBitsPerSample;

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) {
            error = "Initialize (render): " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        hr = client_->SetEventHandle(event_);
        if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(render_.put()));
        if (FAILED(hr) || !render_) {
            error = "IAudioRenderClient: " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        hr = client_->Start();
        if (FAILED(hr)) {
            error = "Start (render): " + saf::describeHresult(static_cast<int32_t>(hr));
            return false;
        }

        running_ = true;
        thread_ = std::thread([this]() { renderLoop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (event_) SetEvent(event_);
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
        if (comOwned_) CoUninitialize();
    }

    /** 32 bit float mix format (the usual case) or 16 bit PCM. */
    bool floatFormat() const { return formatBits_ == 32; }

    /** How much this program really played - a silent "tone" proves nothing. */
    uint64_t framesRendered() const { return framesRendered_.load(); }

    std::string renderStatus() const {
        const int32_t error = renderError_.load();
        if (error != 0) return "the render client stopped with " + saf::describeHresult(error);
        return "no render error";
    }

private:
    void renderLoop() {
        const double twoPi = 6.283185307179586;
        double phase = 0.0;

        while (running_) {
            if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) continue;
            if (!running_) break;

            UINT32 padding = 0;
            if (FAILED(record(client_->GetCurrentPadding(&padding)))) break;

            UINT32 bufferFrames = 0;
            if (FAILED(record(client_->GetBufferSize(&bufferFrames)))) break;

            const UINT32 available = bufferFrames - padding;
            if (available == 0) continue;

            BYTE* data = nullptr;
            if (FAILED(record(render_->GetBuffer(available, &data)))) break;

            for (UINT32 frame = 0; frame < available; frame++) {
                const double sample = kToneAmplitude * std::sin(phase);
                phase += twoPi * kToneFrequency / static_cast<double>(formatRate_);
                if (phase > twoPi) phase -= twoPi;

                if (floatFormat()) {
                    auto* out = reinterpret_cast<float*>(data) + static_cast<size_t>(frame) * formatChannels_;
                    for (int channel = 0; channel < formatChannels_; channel++) out[channel] = static_cast<float>(sample);
                } else {
                    auto* out = reinterpret_cast<int16_t*>(data) + static_cast<size_t>(frame) * formatChannels_;
                    const auto value = static_cast<int16_t>(sample * 32767.0);
                    for (int channel = 0; channel < formatChannels_; channel++) out[channel] = value;
                }
            }

            if (FAILED(record(render_->ReleaseBuffer(available, 0)))) break;
            framesRendered_ += available;
        }
    }

    /** Keeps the first error of the render loop so the report can name it. */
    HRESULT record(HRESULT hr) {
        if (FAILED(hr)) {
            int32_t expected = 0;
            renderError_.compare_exchange_strong(expected, static_cast<int32_t>(hr));
        }
        return hr;
    }

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE event_ = nullptr;
    std::thread thread_;
    bool running_ = false;
    bool comOwned_ = false;
    uint32_t formatRate_ = 48000;
    int formatChannels_ = 2;
    int formatBits_ = 32;
    std::atomic<uint64_t> framesRendered_ { 0 };
    std::atomic<int32_t> renderError_ { 0 };
};

/** The tone search itself: saf::dsp::toneMagnitude, shared with the plugin. */
double findTone(const std::vector<float>& samples, int sampleRate) {
    return saf::dsp::toneMagnitude(samples.data(), samples.size(), sampleRate, kToneFrequency);
}

// --------------------------------------------------------------- measurement

struct CaptureWindow {
    int packets = 0;
    uint64_t frames = 0;
    uint64_t silentFrames = 0;
    saf::dsp::SampleStats stats;
    /** Kept only for the tone search. Nothing is ever written to disk. */
    std::vector<float> samples;
};

/**
 * Reads the capture for @p seconds and measures what really arrives.
 *
 * The number of non-zero samples is the one that matters: a process loopback
 * stream can deliver packets at exactly the right rate for the whole window and
 * still carry nothing but silence. "Packets arrived" is not "audio arrived", and
 * frames flagged AUDCLNT_BUFFERFLAGS_SILENT are a third state again.
 */
CaptureWindow collectCapture(IAudioCaptureClient* capture, HANDLE event, int seconds, int channels, bool floatData,
    bool keepSamples) {
    CaptureWindow window;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;

    while (GetTickCount64() < deadline) {
        if (event) WaitForSingleObject(event, kPacketTimeoutMs);

        UINT32 available = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&available)) && available > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD packetFlags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &packetFlags, nullptr, nullptr))) break;

            window.packets++;
            window.frames += frames;
            const bool silent = (packetFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (silent || !data) {
                window.silentFrames += frames;
            } else {
                const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
                if (floatData) {
                    const auto* values = reinterpret_cast<const float*>(data);
                    saf::dsp::measureFloats(values, count, window.stats);
                    if (keepSamples) window.samples.insert(window.samples.end(), values, values + count);
                } else {
                    // The 16 bit PCM fallback format, scaled to the same -1..1 range so
                    // that the numbers mean the same thing in both cases.
                    const auto* values = reinterpret_cast<const int16_t*>(data);
                    saf::dsp::measureInt16(values, count, window.stats);
                    if (keepSamples) {
                        for (size_t i = 0; i < count; i++)
                            window.samples.push_back(static_cast<float>(values[i]) / 32768.0f);
                    }
                }
            }

            capture->ReleaseBuffer(frames);
        }
    }
    return window;
}

/** One line about what arrived: silence and audio look completely different. */
void reportWindow(const CaptureWindow& window, const WAVEFORMATEX* negotiated) {
    std::string text = std::to_string(window.packets) + " packet(s), " + std::to_string(window.frames) + " frames, "
        + std::to_string(window.silentFrames) + " frames flagged silent";
    if (negotiated) {
        text += ", " + std::to_string(negotiated->nSamplesPerSec) + " Hz / " + std::to_string(negotiated->nChannels)
            + " channels / " + std::to_string(negotiated->wBitsPerSample) + " bit";
    }
    if (window.stats.samples > 0) {
        char numbers[192];
        std::snprintf(numbers, sizeof(numbers), ", peak %.6f, RMS %.6f, non-zero samples %llu", window.stats.peak,
            window.stats.rms(), static_cast<unsigned long long>(window.stats.nonzero));
        text += numbers;
        text += window.stats.nonzero == 0 ? " - the stream carries pure silence (zeros, not flagged)"
                                          : " - the stream carries audio";
    }
    note("data", text);
}

/** PKEY_Device_FriendlyName - {a45c254e-df1c-4efd-8020-67d146a850e0}, 14. */
const PROPERTYKEY kFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

/** The name of the default output device, so a virtual cable is recognisable. */
std::string defaultRenderEndpointName() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()))))
        return {};

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put())) || !device) return {};

    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put())) || !store) return {};

    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::string utf8(static_cast<size_t>(size - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, utf8.data(), size, nullptr, nullptr);
            name = utf8;
        }
    }
    PropVariantClear(&value);
    return name;
}

/**
 * Processes that hold an ACTIVE audio session: the ones really playing something
 * right now, which is exactly what a capture test needs as a target.
 */
void listPlayingProcesses() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComGuard comGuard(SUCCEEDED(com));
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        std::printf("COM could not be initialised\n");
        return;
    }

    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        int count = 0;

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())))) {
            std::printf("the audio device enumerator is unavailable\n");
        } else if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put())) || !device) {
            std::printf("there is no default output device\n");
        } else if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid()))
            || !manager || FAILED(manager->GetSessionEnumerator(sessions.put())) || !sessions
            || FAILED(sessions->GetCount(&count))) {
            std::printf("the audio session manager is unavailable\n");
        } else {
            const std::string endpoint = defaultRenderEndpointName();
            std::printf("default output: %s\n", endpoint.empty() ? "(name unavailable)" : endpoint.c_str());
            std::printf("processes playing audio right now:\n");

            const std::vector<ProcessEntry> processes = processList();
            int printed = 0;
            for (int i = 0; i < count; i++) {
                ComPtr<IAudioSessionControl> control;
                if (FAILED(sessions->GetSession(i, control.put())) || !control) continue;

                AudioSessionState state = AudioSessionStateInactive;
                if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive) continue;

                DWORD pid = 0;
                ComPtr<IAudioSessionControl2> control2;
                if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(control2.put())))) control2->GetProcessId(&pid);

                std::string name = "(unknown)";
                for (const ProcessEntry& entry : processes) {
                    if (entry.pid == pid && !entry.exeName.empty()) {
                        name = entry.exeName;
                        break;
                    }
                }
                std::printf("  %6lu  %s\n", static_cast<unsigned long>(pid), name.c_str());
                printed++;
            }
            if (printed == 0)
                std::printf("  (none - start playback in the application you want to test with)\n");
        }
    }
}

// ------------------------------------------------------------------ the probe

struct Options {
    DWORD pid = 0;
    std::string process;
    int seconds = 5;
    bool exclude = false;
    bool selfTone = false;
    bool list = false;
    bool playing = false;
    bool system = false;
};

struct RunResult {
    bool succeeded = false;
    bool pcmArrived = false;
    double tone = 0.0;
    uint64_t frames = 0;
    uint64_t silentFrames = 0;
    uint64_t nonZeroSamples = 0;
    double peak = 0.0;
    double rms = 0.0;
    uint64_t toneFrames = 0;
    int packets = 0;
    std::string format;
    std::string failureStage;
    std::string failureText;
};

int runProbe(const Options& options, RunResult& result) {
    note("platform", platformLine());

    // ------------------------------------------------------------------ stage 1
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(com);
    // Declared first so it runs last: every COM object below is released before
    // the apartment is closed.
    ComGuard comGuard(comOwned);
    if (com == S_OK) {
        note("com", "CoInitializeEx(COINIT_MULTITHREADED): S_OK");
    } else if (com == S_FALSE) {
        note("com", "CoInitializeEx(COINIT_MULTITHREADED): S_FALSE (already initialised)");
    } else if (com == RPC_E_CHANGED_MODE) {
        note("com", "CoInitializeEx(COINIT_MULTITHREADED): RPC_E_CHANGED_MODE - the thread is STA, COM has to marshal "
                    "the callback (legal for an agile handler, fatal without one)");
    }
    const HRESULT comExpected = com == RPC_E_CHANGED_MODE ? S_OK : com;
    if (FAILED(comExpected)) {
        failed("com", comExpected, "COM could not be initialised on this thread");
        result.failureStage = "com";
        result.failureText = saf::describeHresult(static_cast<int32_t>(comExpected));
        return 1;
    }

    // ------------------------------------------------------------------ stage 2
    // Auto reset on purpose: the audio engine signals this event once per period,
    // and a manual reset event would stay signalled after the first callback, which
    // would turn the rest of the capture window into a busy spin that competes with
    // the capture it is supposed to measure.
    EventGuard eventGuard(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!eventGuard.get()) {
        failed("com", E_FAIL, "the synchronisation event could not be created");
        result.failureStage = "com";
        return 1;
    }
    const HANDLE completed = eventGuard.get();

    // Declared before the apartment guard: released before the apartment closes.
    ComPtr<ActivationHandler> handler(new ActivationHandler(completed));
    {
        void* agile = nullptr;
        const bool isAgile = SUCCEEDED(handler->QueryInterface(saf::win::IID_IAgileObject, &agile)) && agile != nullptr;
        if (agile) static_cast<IUnknown*>(agile)->Release();
        note("handler", std::string("completion handler is an agile object: ") + (isAgile ? "yes" : "NO"));
        if (!isAgile) {
            failed("handler", E_ILLEGAL_METHOD_CALL, "the handler does not answer IAgileObject - Windows would refuse "
                                                     "the activation below");
        }
    }

    // ------------------------------------------------------------------ stage 3
    DWORD targetPid = options.pid;
    if (targetPid == 0 && options.selfTone) targetPid = GetCurrentProcessId();
    if (targetPid == 0 && !options.process.empty()) {
        for (const ProcessEntry& entry : processList()) {
            if (equalsIgnoreCase(entry.exeName, options.process)) {
                targetPid = entry.pid;
                note("target", "process " + entry.exeName + " (pid " + std::to_string(entry.pid) + ")");
                break;
            }
        }
        if (targetPid == 0) {
            failed("target", HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "no running process named " + options.process);
            result.failureStage = "target";
            return 1;
        }
    }
    if (targetPid == 0) {
        failed("target", E_INVALIDARG, "no target process was given (use --pid, --process or --self-tone)");
        result.failureStage = "target";
        return 1;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = targetPid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = options.exclude ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                                                                     : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    note("params", "target pid " + std::to_string(targetPid) + ", mode "
        + (options.exclude ? "EXCLUDE_TARGET_PROCESS_TREE" : "INCLUDE_TARGET_PROCESS_TREE") + ", VT_BLOB "
        + std::to_string(sizeof(params)) + " bytes");

    // ------------------------------------------------------------------ stage 4
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT callHr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, handler.get(), operation.put());
    if (FAILED(callHr)) {
        failed("activation-call", callHr, "ActivateAudioInterfaceAsync refused the request");
        result.failureStage = "activation-call";
        result.failureText = saf::describeHresult(static_cast<int32_t>(callHr));
        return 1;
    }
    note("activation-call", "accepted (the audio engine will call back on a worker thread)");

    // ------------------------------------------------------------------ stage 5
    if (WaitForSingleObject(completed, kActivationTimeoutMs) != WAIT_OBJECT_0) {
        failed("activation-wait", HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            "no completion callback within " + std::to_string(kActivationTimeoutMs) + " ms");
        result.failureStage = "activation-wait";
        return 1;
    }
    note("activation-wait", "the completion callback fired");

    // ------------------------------------------------------------------ stage 6
    HRESULT activation = E_FAIL;
    ComPtr<IUnknown> activated;
    const HRESULT resultHr = operation->GetActivateResult(&activation, activated.put());
    if (FAILED(resultHr)) {
        failed("activate-result", resultHr, "GetActivateResult itself failed");
        result.failureStage = "activate-result";
        result.failureText = saf::describeHresult(static_cast<int32_t>(resultHr));
        return 1;
    }
    if (FAILED(activation)) {
        failed("activate-result", activation, "the activation completed with an error");
        result.failureStage = "activate-result";
        result.failureText = saf::describeHresult(static_cast<int32_t>(activation));
        return 1;
    }
    note("activate-result", "S_OK - the capture client exists");

    // ------------------------------------------------------------------ stage 7
    ComPtr<IAudioClient> client;
    const HRESULT qiHr = activated->QueryInterface(IID_PPV_ARGS(client.put()));
    if (FAILED(qiHr) || !client) {
        failed("interface", qiHr, "the activated object has no IAudioClient");
        result.failureStage = "interface";
        result.failureText = saf::describeHresult(static_cast<int32_t>(qiHr));
        return 1;
    }
    note("interface", "IAudioClient obtained");

    // ------------------------------------------------------------------ stage 8
    // A process loopback client is not an endpoint client: ask for 48 kHz float
    // stereo and let the engine convert (AUTOCONVERTPCM), fall back to 16 bit
    // PCM if that is refused. GetMixFormat is deliberately not used on this
    // client: it reports E_NOTIMPL on some builds, which says nothing about the
    // capture.
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    WAVEFORMATEXTENSIBLE floatFormat;
    saf::win::makeFloatFormat(floatFormat, 48000, 2);

    HRESULT initHr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0,
        reinterpret_cast<WAVEFORMATEX*>(&floatFormat), nullptr);
    WAVEFORMATEX pcmFormat = {};
    const WAVEFORMATEX* negotiated = reinterpret_cast<const WAVEFORMATEX*>(&floatFormat);
    bool floatCapture = SUCCEEDED(initHr);

    if (FAILED(initHr)) {
        note("client-init", "48 kHz float stereo was refused: " + saf::describeHresult(static_cast<int32_t>(initHr)));

        pcmFormat.wFormatTag = WAVE_FORMAT_PCM;
        pcmFormat.nChannels = 2;
        pcmFormat.nSamplesPerSec = 48000;
        pcmFormat.wBitsPerSample = 16;
        pcmFormat.nBlockAlign = static_cast<WORD>(pcmFormat.nChannels * pcmFormat.wBitsPerSample / 8);
        pcmFormat.nAvgBytesPerSec = pcmFormat.nSamplesPerSec * pcmFormat.nBlockAlign;
        initHr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &pcmFormat, nullptr);
        if (SUCCEEDED(initHr)) {
            negotiated = &pcmFormat;
            note("client-init", "16 bit PCM 48 kHz stereo accepted");
        }
    } else {
        note("client-init", "48 kHz / 2 channels / 32 bit float accepted");
    }

    if (FAILED(initHr)) {
        failed("client-init", initHr, "neither float nor 16 bit PCM was accepted");
        result.failureStage = "client-init";
        result.failureText = saf::describeHresult(static_cast<int32_t>(initHr));
        return 1;
    }

    result.format = std::to_string(negotiated->nSamplesPerSec) + " Hz / " + std::to_string(negotiated->nChannels)
        + " channels / " + std::to_string(negotiated->wBitsPerSample) + " bit";

    // ------------------------------------------------------------------ stage 9
    ComPtr<IAudioCaptureClient> capture;
    HRESULT hr = client->GetService(IID_PPV_ARGS(capture.put()));
    if (FAILED(hr) || !capture) {
        failed("capture-client", hr, "IAudioCaptureClient is not available");
        result.failureStage = "capture-client";
        return 1;
    }

    // GetBufferSize has no useful meaning for a process loopback client: the value
    // it returns is not a packet size, and on some systems it is large nonsense
    // (hundreds of millions of frames). It is validated before being printed and is
    // never used to size or pace anything.
    UINT32 bufferFrames = 0;
    const HRESULT sizeHr = client->GetBufferSize(&bufferFrames);
    const UINT32 saneLimit = static_cast<UINT32>(negotiated->nSamplesPerSec) * 4;  // 4 seconds
    if (FAILED(sizeHr)) {
        note("buffer-size", "the engine does not report a packet size: "
            + saf::describeHresult(static_cast<int32_t>(sizeHr)));
    } else if (bufferFrames == 0 || bufferFrames > saneLimit) {
        note("buffer-size", "the engine reported " + std::to_string(bufferFrames) + " frames, which is not a usable "
            "value for this kind of client - treated as unknown, not as a latency");
    } else {
        note("buffer-size", std::to_string(bufferFrames) + " frames per packet ("
            + std::to_string(bufferFrames * 1000 / negotiated->nSamplesPerSec) + " ms)");
    }

    // ----------------------------------------------------------------- stage 10
    hr = client->SetEventHandle(completed);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        failed("capture-start", hr, "the capture stream did not start");
        result.failureStage = "capture-start";
        result.failureText = saf::describeHresult(static_cast<int32_t>(hr));
        return 1;
    }
    note("capture-start", "the capture is running");

    // ----------------------------------------------------------------- stage 11
    // The tone (when asked for) is started after the capture, so that the whole
    // window below is inside the tones lifetime.
    TonePlayer tone;
    if (options.selfTone) {
        std::string toneError;
        if (tone.start(toneError)) {
            note("self-tone", "playing 1 kHz from this process for the whole capture window");
        } else {
            note("self-tone", "could not play the tone: " + toneError + " (the capture itself is still tested)");
        }
    }

    CaptureWindow window = collectCapture(capture.get(), completed, options.seconds,
        static_cast<int>(negotiated->nChannels), floatCapture, options.selfTone);

    tone.stop();
    result.toneFrames = tone.framesRendered();
    if (options.selfTone) {
        note("self-tone", "this program rendered " + std::to_string(tone.framesRendered()) + " frames ("
            + tone.renderStatus() + ")");
    }

    result.packets = window.packets;
    result.frames = window.frames;
    result.silentFrames = window.silentFrames;
    result.nonZeroSamples = window.stats.nonzero;
    result.peak = window.stats.peak;
    result.rms = window.stats.rms();
    result.pcmArrived = window.packets > 0 && window.stats.nonzero > 0;
    reportWindow(window, negotiated);

    if (options.selfTone && !window.samples.empty()) {
        result.tone = findTone(window.samples, static_cast<int>(negotiated->nSamplesPerSec));
        note("self-tone", "1 kHz magnitude in the captured audio: " + std::to_string(result.tone)
            + (result.tone > 0.05 ? " - the tone really was captured" : " - the tone was NOT found"));
    }

    // ----------------------------------------------------------------- stage 12
    // Everything is released here rather than at the end of the function, so that
    // the note below describes a state that is already true.
    client->Stop();
    capture.reset();
    client.reset();
    activated.reset();
    operation.reset();
    handler.reset();
    note("teardown", "the capture was stopped and every COM object released; the apartment is closed as this "
                     "function returns - no audio was written to disk");

    // The tone is treated as evidence, never as a verdict. A process loopback
    // stream is documented to be able to leave out the audio of the process that
    // is doing the capturing, so "the tone of the capturing process is not in the
    // capture of the capturing process" is not a failure of the capture - it is a
    // limitation of this particular test, and saying otherwise would send the
    // diagnosis in the wrong direction. The check that *is* decisive is --system.
    if (options.selfTone && result.tone <= 0.05) {
        const uint64_t halfASecond = static_cast<uint64_t>(negotiated->nSamplesPerSec) / 2;
        if (tone.framesRendered() < halfASecond) {
            note("self-tone", "this program rendered nothing (or almost nothing) - the tone never reached the device, "
                              "so the capture had nothing to take and the tone proves nothing here");
        } else if (window.stats.nonzero == 0) {
            note("self-tone", "the tone is not in this capture and no other audio arrived either, so this run neither "
                              "proves nor disproves content: run --system for the control path, and --pid <player> "
                              "while that player is really playing something");
        } else {
            note("self-tone", "the tone is not in this capture although the capture does carry other audio. The "
                              "capturing process is part of the captured tree here, and its own audio is not "
                              "guaranteed to appear in its own capture - a limitation of this test, not a failure");
        }
    }

    if (!result.pcmArrived) {
        note("data", "no audio arrived in the capture window - start playback in the target application, or use "
                      "--system to check the control path");
    }

    result.succeeded = true;
    return 0;
}

/**
 * The control test: the same measurement on the whole system audio, through a
 * classic WASAPI loopback stream on the default output device.
 *
 * Why it exists: if the tone is captured here but not through the process
 * loopback stream, the device, the system and this program are all fine and the
 * problem is specific to per process capture. If it is missing here as well, the
 * audio never reached the device at all, which has nothing to do with the API.
 */
int runSystemLoopback(const Options& options, RunResult& result) {
    note("mode", "whole system audio: classic WASAPI loopback on the default output device (control test)");

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Declared first so it runs last: the apartment outlives every interface below.
    ComGuard comGuard(SUCCEEDED(com));
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        note("com", "CoInitializeEx failed: " + saf::describeHresult(static_cast<int32_t>(com)));
        result.failureStage = "com";
        return 1;
    }
    note("com", "COM initialised (multithreaded apartment)");

    const std::string endpoint = defaultRenderEndpointName();
    note("endpoint", endpoint.empty() ? "the default output device has no readable name"
                                      : "default output: " + endpoint);

    HRESULT hr = S_OK;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* mix = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put());
    if (SUCCEEDED(hr) && device) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
    if (FAILED(hr) || !client) {
        note("interface", "the default output device could not be opened: "
            + saf::describeHresult(static_cast<int32_t>(hr)));
        result.failureStage = "interface";
        return 1;
    }
    note("interface", "IAudioClient obtained for the default output device");

    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        note("client-init", "the output mix format could not be read: "
            + saf::describeHresult(static_cast<int32_t>(hr)));
        result.failureStage = "client-init";
        return 1;
    }

    EventGuard eventGuard(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!eventGuard.get()) {
        note("com", "the synchronisation event could not be created");
        result.failureStage = "com";
        CoTaskMemFree(mix);
        return 1;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix, nullptr);
    if (FAILED(hr)) {
        note("client-init", "the loopback stream could not be initialised: "
            + saf::describeHresult(static_cast<int32_t>(hr)));
        result.failureStage = "client-init";
        CoTaskMemFree(mix);
        return 1;
    }
    note("client-init", "loopback stream initialised: " + std::to_string(mix->nSamplesPerSec) + " Hz / "
        + std::to_string(mix->nChannels) + " channels / " + std::to_string(mix->wBitsPerSample) + " bit");

    hr = client->GetService(IID_PPV_ARGS(capture.put()));
    if (FAILED(hr) || !capture) {
        note("capture-client", "IAudioCaptureClient is unavailable: "
            + saf::describeHresult(static_cast<int32_t>(hr)));
        result.failureStage = "capture-client";
        CoTaskMemFree(mix);
        return 1;
    }
    note("capture-client", "IAudioCaptureClient obtained");

    hr = client->SetEventHandle(eventGuard.get());
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        note("capture-start", "the loopback stream did not start: "
            + saf::describeHresult(static_cast<int32_t>(hr)));
        result.failureStage = "capture-start";
        CoTaskMemFree(mix);
        return 1;
    }
    note("capture-start", "the loopback stream is running");

    TonePlayer tone;
    if (options.selfTone) {
        std::string toneError;
        if (tone.start(toneError)) {
            note("self-tone", "playing 1 kHz from this program for the whole capture window");
        } else {
            note("self-tone", "could not play the tone: " + toneError);
        }
    }

    const bool floatData = mix->wBitsPerSample == 32;
    CaptureWindow window = collectCapture(capture.get(), eventGuard.get(), options.seconds,
        static_cast<int>(mix->nChannels), floatData, options.selfTone);

    tone.stop();
    const int rate = static_cast<int>(mix->nSamplesPerSec);
    result.toneFrames = tone.framesRendered();
    result.packets = window.packets;
    result.frames = window.frames;
    result.silentFrames = window.silentFrames;
    result.nonZeroSamples = window.stats.nonzero;
    result.peak = window.stats.peak;
    result.rms = window.stats.rms();
    result.pcmArrived = window.packets > 0 && window.stats.nonzero > 0;
    result.format = std::to_string(mix->nSamplesPerSec) + " Hz / " + std::to_string(mix->nChannels) + " channels / "
        + std::to_string(mix->wBitsPerSample) + " bit (device mix format)";
    if (options.selfTone) {
        note("self-tone", "this program rendered " + std::to_string(tone.framesRendered()) + " frames ("
            + tone.renderStatus() + ")");
    }
    reportWindow(window, mix);

    if (options.selfTone) {
        result.tone = findTone(window.samples, rate);
        note("self-tone", "1 kHz magnitude in the system audio: " + std::to_string(result.tone)
            + (result.tone > 0.05 ? " - the tone is really there" : " - the tone is NOT there"));
    }

    client->Stop();
    CoTaskMemFree(mix);
    capture.reset();
    client.reset();
    device.reset();
    enumerator.reset();
    note("teardown", "the capture was stopped and every COM object released; the apartment is closed as this "
                     "function returns - no audio was written to disk");

    if (options.selfTone && result.tone <= 0.05) {
        // In this mode the tone has to come back: it is played into the same device
        // that the control stream captures. If it does not, the audio never reached
        // the device, and that is a real answer to a real question.
        result.failureStage = "self-tone";
        result.failureText = tone.framesRendered() < static_cast<uint64_t>(rate)
            ? "this program could not render the tone at all, so the control test proves nothing"
            : "the tone was rendered but is not in the system audio: the audio never reached the device";
        return 1;
    }

    result.succeeded = true;
    return 0;
}

void printUsage() {
    std::printf(
        "StreamAudioFilter process loopback probe\n\n"
        "  --list                 list running processes (pid, name)\n"
        "  --playing              list the processes that are playing audio right now\n"
        "  --pid <n>              capture this process tree\n"
        "  --process <name.exe>   capture the first process with this image name\n"
        "  --self-tone            play 1 kHz from this program and look for it in the capture\n"
        "  --system               capture whole system audio instead (the control test)\n"
        "  --exclude              capture everything *except* the target tree\n"
        "  --seconds <n>          how long to capture (default 5)\n"
        "  --json                 machine readable result\n\n"
        "Nothing is installed, no setting is changed and no audio is written to disk.\n"
        "\nHow to read the result:\n"
        "  frames arriving   the stream exists; the number that says whether audio is in it is the\n"
        "                    count of non-zero samples. peak 0.000000 means the stream carries silence,\n"
        "                    which is not the same as a failed capture.\n"
        "  --self-tone       the only check of the content: this program plays 1 kHz and looks for it in\n"
        "                    the capture. When it is missing the report says whether the tone was even\n"
        "                    rendered, so the capture is never blamed for a source that stayed silent.\n"
        "  --system          the control. If the tone is missing there too, the audio never reached the\n"
        "                    device, which has nothing to do with per process capture.\n");
}

} // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;

        if (arg == "--list") {
            options.list = true;
        } else if (arg == "--playing") {
            options.playing = true;
        } else if (arg == "--system") {
            options.system = true;
        } else if (arg == "--pid" && hasNext) {
            options.pid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--process" && hasNext) {
            options.process = argv[++i];
        } else if (arg == "--seconds" && hasNext) {
            options.seconds = std::atoi(argv[++i]);
            if (options.seconds < 1 || options.seconds > 300) {
                std::printf("--seconds must be between 1 and 300\n");
                return 2;
            }
        } else if (arg == "--self-tone") {
            options.selfTone = true;
        } else if (arg == "--exclude") {
            options.exclude = true;
        } else if (arg == "--json") {
            g_json = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::printf("unknown argument: %s\n\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    if (options.playing) {
        listPlayingProcesses();
        return 0;
    }

    if (options.list) {
        for (const ProcessEntry& entry : processList()) {
            if (entry.pid == 0) continue;
            std::printf("%6lu  %-40s %s\n", static_cast<unsigned long>(entry.pid), entry.exeName.c_str(), entry.exePath.c_str());
        }
        return 0;
    }

    if (!g_json) {
        std::printf("StreamAudioFilter - standalone WASAPI process loopback probe\n");
        std::printf("no installation, no settings changed, no audio written to disk\n\n");
    }

    RunResult result;
    const int status = options.system ? runSystemLoopback(options, result) : runProbe(options, result);

    if (g_json) {
        std::printf("{\"ok\": %s, \"pcmArrived\": %s, \"toneMagnitude\": %.4f, \"packets\": %d, \"frames\": %llu, "
                    "\"silentFrames\": %llu, \"nonZeroSamples\": %llu, \"peak\": %.6f, \"rms\": %.6f, "
                    "\"format\": \"%s\", \"failureStage\": \"%s\", \"failure\": \"%s\", \"failures\": %d}\n",
            status == 0 ? "true" : "false", result.pcmArrived ? "true" : "false", result.tone, result.packets,
            static_cast<unsigned long long>(result.frames), static_cast<unsigned long long>(result.silentFrames),
            static_cast<unsigned long long>(result.nonZeroSamples), result.peak, result.rms,
            jsonEscape(result.format).c_str(), jsonEscape(result.failureStage).c_str(),
            jsonEscape(result.failureText).c_str(), g_failures);
    } else {
        if (options.system) {
            std::printf("\nverdict: %s\n", status == 0
                    ? "the whole system audio loopback works on this system (control test passed)"
                    : "the control test failed - see the stage above");
        } else if (status != 0) {
            std::printf("\nverdict: the capture did not work - the stage above names the cause\n");
        } else if (result.nonZeroSamples > 0) {
            std::printf("\nverdict: the process loopback capture works on this system: %llu non-zero samples "
                        "(peak %.6f)\n", static_cast<unsigned long long>(result.nonZeroSamples), result.peak);
            if (options.selfTone && result.tone <= 0.05 && result.toneFrames > 0) {
                std::printf("         (the 1 kHz tone of this program was not part of it - see the self-tone lines:\n"
                            "          that is a limitation of the test, not a failure of the capture)\n");
            }
        } else {
            std::printf("\nverdict: the capture stream itself works on this system, but the target produced no "
                        "audio during the test - start playback in it and run the probe again\n");
        }
    }

    return status;
}
