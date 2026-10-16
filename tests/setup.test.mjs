/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — tests for saf-setup.exe on a fake user profile: every
 * scenario runs against a temporary %APPDATA% / %LOCALAPPDATA% with a fake
 * Discord, through the real command line of saf-setup.exe (Windows only).
 *     node tests/setup.test.mjs <saf-setup.exe> <saf-audio-host.exe>
 */

import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const [setupExe, hostExe] = process.argv.slice(2);
if (!setupExe || !hostExe) {
    console.error("usage: node tests/setup.test.mjs <saf-setup.exe> <saf-audio-host.exe>");
    process.exit(2);
}
const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const VERSION = /PLUGIN_VERSION = "([^"]+)"/.exec(readFileSync(join(root, "version.ts"), "utf8"))[1];
const ORIGINAL_ASAR = Buffer.alloc(200 * 1024, 7); // stands in for Discord's real app.asar

function profile({ vencord = false, injected = false } = {}) {
    const base = mkdtempSync(join(tmpdir(), "saf-setup-"));
    const appData = join(base, "Roaming");
    const local = join(base, "Local");
    const resources = join(local, "Discord", "app-1.0.9262", "resources");
    mkdirSync(resources, { recursive: true });
    mkdirSync(join(local, "Discord", "app-1.0.9100", "resources"), { recursive: true }); // an older version: must be ignored
    if (injected) {
        writeFileSync(join(resources, "_app.asar"), ORIGINAL_ASAR);
        writeFileSync(join(resources, "app.asar"), `require("${join(appData, "Vencord", "dist", "patcher.js").replaceAll("\\", "\\\\")}")`);
    } else {
        writeFileSync(join(resources, "app.asar"), ORIGINAL_ASAR);
    }
    if (vencord) {
        mkdirSync(join(appData, "Vencord", "dist"), { recursive: true });
        writeFileSync(join(appData, "Vencord", "dist", "patcher.js"), "// the user's own Vencord");
    }
    return { base, appData, local, resources, discord: join(local, "Discord") };
}

function payload({ version = VERSION } = {}) {
    const dir = mkdtempSync(join(tmpdir(), "saf-payload-"));
    mkdirSync(join(dir, "dist"));
    writeFileSync(join(dir, "dist", "patcher.js"), "// patcher");
    writeFileSync(join(dir, "dist", "preload.js"), "// preload");
    writeFileSync(join(dir, "dist", "renderer.js"), `const PLUGIN_VERSION="${VERSION}";forceHookSoundshare();`);
    writeFileSync(join(dir, "dist", "renderer.js.map"), "maps are not installed");
    copyFileSync(hostExe, join(dir, "saf-audio-host.exe"));
    writeFileSync(join(dir, "saf-version.txt"), `version=${version}\nprotocol=1\nvencord=v1.15.11\n`);
    return dir;
}

function run(p, ...args) {
    const result = spawnSync(setupExe, [...args, "--appdata", p.appData, "--localappdata", p.local], { encoding: "utf8" });
    return { code: result.status, out: `${result.stdout}${result.stderr}` };
}

const dist = p => join(p.appData, "Vencord", "dist");
const host = p => join(p.appData, "Vencord", "streamAudioFilter", "native", "saf-audio-host.exe");

test("lists the newest Discord version", () => {
    const p = profile();
    const { code, out } = run(p, "--list");
    assert.equal(code, 0);
    assert.match(out, /Discord\t1\.0\.9262\t/);
    rmSync(p.base, { recursive: true, force: true });
});

