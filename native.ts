/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Main process side: starts and talks to the audio host (saf-audio-host.exe).
 *
 * The host is a separate process on purpose. Discord takes the share audio
 * from a *process* (see stream/soundshare.ts), so the filtered mix has to be
 * played by one; a crash in audio code can not take Discord down with it; and
 * no native module has to match the Electron version of Discord.
 *
 * The host exits on its own when its stdin closes, so it never outlives
 * Discord. The renderer reaches this file through
 * VencordNative.pluginHelpers.StreamAudioFilter.<function>.
 */

import { RendererSettings } from "@main/settings";
import { type ChildProcessWithoutNullStreams, spawn } from "child_process";
import { app, shell } from "electron";
import { existsSync, mkdirSync } from "fs";
import { join } from "path";

import type { AppInfo, HostInfo, HostStats, MockApp, NativeEnvironment, RuleSetPayload, StartResult, SupportInfo } from "./types";
import { PLUGIN_VERSION, PROTOCOL_VERSION } from "./version";

const HOST_FILE = "saf-audio-host.exe";
const FOLDER_NAME = "streamAudioFilter";
const REQUEST_TIMEOUT_MS = 8000;
const PROBE_TIMEOUT_MS = 15000;

type Answer = Record<string, any> & { id: number; ok?: boolean; error?: string; };

interface Pending {
    resolve(answer: Answer): void;
    timer: NodeJS.Timeout;
}

class HostClient {
    private child: ChildProcessWithoutNullStreams | null = null;
    private buffer = "";
    private nextId = 1;
    private pending = new Map<number, Pending>();
    private starting: Promise<boolean> | null = null;
    private backend: "auto" | "mock" = "auto";
    info: HostInfo = { found: false, running: false, path: "", version: "", protocol: 0, pid: 0, error: "", triedPaths: [] };
    private iconCache = new Map<string, string | null>();
    private stderrTail = "";

    /** Starts the host if it is not running. Never throws. */
    ensure(backend: "auto" | "mock"): Promise<boolean> {
        if (this.child && this.backend === backend && this.info.running) return Promise.resolve(true);
        if (this.child && this.backend !== backend) this.stop();
        if (this.starting) return this.starting;
        this.starting = this.start(backend).finally(() => { this.starting = null; });
        return this.starting;
    }

    private async start(backend: "auto" | "mock"): Promise<boolean> {
        const tried: string[] = [];
        const path = candidatePaths().find(candidate => {
            tried.push(candidate);
            return existsSync(candidate);
        });
        this.info = { found: !!path, running: false, path: path ?? "", version: "", protocol: 0, pid: 0, error: "", triedPaths: tried };
        if (!path) {
            this.info.error = `${HOST_FILE} was not found. Install StreamAudioFilter with its installer, or put the file into ${hostDirectory()}.`;
            return false;
        }

        const args = ["--protect", String(process.pid)];
        if (backend === "mock") args.push("--backend", "mock");

        let child: ChildProcessWithoutNullStreams;
        try {
            child = spawn(path, args, { stdio: "pipe", windowsHide: true });
        } catch (error) {
            this.info.error = `the audio host could not be started: ${(error as Error).message}`;
            return false;
        }

        this.child = child;
        this.backend = backend;
        this.buffer = "";
        this.stderrTail = "";
        child.stdout.setEncoding("utf8");
        child.stdout.on("data", (chunk: string) => this.onData(chunk));
        child.stderr.setEncoding("utf8");
        child.stderr.on("data", (chunk: string) => { this.stderrTail = (this.stderrTail + chunk).slice(-2000); });
        child.on("error", error => this.onExit(`the audio host failed: ${error.message}`));
        child.on("exit", (code, signal) => this.onExit(`the audio host exited (${signal ?? `code ${code}`})${this.stderrTail ? `: ${this.stderrTail.trim()}` : ""}`));

        const hello = await this.request(["hello"]);
        if (!hello.ok) {
            this.info.error = hello.error ?? "the audio host did not answer";
            this.stop();
            return false;
        }

        this.info.version = String(hello.version ?? "");
        this.info.protocol = Number(hello.protocol ?? 0);
        this.info.pid = Number(hello.pid ?? 0);
        if (this.info.protocol !== PROTOCOL_VERSION) {
            // A host that speaks another protocol would be misread: refuse it
            // with an explanation instead of guessing.
            this.info.error = `the audio host ${this.info.version} speaks protocol ${this.info.protocol}, this plugin (${PLUGIN_VERSION}) needs protocol ${PROTOCOL_VERSION}. Reinstall StreamAudioFilter so that both parts come from the same release.`;
            this.stop();
            return false;
        }
        this.info.running = true;
        return true;
    }

