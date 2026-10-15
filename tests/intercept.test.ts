/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Tests for the stream interceptor, above all for the fail-closed rule:
 *
 *   when the user asked for filtering and the filter can not be applied, the
 *   viewers must NOT suddenly hear the applications that were excluded —
 *   the stream audio is removed instead, and the reason is reported.
 *
 * The interceptor only touches navigator.mediaDevices, so the whole decision
 * path can be exercised here with a fake mediaDevices object: no Discord, no
 * Vencord, no Windows.
 *
 * Run:  npx tsx tests/intercept.test.ts
 */

import assert from "node:assert/strict";
import { test } from "node:test";

import { decideAudio, installInterceptor, type InterceptorOptions, type ProblemKind } from "../stream/intercept";

interface FakeTrack {
    kind: string;
    readyState: string;
    stopped: boolean;
    handlers: Record<string, Array<() => void>>;
    stop(): void;
    addEventListener(type: string, listener: () => void): void;
    fireEnded(): void;
}

const videoTrack = makeTrack("video");

interface FakeStream {
    audio: FakeTrack[];
    added: FakeTrack[];
    removed: FakeTrack[];
    getAudioTracks(): FakeTrack[];
    getVideoTracks(): FakeTrack[];
    addTrack(track: FakeTrack): void;
    removeTrack(track: FakeTrack): void;
}

function makeTrack(kind: string): FakeTrack {
    const track: FakeTrack = {
        kind,
        readyState: "live",
        stopped: false,
        handlers: {},
        stop() {
            track.stopped = true;
            track.readyState = "ended";
        },
        addEventListener(type, listener) {
            (track.handlers[type] ??= []).push(listener);
        },
        fireEnded() {
            track.readyState = "ended";
            for (const listener of track.handlers.ended ?? []) listener();
        }
    };
    return track;
}

/** A MediaStream double that records what happened to it. */
function makeStream(withAudio = true): FakeStream {
    const audio = withAudio ? [makeTrack("audio")] : [];
    const stream: FakeStream = {
        audio,
        added: [],
        removed: [],
        getAudioTracks: () => [...stream.audio, ...stream.added],
        getVideoTracks: () => [makeTrack("video") as unknown as FakeTrack],
        addTrack(track) {
            stream.added.push(track);
            stream.audio.push(track);
        },
        removeTrack(track) {
            stream.removed.push(track);
            stream.audio = stream.audio.filter(candidate => candidate !== track);
        }
    };
    stream.getVideoTracks = () => [videoTrack as unknown as FakeTrack];
    return stream;
}

interface Harness {
    options: InterceptorOptions;
    problems: Array<{ message: string; kind: ProblemKind; }>;
    audioStates: Array<{ state: string; reason: string; }>;
    sessionsStarted: number;
    sessionsEnded: number;
    createTrackCalls: number;
}

/** Installs the interceptor with recording callbacks; returns the harness. */
function install(overrides: Partial<InterceptorOptions> = {}): { harness: Harness; handle: ReturnType<typeof installInterceptor>; } {
    const harness: Harness = {
        options: {
            isAvailable: () => true,
            unavailableReason: () => "engine unavailable (test)",
            createTrack: async () => null,
            allowUnfilteredFallback: () => false,
            onSessionStart: () => { harness.sessionsStarted++; },
            onSessionEnd: () => { harness.sessionsEnded++; },
            onProblem: (message, kind) => { harness.problems.push({ message, kind }); },
            onAudioState: (state, reason) => { harness.audioStates.push({ state, reason }); },
            ...overrides
        },
        problems: [],
        audioStates: [],
        sessionsStarted: 0,
        sessionsEnded: 0,
        createTrackCalls: 0
    };

    const userCreate = harness.options.createTrack;
    harness.options.createTrack = async () => {
        harness.createTrackCalls++;
        return userCreate();
    };

    const handle = installInterceptor(harness.options);
    return { harness, handle };
}

/**
 * A fake navigator.mediaDevices; requests are scripted per test.
 * Node's own `navigator` global is a getter, so it has to be replaced with
 * defineProperty instead of an assignment.
 */
function fakeMediaDevices(onDisplayMedia: () => Promise<FakeStream>, onUserMedia?: () => Promise<FakeStream>) {
    const devices = {
        getDisplayMedia: async () => await onDisplayMedia() as unknown as MediaStream,
        getUserMedia: async () => await (onUserMedia ?? onDisplayMedia)() as unknown as MediaStream
    };
    Object.defineProperty(globalThis, "navigator", {
        value: { mediaDevices: devices },
        configurable: true,
        writable: true
    });
    return devices;
}

/** Calls the (patched) media APIs with the double's result type. */
function callDisplayMedia(options: unknown): Promise<FakeStream> {
    return (navigator.mediaDevices.getDisplayMedia as unknown as (options?: unknown) => Promise<FakeStream>)(options);
}