test("fresh install, repair, uninstall: Discord ends up exactly as before", () => {
    const p = profile();
    const pl = payload();
    let r = run(p, "--install", "--payload-dir", pl);
    assert.equal(r.code, 0, r.out);
    assert.ok(existsSync(join(dist(p), "renderer.js")));
    assert.ok(!existsSync(join(dist(p), "renderer.js.map")), "source maps are not installed");
    assert.ok(existsSync(join(dist(p), "saf-version.txt")), "our build is marked");
    assert.ok(existsSync(host(p)));
    assert.deepEqual(readFileSync(join(p.resources, "_app.asar")), ORIGINAL_ASAR, "Discord's app is kept as _app.asar");
    const loader = readFileSync(join(p.resources, "app.asar"));
    assert.ok(loader.length < 2048 && loader.includes("patcher.js"), "a Vencord loader replaces app.asar");
    assert.match(run(p, "--status").out, new RegExp(`ours=1 version=${VERSION.replaceAll(".", "\\.")} host=1`));

    r = run(p, "--install", "--payload-dir", pl);
    assert.equal(r.code, 0, `repair works: ${r.out}`);
    assert.deepEqual(readFileSync(join(p.resources, "_app.asar")), ORIGINAL_ASAR);
    assert.ok(!existsSync(join(p.appData, "Vencord", "dist.saf-backup")), "our own build is not kept as a foreign backup");

    r = run(p, "--uninstall");
    assert.equal(r.code, 0, r.out);
    assert.deepEqual(readFileSync(join(p.resources, "app.asar")), ORIGINAL_ASAR, "app.asar restored byte for byte");
    assert.ok(!existsSync(join(p.resources, "_app.asar")));
    assert.ok(!existsSync(dist(p)), "the Vencord build the installer added is removed");
    assert.ok(!existsSync(host(p)));
    rmSync(p.base, { recursive: true, force: true });
    rmSync(pl, { recursive: true, force: true });
});

test("an existing Vencord is kept and restored", () => {
    const p = profile({ vencord: true, injected: true });
    const pl = payload();
    let r = run(p, "--install", "--payload-dir", pl);
    assert.equal(r.code, 0, r.out);
    assert.equal(readFileSync(join(p.appData, "Vencord", "dist.saf-backup", "patcher.js"), "utf8"), "// the user's own Vencord");
    assert.deepEqual(readFileSync(join(p.resources, "_app.asar")), ORIGINAL_ASAR, "the existing injection is reused");

    r = run(p, "--uninstall");
    assert.equal(r.code, 0, r.out);
    assert.equal(readFileSync(join(dist(p), "patcher.js"), "utf8"), "// the user's own Vencord", "the user's Vencord is back");
    assert.ok(existsSync(join(p.resources, "_app.asar")), "the user's Vencord injection stays");
    rmSync(p.base, { recursive: true, force: true });
    rmSync(pl, { recursive: true, force: true });
});

test("parts of different releases are refused, nothing is changed", () => {
    const p = profile({ vencord: true, injected: true });
    const pl = payload({ version: "9.9.9" });
    const r = run(p, "--install", "--payload-dir", pl);
    assert.notEqual(r.code, 0);
    assert.match(r.out, /9\.9\.9/);
    assert.equal(readFileSync(join(dist(p), "patcher.js"), "utf8"), "// the user's own Vencord");
    assert.ok(!existsSync(join(p.appData, "Vencord", "dist.saf-new")), "the staged files were removed");
    rmSync(p.base, { recursive: true, force: true });
    rmSync(pl, { recursive: true, force: true });
});

test("a failure late in the installation rolls everything back", () => {
    const p = profile({ vencord: true, injected: true });
    // A directory where the loader has to be written makes the last step fail.
    rmSync(join(p.resources, "app.asar"));
    mkdirSync(join(p.resources, "app.asar"));
    const pl = payload();
    const r = run(p, "--install", "--payload-dir", pl);
    assert.notEqual(r.code, 0, r.out);
    assert.match(r.out, /rolling back/);
    assert.equal(readFileSync(join(dist(p), "patcher.js"), "utf8"), "// the user's own Vencord", "the previous build is back");
    assert.ok(!existsSync(join(p.appData, "Vencord", "dist.saf-backup")));
    assert.ok(!existsSync(host(p)));
    rmSync(p.base, { recursive: true, force: true });
    rmSync(pl, { recursive: true, force: true });
});

test("refuses while Discord is running", async () => {
    const p = profile();
    const fake = join(p.discord, "app-1.0.9262", "Discord.exe");
    copyFileSync(hostExe, fake); // any program that stays alive until stdin closes
    const child = spawn(fake, ["--backend", "mock", "--output", "null"], { stdio: "pipe" });
    await new Promise(resolve => setTimeout(resolve, 500));
    const pl = payload();
    const r = run(p, "--install", "--payload-dir", pl);
    child.stdin.end();
    await new Promise(resolve => child.on("exit", resolve));
    assert.notEqual(r.code, 0);
    assert.match(r.out, /Discord is running/);
    assert.ok(!existsSync(dist(p)));
    rmSync(p.base, { recursive: true, force: true });
    rmSync(pl, { recursive: true, force: true });
});
