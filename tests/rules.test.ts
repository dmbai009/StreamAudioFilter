/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Tests for the rule engine: the two modes, live changes, persistence and the
 * order of the application list. No Vencord and no Discord is needed —
 * apps/rules.ts is pure by design.
 *
 * Run:  npx tsx tests/rules.test.ts
 */

import assert from "node:assert/strict";
import { test } from "node:test";

import {
    buildAppList,
    emptyState,
    forgetApp,
    normaliseState,
    resetRules,
    resolveTransmit,
    seedDefaults,
    setMode,
    setTransmit,
    toPayload
} from "../apps/rules";
import type { AppInfo, RulesState } from "../types";

const DISCORD = "C:\\Users\\user\\AppData\\Local\\Discord\\app-1.0.9\\Discord.exe";

function app(partial: Partial<AppInfo> & { appKey: string; }): AppInfo {
    return {
        appKey: partial.appKey,
        displayName: partial.displayName ?? partial.appKey,
        exeName: partial.exeName ?? "app.exe",
        exePath: partial.exePath ?? "C:\\Apps\\app.exe",
        playing: partial.playing ?? true,
        capturable: partial.capturable ?? true,
        ephemeral: partial.ephemeral ?? false,
        transmitting: partial.transmitting ?? false,
        captured: partial.captured ?? false,
        peak: partial.peak ?? 0,
        latencyMs: partial.latencyMs ?? -1,
        format: partial.format ?? "",
        error: partial.error ?? "",
        notCapturableReason: partial.notCapturableReason ?? "",
        starvedFrames: 0,
        state: partial.state ?? "silent"
    };
}

test("mode A transmits unknown applications, mode B does not", () => {
    const all = emptyState("allExcept");
    assert.equal(resolveTransmit(all, "exe:a"), true);

    const only = emptyState("onlySelected");
    assert.equal(resolveTransmit(only, "exe:a"), false);
});

test("an explicit choice wins over the mode and is never reset by switching modes", () => {
    let state = emptyState("allExcept");
    state = setTransmit(state, app({ appKey: "exe:spotify", displayName: "Spotify", exePath: "C:\\Music\\Spotify.exe" }), false);
    state = setTransmit(state, app({ appKey: "exe:game", displayName: "Game", exePath: "C:\\Games\\game.exe" }), true);

    assert.equal(resolveTransmit(state, "exe:spotify"), false);
    assert.equal(resolveTransmit(state, "exe:game"), true);
    assert.equal(resolveTransmit(state, "exe:telegram"), true);

    // Switching to mode B must keep both explicit decisions.
    state = setMode(state, "onlySelected");
    assert.equal(resolveTransmit(state, "exe:spotify"), false);
    assert.equal(resolveTransmit(state, "exe:game"), true);
    // ... and in mode B everything that was never touched stays muted.
    assert.equal(resolveTransmit(state, "exe:telegram"), false);

    state = setMode(state, "allExcept");
    assert.equal(resolveTransmit(state, "exe:telegram"), true);
});

test("rules remember the name and path of an application that is already closed", () => {
    let state = emptyState("allExcept");
    state = setTransmit(state, app({ appKey: "exe:music", displayName: "Плеер", exePath: "C:\\Music\\player.exe" }), false);

    const entries = buildAppList([], state);
    assert.equal(entries.length, 1);
    assert.equal(entries[0].title, "Плеер");
    assert.equal(entries[0].section, "closed");
    assert.equal(entries[0].transmitting, false);
    assert.match(entries[0].subtitle, /закрыт/i);
});

test("the list separates applications with audio, silent ones and saved rules", () => {
    const state = setTransmit(emptyState("allExcept"), app({ appKey: "exe:closed" }), false);
    const live = [
        app({ appKey: "exe:quiet", displayName: "Тихий", playing: false }),
        app({ appKey: "exe:loud", displayName: "Громкий", peak: 0.8, captured: true, state: "transmitting" }),
        app({ appKey: "exe:soft", displayName: "Средний", peak: 0.2 })
    ];

    const entries = buildAppList(live, state, DISCORD);
    const keys = entries.map(entry => entry.appKey);

    assert.deepEqual(keys, ["exe:loud", "exe:soft", "exe:quiet", "exe:closed"]);
    assert.equal(entries[0].section, "current");
    assert.equal(entries[2].section, "idle");
    assert.equal(entries[3].section, "closed");
});

