/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Plugin settings as shown in Vencord's plugin card. Only things a user may
 * actually want to change live here; the technical state (which applications
 * are transmitted) is kept with the rules and is edited in the panel.
 */

import { definePluginSettings } from "@api/Settings";
import { OptionType } from "@utils/types";

export const settings = definePluginSettings({
    filterEnabled: {
        type: OptionType.BOOLEAN,
        description: "Filter the screen share audio (otherwise plain system audio is sent)",
        default: true
    },
    fallbackUnfiltered: {
        type: OptionType.BOOLEAN,
        description: "Send plain system audio when filtering is unavailable. The viewers will hear every application — "
            + "this is off by default: without it the plugin removes the share audio instead of substituting unfiltered audio",
        default: false
    },
    showAccountPanelButton: {
        type: OptionType.BOOLEAN,
        description: "A “Stream audio” button next to the microphone buttons at the bottom of the window",
        default: true
    },
    showFloatingPanel: {
        type: OptionType.BOOLEAN,
        description: "A floating panel with the list of applications",
        default: true
    },
    collapseDuringStream: {
        type: OptionType.BOOLEAN,
        description: "Collapse the floating panel while a share is running",
        default: true
    },
    bufferMs: {
        type: OptionType.SLIDER,
        description: "Audio buffer headroom. More is steadier, less means a lower delay",
        markers: [30, 45, 60, 90, 120, 180],
        default: 60,
        stickToMarkers: false
    },
    maxStreams: {
        type: OptionType.SLIDER,
        description: "How many applications to capture at the same time",
        markers: [4, 8, 12, 16, 24, 32],
        default: 12,
        stickToMarkers: true
    },
    nativeModulePath: {
        type: OptionType.STRING,
        description: "Path to streamaudiofilter.node if the addon was built somewhere else",
        default: "",
        placeholder: "C:\\path\\to\\streamaudiofilter.node"
    },
    testBackend: {
        type: OptionType.BOOLEAN,
        description: "[Test] Test mode: a signal generator is used instead of real audio capture",
        default: false
    }
});
