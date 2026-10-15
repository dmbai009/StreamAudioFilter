// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hr_advice.h"

#include <cstdio>

namespace saf {
namespace {

struct Entry {
    int32_t value;
    const char* name;
    const char* advice;
};

// Kept as a table so the portable tests can iterate over it and assert that
// every documented code is reachable and non-empty.
constexpr Entry kEntries[] = {
    { hr::Ok, "S_OK", "The call succeeded." },
    { hr::False, "S_FALSE", "The call succeeded without producing a result." },
    { hr::NotImpl, "E_NOTIMPL", "The component does not implement this. For an activation call this means the "
                                "requested capture type does not exist in this Windows build." },
    { hr::NoInterface, "E_NOINTERFACE", "The object does not expose the requested interface. For WASAPI this usually "
                                        "means the activation produced a client of a different kind (render instead "
                                        "of capture) or the interface is not available in this build." },
    { hr::Pointer, "E_POINTER", "A required pointer was null — an internal error in the plugin." },
    { hr::Fail, "E_FAIL", "An unspecified failure. See the stage that produced it: this code alone says nothing about "
                          "the cause." },
    { hr::Unexpected, "E_UNEXPECTED", "The COM object was used in a state that does not allow the call — typically a "
                                      "teardown race inside the plugin, not a Windows problem." },
    { hr::IllegalMethodCall, "E_ILLEGAL_METHOD_CALL",
        "The object cannot be called the way it was: for ActivateAudioInterfaceAsync this is the documented result of "
        "passing a completion handler that is not an agile object (one that aggregates the free-threaded marshaler and "
        "implements IAgileObject). It is raised immediately, before Windows even considers the OS build, so it must "
        "never be reported as \"this Windows version is unsupported\"." },
    { hr::Win32InvalidParameter, "E_INVALIDARG",
        "A parameter was rejected. For process loopback this is what a malformed activation blob (wrong PROPVARIANT "
        "type, wrong structure size) or a captured process id of 0 looks like; the filter itself is not evaluated." },
    { hr::Win32AccessDenied, "E_ACCESSDENIED",
        "Access denied. Capturing the audio of a process requires the audio service to accept the request; a protected "
        "or elevated process may refuse it. Administrator rights are not required for the plugin's own processes." },
    { hr::Win32OutOfMemory, "E_OUTOFMEMORY", "Out of memory while activating the capture." },
    { hr::Win32InvalidHandle, "E_HANDLE", "An invalid handle was passed to the audio engine." },
    { hr::Win32FileNotFound, "E_FILE_NOT_FOUND", "A file the plugin needs does not exist." },
    { hr::Win32NotFound, "E_NOTFOUND", "The audio engine reported that the requested element does not exist — an "
                                       "invalid process id or an invalid capture mode." },
    { hr::Win32NotSupported, "HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)",
        "The audio engine does not support this operation in the current configuration." },
    { hr::Win32NotEnoughMemory, "HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)", "Not enough memory." },
    { hr::Win32Timeout, "HRESULT_FROM_WIN32(ERROR_TIMEOUT)", "The operation timed out." },
    { hr::ComNotInitialized, "CO_E_NOTINITIALIZED",
        "The calling thread has no COM apartment. Every thread that touches WASAPI must call CoInitializeEx first "
        "(the plugin uses a dedicated MTA thread for exactly this reason)." },
    { hr::ComAlreadyInitialized, "CO_E_ALREADYINITIALIZED", "CoInitializeEx was called twice with different flags on "
                                                            "the same thread." },
    { hr::RpcChangedMode, "RPC_E_CHANGED_MODE",
        "The thread already lives in a COM apartment of the other model (STA where the plugin wants MTA, or the "
        "opposite). Activation still works, but the completion callback is then delivered through a proxy — which is "
        "only safe for an agile handler. This is logged, not fatal." },
    { hr::RpcWrongThread, "RPC_E_WRONG_THREAD",
        "A COM interface was used from the wrong apartment. With a non-agile handler this is the code that appears "
        "instead of a working callback; with an agile handler it means an object was shared between threads that "
        "must not share it." },
    { hr::RpcCallRejected, "RPC_E_CALL_REJECTED", "COM rejected the call — the target object is busy or shutting "
                                                  "down." },
    { hr::AudclntNotInitialized, "AUDCLNT_E_NOT_INITIALIZED", "The audio client was used before Initialize()." },
    { hr::AudclntAlreadyInitialized, "AUDCLNT_E_ALREADY_INITIALIZED", "The audio client was initialized twice." },
    { hr::AudclntWrongEndpointType, "AUDCLNT_E_WRONG_ENDPOINT_TYPE",
        "The audio client was opened in the wrong direction: process loopback capture is a capture stream, so it needs "
        "the capture side of the API. This code also appears when a loopback stream is opened on a capture device." },
    { hr::AudclntDeviceInvalidated, "AUDCLNT_E_DEVICE_INVALIDATED",
        "The audio endpoint disappeared or was reconfigured (default device changed, headset unplugged). The plugin "
        "treats this as a real capture failure and restarts the capture; audio keeps flowing unfiltered only through "
        "the explicit opt-in." },
    { hr::AudclntNotStopped, "AUDCLNT_E_NOT_STOPPED", "Stop() was called on a client that never started." },
    { hr::AudclntBufferTooLarge, "AUDCLNT_E_BUFFER_TOO_LARGE", "The requested buffer size exceeds the engine limit." },
    { hr::AudclntOutOfOrder, "AUDCLNT_E_OUT_OF_ORDER", "A buffer was released out of order." },
    { hr::AudclntUnsupportedFormat, "AUDCLNT_E_UNSUPPORTED_FORMAT",
        "The audio engine refused the requested format. The plugin asks for 48 kHz float stereo with AUTOCONVERTPCM "
        "and falls back to the endpoint mix format; if both are refused, the format is the problem, not the capture." },
    { hr::AudclntInvalidSize, "AUDCLNT_E_INVALID_SIZE", "An invalid buffer size was requested." },
    { hr::AudclntDeviceInUse, "AUDCLNT_E_DEVICE_IN_USE",
        "The endpoint is in exclusive use by another application (an audio tool holding the device exclusively)." },
    { hr::AudclntBufferOperationPending, "AUDCLNT_E_BUFFER_OPERATION_PENDING", "A buffer operation is already pending." },
    { hr::AudclntThreadNotRegistered, "AUDCLNT_E_THREAD_NOT_REGISTERED", "The thread is not registered with the audio "
                                                                         "engine session." },
    { hr::AudclntExclusiveModeNotAllowed, "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED",
        "Exclusive mode is not allowed for this endpoint. The plugin always uses shared mode." },
    { hr::AudclntEndpointCreateFailed, "AUDCLNT_E_ENDPOINT_CREATE_FAILED", "The audio endpoint could not be created." },
    { hr::AudclntServiceNotRunning, "AUDCLNT_E_SERVICE_NOT_RUNNING",
        "The Windows audio service is not running. Nothing can be captured until it is started — this is a real, "
        "actionable cause." },
    { hr::AudclntEventhandleNotExpected, "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED",
        "The client was initialized without AUDCLNT_STREAMFLAGS_EVENTCALLBACK but SetEventHandle was called." },
    { hr::AudclntEventhandleNotSet, "AUDCLNT_E_EVENTHANDLE_NOT_SET",
        "The client was initialized with event callbacks but no event handle was configured before Start()." },
    { hr::AudclntIncorrectBufferSize, "AUDCLNT_E_INCORRECT_BUFFER_SIZE", "The buffer size does not match the "
                                                                         "negotiated period." },
    { hr::AudclntBufferSizeError, "AUDCLNT_E_BUFFER_SIZE_ERROR", "The buffer size is not usable for this stream." },
    { hr::AudclntBufferError, "AUDCLNT_E_BUFFER_ERROR", "The audio engine failed to deliver a buffer." },
    { hr::AudclntBufferSizeNotAligned, "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED",
        "The requested buffer duration does not align with the engine period and no AUTOCONVERTPCM is in effect." },
    { hr::AudclntInvalidStreamFlag, "AUDCLNT_E_INVALID_STREAM_FLAG",
        "A stream flag is not valid for this client — e.g. loopback flags on a process loopback capture, which does "
        "not use them." },
    { hr::AudclntResourcesInvalidated, "AUDCLNT_E_RESOURCES_INVALIDATED",
        "The capture resources were invalidated (endpoint change, application exit). The plugin treats this as a real "
        "capture failure and stops the filtered path." },
};

const Entry* find(int32_t hr) {
    for (const Entry& entry : kEntries) {
        if (entry.value == hr) return &entry;
    }
    return nullptr;
}

} // namespace

HresultAdvice adviseHresult(int32_t hr) {
    HresultAdvice advice;
    if (const Entry* entry = find(hr)) {
        advice.name = entry->name;
        advice.advice = entry->advice;
        return advice;
    }

    // Unknown code: still say something usable. Two conventions matter here:
    // AUDCLNT_E_* is 0x8889xxxx, HRESULT_FROM_WIN32 is 0x8007xxxx.
    const uint32_t value = static_cast<uint32_t>(hr);
    if ((value & 0xFFFF0000u) == 0x88890000u) {
        advice.name = "AUDCLNT_E_<unknown>";
        advice.advice = "An audio engine error that this plugin does not know by name. The numeric value identifies it "
                        "in the Windows audio documentation.";
    } else if ((value & 0xFFFF0000u) == 0x80070000u) {
        advice.name = "HRESULT_FROM_WIN32(" + std::to_string(value & 0xFFFFu) + ")";
        advice.advice = "A system error wrapped into an HRESULT; the low word is the Win32 error code.";
    } else if ((value & 0xFFFF0000u) == 0x80010000u) {
        advice.name = "RPC_E_<unknown>";
        advice.advice = "A COM marshalling error. This points at the apartment model or at calling a COM object "
                        "across threads, not at the audio engine.";
    }
    return advice;
}

std::string hresultHex(int32_t hr) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<uint32_t>(hr));
    return text;
}

std::string describeHresult(int32_t hr) {
    const HresultAdvice advice = adviseHresult(hr);
    if (advice.name.empty()) return hresultHex(hr) + ": an unknown error code";
    return advice.name + " (" + hresultHex(hr) + "): " + advice.advice;
}

} // namespace saf
