/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Plugin settings as shown in Vencord's plugin card. Which applications are
 * sent is not a setting: it is kept with the rules and edited in the panel.
 */

import { definePluginSettings } from "@api/Settings";
import { OptionType } from "@utils/types";

/** Set by index.tsx: avoids an import cycle between the settings and the UI. */
export const settingHooks = {
    floatingPanel: (_: boolean) => { }
};

export const settings = definePluginSettings({
    filterEnabled: {
        type: OptionType.BOOLEAN,
        description: "Filter the audio of whole screen shares. Off: Discord sends all system audio as usual.",
        default: true
    },
    fallbackUnfiltered: {
        type: OptionType.BOOLEAN,
        description: "Send unfiltered audio when filtering fails. Off (recommended): the share has no audio instead, so excluded apps can never be heard by accident.",
        default: false
    },
    showAccountPanelButton: {
        type: OptionType.BOOLEAN,
        description: "Button next to the microphone and headphone buttons (opens the quick panel)",
        default: true
    },
    showFloatingPanel: {
        type: OptionType.BOOLEAN,
        description: "Floating panel over Discord (starts collapsed; drag it by its title)",
        default: false,
        onChange: (value: boolean) => settingHooks.floatingPanel(value)
    },
    maxStreams: {
        type: OptionType.SLIDER,
        description: "How many applications may be captured at the same time",
        markers: [4, 8, 12, 16, 24, 32],
        default: 12,
        stickToMarkers: true
    },
    hostPath: {
        type: OptionType.STRING,
        description: "Path to saf-audio-host.exe, only if it is not in the default folder",
        default: "",
        placeholder: "C:\\path\\to\\saf-audio-host.exe"
    },
    testBackend: {
        type: OptionType.BOOLEAN,
        description: "[Test] Signal generator instead of real capture (the viewers hear test tones)",
        default: false
    }
});
