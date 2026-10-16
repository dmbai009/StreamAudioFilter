// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strings, HRESULT diagnostics, OS version, process metadata and the capability
// probe. No audio streaming happens here.

// <windows.h> must come first: the audio platform headers and <appmodel.h> both
// build on the macros it defines (WINBASEAPI, DECLSPEC_IMPORT, ...).
#include <windows.h>

#include <appmodel.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../dsp/packet_measure.h"
#include "../hr_advice.h"
#include "../platform.h"
#include "com_iids.h"
#include "com_ptr.h"
#include "audio_subformat.h"
#include "win_internal.h"
#include "windows_backend.h"

namespace saf::win {

// The portable advice table (native/src/hr_advice.cc) carries its own numeric
// HRESULT values, so that diagnostics, tests and the documentation work without
// Windows headers. On Windows the SDK is available, so every one of those numbers
// is cross checked here at compile time: a typo in the table cannot reach a user,
// and the build fails instead of printing a wrong explanation.
static_assert(hr::IllegalMethodCall == static_cast<int32_t>(E_ILLEGAL_METHOD_CALL), "E_ILLEGAL_METHOD_CALL is not 0x8000000E");
static_assert(hr::NotImpl == static_cast<int32_t>(E_NOTIMPL), "E_NOTIMPL changed");
static_assert(hr::NoInterface == static_cast<int32_t>(E_NOINTERFACE), "E_NOINTERFACE changed");
static_assert(hr::Pointer == static_cast<int32_t>(E_POINTER), "E_POINTER changed");
static_assert(hr::Fail == static_cast<int32_t>(E_FAIL), "E_FAIL changed");
static_assert(hr::Unexpected == static_cast<int32_t>(E_UNEXPECTED), "E_UNEXPECTED changed");
static_assert(hr::Win32InvalidParameter == static_cast<int32_t>(E_INVALIDARG), "E_INVALIDARG changed");
static_assert(hr::Win32AccessDenied == static_cast<int32_t>(E_ACCESSDENIED), "E_ACCESSDENIED changed");
static_assert(hr::Win32OutOfMemory == static_cast<int32_t>(E_OUTOFMEMORY), "E_OUTOFMEMORY changed");
static_assert(hr::ComNotInitialized == static_cast<int32_t>(CO_E_NOTINITIALIZED), "CO_E_NOTINITIALIZED changed");
static_assert(hr::RpcChangedMode == static_cast<int32_t>(RPC_E_CHANGED_MODE), "RPC_E_CHANGED_MODE changed");
static_assert(hr::RpcWrongThread == static_cast<int32_t>(RPC_E_WRONG_THREAD), "RPC_E_WRONG_THREAD changed");
static_assert(hr::Win32Timeout == static_cast<int32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)), "ERROR_TIMEOUT changed");
static_assert(hr::AudclntNotInitialized == static_cast<int32_t>(AUDCLNT_E_NOT_INITIALIZED), "AUDCLNT_E_NOT_INITIALIZED changed");
static_assert(hr::AudclntWrongEndpointType == static_cast<int32_t>(AUDCLNT_E_WRONG_ENDPOINT_TYPE), "AUDCLNT_E_WRONG_ENDPOINT_TYPE changed");
static_assert(hr::AudclntDeviceInvalidated == static_cast<int32_t>(AUDCLNT_E_DEVICE_INVALIDATED), "AUDCLNT_E_DEVICE_INVALIDATED changed");
static_assert(hr::AudclntUnsupportedFormat == static_cast<int32_t>(AUDCLNT_E_UNSUPPORTED_FORMAT), "AUDCLNT_E_UNSUPPORTED_FORMAT changed");
static_assert(hr::AudclntServiceNotRunning == static_cast<int32_t>(AUDCLNT_E_SERVICE_NOT_RUNNING), "AUDCLNT_E_SERVICE_NOT_RUNNING changed");
static_assert(hr::AudclntInvalidStreamFlag == static_cast<int32_t>(AUDCLNT_E_INVALID_STREAM_FLAG), "AUDCLNT_E_INVALID_STREAM_FLAG changed");
static_assert(hr::AudclntEventhandleNotSet == static_cast<int32_t>(AUDCLNT_E_EVENTHANDLE_NOT_SET), "AUDCLNT_E_EVENTHANDLE_NOT_SET changed");
static_assert(hr::AudclntResourcesInvalidated == static_cast<int32_t>(AUDCLNT_E_RESOURCES_INVALIDATED), "AUDCLNT_E_RESOURCES_INVALIDATED changed");

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring toWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string hresultName(HRESULT hr) {
    // The portable table knows the COM / generic codes and, more importantly,
    // what they mean (see native/src/hr_advice.cc). The table below only adds
    // the AUDCLNT_E_* values it does not carry, so there is exactly one place
    // where a code gets an explanation.
    {
        const HresultAdvice advice = adviseHresult(static_cast<int32_t>(hr));
        if (!advice.name.empty()) return advice.name;
    }

    struct Entry {
        HRESULT code;
        const char* name;
    };
    // FormatMessage does not know the audio specific codes, and a name is what
    // a user can search for.
    static const Entry table[] = {
        { AUDCLNT_E_NOT_INITIALIZED, "AUDCLNT_E_NOT_INITIALIZED" },
        { AUDCLNT_E_ALREADY_INITIALIZED, "AUDCLNT_E_ALREADY_INITIALIZED" },
        { AUDCLNT_E_WRONG_ENDPOINT_TYPE, "AUDCLNT_E_WRONG_ENDPOINT_TYPE" },
        { AUDCLNT_E_DEVICE_INVALIDATED, "AUDCLNT_E_DEVICE_INVALIDATED" },
        { AUDCLNT_E_NOT_STOPPED, "AUDCLNT_E_NOT_STOPPED" },
        { AUDCLNT_E_BUFFER_TOO_LARGE, "AUDCLNT_E_BUFFER_TOO_LARGE" },
        { AUDCLNT_E_OUT_OF_ORDER, "AUDCLNT_E_OUT_OF_ORDER" },
        { AUDCLNT_E_UNSUPPORTED_FORMAT, "AUDCLNT_E_UNSUPPORTED_FORMAT" },
        { AUDCLNT_E_INVALID_SIZE, "AUDCLNT_E_INVALID_SIZE" },
        { AUDCLNT_E_DEVICE_IN_USE, "AUDCLNT_E_DEVICE_IN_USE" },
        { AUDCLNT_E_BUFFER_OPERATION_PENDING, "AUDCLNT_E_BUFFER_OPERATION_PENDING" },
        { AUDCLNT_E_THREAD_NOT_REGISTERED, "AUDCLNT_E_THREAD_NOT_REGISTERED" },
        { AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED" },
        { AUDCLNT_E_ENDPOINT_CREATE_FAILED, "AUDCLNT_E_ENDPOINT_CREATE_FAILED" },
        { AUDCLNT_E_SERVICE_NOT_RUNNING, "AUDCLNT_E_SERVICE_NOT_RUNNING" },
        { AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED, "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED" },
        { AUDCLNT_E_EXCLUSIVE_MODE_ONLY, "AUDCLNT_E_EXCLUSIVE_MODE_ONLY" },
        { AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL, "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL" },
        { AUDCLNT_E_EVENTHANDLE_NOT_SET, "AUDCLNT_E_EVENTHANDLE_NOT_SET" },
        { AUDCLNT_E_INCORRECT_BUFFER_SIZE, "AUDCLNT_E_INCORRECT_BUFFER_SIZE" },
        { AUDCLNT_E_BUFFER_SIZE_ERROR, "AUDCLNT_E_BUFFER_SIZE_ERROR" },
        { AUDCLNT_E_CPUUSAGE_EXCEEDED, "AUDCLNT_E_CPUUSAGE_EXCEEDED" },
        { AUDCLNT_E_BUFFER_ERROR, "AUDCLNT_E_BUFFER_ERROR" },
        { AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED, "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED" },
        { AUDCLNT_E_INVALID_DEVICE_PERIOD, "AUDCLNT_E_INVALID_DEVICE_PERIOD" },
        { AUDCLNT_E_INVALID_STREAM_FLAG, "AUDCLNT_E_INVALID_STREAM_FLAG" },
        { AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE, "AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE" },
        { AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES, "AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES" },
        { AUDCLNT_E_OFFLOAD_MODE_ONLY, "AUDCLNT_E_OFFLOAD_MODE_ONLY" },
        { AUDCLNT_E_NONOFFLOAD_MODE_ONLY, "AUDCLNT_E_NONOFFLOAD_MODE_ONLY" },
        { AUDCLNT_E_RESOURCES_INVALIDATED, "AUDCLNT_E_RESOURCES_INVALIDATED" },
        { AUDCLNT_E_RAW_MODE_UNSUPPORTED, "AUDCLNT_E_RAW_MODE_UNSUPPORTED" },
        { AUDCLNT_E_ENGINE_PERIODICITY_LOCKED, "AUDCLNT_E_ENGINE_PERIODICITY_LOCKED" },
        { AUDCLNT_E_ENGINE_FORMAT_LOCKED, "AUDCLNT_E_ENGINE_FORMAT_LOCKED" },
        { AUDCLNT_S_BUFFER_EMPTY, "AUDCLNT_S_BUFFER_EMPTY" },
    };
    for (const auto& entry : table) {
        if (entry.code == hr) return entry.name;
    }

    switch (hr) {
        case S_OK: return "S_OK";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_NOTIMPL: return "E_NOTIMPL";
        case E_NOINTERFACE: return "E_NOINTERFACE";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        case E_POINTER: return "E_POINTER";
        case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
        case E_FAIL: return "E_FAIL";
        case REGDB_E_CLASSNOTREG: return "REGDB_E_CLASSNOTREG";
        case RPC_E_CHANGED_MODE: return "RPC_E_CHANGED_MODE";
        case CO_E_NOTINITIALIZED: return "CO_E_NOTINITIALIZED";
        default: break;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

/** The text the OS itself has for the code, empty when it has none. */
std::string systemMessage(HRESULT hr) {
    wchar_t* message = nullptr;
    std::string result;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    if (length && message) {
        std::wstring text(message, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
        // The bare text only: both callers add their own separator, and returning
        // "— text" here produced "NAME — — text" in one place and "the system says:  — text"
        // in the other.
        result = toUtf8(text);
    }
    if (message) LocalFree(message);
    return result;
}

std::string hresultMessage(HRESULT hr) {
    std::string result = hresultName(hr);
    const std::string localized = systemMessage(hr);
    if (!localized.empty()) result += " — " + localized;
    return result;
}

std::string formatHresult(HRESULT hr, const std::string& operation) {
    const HresultAdvice advice = adviseHresult(static_cast<int32_t>(hr));
    std::string result = operation + ": " + hresultName(hr) + " (" + hresultHex(static_cast<int32_t>(hr)) + ")";
    if (!advice.advice.empty()) result += "\n  what it means: " + advice.advice;
    const std::string localized = systemMessage(hr);
    if (!localized.empty()) result += "\n  the system says: " + localized;
    return result;
}

// ------------------------------------------------------------------- version

OsVersion osVersion() {
    OsVersion info;
    // GetVersionEx lies without an application manifest, RtlGetVersion does not.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion) {
            RTL_OSVERSIONINFOW version = {};
            version.dwOSVersionInfoSize = sizeof(version);
            if (rtlGetVersion(&version) == 0) {
                info.major = version.dwMajorVersion;
                info.minor = version.dwMinorVersion;
                info.build = version.dwBuildNumber;
            }
        }
    }

    wchar_t value[128] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName",
            RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        info.productName = value;
    }
    return info;
}

std::string describePlatform() {
    const OsVersion info = osVersion();
    std::string name = toUtf8(info.productName);
    if (name.empty()) name = "Windows";
    return name + " (build " + std::to_string(info.build) + ")";
}

// ------------------------------------------------------------------ processes

std::vector<ProcessNode> processTable() {
    std::vector<ProcessNode> result;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessNode node;
            node.pid = entry.th32ProcessID;
            node.parentPid = entry.th32ParentProcessID;
            result.push_back(node);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

namespace {

/** Opens a process for metadata queries only; never needs admin rights. */
HANDLE openForQuery(uint32_t pid) {
    if (pid == 0) return nullptr;
    return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

} // namespace

std::string processImagePath(uint32_t pid) {
    HANDLE process = openForQuery(pid);
    if (!process) return {};

    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::string result;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        result = toUtf8(std::wstring(buffer, size));
    }
    CloseHandle(process);
    return result;
}

std::string applicationUserModelId(uint32_t pid) {
    HANDLE process = openForQuery(pid);
    if (!process) return {};

    UINT32 length = 0;
    std::string result;
    if (GetApplicationUserModelId(process, &length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length > 0) {
        std::wstring buffer(length, L'\0');
        if (GetApplicationUserModelId(process, &length, buffer.data()) == ERROR_SUCCESS) {
            while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
            result = toUtf8(buffer);
        }
    }
    CloseHandle(process);
    return result;
}

std::string fileDescription(const std::string& path) {
    if (path.empty()) return {};

    const std::wstring widePath = toWide(path);
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(widePath.c_str(), &handle);
    if (size == 0) return {};

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(widePath.c_str(), handle, size, data.data())) return {};

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT translationSize = 0;

    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translations), &translationSize)
        && translationSize >= sizeof(Translation)) {
        const char* fields[] = { "FileDescription", "ProductName" };
        for (const char* field : fields) {
            wchar_t query[128];
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%hs", translations[0].language, translations[0].codePage, field);

            wchar_t* value = nullptr;
            UINT valueSize = 0;
            if (VerQueryValueW(data.data(), query, reinterpret_cast<LPVOID*>(&value), &valueSize) && value && valueSize > 1) {
                const std::string text = toUtf8(std::wstring(value, valueSize - 1));
                if (!text.empty()) return text;
            }
        }
    }
    return {};
}

