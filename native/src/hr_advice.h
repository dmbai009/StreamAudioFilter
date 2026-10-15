// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HRESULT knowledge that must be available everywhere, including platforms that
// have no windows.h (the portable tests, and the documentation generator).
//
// Why this exists: the plugin's Windows code used to report a bare HRESULT, and
// the single most important process loopback failure — E_ILLEGAL_METHOD_CALL,
// which means "the COM completion handler is not an agile object" — looked like
// a Windows version problem. It is not: ActivateAudioInterfaceAsync fails with
// this code *before* it looks at the OS build at all when the handler passed to
// it does not implement IAgileObject (see docs/LIMITATIONS.md). Every HRESULT
// this plugin can hit therefore carries a name and an explanation of what it
// actually means and what to do about it.
//
// The numbers are the Windows SDK values; the portable test suite compares them
// against <audioclient.h> / <winerror.h> on Windows so a typo here fails the
// build instead of misinforming a user.

#pragma once

#include <cstdint>
#include <string>

namespace saf {

/** Named HRESULT values, usable without the Windows SDK. */
namespace hr {

constexpr int32_t Ok = 0;
constexpr int32_t False = 1;

constexpr int32_t NotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t NoInterface = static_cast<int32_t>(0x80004002u);
constexpr int32_t Pointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t Fail = static_cast<int32_t>(0x80004005u);
constexpr int32_t Unexpected = static_cast<int32_t>(0x8000FFFFu);
constexpr int32_t IllegalMethodCall = static_cast<int32_t>(0x8000000Eu);

constexpr int32_t Win32InvalidParameter = static_cast<int32_t>(0x80070057u);
constexpr int32_t Win32AccessDenied = static_cast<int32_t>(0x80070005u);
constexpr int32_t Win32OutOfMemory = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t Win32InvalidHandle = static_cast<int32_t>(0x80070006u);
constexpr int32_t Win32FileNotFound = static_cast<int32_t>(0x80070002u);
constexpr int32_t Win32NotFound = static_cast<int32_t>(0x80070490u);
constexpr int32_t Win32NotSupported = static_cast<int32_t>(0x80070032u);
constexpr int32_t Win32NotEnoughMemory = static_cast<int32_t>(0x80070008u);
constexpr int32_t Win32Timeout = static_cast<int32_t>(0x800705B4u);

constexpr int32_t ComNotInitialized = static_cast<int32_t>(0x800401F0u);
constexpr int32_t ComAlreadyInitialized = static_cast<int32_t>(0x800401F1u);
constexpr int32_t RpcChangedMode = static_cast<int32_t>(0x80010106u);
constexpr int32_t RpcWrongThread = static_cast<int32_t>(0x8001010Eu);
constexpr int32_t RpcCallRejected = static_cast<int32_t>(0x80010001u);

constexpr int32_t AudclntNotInitialized = static_cast<int32_t>(0x88890001u);
constexpr int32_t AudclntAlreadyInitialized = static_cast<int32_t>(0x88890002u);
constexpr int32_t AudclntWrongEndpointType = static_cast<int32_t>(0x88890003u);
constexpr int32_t AudclntDeviceInvalidated = static_cast<int32_t>(0x88890004u);
constexpr int32_t AudclntNotStopped = static_cast<int32_t>(0x88890005u);
constexpr int32_t AudclntBufferTooLarge = static_cast<int32_t>(0x88890006u);
constexpr int32_t AudclntOutOfOrder = static_cast<int32_t>(0x88890007u);
constexpr int32_t AudclntUnsupportedFormat = static_cast<int32_t>(0x88890008u);
constexpr int32_t AudclntInvalidSize = static_cast<int32_t>(0x88890009u);
constexpr int32_t AudclntDeviceInUse = static_cast<int32_t>(0x8889000Au);
constexpr int32_t AudclntBufferOperationPending = static_cast<int32_t>(0x8889000Bu);
constexpr int32_t AudclntThreadNotRegistered = static_cast<int32_t>(0x8889000Cu);
constexpr int32_t AudclntExclusiveModeNotAllowed = static_cast<int32_t>(0x8889000Eu);
constexpr int32_t AudclntEndpointCreateFailed = static_cast<int32_t>(0x8889000Fu);
constexpr int32_t AudclntServiceNotRunning = static_cast<int32_t>(0x88890010u);
constexpr int32_t AudclntEventhandleNotExpected = static_cast<int32_t>(0x88890011u);
constexpr int32_t AudclntEventhandleNotSet = static_cast<int32_t>(0x88890014u);
constexpr int32_t AudclntIncorrectBufferSize = static_cast<int32_t>(0x88890015u);
constexpr int32_t AudclntBufferSizeError = static_cast<int32_t>(0x88890016u);
constexpr int32_t AudclntBufferError = static_cast<int32_t>(0x88890018u);
constexpr int32_t AudclntBufferSizeNotAligned = static_cast<int32_t>(0x88890019u);
constexpr int32_t AudclntInvalidStreamFlag = static_cast<int32_t>(0x88890021u);
constexpr int32_t AudclntResourcesInvalidated = static_cast<int32_t>(0x88890026u);

} // namespace hr

/** What an HRESULT means and what to do about it, without the Windows SDK. */
struct HresultAdvice {
    /** Symbolic name, e.g. "E_ILLEGAL_METHOD_CALL"; empty when unknown. */
    std::string name;
    /** One or two sentences: cause, and what it means for the user. */
    std::string advice;
};

HresultAdvice adviseHresult(int32_t hr);

/** "0x8000000E" in the conventional upper case notation. */
std::string hresultHex(int32_t hr);

/**
 * "E_ILLEGAL_METHOD_CALL (0x8000000E): advice", or just the hex value when the
 * code is unknown. Never empty, so diagnostics can always quote it.
 */
std::string describeHresult(int32_t hr);

} // namespace saf
