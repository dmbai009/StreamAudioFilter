/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// StreamAudioFilter — host replacement must ignore events from the old child.
// SPDX-License-Identifier: GPL-3.0-or-later
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { readFileSync } from "node:fs";
import { stripTypeScriptTypes } from "node:module";
import { test } from "node:test";
import vm from "node:vm";

test("late exit, error and output from a stopped host cannot invalidate its replacement", async () => {
    const children = [];
    const source = readFileSync(new URL("../native.ts", import.meta.url), "utf8");
    const clientSource = source.slice(source.indexOf("class HostClient {"), source.indexOf("\nconst host ="));
    const context = vm.createContext({
        existsSync: () => true,
        candidatePaths: () => ["fake-host.exe"],
        process: { pid: 123 },
        PROTOCOL_VERSION: 2,
        PLUGIN_VERSION: "test",
        REQUEST_TIMEOUT_MS: 100,
        setTimeout, clearTimeout,
        spawn: () => {
            const child = new EventEmitter();
            child.stdout = new EventEmitter();
            child.stderr = new EventEmitter();
            child.stdout.setEncoding = child.stderr.setEncoding = () => {};
            child.stdin = {
                end() {},
                write(line) {
                    const id = Number(line.split("\t")[0]);
                    queueMicrotask(() => child.stdout.emit("data", JSON.stringify({ id, ok: true, protocol: 2, version: "test", pid: children.length }) + "\n"));
                }
            };
            children.push(child);
            return child;
        }
    });
    vm.runInContext(stripTypeScriptTypes(clientSource) + "\nglobalThis.client = new HostClient();", context);
    const { client } = context;
    assert.equal(await client.ensure("mock"), true);
    const first = children[0];
    client.stop();
    assert.equal(await client.ensure("mock"), true);
    first.stdout.emit("data", "broken old output\n");
    first.stderr.emit("data", "old error");
    first.emit("error", new Error("late old error"));
    first.emit("exit", 0, null);
    assert.equal(client.info.running, true);
    assert.equal(client.info.pid, 2);
    assert.equal((await client.request(["stats"])).ok, true);
    client.stop();
    children[1].emit("exit", 0, null);
});