    private onData(chunk: string) {
        this.buffer += chunk;
        let newline: number;
        while ((newline = this.buffer.indexOf("\n")) >= 0) {
            const line = this.buffer.slice(0, newline).trim();
            this.buffer = this.buffer.slice(newline + 1);
            if (!line) continue;
            let answer: Answer;
            try {
                answer = JSON.parse(line);
            } catch {
                continue;
            }
            const pending = this.pending.get(answer.id);
            if (!pending) continue;
            this.pending.delete(answer.id);
            clearTimeout(pending.timer);
            pending.resolve(answer);
        }
    }

    private onExit(reason: string) {
        if (!this.child) return;
        this.child = null;
        this.info = { ...this.info, running: false, pid: 0, error: reason };
        for (const [id, pending] of this.pending) {
            clearTimeout(pending.timer);
            pending.resolve({ id, ok: false, error: reason });
        }
        this.pending.clear();
    }

    /** Sends one command; resolves with { ok: false } instead of rejecting. */
    request(args: (string | number)[], timeoutMs = REQUEST_TIMEOUT_MS): Promise<Answer> {
        const { child } = this;
        if (!child) return Promise.resolve({ id: 0, ok: false, error: this.info.error || "the audio host is not running" });

        const id = this.nextId++;
        // TAB and newline separate fields and commands: they must never be part
        // of a value (Windows paths can not contain them anyway).
        const line = [id, ...args].map(value => String(value).replace(/[\t\r\n]/g, " ")).join("\t") + "\n";
        return new Promise(resolve => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                resolve({ id, ok: false, error: `the audio host did not answer "${args[0]}" within ${timeoutMs} ms` });
            }, timeoutMs);
            this.pending.set(id, { resolve, timer });
            try {
                child.stdin.write(line);
            } catch (error) {
                clearTimeout(timer);
                this.pending.delete(id);
                resolve({ id, ok: false, error: `the audio host can not be reached: ${(error as Error).message}` });
            }
        });
    }

    stop() {
        const { child } = this;
        if (!child) return;
        // Closing stdin is the regular way to end the host: it stops its
        // output and captures and exits by itself.
        try {
            child.stdin.end();
        } catch { }
        const killTimer = setTimeout(() => {
            try {
                child.kill();
            } catch { }
        }, 3000);
        child.once("exit", () => clearTimeout(killTimer));
        this.onExit("stopped");
        this.info.error = "";
    }

    async icon(exePath: string): Promise<string | null> {
        if (!exePath) return null;
        if (this.iconCache.has(exePath)) return this.iconCache.get(exePath)!;
        let result: string | null = null;
        try {
            const image = await app.getFileIcon(exePath, { size: "normal" });
            if (image && !image.isEmpty()) result = image.toDataURL();
        } catch {
            result = null;
        }
        if (this.iconCache.size > 128) this.iconCache.clear();
        this.iconCache.set(exePath, result);
        return result;
    }
}

const host = new HostClient();
app.on("will-quit", () => host.stop());

/**
 * Vencord's data directory. The installer puts the host here, outside of
 * Discord's and Vencord's own folders, so that their updates can not delete it.
 */
function dataDir(): string {
    const env = process.env.VENCORD_USER_DATA_DIR;
    if (env) return env;
    const discordData = process.env.DISCORD_USER_DATA_DIR;
    if (discordData) return join(discordData, "..", "Vencord");
    return join(app.getPath("userData"), "..", "Vencord");
}

function hostDirectory(): string {
    return join(dataDir(), FOLDER_NAME, "native");
}

function candidatePaths(): string[] {
    const candidates: string[] = [];
    const configured = String((RendererSettings.store as any)?.plugins?.StreamAudioFilter?.hostPath ?? "").trim();
    if (configured) candidates.push(configured);
    // One folder per version: a new path carries none of the per executable
    // state Windows remembers (volume, mute) for an older one, and an update
    // never has to replace an executable that is still running.
    candidates.push(join(hostDirectory(), PLUGIN_VERSION, HOST_FILE));
    candidates.push(join(hostDirectory(), HOST_FILE));
    // A Vencord source build keeps userplugins next to dist/.
    for (const relative of ["..", join("..", ".."), join("..", "..", "..")]) {
        candidates.push(join(__dirname, relative, "src", "userplugins", "StreamAudioFilter", "native", "build", "host", HOST_FILE));
    }
    return candidates;
}

