// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// COM identities that the Windows SDK headers only *declare*: their definitions
// live in uuid.lib, and not every SDK / MSVC combination ships them there (the
// Windows 10.0.26100 SDK used by the CI image does not). Without a definition
// the addon fails to link with
//     LNK2001: unresolved external symbol IID_IActivateAudioInterfaceCompletionHandler
//     LNK2001: unresolved external symbol IID_IAudioSessionNotification
//
// The values are fixed by Windows and must never be changed. They are emitted
// with selectany ("pick any") linkage, which is the same thing the SDK's own
// INITGUID path does: the definition lands in a COMDAT section, so it cannot
// collide with a copy coming from uuid.lib, and the header can be included from
// as many translation units as needed.
//
// The macro is deliberately *not* named DEFINE_GUID: <guiddef.h> (pulled in by
// <windows.h>) already defines that name as a bare declaration, and reusing it
// would silently turn the definitions below into declarations again.

#pragma once

#include <windows.h>

#ifndef DECLSPEC_SELECTANY
#    define DECLSPEC_SELECTANY __declspec(selectany)
#endif

#define SAF_DEFINE_IID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    EXTERN_C const GUID DECLSPEC_SELECTANY name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

// IActivateAudioInterfaceCompletionHandler — {41D949AB-9862-444A-80F6-C261334DA5EB}
SAF_DEFINE_IID(IID_IActivateAudioInterfaceCompletionHandler,
    0x41d949ab, 0x9862, 0x444a, 0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb);

// IAudioSessionNotification — {641DD20B-4D41-49CC-ABA3-174B9477BB08}
SAF_DEFINE_IID(IID_IAudioSessionNotification,
    0x641dd20b, 0x4d41, 0x49cc, 0xab, 0xa3, 0x17, 0x4b, 0x94, 0x77, 0xbb, 0x08);

// IActivateAudioInterfaceAsyncOperation — {72A22D78-CDE4-431D-B8CC-843A71199B6D}
SAF_DEFINE_IID(IID_IActivateAudioInterfaceAsyncOperation,
    0x72a22d78, 0xcde4, 0x431d, 0xb8, 0xcc, 0x84, 0x3a, 0x71, 0x19, 0x9b, 0x6d);
