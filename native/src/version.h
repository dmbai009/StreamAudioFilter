// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One version for every part of a release: the plugin, the audio host, the
// installer and the Vencord bundle are built from the same tag. The protocol
// number changes only when the line protocol between the plugin and the host
// changes; the plugin refuses a host that speaks a different protocol instead
// of misinterpreting it.

#pragma once

#define SAF_VERSION "0.3.0"
#define SAF_PROTOCOL 2