function callUserMedia(constraints: unknown): Promise<FakeStream> {
    return (navigator.mediaDevices.getUserMedia as unknown as (constraints?: unknown) => Promise<FakeStream>)(constraints);
}

function lastAudioState(harness: Harness) {
    return harness.audioStates[harness.audioStates.length - 1]?.state;
}

function errorProblems(harness: Harness) {
    return harness.problems.filter(problem => problem.kind === "error");
}

// ------------------------------------------------------------------ policy

test("the fail closed policy is a pure decision table", () => {
    // Filtering switched off: Discord's own audio is left alone (the user's
    // explicit choice, not a failure).
    assert.equal(decideAudio({ filteringWanted: false, filteredTrack: false, fallbackAllowed: false }), "passthrough");
    assert.equal(decideAudio({ filteringWanted: false, filteredTrack: true, fallbackAllowed: false }), "passthrough");

    // Filtering wanted and possible.
    assert.equal(decideAudio({ filteringWanted: true, filteredTrack: true, fallbackAllowed: false }), "filtered");

    // Filtering wanted, impossible: mute, never transmit unfiltered audio.
    assert.equal(decideAudio({ filteringWanted: true, filteredTrack: false, fallbackAllowed: false }), "muted");

    // ... unless the user explicitly allowed the fallback.
    assert.equal(decideAudio({ filteringWanted: true, filteredTrack: false, fallbackAllowed: true }), "passthrough");
});

// ------------------------------------------------------------ happy path

test("a filtered track replaces Discord's system audio", async () => {
    const stream = makeStream();
    const filtered = makeTrack("audio");
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ createTrack: async () => filtered as unknown as MediaStreamTrack });

    try {
        const result = await callDisplayMedia({ audio: true, video: true });

        assert.equal(result, stream as unknown as FakeStream, "the same stream object is returned");
        assert.equal(stream.audio.length, 1, "exactly one audio track");
        assert.equal(stream.audio[0], filtered as unknown as FakeTrack, "our track is the audio of the share");
        assert.equal(stream.removed.length, 1, "Discord's system audio was removed");
        assert.equal(stream.removed[0].stopped, true, "and stopped, so it can not capture anything");
        assert.equal(harness.sessionsStarted, 1, "the session is reported as started");
        assert.equal(lastAudioState(harness), "filtered");
        assert.equal(handle.isFiltering(), true);
        assert.deepEqual(errorProblems(harness), [], "no error is reported");
    } finally {
        handle.uninstall();
    }
});

// --------------------------------------------------------------- fail closed

test("filtering wanted but impossible: the stream audio is removed, not passed through unfiltered", async () => {
    const stream = makeStream();
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ createTrack: async () => null });

    try {
        await callDisplayMedia({ audio: true });

        // The core of the audit: no unfiltered audio reaches the viewers.
        assert.equal(stream.audio.length, 0, "no audio track is left in the stream");
        assert.equal(stream.added.length, 0, "nothing was added");
        assert.equal(stream.removed.length, 1, "Discord's system audio was removed");
        assert.equal(lastAudioState(harness), "muted", "the state says the stream is muted");
        assert.equal(harness.sessionsEnded, 1, "the session is closed (capture gate closed as well)");
        assert.equal(handle.isFiltering(), false);

        const errors = errorProblems(harness);
        assert.equal(errors.length, 1, "the failure is reported as an error");
        assert.match(errors[0].message, /engine unavailable/, "the reason is in the message");
        assert.match(errors[0].message, /removed from the share/, "the message says that no audio is transmitted");
    } finally {
        handle.uninstall();
    }
});

test("a throwing filter mutes the share instead of breaking it", async () => {
    const stream = makeStream();
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({
        createTrack: async () => { throw new Error("the native addon crashed"); }
    });

    try {
        const result = await callDisplayMedia({ audio: true });

        assert.equal(result, stream as unknown as FakeStream, "the share still starts (the exception never reaches Discord)");
        assert.equal(stream.audio.length, 0, "but without any audio");
        assert.equal(lastAudioState(harness), "muted");
        assert.match(errorProblems(harness)[0].message, /native addon crashed/, "the engine error is reported verbatim");
    } finally {
        handle.uninstall();
    }
});

test("the unfiltered fallback only happens when the user enabled it, and it is announced", async () => {
    const stream = makeStream();
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({
        createTrack: async () => null,
        allowUnfilteredFallback: () => true
    });

    try {
        await callDisplayMedia({ audio: true });

        assert.equal(stream.audio.length, 1, "Discord's audio stays");
        assert.equal(stream.removed.length, 0, "nothing was removed");
        assert.equal(lastAudioState(harness), "unfiltered");
        const warnings = harness.problems.filter(problem => problem.kind === "warning");
        assert.equal(warnings.length, 1, "the user is warned, it is not silent");
        assert.match(warnings[0].message, /viewers hear every application|every application/i);
    } finally {
        handle.uninstall();
    }
});

