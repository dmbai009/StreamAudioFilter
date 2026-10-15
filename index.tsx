/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 *
 * What it does: the user decides which applications the *viewers* of a screen
 * share hear. Everything else stays exactly as it is — the local playback, the
 * microphone, the voice channel, the system sound settings and the Discord
 * devices are never touched, and no virtual audio device is created.
 *
 * How it works:
 *  1. The renderer hook (stream/intercept.ts) replaces the audio track Discord
 *     would have taken from the system with a track built from the native engine.
 *  2. The native engine (native/) captures the selected applications through
 *     WASAPI process loopback — endpoint independent, so it also works when the
 *     user plays through Voicemeeter, a USB headset or an HDMI sink.
 *  3. Changes to the selection are applied to the running mix immediately; the
 *     screen share itself is never restarted.
 *
 * Configuration is persisted per application (executable path + name), not per
 * process id, so it survives restarts and is never applied to a different app
 * that happens to share a display name.
 */

import ErrorBoundary from "@components/ErrorBoundary";
import definePlugin from "@utils/types";
import { Menu } from "@webpack/common";

import { store } from "./apps/store";
import { settings } from "./settings";
import { installInterceptor, type InterceptorHandle } from "./stream/intercept";
import managedStyle from "./style.css?managed";
import { AccountPanelButton } from "./ui/AccountPanelButton";
import { mountFloatingPanel, unmountFloatingPanel } from "./ui/FloatingPanelRoot";
import { SettingsPanel } from "./ui/SettingsPanel";

let interceptor: InterceptorHandle | null = null;

export default definePlugin({
    name: "StreamAudioFilter",
    description: "Choose which applications the viewers of a screen share hear. Local playback, the microphone and the voice call are never touched.",
    searchTerms: ["audio", "stream", "screenshare", "wasapi", "sound", "share"],
    tags: ["Voice", "Media"],
    authors: [{ name: "StreamAudioFilter", id: 0n }],

    settings,
    managedStyle,
    settingsAboutComponent: SettingsPanel,

    patches: [
        {
            // The row with Discord's own microphone and headphone buttons in the
            // account panel (bottom left). The anchor is Discord's intl key.
            find: "#{intl::USER_PROFILE_ACCOUNT_POPOUT_BUTTON_A11Y_LABEL}",
            replacement: {
                match: /children:\[(?=.{0,25}?accountContainerRef)/,
                replace: "children:[$self.AccountPanelButton(arguments[0]),"
            }
        }
    ],

    flux: {
        // Discord tells us when a stream starts and ends. The interceptor does
        // the actual work; these handlers make the state visible even when the
        // audio request never reaches the renderer (which would be a failure
        // worth reporting instead of hiding).
        STREAM_CREATE: () => {
            store.markStreamStarted();
            interceptor?.noteStreamStart();
        },
        STREAM_DELETE: () => store.onSessionEnd()
    },

    toolboxActions() {
        return (
            <>
                <Menu.MenuItem
                    id="vc-saf-toggle-panel"
                    label="Stream audio: show the panel"
                    action={() => {
                        settings.store.showFloatingPanel = true;
                        mountFloatingPanel();
                    }}
                />
                <Menu.MenuItem
                    id="vc-saf-open-filters"
                    label="Stream audio: reset the filters"
                    action={() => void store.resetFilters()}
                />
            </>
        );
    },

    async start() {
        await store.loadRules();

        interceptor = installInterceptor({
            isAvailable: () => settings.store.filterEnabled,
            unavailableReason: () => store.getSnapshot().lastError || "filtering is switched off in the plugin settings",
            createTrack: () => store.startFilteredTrack(),
            // Fail closed by default: without this explicit opt-in the plugin
            // removes the stream audio when the filter can not be applied
            // instead of letting the viewers hear the excluded applications.
            allowUnfilteredFallback: () => settings.store.fallbackUnfiltered,
            onSessionStart: () => store.markStreamStarted(),
            onSessionEnd: () => store.onSessionEnd(),
            onProblem: (message, kind) => kind === "error" ? store.setLastError(message) : store.setLastWarning(message),
            onAudioState: (state, reason) => store.onAudioState(state, reason)
        });

        if (settings.store.showFloatingPanel) mountFloatingPanel();
    },

    stop() {
        interceptor?.uninstall();
        interceptor = null;
        unmountFloatingPanel();
        void store.stopEngine();
    },

    AccountPanelButton: ErrorBoundary.wrap(AccountPanelButton, { noop: true })
});
