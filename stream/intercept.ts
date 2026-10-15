/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Getting our audio into the screen share, and keeping the filter from failing
 * open.
 * Discord's desktop client obtains screen share media through the standard
 * WebRTC entry points of the renderer. We wrap exactly those entry points and
 * replace the audio track Discord would have received from the system with the
 * filtered track from the native engine:
 *     getDisplayMedia({video: ..., audio: ...})   ->  same video, our audio
 *     getUserMedia({audio: {mandatory: {chromeMediaSource: "desktop"}}})
 * What the wrapper does *not* do is hand the unfiltered system audio back when
 * something goes wrong. Without filtering, "exclude Spotify" would turn into
 * "the viewers hear Spotify as soon as the filter breaks" — a failure that is
 * silent and exactly the opposite of what the user asked for. So:
 *
 *   user asked for filtering, our track is ready   -> our track
 *   user asked for filtering, our track is missing  -> no stream audio at all
 *                                                    (muted, loudly reported),
 *                                                    unless the user explicitly
 *                                                    enabled the unfiltered
 *                                                    fallback in the settings
 *   user did not ask for filtering                  -> Discord's own stream
 *                                                    stays untouched
 *
 * Other invariants:
 *  * Discord's "Передавать звук" switch keeps working: when Discord does not ask
 *    for audio, we never add an audio track (we only ever remove).
 *  * The video stream is passed through untouched.
 *  * Nothing changes on the microphone path.
 *  * We can only control streams that pass through these entry points. If a
 *    share is created without them, that is reported instead of hidden
 *    (noteStreamStart), because the plugin can not mute what it never saw.
 */

export type ProblemKind = "info" | "warning" | "error";

/** What the plugin is allowed to do with the audio of a stream. */
export interface AudioPolicy {
    /** The user switched filtering on. */
    filteringWanted: boolean;
    /** A filtered track exists and can be installed. */
    filteredTrack: boolean;
    /** The user explicitly opted into unfiltered transmission as a fallback. */
    fallbackAllowed: boolean;
}

/**
 * filtered    — our track replaces Discord's system audio.
 * muted       — nothing is transmitted: the safe answer when the user asked for
 *               filtering and we can not provide it.
 * passthrough — Discord's own capture is left alone. Only ever used when
 *               filtering is switched off (the user's explicit choice) or when
 *               the user explicitly allowed the unfiltered fallback.
 */
export type AudioDecision = "filtered" | "muted" | "passthrough";

/**
 * What the panels show about the audio of the current share. Deliberately not
 * the same union as AudioDecision: "passthrough" has two very different causes
 * (the filter is switched off, or the user allowed the unfiltered fallback),
 * and the UI must be able to tell them apart.
 */
export type ShareAudioState = "none" | "filtered" | "unfiltered" | "muted";

/** The whole fail-closed policy in one testable function. */
export function decideAudio(policy: AudioPolicy): AudioDecision {
    if (!policy.filteringWanted) return "passthrough";
    if (policy.filteredTrack) return "filtered";
    return policy.fallbackAllowed ? "passthrough" : "muted";
}

export interface InterceptorOptions {
    /** Filtering is switched on and the engine is usable. */
    isAvailable(): boolean;
    /** Human readable reason shown when filtering is on but can not be applied. */
    unavailableReason(): string;
    /** Creates the filtered audio track (starts the engine if needed). */
    createTrack(): Promise<MediaStreamTrack | null>;
    /** Explicit opt-in to transmit Discord's unfiltered audio if filtering fails. */
    allowUnfilteredFallback(): boolean;
    /** Called once per share so the UI can show live state. */
    onSessionStart(): void;
    onSessionEnd(): void;
    onProblem(message: string, kind: ProblemKind): void;
    /** Audio state of the share, so every panel can show the same truth. */
    onAudioState(state: ShareAudioState, reason: string): void;
}

export interface InterceptorHandle {
    uninstall(): void;
    /**
     * Discord reports a new screen share. Called through the flux hook: if the
     * share never went through the wrapped media APIs, the plugin can not
     * control its audio and says so.
     */
    noteStreamStart(): void;
    /** True while a share with our filtered audio is running. */
    isFiltering(): boolean;
}

const DESKTOP_AUDIO_SOURCES = ["desktop", "screen", "window", "tab"];
/** A media request that arrives this long before STREAM_CREATE counts as ours. */
const MEDIA_REQUEST_WINDOW_MS = 20000;

