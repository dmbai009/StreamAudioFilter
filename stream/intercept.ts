/*
 * StreamAudioFilter — per application audio filtering for screen shares
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Getting our audio into the screen share.
 *
 * Discord's desktop client obtains screen share media through the standard
 * WebRTC entry points of the renderer. We wrap exactly those entry points and
 * replace the audio track Discord would have received from the system with the
 * filtered track from the native engine:
 *
 *     getDisplayMedia({video: ..., audio: ...})   ->  same video, our audio
 *     getUserMedia({audio: {mandatory: {chromeMediaSource: "desktop"}}})
 *
 * Consequences:
 *  * Discord's own "Передавать звук" switch keeps working: when Discord does not
 *    ask for audio, no audio track is added by us (we never force it on).
 *  * The video stream is passed through untouched.
 *  * Nothing changes on the microphone path.
 *  * If filtering is unavailable, the original stream is returned unchanged and
 *    the reason is reported — no silent substitution.
 */

export interface InterceptorOptions {
    /** Filtering is switched on and the engine is usable. */
    isAvailable(): boolean;
    /** Human readable reason shown when filtering is on but can not be applied. */
    unavailableReason(): string;
    /** Creates the filtered audio track (starts the engine if needed). */
    createTrack(): Promise<MediaStreamTrack | null>;
    /** Called once per share so the UI can show live state. */
    onSessionStart(): void;
    onSessionEnd(): void;
    onProblem(message: string): void;
}

export interface InterceptorHandle {
    uninstall(): void;
}

const DESKTOP_AUDIO_SOURCES = ["desktop", "screen", "window", "tab"];

interface MediaDevicesLike {
    getDisplayMedia?: (options?: DisplayMediaStreamOptions) => Promise<MediaStream>;
    getUserMedia?: (constraints?: MediaStreamConstraints) => Promise<MediaStream>;
}

/** True when the caller asked for audio at all. */
function wantsAudio(options: DisplayMediaStreamOptions | undefined): boolean {
    if (!options) return false;
    const audio = (options as { audio?: unknown; }).audio;
    return audio === true || (typeof audio === "object" && audio !== null);
}

/** True for the "capture the desktop, including its sound" constraints. */
function isDesktopAudioRequest(constraints: MediaStreamConstraints | undefined): boolean {
    const audio = constraints?.audio;
    if (!audio || typeof audio !== "object") return false;
    const mandatory = (audio as { mandatory?: Record<string, unknown>; }).mandatory;
    if (!mandatory) return false;
    const source = mandatory.chromeMediaSource;
    return typeof source === "string" && DESKTOP_AUDIO_SOURCES.includes(source);
}

let installed = false;
let activeStream: MediaStream | null = null;
let activeTrack: MediaStreamTrack | null = null;

export function installInterceptor(options: InterceptorOptions): InterceptorHandle {
    if (installed) throw new Error("StreamAudioFilter: перехват уже установлен");
    installed = true;

    const mediaDevices = navigator.mediaDevices as MediaDevicesLike;
    const originalGetDisplayMedia = mediaDevices.getDisplayMedia?.bind(mediaDevices);
    const originalGetUserMedia = mediaDevices.getUserMedia?.bind(mediaDevices);

    function cleanup(reason: string) {
        if (activeTrack) {
            const track = activeTrack;
            activeTrack = null;
            try {
                track.stop();
            } catch { }
        }
        if (activeStream) {
            activeStream = null;
        }
        options.onSessionEnd();
        void reason;
    }

    function watchStream(stream: MediaStream) {
        const video = stream.getVideoTracks()[0];
        if (video) {
            video.addEventListener("ended", () => cleanup("трансляция завершена"), { once: true });
        }
        activeStream = stream;
    }

    /** Replaces the audio of a freshly created stream, when possible. */
    async function applyFilter(stream: MediaStream, context: string): Promise<MediaStream> {
        if (!options.isAvailable()) {
            options.onProblem(`${context}: фильтрация недоступна — ${options.unavailableReason()}`);
            return stream;
        }

        const track = await options.createTrack();
        if (!track) {
            options.onProblem(`${context}: не удалось создать фильтрованный звук — ${options.unavailableReason()}`);
            return stream;
        }

        // Drop what Discord captured itself: keeping it would send unfiltered
        // audio, would capture our own result again and would waste CPU.
        for (const existing of stream.getAudioTracks()) {
            stream.removeTrack(existing);
            try {
                existing.stop();
            } catch { }
        }

        stream.addTrack(track);
        activeTrack = track;
        track.addEventListener("ended", () => cleanup("звук трансляции остановлен"), { once: true });

        options.onSessionStart();
        return stream;
    }

    if (originalGetDisplayMedia) {
        mediaDevices.getDisplayMedia = async function (constraints?: DisplayMediaStreamOptions) {
            const stream = await originalGetDisplayMedia!(constraints);
            watchStream(stream);

            if (!wantsAudio(constraints)) {
                // Discord's "Передавать звук" is off: we must not switch it on.
                options.onProblem("Discord не запросил звук демонстрации — передача звука выключена в интерфейсе");
                return stream;
            }

            return applyFilter(stream, "демонстрация экрана");
        };
    }

    if (originalGetUserMedia) {
        mediaDevices.getUserMedia = async function (constraints?: MediaStreamConstraints) {
            const stream = await originalGetUserMedia!(constraints);
            if (!isDesktopAudioRequest(constraints)) return stream;

            watchStream(stream);
            return applyFilter(stream, "захват системного звука");
        };
    }

    return {
        uninstall() {
            installed = false;
            if (activeTrack) {
                try {
                    activeTrack.stop();
                } catch { }
                activeTrack = null;
            }
            activeStream = null;
            if (originalGetDisplayMedia) mediaDevices.getDisplayMedia = originalGetDisplayMedia;
            if (originalGetUserMedia) mediaDevices.getUserMedia = originalGetUserMedia;
        }
    };
}

/** True while a filtered share is running. */
export function isFilteringActive(): boolean {
    return activeTrack !== null && activeTrack.readyState === "live";
}
