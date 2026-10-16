/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — the decision of what Discord captures for a share.
 * This is the privacy critical table: excluded applications must never reach
 * the viewers through a path the plugin controls, and the plugin must never
 * claim control it does not have.
 *
 * Run:  npx tsx tests/soundshare.test.ts
 */

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";

import { decideSoundshare, isOwnStream, readGoLiveAction, type SoundshareInput, SYSTEM_SOUNDSHARE_PID } from "../stream/soundshare";

const HOST = 4242;

function input(partial: Partial<SoundshareInput> = {}): SoundshareInput {
    return {
        context: "stream",
        sourceId: "screen:0:0",
        sound: true,
        originalPid: SYSTEM_SOUNDSHARE_PID,
        filterEnabled: true,
        hostPid: HOST,
        hostProblem: "",
        fallbackAllowed: false,
        ...partial
    };
}

test("a whole screen share with audio is redirected to the host", () => {
    assert.deepEqual(decideSoundshare(input()), { pid: HOST, audio: "starting", redirect: true, reason: "" });
});

test("fail closed: no host and no explicit fallback means no audio at all", () => {
    const decision = decideSoundshare(input({ hostPid: 0, hostProblem: "saf-audio-host.exe was not found" }))!;
    assert.equal(decision.pid, null, "Discord must capture nothing");
    assert.equal(decision.audio, "muted");
    assert.equal(decision.redirect, false);
    assert.match(decision.reason, /not found/);
});

test("the unfiltered fallback only happens when the user enabled it, and says so", () => {
    const decision = decideSoundshare(input({ hostPid: 0, hostProblem: "x", fallbackAllowed: true }))!;
    assert.equal(decision.pid, SYSTEM_SOUNDSHARE_PID);
    assert.equal(decision.audio, "unfiltered");
});

test("a working host is used even when the fallback is enabled", () => {
    assert.equal(decideSoundshare(input({ fallbackAllowed: true }))!.pid, HOST);
});

test("filtering switched off leaves Discord's capture alone", () => {
    assert.deepEqual(decideSoundshare(input({ filterEnabled: false })), { pid: SYSTEM_SOUNDSHARE_PID, audio: "off", redirect: false, reason: "" });
});

test("window shares keep the window's own application", () => {
    const decision = decideSoundshare(input({ sourceId: "window:12345:0", originalPid: 777 }))!;
    assert.equal(decision.pid, 777);
    assert.equal(decision.audio, "window");
    assert.equal(decision.redirect, false);
});

test("Discord's sound switch off: nothing is added", () => {
    const decision = decideSoundshare(input({ sound: false }))!;
    assert.equal(decision.audio, "noAudio");
    assert.equal(decision.redirect, false);
});

test("other contexts (clips, default) are never touched", () => {
    assert.equal(decideSoundshare(input({ context: "default" })), null);
    assert.equal(decideSoundshare(input({ context: undefined })), null);
});

test("reading Discord's action is defensive", () => {
    assert.deepEqual(readGoLiveAction(undefined), { context: undefined, sourceId: "", sound: false });
    assert.deepEqual(readGoLiveAction({ settings: null }), { context: undefined, sourceId: "", sound: false });
    assert.deepEqual(
        readGoLiveAction({ settings: { context: "stream", desktopSettings: { sourceId: "screen:1:0", sound: true } } }),
        { context: "stream", sourceId: "screen:1:0", sound: true }
    );
});

test("own stream detection", () => {
    assert.equal(isOwnStream("guild:1:2:999", "999"), true);
    assert.equal(isOwnStream("call:2:999", "999"), true);
    assert.equal(isOwnStream("guild:1:2:998", "999"), false);
    assert.equal(isOwnStream(undefined, "999"), false);
    assert.equal(isOwnStream("guild:1:2:999", undefined), false);
});

test("the patch regexes match the Discord code they were written against", () => {
    // Excerpt of Discord's MediaEngineStore (desktop 1.0.9262, October 2026),
    // the exact text the plugin's patch has to find.
    const excerpt = "o=i.startsWith(`${eq.fS.ACTIVITY}:`),d=o?null:eG.A.getPidFromDesktopSource(i);p.isPlatformEmbedded&&!o&&({soundshareId:e,soundshareSession:n}=nD(d,r)),s!==tl&&(null!=a&&tt.setGoLiveSource(null,tl),tl=s)"
        + ";getExperimentalSoundshare(){let e=t7().experimentalSoundshare2;return this.supportsExperimentalSoundshare()}";
    // Vencord's \i: an identifier.
    const source = readFileSync(join(__dirname, "..", "index.tsx"), "utf8").replace(/\r\n/g, "\n");
    const patterns = [...source.matchAll(/match: (\/.*\/),?\n/g)].map(m => m[1]).slice(0, 2);
    assert.equal(patterns.length, 2);
    for (const literal of patterns) {
        const body = literal.slice(1, literal.lastIndexOf("/")).replaceAll("\\i", "[A-Za-z_$][\\w$]*");
        const matches = excerpt.match(new RegExp(body, "g")) ?? [];
        assert.equal(matches.length, 1, `exactly one match for ${literal}`);
    }
});
