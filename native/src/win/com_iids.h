// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// COM identities that are declared by the Windows SDK headers but are not
// exported from uuid.lib by every Windows SDK / MSVC combination. The values
// are fixed by Windows and must never be changed.
//
// Included without <initguid.h> on purpose: DEFINE_GUID then expands to a
// selectable (COMDAT) definition, so this header can be used from several
// translation units without duplicate-symbol errors.

#pragma once

#include <windows.h>

#include <guiddef.h>

#ifndef DEFINE_GUID
#    define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        EXTERN_C const GUID DECLSPEC_SELECTANY name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#endif

// {41D949AB-9862-444A-80F6-C261334DA5EB}
DEFINE_GUID(IID_IActivateAudioInterfaceCompletionHandler,
    0x41d949ab, 0x9862, 0x444a, 0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb);

// {641DD20B-4D41-49CC-ABA3-174B9477BB08}
DEFINE_GUID(IID_IAudioSessionNotification,
    0x641dd20b, 0x4d41, 0x49cc, 0xab, 0xa3, 0x17, 0x4b, 0x94, 0x77, 0xbb, 0x08);

// {72A22D78-CDE4-431D-B8CC-843A71199B6D}
DEFINE_GUID(IID_IActivateAudioInterfaceAsyncOperation,
    0x72a22d78, 0xcde4, 0x431d, 0xb8, 0xcc, 0x84, 0x3a, 0x71, 0x19, 0x9b, 0x6d);