std::string fileNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ------------------------------------------------------------------ mix format

bool defaultMixFormat(WAVEFORMATEX** format) {
    *format = nullptr;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) return false;

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put())) || !device) return false;

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid())) || !client) return false;

    return SUCCEEDED(client->GetMixFormat(format)) && *format != nullptr;
}

// ---------------------------------------------------------------------- probe

namespace {

/**
 * Minimum build that is known to implement process loopback.
 *
 * Microsoft documents the API as "Windows 10 Build 20348" (Server 2022), which
 * is the build the public contract was written against. In practice the audio
 * engine side has been present since Windows 10 version 2004 (build 19041):
 * that is the build OBS requires for its "Application Audio Capture" source and
 * the build every third party process loopback implementation relies on.
 *
 * Because both statements can only be *verified* by asking the operating
 * system, nothing below decides by version number: the walk performs a real
 * activation and reports the HRESULT of the stage that failed. The build number
 * is context for the user, never the verdict.
 */
constexpr uint32_t kProcessLoopbackSinceBuild = 19041;  // Windows 10, version 2004
constexpr uint32_t kDocumentedBuild = 20348;            // Windows Server 2022 / Windows 11 SDK

// Every step of the walk below is bounded: a diagnostics call has to return.
constexpr DWORD kProbeActivationTimeoutMs = 5000;  // wait for ActivateCompleted
constexpr DWORD kProbePacketTimeoutMs = 150;       // wait for a capture packet
constexpr int kProbeListenMs = 150;                // how long packets are read

/** A number with a fixed number of decimals, for the log lines. */
std::string decimal(double value, int digits) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

/** Signals the completion event when the activation finished. */
class ProbeActivationHandler : public ComCallback<IActivateAudioInterfaceCompletionHandler, &IID_IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ProbeActivationHandler(HANDLE completed) : completed_(completed) {}

