/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 *
 * The user decides which applications the *viewers* of a whole screen share
 * hear. Local playback, the microphone, the voice channel, the system sound
 * settings and Discord's devices are never touched, and no virtual audio
 * device is created.
 *
 * How it works (details in stream/soundshare.ts and native/src/output_sink.h):
 *  1. saf-audio-host captures the allowed applications (WASAPI process
 *     loopback) and plays the mix into a session that is muted locally.
 *  2. Discord's native voice module captures screen share audio from a
 *     process id. The patch below hands it the host's pid instead of "the whole
 *     system", in Discord's hook based mode, which reads the audio before the
 *     session mute.
 *  3. Rule changes reach the running mix immediately; the share itself is
 *     never restarted.
 */

import ErrorBoundary from "@components/ErrorBoundary";
import definePlugin from "@utils/types";
import { search } from "@webpack";
import { Menu, SelectedChannelStore, UserStore } from "@webpack/common";

import { store } from "./apps/store";
import { settingHooks, settings } from "./settings";
import { type GoLiveAction, isOwnStream } from "./stream/soundshare";
import managedStyle from "./style.css?managed";
import { AccountPanelButton } from "./ui/AccountPanelButton";
import { mountFloatingPanel, unmountFloatingPanel } from "./ui/FloatingPanelRoot";
import { SettingsPanel } from "./ui/SettingsPanel";

function isOwn(action: { streamKey?: unknown; }) {
    return isOwnStream(action?.streamKey, UserStore?.getCurrentUser?.()?.id);
}

/** Whether the two replacements below really are in Discord's code. */
function detectPatches() {
    const found = (code: string) => {
        try {
            return Object.keys(search(code)).length > 0;
        } catch {
            return false;
        }
    };
    return { pidHook: found(".soundshareSource("), modeHook: found(".forceHookSoundshare()") };
}

export default definePlugin({
    name: "StreamAudioFilter",
    description: "Choose which applications the viewers of a screen share hear. Local playback, the microphone and the voice call are never touched.",
    searchTerms: ["audio", "stream", "screenshare", "go live", "wasapi", "sound", "share"],
    tags: ["Voice", "Media"],
    authors: [{ name: "StreamAudioFilter", id: 0n }],

    settings,
    managedStyle,
    settingsAboutComponent: SettingsPanel,

    patches: [
        {
            // MediaEngineStore. The string only exists in this module.
            find: "getExperimentalSoundshare(){",
            replacement: [
                {
                    // MEDIA_ENGINE_SET_GO_LIVE_SOURCE: the pid Discord captures
                    // the share audio from (1 = the whole system).
                    //   d=…getPidFromDesktopSource(i);…({soundshareId:e,soundshareSession:n}=nD(d,r))
                    match: /(getPidFromDesktopSource\(\i\);.{0,80}?\{soundshareId:\i,soundshareSession:\i\}=\i\()(\i)(,\i\))/,
                    replace: "$1$self.soundshareSource($2,arguments[0])$3"
                },
                {
                    // The capture method: hook based capture is required while
                    // the share audio comes from the host (its session is muted).
                    match: /getExperimentalSoundshare\(\)\{/,
                    replace: "$&if($self.forceHookSoundshare())return!1;"
                }
            ]
        },
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
        VOICE_CHANNEL_SELECT(action: { channelId?: string | null; }) {
            store.onVoiceChannel(action.channelId);
        },
        STREAM_CREATE(action: { streamKey?: string; }) {
            if (isOwn(action)) store.onOwnStreamStarted();
        },
        STREAM_DELETE(action: { streamKey?: string; }) {
            if (isOwn(action)) store.onOwnStreamEnded();
        },
        STREAM_CLOSE(action: { streamKey?: string; }) {
            if (isOwn(action)) store.onOwnStreamEnded();
        },
        MEDIA_ENGINE_SOUNDSHARE_FAILED(action: { errorMessage?: string; }) {
            store.onSoundshareFailed(String(action.errorMessage ?? ""));
        }
    },

    soundshareSource(pid: number | null, action: GoLiveAction | undefined) {
        return store.soundshareSource(pid, action);
    },

    forceHookSoundshare() {
        return store.forceHookSoundshare();
    },

    toolboxActions() {
        return (
            <Menu.MenuItem
                id="vc-saf-toggle-panel"
                label="Stream audio: floating panel"
                action={() => { settings.store.showFloatingPanel = !settings.store.showFloatingPanel; }}
            />
        );
    },

    async start() {
        settingHooks.floatingPanel = value => value ? mountFloatingPanel() : unmountFloatingPanel();
        await store.init(detectPatches());
        store.onVoiceChannel(SelectedChannelStore?.getVoiceChannelId?.() ?? null);
        if (settings.store.showFloatingPanel) mountFloatingPanel();
    },

    stop() {
        settingHooks.floatingPanel = () => { };
        unmountFloatingPanel();
        void store.shutdown();
    },

    AccountPanelButton: ErrorBoundary.wrap(AccountPanelButton, { noop: true })
});
