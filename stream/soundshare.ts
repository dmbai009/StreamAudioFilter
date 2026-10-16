/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — how the plugin takes over the audio of a screen share.
 *
 * Discord Desktop does not capture screen share audio in JavaScript: its native
 * voice module (discord_voice) captures it, and no MediaStream ever carries it.
 * What JavaScript decides is *which process* the native module captures:
 *
 *   MEDIA_ENGINE_SET_GO_LIVE_SOURCE
 *     -> getPidFromDesktopSource("screen:…") = 1   (1 = the whole system)
 *     -> soundshare pid  -> desktopDescription.soundshareId -> discord_voice
 *
 * The plugin patches that one value (index.tsx). For a whole screen share with
 * audio it hands Discord the pid of saf-audio-host, which plays the filtered
 * mix into a locally muted session, and asks for Discord's hook based capture
 * (getExperimentalSoundshare() == false), which reads an application's audio
 * before the session volume. Result: the viewers hear the mix, the user hears
 * nothing twice, video and microphone are untouched.
 *
 * Fail closed: when filtering is wanted but the host is not available, the
 * share gets no audio at all (pid null) unless the user explicitly allowed
 * plain system audio. This file is pure (no Vencord imports) so the decision
 * table is unit tested.
 */

import type { ShareAudio } from "../types";

/** Discord's soundshare pid for "everything" (screens and the system picker). */
export const SYSTEM_SOUNDSHARE_PID = 1;

/** The part of Discord's MEDIA_ENGINE_SET_GO_LIVE_SOURCE action we read. */
export interface GoLiveAction {
    settings?: {
        context?: string;
        desktopSettings?: { sourceId?: string; sound?: boolean; } | null;
    } | null;
}

export interface SoundshareInput {
    /** Discord's media context: "stream" for Go Live, other values for clips etc. */
    context: string | undefined;
    sourceId: string;
    sound: boolean;
    /** The pid Discord computed (1 = whole system, a window's pid, or null). */
    originalPid: number | null;
    filterEnabled: boolean;
    /** pid of a running audio host, 0 when there is none. */
    hostPid: number;
    /** Why the host is not usable (shown when hostPid is 0). */
    hostProblem: string;
    fallbackAllowed: boolean;
}

export interface SoundshareDecision {
    /** What Discord should capture; null = no share audio at all. */
    pid: number | null;
    audio: ShareAudio;
    /** Discord must use its hook based capture (the host session is muted). */
    redirect: boolean;
    reason: string;
}

export function readGoLiveAction(action: GoLiveAction | undefined) {
    const settings = action?.settings ?? null;
    const desktop = settings?.desktopSettings ?? null;
    return {
        context: settings?.context,
        sourceId: typeof desktop?.sourceId === "string" ? desktop.sourceId : "",
        sound: desktop?.sound === true
    };
}

/**
 * Decides what Discord captures for a share. Returns null for anything that is
 * not a screen share of the user (clips, activities): those are left alone.
 */
export function decideSoundshare(input: SoundshareInput): SoundshareDecision | null {
    if (input.context !== "stream") return null;

    const keep = (audio: ShareAudio, reason = ""): SoundshareDecision => ({ pid: input.originalPid, audio, redirect: false, reason });

    if (!input.sound) return keep("noAudio");
    // A window share: Discord captures exactly that application (its process
    // tree). That already is a per application choice made by the user, and
    // replacing it would be surprising, so it is left alone.
    if (input.originalPid !== SYSTEM_SOUNDSHARE_PID) return keep("window");
    if (!input.filterEnabled) return keep("off");

    if (input.hostPid > 0) {
        return { pid: input.hostPid, audio: "starting", redirect: true, reason: "" };
    }

    const problem = input.hostProblem || "the audio host is not running";
    if (input.fallbackAllowed) return keep("unfiltered", problem);
    return { pid: null, audio: "muted", redirect: false, reason: problem };
}

/** True when a streamKey ("guild:…:…:<user>" or "call:…:<user>") belongs to @p userId. */
export function isOwnStream(streamKey: unknown, userId: string | undefined): boolean {
    if (typeof streamKey !== "string" || !userId) return false;
    return streamKey.split(":").pop() === userId;
}