    /**
     * Called by COM from a worker thread of the multi threaded apartment, which
     * is exactly the call that produces E_ILLEGAL_METHOD_CALL (0x8000000E) when
     * the handler is not an agile object (see the comment in com_ptr.h). Nothing
     * but an event is touched here, because the thread is not ours.
     */
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(completed_);
        return S_OK;
    }

private:
    HANDLE completed_;
};

/** Labelled steps, so a failure always names the stage it happened in. */
class StageLog {
public:
    void note(const std::string& stage, const std::string& text) {
        text_ += "[" + stage + "] " + text + "\n";
    }

    void failed(const std::string& stage, HRESULT hr, const std::string& what) {
        note(stage, "FAILED — " + what);
        note(stage, describeHresult(static_cast<int32_t>(hr)));
    }

    const std::string& text() const { return text_; }

private:
    std::string text_;
};

/** The outcome of the walk, as the UI and the documentation need it. */
struct ProbeRun {
    /** The whole walk succeeded: this system really provides the capture. */
    bool supported = false;
    /** Packets with actual samples arrived — the capture is not merely "open". */
    bool pcmArrived = false;
    /** The control activation without a process filter succeeded. */
    bool engineWorks = false;
    /** Stage by stage outcome, including the exact HRESULT of the failure. */
    std::string stages;
};

