/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Turns the mixed PCM from the native engine into a genuine MediaStreamTrack
 * that Discord can put into the screen share.
 * Why not a virtual audio device? A virtual device would show up as an input
 * device, would have to be selected by the user and would violate the
 * requirements of the project (no virtual devices, no device changes). A track
 * created here lives only inside the renderer: Discord sends exactly this audio,
 * nothing is added to the system, and the local playback is untouched.
 * The output is one continuous Web Audio graph whose composition changes when
 * applications are added or removed — never a restart per change.
 */

export interface AudioPipelineStats {
    /** Frames actually scheduled for playback. */
    scheduledFrames: number;
    /** Times the buffer ran dry (counted, never hidden). */
    underruns: number;
    /** Frames dropped because they arrived after their playback time. */
    droppedFrames: number;
    /** Current scheduling headroom in milliseconds. */
    bufferedMs: number;
    sampleRate: number;
    /** True when the browser refused the requested sample rate. */
    resampled: boolean;
}

export interface FilteredTrackOptions {
    /** Sample rate the native engine produces (48000 by default). */
    sampleRate: number;
    /** Scheduling headroom. Lower = less delay, higher = more robust. */
    bufferedMs: number;
    /** Pulls mixed PCM, interleaved stereo float32. */
    pull(frames: number): Promise<Uint8Array>;
    onStats?(stats: AudioPipelineStats): void;
    onError?(message: string): void;
}

export interface FilteredTrack {
    track: MediaStreamTrack;
    stats: AudioPipelineStats;
    stop(): void;
}

const PULL_INTERVAL_MS = 10;
const MAX_PULL_FAILURES = 200;

function toFloat32(data: Uint8Array): Float32Array {
    // Copy: the IPC buffer may not be aligned for a float32 view.
    const copy = new ArrayBuffer(data.byteLength);
    new Uint8Array(copy).set(data);
    return new Float32Array(copy);
}

/**
 * Creates the filtered audio track.
 *
 * The graph is trivial on purpose — buffers are scheduled back to back at exact
 * times, which keeps the output sample accurate without a worklet (no blob URL,
 * no CSP interaction, no code evaluation).
 */