interface MediaDevicesLike {
    getDisplayMedia?: (options?: DisplayMediaStreamOptions) => Promise<MediaStream>;
    getUserMedia?: (constraints?: MediaStreamConstraints) => Promise<MediaStream>;
}

/** True when the caller asked for audio at all. */
function wantsAudio(options: DisplayMediaStreamOptions | undefined): boolean {
    if (!options) return false;
    const { audio } = (options as { audio?: unknown; });
    return audio === true || (typeof audio === "object" && audio !== null);
}

/** True for the "capture the desktop, including its sound" constraints. */
function isDesktopAudioRequest(constraints: MediaStreamConstraints | undefined): boolean {
    const audio = constraints?.audio;
    if (!audio || typeof audio !== "object") return false;
    const { mandatory } = (audio as { mandatory?: Record<string, unknown>; });
    if (!mandatory) return false;
    const source = mandatory.chromeMediaSource;
    return typeof source === "string" && DESKTOP_AUDIO_SOURCES.includes(source);
}

/** Adds the filtered track to a stream and remembers it for cleanup. */
function installTrack(stream: MediaStream, track: MediaStreamTrack): void {
    for (const existing of stream.getAudioTracks()) {
        stream.removeTrack(existing);
        try {
            existing.stop();
        } catch { }
    }
    stream.addTrack(track);
}

/** Removes the system audio from a stream (fail-closed). */
function stripAudio(stream: MediaStream): number {
    let removed = 0;
    for (const existing of stream.getAudioTracks()) {
        stream.removeTrack(existing);
        removed++;
        try {
            existing.stop();
        } catch { }
    }
    return removed;
}

let installedHandle: InterceptorHandle | null = null;

