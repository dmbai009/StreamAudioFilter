/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 *
 * End to end test of the boundary between JavaScript and C++: the addon is
 * loaded, the scripted test backend is used instead of WASAPI, and everything
 * the renderer does (start, rules, live changes, reading PCM, statistics, the
 * capture gate, stop) is exercised through the real N-API entry points.
 *
 * Run:  node tests/addon.test.mjs [path/to/streamaudiofilter.node]
 */

import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { resolve } from "node:path";
import { test } from "node:test";
import { setTimeout as delay } from "node:timers/promises";

const require = createRequire(import.meta.url);
const modulePath = resolve(process.argv[2] ?? "native/build/Release/streamaudiofilter.node");
const addon = require(modulePath);

const APPS = [
    { appKey: "exe:game", displayName: "Игра", exePath: "C:\\Games\\game.exe", exeName: "game.exe", pid: 100, parentPid: 1, playing: true, capturable: true },
    { appKey: "exe:music", displayName: "Плеер", exePath: "C:\\Music\\player.exe", exeName: "player.exe", pid: 200, parentPid: 1, playing: true, capturable: true },
    { appKey: "exe:chat", displayName: "Чат", exePath: "C:\\Chat\\chat.exe", exeName: "chat.exe", pid: 300, parentPid: 1, playing: false, capturable: true }
];

const json = value => JSON.parse(value);

/** Waits until the condition holds, so a slow CI machine cannot make the test flaky. */
async function waitUntil(condition, what, timeoutMs = 3000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (condition()) return;
        await delay(25);
    }
    assert.fail(`timed out waiting until ${what}`);
}

/** Peak absolute sample of an interleaved float32 buffer. */
function peakOf(buffer) {
    const view = new Float32Array(buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength));
    let peak = 0;
    for (const sample of view) peak = Math.max(peak, Math.abs(sample));
    return peak;
}

test("version reports the addon and its Node-API level", () => {
    const version = json(addon.version());
    assert.match(version.version, /^\d+\.\d+\.\d+$/);
    assert.equal(version.napi, 8);
});

test("probe explains the situation instead of guessing", () => {
    const info = json(addon.probe("auto"));
    assert.equal(typeof info.supported, "boolean");
    assert.equal(typeof info.os, "string");
    assert.equal(typeof info.reason, "string");
    assert.ok(Array.isArray(info.checks));

    // On the test backend the same call must explicitly report a working
    // fallback, so a developer can verify the interface anywhere.
    const mock = json(addon.probe("mock"));
    assert.ok(Array.isArray(mock.checks));
});

test("reading before the engine runs is a no-op, not a crash", () => {
    const empty = addon.readPcm(4800);
    assert.equal(empty.byteLength, 0);
    assert.equal(json(addon.stats()).running, false);
    assert.equal(json(addon.stop()).ok, true);
});

