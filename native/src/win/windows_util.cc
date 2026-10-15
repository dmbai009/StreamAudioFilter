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
#include <vector>

#include "../platform.h"
#include "com_iids.h"
#include "com_ptr.h"
#include "audio_subformat.h"
#include "win_internal.h"
#include "windows_backend.h"

namespace saf::win {

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

/** Signals the completion event when the activation finished. */
class ProbeActivationHandler : public ComCallback<IActivateAudioInterfaceCompletionHandler, &IID_IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ProbeActivationHandler(HANDLE completed) : completed_(completed) {}

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(completed_);
        return S_OK;
    }

private:
    HANDLE completed_;
};

} // namespace

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
 * system, this file never decides by version number: it performs a real
 * activation attempt and reports its HRESULT. The build number is context for
 * the user, not the verdict (see docs/LIMITATIONS.md).
 */
constexpr uint32_t kProcessLoopbackSinceBuild = 19041;  // Windows 10, version 2004
constexpr uint32_t kDocumentedBuild = 20348;            // Windows Server 2022 / Windows 11 SDK

bool probeProcessLoopback(std::string& detail) {
    const OsVersion os = osVersion();
    detail = describePlatform() + "\n";

    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) {
        detail += "не удалось создать событие синхронизации";
        return false;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    ComPtr<ProbeActivationHandler> handler(new ProbeActivationHandler(completed));

    const HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, handler.get(), operation.put());

    bool supported = false;
    if (FAILED(hr)) {
        detail += "ActivateAudioInterfaceAsync: " + formatHresult(hr, "не удалось вызвать") + "\n";
        if (hr == E_INVALIDARG || hr == E_NOTIMPL) {
            detail += "Система отказала в вызове: так выглядит отсутствие захвата по процессу. Windows 10 "
                      "сборки 19041 (версия 2004) и новее этот захват поддерживают, поэтому сначала проверьте "
                      "обновления Windows и целостность аудиослужбы.\n";
        }
    } else {
        const DWORD wait = WaitForSingleObject(completed, 5000);
        if (wait != WAIT_OBJECT_0) {
            detail += "тайм-аут ожидания активации\n";
        } else {
            HRESULT activateResult = E_FAIL;
            ComPtr<IUnknown> activated;
            if (FAILED(operation->GetActivateResult(&activateResult, activated.put())) || FAILED(activateResult)) {
                detail += "GetActivateResult: " + formatHresult(FAILED(activateResult) ? activateResult : E_FAIL, "активация отклонена") + "\n";
                if (os.build != 0 && os.build < kProcessLoopbackSinceBuild) {
                    detail += "Сборка " + std::to_string(os.build) + " старше " + std::to_string(kProcessLoopbackSinceBuild)
                              + " (Windows 10 версии 2004) — в этой версии захвата по процессу действительно нет "
                              "ни в одном приложении.\n";
                } else if (os.build != 0) {
                    detail += "Сборка " + std::to_string(os.build) + " формально поддерживает захват по процессу "
                              "(нужна 19041/2004 и новее), но активация отклонена — причина в самом коде отказа выше, "
                              "а не в версии Windows.\n";
                }
            } else {
                supported = true;
                detail += "активация клиента захвата отдельного процесса прошла успешно\n";
            }
        }
    }

    // The same probe without a process filter: available on all Windows versions
    // and proves that the audio engine itself is alive.
    {
        PROPVARIANT empty = {};
        ComPtr<IActivateAudioInterfaceAsyncOperation> fallbackOperation;
        HANDLE fallbackEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ComPtr<ProbeActivationHandler> fallbackHandler(new ProbeActivationHandler(fallbackEvent));
        const HRESULT plainHr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &empty, fallbackHandler.get(), fallbackOperation.put());
        if (SUCCEEDED(plainHr) && WaitForSingleObject(fallbackEvent, 5000) == WAIT_OBJECT_0) {
            HRESULT activateResult = E_FAIL;
            ComPtr<IUnknown> activated;
            if (SUCCEEDED(fallbackOperation->GetActivateResult(&activateResult, activated.put())) && SUCCEEDED(activateResult)) {
                detail += "активация без фильтра по процессу успешна (аудиодвижок работает)\n";
            }
        }
        if (fallbackEvent) CloseHandle(fallbackEvent);
    }

    if (completed) CloseHandle(completed);
    return supported;
}

/** What the build number means for process loopback (context, not a verdict). */
std::string buildCheckText(uint32_t build) {
    if (build == 0) return "Версия Windows не определена — решение принимает только попытка активации";
    if (build < kProcessLoopbackSinceBuild) {
        return "Windows build " + std::to_string(build) + " (< " + std::to_string(kProcessLoopbackSinceBuild)
            + ", Windows 10 до версии 2004): захвата по процессу нет";
    }
    if (build < kDocumentedBuild) {
        return "Windows build " + std::to_string(build) + " (Windows 10 версии 2004 и новее): захват по процессу "
            "работает, хотя в документации указана сборка " + std::to_string(kDocumentedBuild);
    }
    return "Windows build " + std::to_string(build) + " >= " + std::to_string(kDocumentedBuild)
        + ": захват по процессу в документации и на практике";
}

SupportInfo probeSupport() {
    SupportInfo info;
    const OsVersion os = osVersion();
    info.os = describePlatform();
    info.build = os.build;

    std::string detail;
    info.supported = probeProcessLoopback(detail);
    info.detail = detail;

    if (info.supported) {
        info.reason = "Захват звука отдельных приложений доступен (проверено настоящей попыткой активации).";
        info.checks.push_back("Реальная активация захвата по процессу: успешно (HRESULT S_OK)");
        info.checks.push_back(buildCheckText(os.build));
        info.checks.push_back("Отдельные устройства вывода не требуются: захват идёт за процессом");
        info.checks.push_back("Права администратора не нужны");
    } else {
        info.reason = os.build != 0 && os.build < kProcessLoopbackSinceBuild
            ? "Windows сборки " + std::to_string(os.build) + " старше " + std::to_string(kProcessLoopbackSinceBuild)
              + " (Windows 10 версии 2004): захвата звука по процессу в этой версии нет ни в одном приложении. "
              "Обновление Windows до 2004 и новее включает фильтрацию."
            : "Не удалось активировать захват звука отдельного процесса, хотя сборка Windows его поддерживает — "
              "точный код ошибки ниже, версия системы здесь не при чём.";
        info.checks.push_back("Реальная активация захвата по процессу: отказ (подробности ниже)");
        info.checks.push_back(buildCheckText(os.build));
        info.checks.push_back("Захват всего системного звука доступен, но фильтровать приложения в нём нельзя");
        info.checks.push_back("Плагин не будет подменять фильтрацию другими способами (виртуальные устройства, "
                              "маршрутизация) — это запрещено требованиями");
    }
    return info;
}

} // namespace saf::win