std::string formatOf(const WAVEFORMATEX* format) {
    if (!format) return "unknown";
    return std::to_string(format->nSamplesPerSec) + " Hz / " + std::to_string(format->nChannels) + " channels / "
        + std::to_string(format->wBitsPerSample) + " bit";
}

/**
 * Measures one packet with the shared, portable and tested implementation
 * (native/src/dsp/packet_measure.h). Nothing is stored and nothing is written to
 * disk: two numbers are updated as the packet is read.
 */
void measurePacket(const BYTE* data, UINT32 frames, int channels, bool floatData, saf::dsp::SampleStats& stats) {
    if (!data || frames == 0 || channels <= 0) return;
    const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    if (floatData) {
        saf::dsp::measureFloats(reinterpret_cast<const float*>(data), count, stats);
    } else {
        saf::dsp::measureInt16(reinterpret_cast<const int16_t*>(data), count, stats);
    }
}

/**
 * One ActivateAudioInterfaceAsync, with every distinct failure it can produce in
 * a field of its own: the call, the callback, GetActivateResult, and the result
 * the activation itself produced. Collapsing those four into "it failed" is what
 * made the original problem invisible.
 */
class ActivationAttempt {
public:
    explicit ActivationAttempt(HANDLE completed)
        : completed_(completed)
        , handler_(new ProbeActivationHandler(completed)) {}

