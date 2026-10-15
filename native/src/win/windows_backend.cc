// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows_backend.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "../app_identity.h"
#include "../dsp/convert.h"
#include "../dsp/resampler.h"
#include "../json.h"
#include "com_ptr.h"

namespace saf::win {

namespace {

constexpr int kTargetChannels = 2;

/** 20 ms: low latency, plenty of room for the scheduler. */
constexpr REFERENCE_TIME kBufferDuration = 200000;

std::string narrow(const wchar_t* value) {
    return value ? toUtf8(value) : std::string();
}

} // namespace

// ------------------------------------------------------------------ strings

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
    struct Entry {
        HRESULT code;
        const char* name;
    };
    // The audio specific codes are not all covered by FormatMessage, and a name
    // is what a user can search for.
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
        { AUDCLNT_E_HEADTRACKING_ENABLED, "AUDCLNT_E_HEADTRACKING_ENABLED" },
        { AUDCLNT_E_HEADTRACKING_UNSUPPORTED, "AUDCLNT_E_HEADTRACKING_UNSUPPORTED" },
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

std::string hresultMessage(HRESULT hr) {
    std::string result = hresultName(hr);

    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    if (length && message) {
        std::wstring text(message, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
        result += " — " + toUtf8(text);
    }
    if (message) LocalFree(message);
    return result;
}

std::string formatHresult(HRESULT hr, const std::string& operation) {
    return operation + ": " + hresultMessage(hr);
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
    std::string name = narrow(info.productName.c_str());
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

/** "Google Chrome" instead of chrome.exe, taken from the version resource. */
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
    std::string result;

    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translations), &translationSize)
        && translationSize >= sizeof(Translation)) {
        const char* fields[] = { "FileDescription", "ProductName" };
        for (const char* field : fields) {
            wchar_t query[128];
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%hs", translations[0].language, translations[0].codePage, field);

            wchar_t* value = nullptr;
            UINT valueSize = 0;
            if (VerQueryValueW(data.data(), query, reinterpret_cast<LPVOID*>(&value), &valueSize) && value && valueSize > 1) {
                result = toUtf8(std::wstring(value, valueSize - 1));
                if (!result.empty()) return result;
            }
        }
    }
    return result;
}

std::string fileNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

// ------------------------------------------------------- session enumeration

namespace {

/**
 * Aggregates audio sessions of all active render endpoints into a cached
 * snapshot, refreshed from a dedicated MTA worker thread. The audio session
 * API delivers its callback on the COM thread that registered it, so keeping
 * that thread under our control avoids depending on the host's apartment.
 */
class SessionEnumerator : public IAppEnumerator {
public:
    explicit SessionEnumerator(std::string& error) {
        error.clear();
    }

    ~SessionEnumerator() override { stop(); }

    bool start(std::string& error) override {
        if (running_.exchange(true)) return true;

        std::promise<std::string> ready;
        auto future = ready.get_future();
        thread_ = std::thread([this, &ready] { threadMain(std::move(ready)); });

        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            error = "тайм-аут инициализации перечисления аудиосессий";
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
        if (thread_.joinable()) thread_.join();

        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        processes_.clear();
        manager_.reset();
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

    std::string describe() const override { return "WASAPI (IAudioSessionManager2, все активные устройства вывода)"; }

private:
    /** Receives OnSessionCreated, which is what makes mode A work live. */
    class SessionNotification : public ComCallback<IAudioSessionNotification, &IID_IAudioSessionNotification> {
    public:
        explicit SessionNotification(SessionEnumerator* owner) : owner_(owner) {}
        HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
            owner_->changed_ = true;
            return S_OK;
        }

    private:
        SessionEnumerator* owner_;
    };

    void threadMain(std::promise<std::string> ready);

    /** Re-reads sessions from all active render endpoints. */
    bool refresh(std::string& error);
    void collectEndpoint(IAudioSessionManager2* manager);

    std::atomic<bool> running_ { false };
    std::atomic<bool> changed_ { false };
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<SessionInfo> sessions_;
    std::vector<ProcessNode> processes_;