test("the whole pipeline works end to end on the test backend", async () => {
    const started = json(addon.start({ sampleRate: 48000, blockMs: 10, maxStreams: 8, outputBufferMs: 200, backend: "mock" }));
    assert.equal(started.ok, true, started.error);
    assert.equal(json(addon.stats()).running, true);

    addon.setMockApps(APPS);
    addon.refresh();

    // The list is visible even before anything is captured.
    await waitUntil(() => json(addon.apps()).length === 3, "the application list is visible");
    const apps = json(addon.apps());
    assert.equal(apps.length, 3);
    const game = apps.find(app => app.appKey === "exe:game");
    assert.equal(game.displayName, "Игра");
    assert.ok(["transmitting", "silent", "excluded"].includes(game.state));

    // Nothing is captured while the capture gate is closed ...
    addon.setRules({ mode: 0, overrides: {}, active: false });
    await delay(200);
    assert.equal(json(addon.stats()).activeStreams, 0, "capture gate must keep the engine quiet");

    // ... and the mixing starts as soon as it opens.
    addon.setRules({ mode: 0, overrides: {}, active: true });
    await waitUntil(() => json(addon.stats()).activeStreams >= 2, "the captures start");
    const live = json(addon.stats());
    assert.ok(live.activeStreams >= 2, `expected at least two captures, got ${live.activeStreams}`);

    // PCM arrives, is stereo float32 and is not silence. The first block after
    // starting is silence by design: audio begins with the first captured block
    // instead of a jump from zero, which is what keeps the start click free.
    let pcm = addon.readPcm(4800);
    for (let attempt = 0; attempt < 40 && peakOf(pcm) < 0.05; attempt++) {
        await delay(50);
        pcm = addon.readPcm(4800);
    }
    assert.equal(pcm.byteLength, 4800 * 2 * 4, "4800 frames of interleaved stereo float32");
    assert.ok(peakOf(pcm) > 0.05, "the mixed signal must not be silent");
    assert.ok(peakOf(pcm) <= 1.0, "the limiter must keep the signal inside the valid range");

    // Excluding an application right now must keep the output flowing.
    const excluded = json(addon.setRules({ mode: 0, overrides: { "exe:game": false }, active: true }));
    assert.equal(excluded.ok, true);
    await waitUntil(() => {
        const game = json(addon.apps()).find(app => app.appKey === "exe:game");
        return game && game.transmitting === false;
    }, "the exclusion is applied");

    // Reads are paced like the renderer does it (it keeps two requests in
    // flight): the engine must never stall because an application was switched
    // off, and it must not answer with "nothing" for a whole second either.
    let frames = 0;
    for (let i = 0; i < 20; i++) {
        frames += addon.readPcm(960).byteLength / 8;
        await delay(20);
    }
    assert.ok(frames >= 20 * 960 * 0.5, `output must stay continuous, got ${frames} frames`);

    const afterExclusion = json(addon.apps()).find(app => app.appKey === "exe:game");
    assert.equal(afterExclusion.transmitting, false);
    assert.equal(afterExclusion.state, "excluded");
    assert.equal(afterExclusion.captured, false);

    const other = json(addon.apps()).find(app => app.appKey === "exe:music");
    assert.equal(other.transmitting, true, "other applications keep playing to the viewers");

    // Mode B is empty without explicit rules.
    addon.setRules({ mode: 1, overrides: {}, active: true });
    await waitUntil(() => json(addon.stats()).activeStreams === 0, "mode B transmits nothing by default");

    // ... and only what was allowed explicitly.
    addon.setRules({ mode: 1, overrides: { "exe:music": true }, active: true });
    await waitUntil(() => json(addon.stats()).activeStreams === 1, "mode B captures exactly the allowed application");
    const modeB = json(addon.apps());
    assert.equal(modeB.find(app => app.appKey === "exe:music").transmitting, true);
    assert.equal(modeB.find(app => app.appKey === "exe:game").transmitting, false);

    // A new application appears while the stream is running.
    addon.setMockApps([...APPS, { appKey: "exe:new", displayName: "Новое", exePath: "C:\\New\\new.exe", pid: 400, parentPid: 1, playing: true, capturable: true }]);
    addon.setRules({ mode: 0, overrides: {}, active: true });
    await waitUntil(() => json(addon.apps()).length === 4, "a new application shows up");
    const withNew = json(addon.apps());
    assert.equal(withNew.length, 4);
    assert.equal(withNew.find(app => app.appKey === "exe:new").transmitting, true, "mode A picks up new applications automatically");

    // An application that disappears stops being captured without touching the mix.
    addon.setMockApps(APPS.slice(1));
    await waitUntil(() => json(addon.apps()).length === 2, "closed applications disappear from the list");
    const reduced = json(addon.apps());
    assert.deepEqual(reduced.map(app => app.appKey).sort(), ["exe:chat", "exe:music"]);
    assert.ok(json(addon.stats()).outputFrames > 0, "the output stream survives applications coming and going");

    // Statistics are the numbers the panel shows.
    const stats = json(addon.stats());
    assert.equal(stats.sampleRate, 48000);
    assert.equal(stats.channels, 2);
    assert.ok(stats.blocksMixed > 0);
    assert.ok(Array.isArray(stats.warnings));

    assert.equal(json(addon.stop()).ok, true);
    assert.equal(json(addon.stats()).running, false);
    assert.equal(addon.readPcm(480).byteLength, 0);
});

test("an application started twice is one entry with two captured instances", async () => {
    assert.equal(json(addon.start({ sampleRate: 48000, blockMs: 10, maxStreams: 8, outputBufferMs: 200, backend: "mock" })).ok, true);

    // Same application (same executable), two independent process trees.
    const game = APPS[0];
    addon.setMockApps([
        game,
        { ...game, pid: 302 }
    ]);
    addon.refresh();
    addon.setRules({ mode: 1, overrides: { "exe:game": true }, active: true });

    await waitUntil(() => {
        const listed = json(addon.apps());
        return listed.length === 1 && listed[0].instances === 2;
    }, "both instances are grouped into one entry");

    const listed = json(addon.apps());
    assert.equal(listed.length, 1, "one application, not two entries");
    assert.equal(listed[0].instances, 2, "the panel knows that two instances are summed");
    assert.equal(listed[0].captured, true);
    assert.match(listed[0].format, /экземпляр/, `format describes both trees: ${listed[0].format}`);

    // One stream for the application: the two instances are summed inside the
    // capture, so the engine does not mix the same application twice.
    await waitUntil(() => json(addon.stats()).activeStreams === 1, "exactly one stream for the application");
    await waitUntil(() => {
        const pcm = addon.readPcm(4800);
        return pcm.byteLength > 0 && peakOf(pcm) > 0.05;
    }, "the summed audio of both instances is transmitted");

    addon.setRules({ mode: 0, overrides: {}, active: false });
    addon.stop();
});

test("bad arguments are clamped instead of breaking the engine", async () => {
    json(addon.start({ sampleRate: 12345, blockMs: 1000, maxStreams: 0, backend: "mock" }));
    const stats = json(addon.stats());
    assert.equal(stats.sampleRate, 48000, "an unsupported sample rate falls back to 48 kHz");

    // A negative frame count is meaningless; the addon answers with one default
    // block (10 ms) instead of failing, and never with more than that.
    assert.ok(addon.readPcm(-100).byteLength <= 480 * 2 * 4);
    assert.ok(addon.readPcm(0).byteLength <= 480 * 2 * 4);
    // An absurd request is clamped (480000 frames) instead of allocating
    // whatever the renderer asks for.
    const clamped = addon.readPcm(1e9);
    assert.ok(clamped.byteLength <= 480000 * 2 * 4,
        `a huge request must be clamped, got ${clamped.byteLength} bytes`);
    assert.ok(Buffer.isBuffer(clamped));

    assert.equal(json(addon.setRules({})).ok, true, "an incomplete rule set must not throw");
    addon.stop();
});