    bool run(REFIID riid, PROPVARIANT* params) {
        // Declaration order is the destruction order in reverse: the operation is
        // released first, the handler last, because Windows holds a reference to
        // the handler until the operation is gone.
        call = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, riid, params, handler_.get(), operation_.put());
        if (FAILED(call)) return false;

        if (WaitForSingleObject(completed_, kProbeActivationTimeoutMs) != WAIT_OBJECT_0) return false;
        callbackFired = true;

        resultCall = operation_->GetActivateResult(&activation, object.put());
        return SUCCEEDED(resultCall) && SUCCEEDED(activation) && object.get() != nullptr;
    }

    /** Does the handler passed to the API answer IAgileObject? */
    bool handlerIsAgile() const { return handler_->isAgile(); }

    std::string handlerAddress() const {
        char text[32];
        std::snprintf(text, sizeof(text), "%p", static_cast<const void*>(handler_.get()));
        return text;
    }

    HRESULT call = E_FAIL;           // ActivateAudioInterfaceAsync itself
    bool callbackFired = false;      // did ActivateCompleted run?
    HRESULT resultCall = E_FAIL;     // GetActivateResult itself
    HRESULT activation = E_FAIL;     // the result the activation produced
    ComPtr<IUnknown> object;         // the activated object

private:
    HANDLE completed_ = nullptr;
    ComPtr<ProbeActivationHandler> handler_;
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation_;
};

/** The activation without a process filter: available on any Windows version. */
bool controlActivation(StageLog& log) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;

    ActivationAttempt attempt(event);
    PROPVARIANT empty = {};
    const bool ok = attempt.run(__uuidof(IAudioClient), &empty);

    if (ok) {
        log.note("control", "the same activation without a process filter succeeded: the audio engine and the "
                            "asynchronous activation path work");
    } else if (FAILED(attempt.call)) {
        log.note("control", "the activation without a process filter failed in the call itself: "
            + hresultName(attempt.call) + " — the asynchronous activation path is broken on this system, "
            "independently of per process capture");
    } else if (!attempt.callbackFired) {
        log.note("control", "the activation without a process filter timed out: no completion callback");
    } else {
        log.note("control", "the activation without a process filter was refused: "
            + describeHresult(static_cast<int32_t>(SUCCEEDED(attempt.resultCall) ? attempt.activation : attempt.resultCall)));
    }

    CloseHandle(event);
    return ok;
}