    ComPtr<IMMDeviceEnumerator> deviceEnumerator_;
    ComPtr<IAudioSessionManager2> manager_;
    ComPtr<SessionNotification> notification_;
    std::string endpointId_;
};

void SessionEnumerator::threadMain(std::promise<std::string> ready) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comResult);

    std::string error;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(deviceEnumerator_.put()));
    if (FAILED(hr)) {
        error = formatHresult(hr, "не удалось создать MMDeviceEnumerator");
    }

    if (error.empty()) {
        notification_ = ComPtr<SessionNotification>(new SessionNotification(this));
        refresh(error);
    }

    ready.set_value(error);
    if (!error.empty()) {
        if (comOwned) CoUninitialize();
        return;
    }

    auto lastFullRefresh = std::chrono::steady_clock::now();
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        // Session creation is event driven, state changes (an application that
        // stopped playing) are picked up by this cheap periodic refresh.
        if (std::chrono::steady_clock::now() - lastFullRefresh >= std::chrono::milliseconds(1000)) {
            std::string refreshError;
            refresh(refreshError);
            lastFullRefresh = std::chrono::steady_clock::now();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (manager_) manager_->UnregisterSessionNotification(notification_.get());
    }
    manager_.reset();
    deviceEnumerator_.reset();
    if (comOwned) CoUninitialize();
}

void SessionEnumerator::collectEndpoint(IAudioSessionManager2* manager) {
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

        // Session peak meter, when the session exposes one. It is optional: the
        // UI falls back to the capture level and the session state.
        ComPtr<IAudioMeterInformation> meter;
        if (SUCCEEDED(control2->QueryInterface(IID_PPV_ARGS(meter.put()))) && meter) {
            float peak = 0.0f;
            if (SUCCEEDED(meter->GetPeakValue(&peak))) info.peak = peak;
        }

        wchar_t* sessionId = nullptr;
        if (SUCCEEDED(control2->GetSessionIdentifier(&sessionId)) && sessionId) {
            info.sessionId = toUtf8(sessionId);
            CoTaskMemFree(sessionId);
        }
        if (info.sessionId.empty()) info.sessionId = std::to_string(pid) + ":" + std::to_string(i);

        if (info.isSystemSounds) {
            info.displayName = "Звуки системы";
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

        // Applications without any way to identify them (protected processes)
        // can still be captured, but their rule can not survive a restart.
        if (info.exePath.empty() && aumid.empty() && !info.isSystemSounds) {
            info.notCapturableReason = "не удалось определить исполняемый файл процесса";
        }

        sessions_.push_back(std::move(info));
    }
}

bool SessionEnumerator::refresh(std::string& error) {
    ComPtr<IMMDeviceCollection> devices;
    HRESULT hr = deviceEnumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put());
    if (FAILED(hr)) {
        error = formatHresult(hr, "не удалось перечислить устройства вывода");
        return false;
    }

    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) {
        error = "не удалось получить количество устройств вывода";
        return false;
    }

    std::vector<SessionInfo> collected;
    std::vector<std::string> endpointIds;

    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, device.put())) || !device) continue;

        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            endpointIds.push_back(toUtf8(id));
            CoTaskMemFree(id);
        }

        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid())) || !manager) continue;

        // Register notifications on the default endpoint only: it is the one
        // applications start playing on in practice, and notifications on every
        // endpoint would be redundant.
        const bool isDefault = i == 0;
        if (isDefault) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!manager_ || endpointIds.back() != endpointId_) {
                if (manager_) manager_->UnregisterSessionNotification(notification_.get());
                manager_ = std::move(manager);
                manager_->RegisterSessionNotification(notification_.get());
                endpointId_ = endpointIds.back();
                std::lock_guard<std::mutex> sessionsLock(mutex_);
                (void)sessionsLock;
                collectEndpointLocked(collected, manager_.get());
                continue;
            }
        }
        collectEndpoint(collected, manager.get());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sessions_.empty() || !collected.empty()) {
            // Session ids are unique per session instance, so comparing them
            // detects both new and closed sessions.
            std::set<std::string> before, after;
            for (const auto& session : sessions_) before.insert(session.sessionId);
            for (const auto& session : collected) after.insert(session.sessionId);
            if (before != after) changed_ = true;
        }
        sessions_ = std::move(collected);
    }

    auto processes = processTable();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processes_ = std::move(processes);
    }
    return true;
}

} // namespace

} // namespace saf::win
