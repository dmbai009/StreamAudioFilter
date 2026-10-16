/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — tests for the rule engine: switch semantics, both
 * modes, persistence, migration and the application list. apps/rules.ts is
 * pure, so neither Vencord nor Discord is needed.
 *
 * Run:  npx tsx tests/rules.test.ts
 */

import assert from "node:assert/strict";
import { test } from "node:test";

import { buildAppList, emptyState, forgetApp, normaliseState, resetRules, resolveTransmit, RULES_VERSION, setMode, setTransmit, toPayload } from "../apps/rules";
import type { AppInfo } from "../types";

function app(partial: Partial<AppInfo> & { appKey: string; }): AppInfo {
    return {
        displayName: partial.appKey,
        exeName: "app.exe",
        exePath: `C:\\Apps\\${partial.appKey}.exe`,
        playing: true,
        capturable: true,
        ephemeral: false,
        transmitting: false,
        captured: false,
        peak: 0,
        latencyMs: -1,
        format: "",
        error: "",
        notCapturableReason: "",
        blockedReason: "",
        coveredBy: "",
        starvedFrames: 0,
        state: "silent",
        instances: 1,
        devices: [],
        silentCapture: false,
        ...partial
    };
}

test("the mode only decides apps without a rule", () => {
    let state = emptyState("allExcept");
    assert.equal(resolveTransmit(state, "exe:a"), true);
    state = setTransmit(state, { appKey: "exe:a" }, false);
    assert.equal(resolveTransmit(state, "exe:a"), false);
    assert.equal(resolveTransmit(state, "exe:b"), true, "untouched app follows mode A");

    state = setMode(state, "onlySelected");
    assert.equal(resolveTransmit(state, "exe:a"), false, "a saved choice survives a mode change");
    assert.equal(resolveTransmit(state, "exe:b"), false, "untouched app follows mode B");

    state = setTransmit(state, { appKey: "exe:b" }, true);
    assert.equal(resolveTransmit(state, "exe:b"), true);
});

test("the switch value never flips meaning with the mode", () => {
    const live = [app({ appKey: "exe:a" }), app({ appKey: "exe:b" })];
    let state = setTransmit(emptyState("allExcept"), { appKey: "exe:a" }, true);

    const a1 = buildAppList(live, state).find(e => e.appKey === "exe:a")!;
    state = setMode(state, "onlySelected");
    const a2 = buildAppList(live, state).find(e => e.appKey === "exe:a")!;
    assert.equal(a1.allowed, true);
    assert.equal(a2.allowed, true, "an explicitly allowed app stays allowed in both modes");

    const b = buildAppList(live, state).find(e => e.appKey === "exe:b")!;
    assert.equal(b.allowed, false);
    assert.equal(b.byDefault, true, "an app without a rule is marked as following the default");
    assert.equal(a2.byDefault, false);
});

test("payload carries every saved choice and the capture gate", () => {
    const state = setTransmit(setTransmit(emptyState("onlySelected"), { appKey: "exe:a" }, true), { appKey: "exe:b" }, false);
    assert.deepEqual(toPayload(state, true), { mode: 1, overrides: { "exe:a": true, "exe:b": false }, active: true });
    assert.equal(toPayload(state, false).active, false);
});

test("reset and forget", () => {
    let state = setTransmit(setTransmit(emptyState(), { appKey: "exe:a" }, false), { appKey: "exe:b" }, false);
    state = forgetApp(state, "exe:a");
    assert.deepEqual(Object.keys(state.rules), ["exe:b"]);
    state = resetRules(state);
    assert.deepEqual(state.rules, {});
});

test("normalising drops garbage and migrates version 1", () => {
    const raw = {
        mode: "onlySelected",
        version: 1,
        rules: {
            "exe:c:\\users\\u\\appdata\\local\\discord\\app-1.0.9045\\discord.exe": { transmit: false, name: "Discord (voice call)" },
            "exe:c:\\games\\game.exe": { transmit: true, name: "Game" },
            "exe:broken": { transmit: "yes" },
            "exe:null": null
        }
    };
    const state = normaliseState(raw);
    assert.equal(state.mode, "onlySelected");
    assert.equal(state.version, RULES_VERSION);
    assert.deepEqual(Object.keys(state.rules), ["exe:c:\\games\\game.exe"], "the seeded Discord rule of v1 is removed, broken rules are dropped");

    const user = normaliseState({ version: 1, rules: { "exe:c:\\tools\\discord.exe": { transmit: false, name: "My Discord tool" } } });
    assert.equal(Object.keys(user.rules).length, 1, "a rule the user made is never removed by the migration");

    assert.deepEqual(normaliseState("nonsense"), emptyState());
});

test("list sections, closed apps and non capturable apps", () => {
    let state = setTransmit(emptyState(), { appKey: "exe:closed", displayName: "Closed app", exePath: "C:\\x\\closed.exe" }, false);
    state = setTransmit(state, { appKey: "exe:uwp" }, true);
    const live = [
        app({ appKey: "exe:b", displayName: "Beta", playing: true }),
        app({ appKey: "exe:a", displayName: "Alpha", playing: false }),
        app({ appKey: "exe:uwp", displayName: "Store app", capturable: false, notCapturableReason: "protected" })
    ];
    const list = buildAppList(live, state);
    assert.deepEqual(list.map(e => [e.appKey, e.section]), [
        ["exe:b", "current"],
        ["exe:uwp", "current"],
        ["exe:a", "idle"],
        ["exe:closed", "closed"]
    ]);
    assert.equal(list.find(e => e.appKey === "exe:uwp")!.allowed, false, "an app that can not be captured is never shown as allowed");
    const closed = list.find(e => e.appKey === "exe:closed")!;
    assert.equal(closed.title, "Closed app");
    assert.equal(closed.exeName, "closed.exe");
});

test("live state passes through to the list", () => {
    const list = buildAppList([app({
        appKey: "exe:a", captured: true, silentCapture: true, devices: ["Line 1 (Virtual Audio Cable)"],
        state: "blocked", blockedReason: "Game runs inside", coveredBy: "", instances: 2
    })], emptyState());
    const [entry] = list;
    assert.equal(entry.captured, true);
    assert.equal(entry.silentCapture, true);
    assert.deepEqual(entry.devices, ["Line 1 (Virtual Audio Cable)"]);
    assert.equal(entry.blockedReason, "Game runs inside");
    assert.equal(entry.instances, 2);
});

test("audio routers are blocked by default in both modes, but can be allowed", () => {
    const repeater = "exe:c:\\program files\\virtual audio cable\\audiorepeater.exe";
    let state = emptyState("allExcept");
    assert.equal(resolveTransmit(state, repeater), false, "a repeater re-plays everything: never sent by default");
    assert.deepEqual(toPayload(state, true, [repeater, "exe:c:\\a.exe"]).overrides, { [repeater]: false });
    const [entry] = buildAppList([app({ appKey: repeater })], state);
    assert.equal(entry.router, true);
    assert.equal(entry.allowed, false);
    state = setTransmit(state, { appKey: repeater }, true);
    assert.equal(resolveTransmit(state, repeater), true, "an explicit choice wins");
    assert.deepEqual(toPayload(state, true, [repeater]).overrides, { [repeater]: true });
});

test("non capturable apps are not marked as following the default", () => {
    const [entry] = buildAppList([app({ appKey: "system:sounds", capturable: false })], emptyState());
    assert.equal(entry.byDefault, false);
});