/** The whole walk, on the thread that calls it (a dedicated MTA thread). */
ProbeRun walkProcessLoopback() {
    ProbeRun run;
    StageLog log;

    // ---------------------------------------------------------------- stage 1
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(com);
    const HRESULT comExpected = com == RPC_E_CHANGED_MODE ? S_OK : com;
    if (com == S_OK) {
        log.note("com", "CoInitializeEx(COINIT_MULTITHREADED) on a dedicated probe thread: S_OK");
    } else if (com == S_FALSE) {
        log.note("com", "CoInitializeEx(COINIT_MULTITHREADED): S_FALSE — the thread was already in a multi threaded "
                        "apartment");
    } else if (com == RPC_E_CHANGED_MODE) {
        log.note("com", "CoInitializeEx(COINIT_MULTITHREADED): RPC_E_CHANGED_MODE — the thread is in a single threaded "
                        "apartment, so COM has to marshal the completion callback. That is legal for an agile handler "
                        "and illegal without one, which is the second way to get E_ILLEGAL_METHOD_CALL");
    }

    if (FAILED(comExpected)) {
        log.failed("com", comExpected, "COM could not be initialised on the probe thread");
        run.stages = log.text();
        return run;
    }

    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) {
        log.failed("com", E_FAIL, "the synchronisation event of the probe could not be created");
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }

    // ---------------------------------------------------------------- stage 2
    ActivationAttempt attempt(completed);
    log.note("handler", std::string("completion handler at ") + attempt.handlerAddress()
        + " answers IAgileObject: " + (attempt.handlerIsAgile() ? "yes" : "NO"));
    if (!attempt.handlerIsAgile()) {
        log.note("handler", "without an agile handler Windows refuses the call below with E_ILLEGAL_METHOD_CALL "
                            "(0x8000000E) before it looks at the process, the format or the Windows build");
    }

    // ---------------------------------------------------------------- stage 3
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    log.note("params", "target process " + std::to_string(params.ProcessLoopbackParams.TargetProcessId)
        + " (this process), mode PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE, activation payload "
        + std::to_string(sizeof(params)) + " bytes as VT_BLOB");

    // ---------------------------------------------------------------- stage 4
    attempt.run(__uuidof(IAudioClient), &activateParams);

    if (FAILED(attempt.call)) {
        log.failed("activation-call", attempt.call, "ActivateAudioInterfaceAsync refused the request before anything "
                                                    "was activated");
        if (attempt.call == hr::IllegalMethodCall && attempt.handlerIsAgile()) {
            log.note("activation-call", "the plugin's handler *is* agile, so this is not the usual non-agile handler "
                                        "case: the apartment of the caller is the remaining candidate (see the com "
                                        "stage above)");
        }
        run.engineWorks = controlActivation(log);
        CloseHandle(completed);
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }

    log.note("activation-call", std::string("ActivateAudioInterfaceAsync accepted the request (") + hresultName(attempt.call) + ")");

    // ---------------------------------------------------------------- stage 5
    if (!attempt.callbackFired) {
        log.note("activation-wait", "the completion callback did not fire within "
            + std::to_string(kProbeActivationTimeoutMs) + " ms — reported as a stage of its own, not as a generic "
            "failure");
        run.engineWorks = controlActivation(log);
        CloseHandle(completed);
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }

    // ---------------------------------------------------------------- stage 6
    if (FAILED(attempt.resultCall)) {
        log.failed("activate-result", attempt.resultCall, "GetActivateResult itself failed — the asynchronous operation "
                                                         "never produced a result");
    } else if (FAILED(attempt.activation)) {
        log.failed("activate-result", attempt.activation, "the activation completed with an error (GetActivateResult "
                                                          "itself succeeded, the result it reported is the failure)");
    } else {
        log.note("activate-result", "GetActivateResult: S_OK, activation result: S_OK");
    }

    if (FAILED(attempt.activation) || !attempt.object) {
        run.engineWorks = controlActivation(log);
        CloseHandle(completed);
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }

    // ---------------------------------------------------------------- stage 7
    ComPtr<IAudioClient> client;
    const HRESULT interfaceHr = attempt.object->QueryInterface(IID_PPV_ARGS(client.put()));
    if (FAILED(interfaceHr) || !client) {
        log.failed("interface", interfaceHr, "the activated object does not expose IAudioClient");
        run.engineWorks = controlActivation(log);
        CloseHandle(completed);
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }
    log.note("interface", "the activated object exposes IAudioClient (QueryInterface: S_OK)");

    // ---------------------------------------------------------------- stage 8
    // The process loopback client is not an endpoint client, so its format is
    // negotiated: ask for 48 kHz float stereo and let the engine convert
    // (AUTOCONVERTPCM), then fall back to the endpoint mix format.
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    WAVEFORMATEXTENSIBLE preferred;
    makeFloatFormat(preferred, 48000, 2);

    // What the engine agreed to, so the packets below can be read correctly.
    const WAVEFORMATEX* negotiated = nullptr;
    bool captureIsFloat = false;

    HRESULT initHr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0,
        reinterpret_cast<WAVEFORMATEX*>(&preferred), nullptr);
    if (SUCCEEDED(initHr)) {
        negotiated = reinterpret_cast<const WAVEFORMATEX*>(&preferred);
        captureIsFloat = true;
        log.note("client-init", "200000 = 20 ms buffer accepted with 48 kHz / 2 channels / 32 bit float "
                                "(AUTOCONVERTPCM lets the engine convert)");
    } else {
        log.note("client-init", "48 kHz float stereo was refused: " + hresultName(initHr) + " ("
            + hresultHex(static_cast<int32_t>(initHr)) + ") — retrying with the endpoint mix format");
        WAVEFORMATEX* mixFormat = nullptr;
        if (defaultMixFormat(&mixFormat) && mixFormat) {
            initHr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, mixFormat, nullptr);
            if (SUCCEEDED(initHr)) {
                negotiated = mixFormat;
                captureIsFloat = mixFormat->wBitsPerSample == 32;
                log.note("client-init", "the endpoint mix format was accepted: " + formatOf(mixFormat));
            }
            CoTaskMemFree(mixFormat);
        } else {
            log.note("client-init", "the endpoint mix format could not be read");
        }
    }

    if (FAILED(initHr)) {
        log.failed("client-init", initHr, "no format was accepted by the audio engine");
        run.engineWorks = controlActivation(log);
        CloseHandle(completed);
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }

    // ---------------------------------------------------------------- stage 9
    ComPtr<IAudioCaptureClient> captureClient;
    HRESULT hr = client->GetService(IID_PPV_ARGS(captureClient.put()));
    if (FAILED(hr) || !captureClient) {
        log.failed("capture-client", hr, "IAudioCaptureClient is not available on this client");
        run.engineWorks = controlActivation(log);
        CloseHandle(completed);
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }

    // GetBufferSize is not meaningful for a process loopback client - the value is
    // not a packet size, and on some systems it is large nonsense (the standalone
    // probe reported hundreds of millions of frames). It is therefore only printed
    // when it is plausible, and it is never used to size or pace anything: the
    // stream is paced by the completion event.
    UINT32 bufferFrames = 0;
    const HRESULT sizeHr = client->GetBufferSize(&bufferFrames);
    const UINT32 saneLimit = 48000u * 4;  // 4 seconds at the format we ask for
    if (FAILED(sizeHr)) {
        log.note("buffer-size", "the engine does not report a packet size: " + hresultName(sizeHr));
    } else if (bufferFrames == 0 || bufferFrames > saneLimit) {
        log.note("buffer-size", "the engine reported " + std::to_string(bufferFrames)
            + " frames, which is not a usable packet size for this kind of client - treated as unknown, not as a latency");
    } else {
        log.note("buffer-size", std::to_string(bufferFrames) + " frames per packet");
    }

    // --------------------------------------------------------------- stage 10
    hr = client->SetEventHandle(completed);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        log.failed("capture-start", hr, "the capture stream could not be started");
        run.engineWorks = controlActivation(log);
        CloseHandle(completed);
        if (comOwned) CoUninitialize();
        run.stages = log.text();
        return run;
    }
    log.note("capture-start", "SetEventHandle + Start: the stream is running");

    // --------------------------------------------------------------- stage 11
    // Read what the engine delivers for a moment. Audio is counted, never
    // stored: nothing is written to disk anywhere in this file.
    uint64_t frames = 0;
    uint64_t silentFrames = 0;
    saf::dsp::SampleStats stats;
    int packets = 0;
    const int channels = negotiated ? negotiated->nChannels : 0;
    const ULONGLONG deadline = GetTickCount64() + kProbeListenMs;
    while (GetTickCount64() < deadline) {
        WaitForSingleObject(completed, kProbePacketTimeoutMs);

        UINT32 available = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&available)) && available > 0) {
            BYTE* data = nullptr;
            UINT32 count = 0;
            DWORD packetFlags = 0;
            if (FAILED(captureClient->GetBuffer(&data, &count, &packetFlags, nullptr, nullptr))) break;
            ++packets;
            frames += count;
            if (packetFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silentFrames += count;
            } else {
                measurePacket(data, count, channels, captureIsFloat, stats);
            }
            captureClient->ReleaseBuffer(count);
        }
    }

    // "A packet arrived" is not "audio arrived", and "samples are not zero" is not
    // "audio arrived" either: the noise floor of the capture path is made of samples
    // that are not exactly zero. The level decides, and the same classifier the
    // standalone probe uses decides it (saf::dsp::classifyContent).
    run.pcmArrived = packets > 0 && stats.carriesAudio();
    const bool audible = saf::dsp::classifyContent(stats) == saf::dsp::PacketContent::Audio;
    if (packets == 0) {
        log.note("data", "no packet arrived within " + std::to_string(kProbeListenMs) + " ms — the stream exists, the "
                         "engine delivered nothing (silence of the captured process, or a paused source)");
    } else if (audible) {
        log.note("data", std::to_string(packets) + " packet(s), " + std::to_string(frames) + " frames in "
            + std::to_string(kProbeListenMs) + " ms, " + std::to_string(silentFrames) + " of them flagged silent, "
            + std::to_string(stats.nonzero) + " non-zero samples, peak " + decimal(saf::dsp::dbfs(stats.peak), 1) + " dBFS — audio "
            "is in the window");
    } else if (run.pcmArrived) {
        log.note("data", std::to_string(packets) + " packet(s), " + std::to_string(frames) + " frames in "
            + std::to_string(kProbeListenMs) + " ms, " + std::to_string(stats.nonzero) + " non-zero samples, but the "
            "peak is only " + decimal(saf::dsp::dbfs(stats.peak), 1) + " dBFS: what arrived is the noise floor of the capture "
            "path, not audio — the stream works, nothing audible was playing");
    } else {
        log.note("data", std::to_string(packets) + " packet(s), " + std::to_string(frames) + " frames in "
            + std::to_string(kProbeListenMs) + " ms, " + std::to_string(silentFrames) + " of them flagged silent — "
            "the stream carries silence (all samples are zero), which says nothing about support: the probed process "
            "was simply not playing anything");
    }

    // --------------------------------------------------------------- stage 12
    client->Stop();
    run.supported = true;
    log.note("teardown", "Stop + release of every interface, COM apartment closed");

    run.engineWorks = controlActivation(log);

    CloseHandle(completed);
    if (comOwned) CoUninitialize();
    run.stages = log.text();
    return run;
}

