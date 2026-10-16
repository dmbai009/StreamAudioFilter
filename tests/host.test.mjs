/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const hostPath = process.argv[2] ?? process.env.SAF_HOST;
if (!hostPath) {
    console.error("usage: node tests/host.test.mjs <saf-audio-host>");
    process.exit(2);
}

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function startHost(args = []) {
    const child = spawn(hostPath, ["--backend", "mock", "--output", "null", ...args], { stdio: "pipe" });
    let buffer = "";
    let nextId = 1;
    const waiting = new Map();
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", chunk => {
        buffer += chunk;
        let newline;
        while ((newline = buffer.indexOf("\n")) >= 0) {
            const line = buffer.slice(0, newline);
            buffer = buffer.slice(newline + 1);
            const answer = JSON.parse(line);
            waiting.get(answer.id)?.(answer);
            waiting.delete(answer.id);
        }
    });
    const send = (...args) => new Promise((resolve, reject) => {
        const id = nextId++;
        const timer = setTimeout(() => reject(new Error(`no answer to ${args[0]}`)), 5000);
        waiting.set(id, answer => { clearTimeout(timer); resolve(answer); });
        child.stdin.write([id, ...args].join("\t") + "\n");
    });
    const exited = new Promise(resolve => child.on("exit", code => resolve(code)));
    // A failing assertion must not leave the host running (and the runner hanging).
    test.after?.(() => { if (child.exitCode === null) child.kill(); });
    return { child, send, exited };
}

const app = (key, name, pid, parent = 1) => [key, name, `C:\\${name}\\${name}.exe`, `${name}.exe`, pid, parent, 1, 1].join("\x1f");

test("handshake: version and protocol match the plugin", async () => {
    const { send, child, exited } = startHost();
    const hello = await send("hello");
    const version = readFileSync(join(root, "version.ts"), "utf8");
    assert.equal(hello.ok, true);
    assert.equal(hello.version, /PLUGIN_VERSION = "([^"]+)"/.exec(version)[1], "host and plugin versions must be equal");
    assert.equal(hello.protocol, Number(/PROTOCOL_VERSION = (\d+)/.exec(version)[1]));
    assert.equal(hello.pid, child.pid);
    child.stdin.end();
    assert.equal(await exited, 0, "the host exits cleanly when stdin closes");
});

test("listing, capture gate, rules reach the mix, output level", async () => {
    const { send, child, exited } = startHost();
    assert.equal((await send("start", 8)).ok, true);
    assert.equal((await send("mockApps", app("mock:game", "Game", 4200), app("mock:music", "Music", 4300))).ok, true);
    assert.equal((await send("output", 1)).ok, true);
    await send("rules", 0, 0);
    await sleep(600);

    let { apps } = await send("apps");
    assert.deepEqual(apps.map(a => a.appKey).sort(), ["mock:game", "mock:music"]);
    assert.ok(apps.every(a => !a.captured), "nothing is captured while the gate is closed");

    await send("rules", 0, 1, "mock:music", 0);
    await sleep(2300);
    apps = (await send("apps")).apps;
    assert.equal(apps.find(a => a.appKey === "mock:game").state, "transmitting");
    assert.equal(apps.find(a => a.appKey === "mock:music").state, "excluded");
    let stats = await send("stats");
    assert.equal(stats.engine.activeStreams, 1);
    assert.ok(stats.output.running && stats.output.renderedFrames > 0);
    assert.ok(stats.output.peak >= 200 && stats.output.peak <= 300, `one 0.25 tone on the output, got ${stats.output.peak / 1000}`);

    await send("rules", 1, 1);
    // The output peak is measured over completed one second windows.
    await sleep(2300);
    stats = await send("stats");
    assert.equal(stats.engine.activeStreams, 0, "mode B without rules captures nothing");
    assert.equal(stats.output.peak, 0, "and the output is silent");

    await send("output", 0);
    assert.equal((await send("stats")).output.running, false);
    child.stdin.end();
    assert.equal(await exited, 0);
});

test("Discord's process tree is never captured", async () => {
    const { send, child, exited } = startHost(["--protect", "5000"]);
    await send("start", 8);
    await send("mockApps", app("mock:discord", "Discord", 5001, 5000), app("mock:game", "Game", 4200));
    await send("rules", 0, 1);
    await sleep(1200);
    const { apps } = await send("apps");
    const discord = apps.find(a => a.appKey === "mock:discord");
    assert.equal(discord.capturable, false);
    assert.equal(discord.captured, false);
    assert.equal(apps.find(a => a.appKey === "mock:game").captured, true);
    child.stdin.end();
    assert.equal(await exited, 0);
});

test("unknown commands and bad input are answered, not fatal", async () => {
    const { send, child, exited } = startHost();
    assert.equal((await send("nonsense")).ok, false);
    assert.equal((await send("rules", 0, 1)).ok, false, "rules without a running engine are refused");
    assert.equal((await send("output", 1)).ok, false, "output without a running engine is refused");
    assert.equal((await send("hello")).ok, true, "still alive");
    child.stdin.end();
    assert.equal(await exited, 0);
});