export async function createFilteredTrack(options: FilteredTrackOptions): Promise<FilteredTrack> {
    const requestedRate = options.sampleRate || 48000;
    const context = new AudioContext({ sampleRate: requestedRate, latencyHint: "interactive" });
    const destination = context.createMediaStreamDestination();
    const track = destination.stream.getAudioTracks()[0];

    const stats: AudioPipelineStats = {
        scheduledFrames: 0,
        underruns: 0,
        droppedFrames: 0,
        bufferedMs: 0,
        sampleRate: context.sampleRate,
        resampled: context.sampleRate !== requestedRate
    };

    let nextStartTime = 0;
    let stopped = false;
    let pullFailures = 0;
    // Assigned right after the pump is started; stop() is re-entrant safe because
    // every entry point guards on `stopped`.
    let handle: FilteredTrack | null = null;

    // The requested rate was refused (rare): resample linearly.
    const ratio = stats.resampled ? context.sampleRate / requestedRate : 1;
    let resampleCarry = 0;

    let left = new Float32Array(4096);
    let right = new Float32Array(4096);

    function schedule(interleaved: Float32Array) {
        const frames = interleaved.length >> 1;
        if (frames <= 0) return;

        let outputFrames = frames;
        let source = interleaved;
        if (stats.resampled) {
            // Linear interpolation is enough here: the native side is asked for
            // the context's rate first, this path is a safety net only.
            const total = Math.floor((frames + resampleCarry) / ratio);
            const resampled = new Float32Array(total * 2);
            let position = -resampleCarry;
            for (let i = 0; i < total; i++) {
                const index = position;
                const base = Math.floor(index);
                const fraction = index - base;
                for (let c = 0; c < 2; c++) {
                    const a = interleaved[Math.max(0, base) * 2 + c] ?? 0;
                    const b = interleaved[Math.min(frames - 1, base + 1) * 2 + c] ?? 0;
                    resampled[i * 2 + c] = a + (b - a) * fraction;
                }
                position += ratio;
            }
            resampleCarry = Math.max(0, position - frames);
            source = resampled;
            outputFrames = total;
        }

        if (outputFrames <= 0) return;
        if (left.length < outputFrames) {
            left = new Float32Array(outputFrames);
            right = new Float32Array(outputFrames);
        }

        for (let i = 0; i < outputFrames; i++) {
            left[i] = source[i * 2];
            right[i] = source[i * 2 + 1];
        }

        const buffer = context.createBuffer(2, outputFrames, context.sampleRate);
        buffer.copyToChannel(left.subarray(0, outputFrames), 0);
        buffer.copyToChannel(right.subarray(0, outputFrames), 1);

        const node = context.createBufferSource();
        node.buffer = buffer;
        node.connect(destination);

        const now = context.currentTime;
        const margin = options.bufferedMs / 1000;
        if (nextStartTime < now + 0.005) {
            // The buffer ran dry: restart the timeline instead of gluing a gap
            // into the middle of a block (which would click).
            stats.underruns++;
            nextStartTime = now + margin;
        }

        try {
            node.start(nextStartTime);
        } catch (error) {
            stats.droppedFrames += outputFrames;
            options.onError?.(`could not schedule an audio block: ${(error as Error).message}`);
            return;
        }

        nextStartTime += outputFrames / context.sampleRate;
        stats.scheduledFrames += outputFrames;
        stats.bufferedMs = Math.max(0, (nextStartTime - context.currentTime) * 1000);
        options.onStats?.(stats);
    }

    /**
     * Ends the pipeline and says why.
     *
     * A dead pipeline must never look like a working filter: silently producing
     * nothing would leave the panel claiming "filtering active" while viewers
     * hear silence, and — worse — a later fallback to the raw system audio would
     * look harmless. Stopping the track makes the failure visible to Discord and
     * to the fail closed interceptor, which watches the share for exactly that.
     */
    function shutDown(message: string) {
        if (stopped) return;
        options.onError?.(message);
        if (handle) {
            handle.stop();
            return;
        }
        stopped = true;
        try {
            track.stop();
        } catch { }
    }

    async function pump() {
        while (!stopped) {
            let chunks: Uint8Array[];
            try {
                // Two requests in flight keep the pipe busy without latency.
                chunks = await Promise.all([options.pull(PULL_INTERVAL_MS * 48), options.pull(PULL_INTERVAL_MS * 48)]);
                pullFailures = 0;
            } catch (error) {
                pullFailures++;
                options.onError?.(`the audio pull failed: ${(error as Error).message}`);
                if (pullFailures > MAX_PULL_FAILURES) {
                    // The pipeline is dead. Giving up silently would leave the
                    // viewers with a track that never produces audio while the
                    // panel still shows "filtering active", so the track is
                    // ended instead: the interceptor sees it and reports that
                    // the share has no filtered audio any more.
                    shutDown(`the filtered audio stopped: ${pullFailures} consecutive pull failures`);
                    break;
                }
                await new Promise(resolve => setTimeout(resolve, 50));
                continue;
            }

            let produced = false;
            for (const chunk of chunks) {
                if (!chunk || chunk.byteLength < 8) continue;
                produced = true;
                schedule(toFloat32(chunk));
            }
            if (!produced) await new Promise(resolve => setTimeout(resolve, PULL_INTERVAL_MS));
        }
    }

    // The context may start suspended; a share always follows a user click, so
    // resuming is allowed here.
    try {
        await context.resume();
    } catch (error) {
        options.onError?.(`could not resume the audio context: ${(error as Error).message}`);
    }

    // Keep the composition of the stream stable from Discord's point of view:
    // constraints on a synthetic track are meaningless, but Discord asks.
    patchTrack(track, stats);

    const created: FilteredTrack = {
        track,
        stats,
        stop() {
            if (stopped) return;
            stopped = true;
            try {
                track.stop();
            } catch { }
            try {
                destination.disconnect();
            } catch { }
            void context.close().catch(() => { });
        }
    };
    handle = created;

    // Started last: from here on every failure path can close the whole pipeline.
    void pump();

    return created;
}

function patchTrack(track: MediaStreamTrack, stats: AudioPipelineStats) {
    const anyTrack = track as any;

    anyTrack.applyConstraints = async () => { };
    anyTrack.getConstraints = () => ({});
    anyTrack.getCapabilities = () => ({});
    anyTrack.getSettings = () => ({
        sampleRate: stats.sampleRate,
        channelCount: 2,
        echoCancellation: false,
        noiseSuppression: false,
        autoGainControl: false
    });

    const originalStop = track.stop.bind(track);
    let stoppedOnce = false;
    anyTrack.stop = () => {
        if (stoppedOnce) return;
        stoppedOnce = true;
        originalStop();
    };
}