/** Runs the walk on a thread of its own, so the caller's apartment cannot matter. */
ProbeRun runProbeOnOwnThread() {
    ProbeRun run;
    std::thread worker([&run]() { run = walkProcessLoopback(); });
    worker.join();
    return run;
}

} // namespace

/** What the build number means for process loopback (context, not a verdict). */
std::string buildCheckText(uint32_t build) {
    if (build == 0) return "The Windows version could not be determined — only a real activation attempt gives a verdict";
    if (build < kProcessLoopbackSinceBuild) {
        return "Windows build " + std::to_string(build) + " (< " + std::to_string(kProcessLoopbackSinceBuild)
            + ", Windows 10 before version 2004): there is no per process capture";
    }
    if (build < kDocumentedBuild) {
        return "Windows build " + std::to_string(build) + " (Windows 10 version 2004 and newer): per process capture "
            "works, even though the documentation names build " + std::to_string(kDocumentedBuild);
    }
    return "Windows build " + std::to_string(build) + " >= " + std::to_string(kDocumentedBuild)
        + ": per process capture in the documentation and in practice";
}

SupportInfo probeSupport() {
    SupportInfo info;
    const OsVersion os = osVersion();
    info.os = describePlatform();
    info.build = os.build;

    // The walk runs on a thread of its own: the caller's COM apartment must not
    // decide whether this probe can work.
    const ProbeRun run = runProbeOnOwnThread();
    info.detail = run.stages;
    info.supported = run.supported;

    if (run.supported) {
        info.reason = "Capturing the audio of individual applications is available on this system — verified by a real "
                      "activation, not by a version number.";
        info.checks.push_back("Per process capture activated: success (ActivateAudioInterfaceAsync -> S_OK, "
                              "IAudioClient obtained)");
        info.checks.push_back("IAudioClient.Initialize + Start: success");
        info.checks.push_back(run.pcmArrived
                ? "The capture delivered packets during the short test window — the audio path works; whether "
                  "anything audible was playing is a different question, and the standalone probe's [levels] lines "
                  "answer it for a chosen application"
                : "No samples arrived during the short test window — expected when the probed process plays nothing; "
                  "the standalone probe can be pointed at an application that does play audio");
        info.checks.push_back(run.engineWorks
                ? "The activation without a process filter works too: the audio engine and the asynchronous activation "
                  "path are healthy"
                : "The activation without a process filter did not work either — that points at the audio service or the "
                  "activation path, not at per process capture");
        info.checks.push_back(buildCheckText(os.build));
        info.checks.push_back("No separate output device is needed: the capture follows the process");
        info.checks.push_back("Administrator rights are not needed");
    } else if (os.build != 0 && os.build < kProcessLoopbackSinceBuild) {
        info.reason = "Windows build " + std::to_string(os.build) + " is older than " + std::to_string(kProcessLoopbackSinceBuild)
            + " (Windows 10 version 2004): this version has no per process audio capture, in any application. "
              "Updating Windows to 2004 or newer enables filtering.";
        info.checks.push_back("Real activation of a per process capture: refused");
        info.checks.push_back(buildCheckText(os.build));
        info.checks.push_back("Capturing all system audio is available, but applications cannot be filtered in it");
        info.checks.push_back("The plugin will not replace filtering with other means (virtual devices, routing) — the "
                              "requirements forbid it");
    } else {
        info.reason = "A per process audio capture could not be activated even though this Windows build supports it. "
                      "The stage log above names the step that failed and its exact HRESULT — the version of the system "
                      "is not the reason.";
        info.checks.push_back("Real activation of a per process capture: refused (stage log above)");
        info.checks.push_back(buildCheckText(os.build));
        info.checks.push_back(run.engineWorks
                ? "The activation without a process filter works: the audio engine itself is fine, so the failure is "
                  "specific to the per process activation"
                : "The activation without a process filter failed too: this is not specific to per process capture");
        info.checks.push_back("Capturing all system audio is available, but applications cannot be filtered in it");
        info.checks.push_back("The plugin will not replace filtering with other means (virtual devices, routing) — the "
                              "requirements forbid it");
    }
    return info;
}

} // namespace saf::win