test("applications that can not be captured are marked and never shown as transmitted", () => {
    const state = emptyState("allExcept");
    const entries = buildAppList([
        app({ appKey: "exe:protected", capturable: false, notCapturableReason: "защищённый процесс", state: "unsupported" })
    ], state);

    assert.equal(entries[0].capturable, false);
    assert.equal(entries[0].transmitting, false);
    assert.match(entries[0].error, /защищ/);
});

test("the payload carries the mode, every explicit decision and the capture gate", () => {
    let state = seedDefaults(emptyState("allExcept"), DISCORD);
    state = setTransmit(state, app({ appKey: "exe:music" }), false);

    const stopped = toPayload(state, false);
    assert.equal(stopped.mode, 0);
    assert.equal(stopped.active, false);
    assert.equal(stopped.overrides["exe:music"], false);
    assert.equal(stopped.overrides[`exe:${DISCORD.toLowerCase()}`], false);

    const running = toPayload(setMode(state, "onlySelected"), true);
    assert.equal(running.mode, 1);
    assert.equal(running.active, true);
});

test("Discord itself is excluded by default so the voice channel is never sent back", () => {
    const first = seedDefaults(emptyState("allExcept"), DISCORD);
    const key = `exe:${DISCORD.toLowerCase()}`;
    assert.equal(first.rules[key].transmit, false);

    // Seeding twice must not undo a decision the user made in the meantime.
    const changed = setTransmit(first, app({ appKey: key }), true);
    const again = seedDefaults(changed, DISCORD);
    assert.equal(again.rules[key].transmit, true);

    // ... and without a known Discord path nothing is seeded at all.
    assert.deepEqual(seedDefaults(emptyState("allExcept"), "").rules, {});
});

test("reset keeps the mode, forget removes a single application", () => {
    let state = setMode(emptyState("allExcept"), "onlySelected");
    state = setTransmit(state, app({ appKey: "exe:a" }), true);
    state = setTransmit(state, app({ appKey: "exe:b" }), false);

    const afterReset = resetRules(state);
    assert.equal(afterReset.mode, "onlySelected");
    assert.deepEqual(afterReset.rules, {});

    const afterForget = forgetApp(state, "exe:a");
    assert.equal(afterForget.rules["exe:a"], undefined);
    assert.equal(afterForget.rules["exe:b"] !== undefined, true);
});

test("a broken or outdated store never breaks the plugin", () => {
    const broken = [null, undefined, 42, "text", {}, { mode: "nonsense", rules: { a: 1, b: { transmit: "yes" } } }];

    for (const raw of broken) {
        const state: RulesState = normaliseState(raw as unknown);
        assert.equal(state.mode, "allExcept");
        assert.deepEqual(state.rules, {});
    }

    const valid = normaliseState({
        mode: "onlySelected",
        version: 1,
        rules: { "exe:a": { transmit: false, name: "A", exePath: "C:\\a.exe", lastSeen: 1 }, "exe:b": { transmit: true } }
    });
    assert.equal(valid.mode, "onlySelected");
    assert.equal(valid.rules["exe:a"].name, "A");
    assert.equal(valid.rules["exe:b"].transmit, true);
});

test("different applications with the same display name stay separate", () => {
    let state = emptyState("allExcept");
    state = setTransmit(state, app({ appKey: "exe:first", displayName: "Chrome", exePath: "C:\\A\\chrome.exe" }), false);

    const entries = buildAppList([
        app({ appKey: "exe:first", displayName: "Chrome", exePath: "C:\\A\\chrome.exe" }),
        app({ appKey: "exe:second", displayName: "Chrome", exePath: "D:\\B\\chrome.exe" })
    ], state);

    const first = entries.find(entry => entry.appKey === "exe:first")!;
    const second = entries.find(entry => entry.appKey === "exe:second")!;
    assert.equal(first.transmitting, false);
    assert.equal(second.transmitting, true);
});