function unsupported(reason: string): SupportInfo {
    return { supported: false, os: process.platform === "win32" ? "Windows" : process.platform, build: 0, reason, detail: "", checks: [], platform: process.platform };
}

// ------------------------------------------------------------------- exports

export async function getEnvironment(_: unknown, backend: "auto" | "mock" = "auto"): Promise<NativeEnvironment> {
    await host.ensure(backend);
    let support: SupportInfo = unsupported(host.info.error || "the audio host is not running");
    if (host.info.running) {
        const answer = await host.request(["probe"], PROBE_TIMEOUT_MS);
        support = answer.ok ? answer.support as SupportInfo : unsupported(answer.error ?? "the check failed");
    }
    return {
        platform: process.platform,
        arch: process.arch,
        isWindows: process.platform === "win32",
        host: { ...host.info },
        support,
        hostDir: hostDirectory(),
        pluginVersion: PLUGIN_VERSION,
        pluginProtocol: PROTOCOL_VERSION,
        mock: backend === "mock"
    };
}

/** The pid Discord should take the share audio from (0 when unavailable). */
export async function ensureHost(_: unknown, backend: "auto" | "mock" = "auto"): Promise<HostInfo> {
    await host.ensure(backend);
    return { ...host.info };
}

export async function startEngine(_: unknown, maxStreams: number, backend: "auto" | "mock" = "auto"): Promise<StartResult> {
    if (!await host.ensure(backend)) return { ok: false, error: host.info.error };
    const answer = await host.request(["start", Math.max(1, Math.min(64, Math.round(maxStreams || 12)))]);
    return { ok: !!answer.ok, error: answer.error, warning: answer.warning };
}

export async function stopEngine(_: unknown): Promise<StartResult> {
    if (!host.info.running) return { ok: true };
    const answer = await host.request(["stop"]);
    return { ok: !!answer.ok, error: answer.error };
}

export async function setRules(_: unknown, rules: RuleSetPayload): Promise<StartResult> {
    const args: (string | number)[] = ["rules", rules.mode, rules.active ? 1 : 0];
    for (const [key, transmit] of Object.entries(rules.overrides)) args.push(key, transmit ? 1 : 0);
    const answer = await host.request(args);
    return { ok: !!answer.ok, error: answer.error };
}

export async function setOutput(_: unknown, on: boolean): Promise<StartResult> {
    const answer = await host.request(["output", on ? 1 : 0]);
    return { ok: !!answer.ok, error: answer.error, pid: Number(answer.pid ?? 0) };
}

export async function refreshApps(_: unknown): Promise<StartResult> {
    const answer = await host.request(["refresh"]);
    return { ok: !!answer.ok, error: answer.error };
}

export async function listApps(_: unknown): Promise<AppInfo[]> {
    if (!host.info.running) return [];
    const answer = await host.request(["apps"]);
    return answer.ok && Array.isArray(answer.apps) ? answer.apps as AppInfo[] : [];
}

export async function getStats(_: unknown): Promise<HostStats | null> {
    if (!host.info.running) return null;
    const answer = await host.request(["stats"]);
    if (!answer.ok) return null;
    return { engine: answer.engine ?? null, output: answer.output ?? null };
}

export async function probeSupport(_: unknown): Promise<SupportInfo> {
    if (!host.info.running) return unsupported(host.info.error || "the audio host is not running");
    const answer = await host.request(["probe"], PROBE_TIMEOUT_MS);
    return answer.ok ? answer.support as SupportInfo : unsupported(answer.error ?? "the check failed");
}

export async function setMockApps(_: unknown, apps: MockApp[]): Promise<StartResult> {
    const encoded = apps.map(a => [a.appKey, a.displayName, a.exePath, a.exeName, a.pid, a.parentPid, a.playing ? 1 : 0, a.capturable ? 1 : 0].join("\x1f"));
    const answer = await host.request(["mockApps", ...encoded]);
    return { ok: !!answer.ok, error: answer.error };
}

export async function restartHost(_: unknown, backend: "auto" | "mock" = "auto"): Promise<HostInfo> {
    host.stop();
    await host.ensure(backend);
    return { ...host.info };
}

export async function shutdownHost(_: unknown): Promise<void> {
    host.stop();
}

export async function getAppIcon(_: unknown, exePath: string): Promise<string | null> {
    return host.icon(exePath);
}

export async function openHostFolder(_: unknown): Promise<void> {
    const directory = hostDirectory();
    try {
        mkdirSync(directory, { recursive: true });
    } catch { }
    await shell.openPath(directory);
}