test("filtering switched off by the user leaves Discord completely alone", async () => {
    const stream = makeStream();
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ isAvailable: () => false, createTrack: async () => null });

    try {
        await callDisplayMedia({ audio: true });

        assert.equal(stream.audio.length, 1, "Discord's audio is untouched");
        assert.equal(stream.removed.length, 0);
        assert.equal(harness.createTrackCalls, 0, "no capture is started for a disabled filter");
        assert.equal(lastAudioState(harness), "none");
        assert.equal(harness.problems.length, 1);
        assert.equal(harness.problems[0].kind, "info", "this is information, not an error");
    } finally {
        handle.uninstall();
    }
});

// --------------------------------------------------- the other entry points

test("desktop audio requested through getUserMedia is filtered the same way", async () => {
    const stream = makeStream();
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ createTrack: async () => null });

    try {
        await callUserMedia({
            audio: { mandatory: { chromeMediaSource: "desktop" } }
        });

        assert.equal(stream.audio.length, 0, "muted as well");
        assert.equal(lastAudioState(harness), "muted");
    } finally {
        handle.uninstall();
    }
});

test("the microphone is never touched", async () => {
    const stream = makeStream();
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ createTrack: async () => null });

    try {
        const result = await callUserMedia({ audio: true });

        assert.equal(result, stream as unknown as FakeStream);
        assert.equal(stream.audio.length, 1, "the microphone track is untouched");
        assert.equal(harness.createTrackCalls, 0, "no capture is started for the microphone");
        assert.deepEqual(harness.problems, [], "and nothing is reported");
    } finally {
        handle.uninstall();
    }
});

test("a video only share never gets audio forced onto it", async () => {
    const stream = makeStream(false);
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ createTrack: async () => makeTrack("audio") as unknown as MediaStreamTrack });

    try {
        await callDisplayMedia({ video: true, audio: false });

        assert.equal(stream.audio.length, 0, "no audio track is added");
        assert.equal(harness.createTrackCalls, 0, "nothing is captured");
        assert.equal(harness.problems[0].kind, "info");
    } finally {
        handle.uninstall();
    }
});

// ------------------------------------------- Discord without our entry points

test("a share that never went through the wrapped APIs is reported, not ignored", async () => {
    fakeMediaDevices(async () => makeStream());
    const { harness, handle } = install({ createTrack: async () => makeTrack("audio") as unknown as MediaStreamTrack });

    try {
        // Discord started a stream without any intercepted media request: the
        // plugin can not filter it, so the user has to know.
        handle.noteStreamStart();

        assert.equal(lastAudioState(harness), "unfiltered");
        const errors = errorProblems(harness);
        assert.equal(errors.length, 1, "a loud error is reported");
        assert.match(errors[0].message, /not requested through the plugin/i, "the share did not use the interceptor");
        assert.match(errors[0].message, /cannot be filtered/i, "and it says why that matters");
    } finally {
        handle.uninstall();
    }
});

test("a share that used the wrapped APIs is not reported as a problem", async () => {
    const stream = makeStream();
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ createTrack: async () => makeTrack("audio") as unknown as MediaStreamTrack });

    try {
        await callDisplayMedia({ audio: true });
        handle.noteStreamStart();

        assert.deepEqual(harness.problems, [], "no problem: the stream audio is ours");
        assert.equal(handle.isFiltering(), true);
    } finally {
        handle.uninstall();
    }
});

// -------------------------------------------------------------- live changes

test("when the filtered track dies the share is reported instead of silently going quiet", async () => {
    const stream = makeStream();
    const filtered = makeTrack("audio");
    fakeMediaDevices(async () => stream);
    const { harness, handle } = install({ createTrack: async () => filtered as unknown as MediaStreamTrack });

    try {
        await callDisplayMedia({ audio: true });
        filtered.fireEnded();

        assert.equal(handle.isFiltering(), false, "filtering is no longer active");
        assert.equal(harness.sessionsEnded, 1, "the session ended");
        const errors = errorProblems(harness);
        assert.equal(errors.length, 1, "an error is reported");
        assert.match(errors[0].message, /filtered share audio stopped/);
    } finally {
        handle.uninstall();
    }
});

test("uninstall restores the original APIs and can be installed again", async () => {
    const stream = makeStream();
    const devices = fakeMediaDevices(async () => stream);
    const original = devices.getDisplayMedia;
    const first = install();
    first.handle.uninstall();
    assert.equal(devices.getDisplayMedia, original, "the original function is back");

    const second = install();
    try {
        await callDisplayMedia({ audio: true });
        assert.equal(second.harness.createTrackCalls, 1, "the second install works normally");
    } finally {
        second.handle.uninstall();
    }
});