export function installInterceptor(options: InterceptorOptions): InterceptorHandle {
    if (installedHandle) throw new Error("StreamAudioFilter: перехват уже установлен");

    const mediaDevices = navigator.mediaDevices as MediaDevicesLike;
    // The unbound originals are kept for uninstall() (a bound copy is not the
    // same function object), the bound copies are used for calling.
    const rawGetDisplayMedia = mediaDevices.getDisplayMedia;
    const rawGetUserMedia = mediaDevices.getUserMedia;
    const originalGetDisplayMedia = rawGetDisplayMedia?.bind(mediaDevices);
    const originalGetUserMedia = rawGetUserMedia?.bind(mediaDevices);

    let activeStream: MediaStream | null = null;
    let activeTrack: MediaStreamTrack | null = null;
    /** True once the share audio went through us with a positive outcome. */
    let audioHandled = false;
    let lastMediaRequest = 0;
    let pendingRequests = 0;

    function stopActiveTrack() {
        if (!activeTrack) return;
        const track = activeTrack;
        activeTrack = null;
        try {
            track.stop();
        } catch { }
    }

    /**
     * Ends the session without touching Discord's own stream: used when the
     * filtered track died and the share keeps running.
     */
    function cleanup(reason: string, kind: ProblemKind = "warning") {
        stopActiveTrack();
        activeStream = null;
        audioHandled = false;
        options.onAudioState("none", reason);
        options.onSessionEnd();
        if (reason) options.onProblem(reason, kind);
    }

    function watchStream(stream: MediaStream) {
        const video = stream.getVideoTracks()[0];
        if (video) {
            video.addEventListener("ended", () => cleanup("трансляция завершена", "info"), { once: true });
        }
        activeStream = stream;
    }

    /** Notifies the UI about the audio state of the current share. */
    function reportAudio(state: ShareAudioState, reason: string) {
        audioHandled = state === "filtered";
        options.onAudioState(state, reason);
    }

    /**
     * Decides what the viewers get for a freshly created stream.
     * Never throws: a failing filter must not break the share, it must mute it.
     */
    async function applyFilter(stream: MediaStream, context: string): Promise<MediaStream> {
        const filteringWanted = options.isAvailable();

        let track: MediaStreamTrack | null = null;
        let failure = "";
        if (filteringWanted) {
            try {
                track = await options.createTrack();
            } catch (error) {
                track = null;
                failure = (error as Error)?.message ?? String(error);
            }
            if (!track) failure = failure || options.unavailableReason();
        }

        const fallbackAllowed = options.allowUnfilteredFallback();
        const decision = decideAudio({ filteringWanted, filteredTrack: !!track, fallbackAllowed });

        if (decision === "filtered" && track) {
            installTrack(stream, track);
            activeTrack = track;
            track.addEventListener("ended", () => cleanup(`фильтрованный звук трансляции остановлен — зрители больше не слышат звук (${context})`, "error"), { once: true });
            reportAudio("filtered", "");
            options.onSessionStart();
            return stream;
        }

        if (decision === "muted") {
            stripAudio(stream);
            reportAudio("muted", failure);
            // No capture gate open, no idle engine: the share has no audio.
            options.onSessionEnd();
            options.onProblem(
                `${context}: ${failure}. Системный звук убран из трансляции — зрители не услышат ни исключённые, `
                + "ни остальные приложения. Чтобы вернуть обычный звук Discord, включите в настройках плагина "
                + "«Передавать системный звук без фильтрации». Плагин не подставляет звук молча.",
                "error"
            );
            return stream;
        }

        // Only reachable when filtering is off or the fallback was enabled.
        if (decision === "passthrough") {
            if (filteringWanted) {
                // Explicitly allowed by the user: say it loudly instead of
                // silently sending everything.
                reportAudio("unfiltered", failure);
                options.onProblem(
                    `${context}: фильтрация недоступна (${failure}), передаётся обычный системный звук — как включено `
                    + "в настройках («Передавать системный звук без фильтрации»). Зрители слышат все приложения.",
                    "warning"
                );
            } else {
                reportAudio("none", "");
                options.onProblem(`${context}: фильтрация выключена в настройках плагина — звук передаётся как обычно`, "info");
            }
            return stream;
        }

        return stream;
    }

    async function wrapDisplayMedia(constraints?: DisplayMediaStreamOptions) {
        pendingRequests++;
        let stream: MediaStream;
        try {
            stream = await originalGetDisplayMedia!(constraints);
        } finally {
            pendingRequests--;
            lastMediaRequest = Date.now();
        }
        watchStream(stream);

        if (!wantsAudio(constraints)) {
            // Discord's "Передавать звук" is off: we must not switch it on.
            reportAudio("none", "");
            options.onProblem("Discord не запросил звук демонстрации — передача звука выключена в интерфейсе", "info");
            return stream;
        }

        return applyFilter(stream, "демонстрация экрана");
    }

    async function wrapUserMedia(constraints?: MediaStreamConstraints) {
        pendingRequests++;
        let stream: MediaStream;
        try {
            stream = await originalGetUserMedia!(constraints);
        } finally {
            pendingRequests--;
            if (isDesktopAudioRequest(constraints)) lastMediaRequest = Date.now();
        }
        if (!isDesktopAudioRequest(constraints)) return stream;

        watchStream(stream);
        return applyFilter(stream, "захват системного звука");
    }

    if (originalGetDisplayMedia) mediaDevices.getDisplayMedia = wrapDisplayMedia;
    if (originalGetUserMedia) mediaDevices.getUserMedia = wrapUserMedia;

    /**
     * A share started. If no media request passed through the wrapper, Discord
     * obtained the audio some other way: we can not filter it, and the user has
     * to know before assuming the exclusions work.
     */
    function noteStreamStart() {
        const now = Date.now();
        const intercepted = pendingRequests > 0 || now - lastMediaRequest < MEDIA_REQUEST_WINDOW_MS;
        if (audioHandled || intercepted) return;

        options.onAudioState("unfiltered", "Discord не использовал перехватываемый способ получения звука");
        options.onProblem(
            "Началась трансляция, но запрос звука не прошёл через плагин: Discord получил звук другим способом, и "
            + "отфильтровать его нельзя. Отключите «Передавать звук» у этой трансляции, чтобы зрители не слышали "
            + "исключённые приложения, и напишите о проблеме — способ получения звука нужно добавить в перехват.",
            "error"
        );
    }

    const handle: InterceptorHandle = {
        noteStreamStart,
        // A share is only "filtering" while our track is live in the share we
        // installed it into.
        isFiltering: () => audioHandled && activeStream !== null && activeTrack !== null && activeTrack.readyState === "live",
        uninstall() {
            if (installedHandle !== handle) return;
            installedHandle = null;
            stopActiveTrack();
            activeStream = null;
            audioHandled = false;
            if (rawGetDisplayMedia) mediaDevices.getDisplayMedia = rawGetDisplayMedia;
            if (rawGetUserMedia) mediaDevices.getUserMedia = rawGetUserMedia;
        }
    };

    installedHandle = handle;
    return handle;
}
