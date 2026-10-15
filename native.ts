/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Main process side. Everything that needs node/electron lives here:
 *  * loading (and explaining) the native addon,
 *  * the capability probe,
 *  * pulling the mixed PCM for the renderer,
 *  * application icons via Electron's own icon extraction (no extra code).
 * The renderer talks to this file through VencordNative.pluginHelpers
 * .StreamAudioFilter.<method>.
 */

import { RendererSettings } from "@main/settings";
import { app, dialog, shell } from "electron";
import { existsSync, readdirSync } from "fs";
import { createRequire } from "module";
import { join } from "path";

import type {
    AddonInfo,
    AppInfo,
    EngineStartOptions,
    EngineStats,
    MockApp,
    NativeEnvironment,
    RuleSetPayload,
    StartResult,
    SupportInfo
} from "./types";

const require_ = createRequire(__filename);

const ADDON_FILE = "streamaudiofilter.node";
const FOLDER_NAME = "streamAudioFilter";

interface AddonApi {
    start(options: EngineStartOptions): string;
    stop(): string;
    setRules(rules: RuleSetPayload): string;
    apps(): string;
    stats(): string;
    readPcm(frames: number): Uint8Array;
    refresh(): string;
    probe(backend: string): string;
    version(): string;
    setMockApps(apps: MockApp[]): string;
}

interface LoadedAddon {
    api: AddonApi | null;
    info: AddonInfo;
}

let cached: LoadedAddon | null = null;
let mockActive = false;
let mockTimer: NodeJS.Timeout | null = null;
const iconCache = new Map<string, string | null>();

/**
 * Vencord's data directory, where the built addon is expected. Keeping the
 * addon outside of node_modules means a Discord/Vencord update can not delete
 * it, and it keeps the plugin usable after an update without a rebuild.
 */
function dataDir(): string {
    const env = process.env.VENCORD_USER_DATA_DIR;
    if (env) return env;
    const discordData = process.env.DISCORD_USER_DATA_DIR;
    if (discordData) return join(discordData, "..", "Vencord");
    return join(app.getPath("userData"), "..", "Vencord");
}

export function addonDirectory(): string {
    return join(dataDir(), FOLDER_NAME, "native");
}

function candidatePaths(): string[] {
    const candidates: string[] = [];

    // 1. Explicitly configured path (advanced users).
    const configured = String((RendererSettings.store as any)?.streamAudioFilterPath ?? "");
    if (configured) candidates.push(configured);

    // 2. The recommended location: <Vencord data>/streamAudioFilter/native/.
    candidates.push(join(addonDirectory(), ADDON_FILE));
    candidates.push(join(addonDirectory(), "build", "Release", ADDON_FILE));

    // 3. A Vencord source build keeps the userplugins next to dist/.
    for (const relative of ["..", join("..", ".."), join("..", "..", "..")]) {
        candidates.push(join(__dirname, relative, "src", "userplugins", "StreamAudioFilter", "native", "build", "Release", ADDON_FILE));
        candidates.push(join(__dirname, relative, "src", "userplugins", "streamAudioFilter", "native", "build", "Release", ADDON_FILE));
    }

    // 4. Next to the addon directory (drag & drop installs).
    try {
        for (const entry of readdirSync(join(dataDir(), FOLDER_NAME))) {
            if (entry.endsWith(".node")) candidates.push(join(dataDir(), FOLDER_NAME, entry));
        }
    } catch { }

    return candidates;
}

function loadAddon(force = false): LoadedAddon {
    if (cached && !force) return cached;

    const info: AddonInfo = {
        loaded: false,
        path: "",
        version: "",
        error: "",
        triedPaths: [],
        napi: 0
    };

    const errors: string[] = [];
    for (const candidate of candidatePaths()) {
        info.triedPaths.push(candidate);
        if (!existsSync(candidate)) continue;

        try {
            const api = require_(candidate) as AddonApi;
            if (typeof api?.readPcm !== "function" || typeof api?.start !== "function") {
                errors.push(`${candidate}: the addon does not export the expected functions`);
                continue;
            }

            info.loaded = true;
            info.path = candidate;
            try {
                const version = JSON.parse(api.version()) as { version: string; napi: number; };
                info.version = version.version;
                info.napi = version.napi;
            } catch { }

            cached = { api, info };
            return cached;
        } catch (error) {
            errors.push(`${candidate}: ${(error as Error)?.message ?? error}`);
        }
    }

    info.error = errors.length
        ? errors.join("\n")
        : "the native addon was not found — build it with scripts/build-native.ps1 (see the documentation)";

    cached = { api: null, info };
    return cached;
}

// ------------------------------------------------------------------ environment

export async function getEnvironment(_: unknown): Promise<NativeEnvironment> {
    const { api, info } = loadAddon();

    let support: SupportInfo;
    if (api) {
        try {
            support = JSON.parse(api.probe("auto")) as SupportInfo;
        } catch (error) {
            support = unsupported(`the check failed: ${(error as Error).message}`);
        }
    } else {
        support = unsupported(info.error);
    }

    return {
        platform: process.platform,
        arch: process.arch,
        isWindows: process.platform === "win32",
        addon: info,
        support,
        dataDir: addonDirectory(),
        discordExecutable: process.execPath,
        mock: mockActive
    };
}

function unsupported(reason: string): SupportInfo {
    return {
        supported: false,
        os: process.platform === "win32" ? "Windows" : process.platform,
        build: 0,
        reason,
        detail: "",
        checks: [],
        platform: process.platform
    };
}

export async function probeSupport(_: unknown, backend = "auto"): Promise<SupportInfo> {
    const { api, info } = loadAddon();
    if (!api) return unsupported(info.error);
    try {
        return JSON.parse(api.probe(backend)) as SupportInfo;
    } catch (error) {
        return unsupported(`the check failed: ${(error as Error).message}`);
    }
}

export async function getAddonInfo(_: unknown): Promise<AddonInfo> {
    return loadAddon().info;
}

/** Reveals the folder the addon is expected in. */
export async function openAddonFolder(_: unknown): Promise<void> {
    const directory = addonDirectory();
    if (!existsSync(directory)) {
        try {
            require("fs").mkdirSync(directory, { recursive: true });
        } catch { }
    }
    await shell.openPath(directory);
}

// ----------------------------------------------------------------------- engine

export async function startEngine(_: unknown, options: EngineStartOptions): Promise<StartResult> {
    const { api, info } = loadAddon();

    if (!api) {
        // Without the addon the renderer can still be validated end to end with
        // the scripted test backend; the UI says so explicitly.
        if (options.backend === "mock") return { ok: false, error: info.error };
        return { ok: false, error: info.error };
    }

    try {
        const result = JSON.parse(api.start(options)) as StartResult;
        if (result.ok && options.backend === "mock") startMockScenario();
        return result;
    } catch (error) {
        return { ok: false, error: `could not start the audio engine: ${(error as Error).message}` };
    }
}

export async function stopEngine(_: unknown): Promise<StartResult> {
    stopMockScenario();
    const { api } = loadAddon();
    if (!api) return { ok: true };
    try {
        return JSON.parse(api.stop()) as StartResult;
    } catch (error) {
        return { ok: false, error: `could not stop the engine: ${(error as Error).message}` };
    }
}

export async function setRules(_: unknown, rules: RuleSetPayload): Promise<StartResult> {
    const { api } = loadAddon();
    if (!api) return { ok: false, error: "the native addon is not loaded" };
    try {
        return JSON.parse(api.setRules(rules)) as StartResult;
    } catch (error) {
        return { ok: false, error: `could not apply the rules: ${(error as Error).message}` };
    }
}

/**
 * Asks the engine to re-read the application list instead of using the cached
 * one; the "Refresh list" button in the panel uses this.
 */
export async function refreshApps(_: unknown): Promise<StartResult> {
    const { api } = loadAddon();
    if (!api) return { ok: false, error: "the native addon is not loaded" };
    try {
        return JSON.parse(api.refresh()) as StartResult;
    } catch (error) {
        return { ok: false, error: (error as Error).message };
    }
}

export async function listApps(_: unknown): Promise<AppInfo[]> {
    const { api } = loadAddon();
    if (!api) return [];
    try {
        return JSON.parse(api.apps()) as AppInfo[];
    } catch {
        return [];
    }
}

export async function getStats(_: unknown): Promise<EngineStats | null> {
    const { api } = loadAddon();
    if (!api) return null;
    try {
        return JSON.parse(api.stats()) as EngineStats;
    } catch {
        return null;
    }
}

/**
 * Pulls mixed PCM. The renderer keeps two requests in flight, so the audio
 * never waits for the event loop to come back around.
 */
export async function readPcm(_: unknown, frames: number): Promise<Uint8Array> {
    const { api } = loadAddon();
    if (!api) return new Uint8Array(0);
    try {
        return api.readPcm(frames);
    } catch {
        return new Uint8Array(0);
    }
}

export async function setMockApps(_: unknown, apps: MockApp[]): Promise<StartResult> {
    const { api } = loadAddon();
    if (!api) return { ok: false, error: "the native addon is not loaded" };
    try {
        api.setMockApps(apps);
        return { ok: true };
    } catch (error) {
        return { ok: false, error: (error as Error).message };
    }
}

// ------------------------------------------------------------------ app icons

/**
 * Application icon as a data URL, or null when there is none.
 *
 * Electron already knows how to read icons out of executables, so no PE parser
 * is needed. Results are cached because a preference store is not a bad place
 * for at most a few dozen small images.
 */
export async function getAppIcon(_: unknown, exePath: string): Promise<string | null> {
    if (!exePath) return null;
    if (iconCache.has(exePath)) return iconCache.get(exePath)!;

    let result: string | null = null;
    try {
        const image = await app.getFileIcon(exePath, { size: "small" });
        if (image && !image.isEmpty()) result = image.toDataURL();
    } catch {
        result = null;
    }

    if (iconCache.size > 64) iconCache.clear();
    iconCache.set(exePath, result);
    return result;
}

// ---------------------------------------------------------------- test scenario

/**
 * Test mode: a scripted application list plus a signal generator in the native
 * mock backend. Used to verify the whole pipeline (engine → renderer → track)
 * without WASAPI; never enabled implicitly.
 */
function startMockScenario(): void {
    mockActive = true;
    const scenario: MockApp[] = [
        { appKey: "mock:game", displayName: "Demo game", exePath: "C:\\Games\\Demo\\demo.exe", exeName: "demo.exe", pid: 4200, parentPid: 1, playing: true, capturable: true },
        { appKey: "mock:music", displayName: "Demo player", exePath: "C:\\Music\\player.exe", exeName: "player.exe", pid: 4300, parentPid: 1, playing: true, capturable: true },
        { appKey: "mock:chat", displayName: "Demo chat app", exePath: "C:\\Chat\\chat.exe", exeName: "chat.exe", pid: 4400, parentPid: 1, playing: false, capturable: true }
    ];

    const { api } = loadAddon();
    try {
        api?.setMockApps(scenario);
    } catch { }

    if (mockTimer) clearInterval(mockTimer);
    let ticks = 0;
    mockTimer = setInterval(() => {
        ticks++;
        const apps = [...scenario];
        if (ticks === 2) {
            // A new application starts playing: mode A must pick it up live.
            apps.push({ appKey: "mock:new", displayName: "Demo browser", exePath: "C:\\Browser\\browser.exe", exeName: "browser.exe", pid: 4500, parentPid: 1, playing: true, capturable: true });
        }
        if (ticks === 4) apps[1].playing = false;
        try {
            loadAddon().api?.setMockApps(apps);
        } catch { }
    }, 4000);
}

function stopMockScenario(): void {
    mockActive = false;
    if (mockTimer) {
        clearInterval(mockTimer);
        mockTimer = null;
    }
}

// ------------------------------------------------------------------ dialog glue

/** Explicitly asks before doing anything the user might not expect. */
export async function confirmAndRevealInstructions(_: unknown): Promise<boolean> {
    const { response } = await dialog.showMessageBox({
        type: "question",
        buttons: ["Show the path", "Cancel"],
        defaultId: 0,
        cancelId: 1,
        title: "StreamAudioFilter",
        message: "Building the native addon",
        detail:
            "The plugin is shipped as source only and downloads nothing from the internet.\n\n"
            + `Build it yourself with scripts/build-native.ps1, or copy ${ADDON_FILE} to:\n${addonDirectory()}`
    });

    if (response !== 0) return false;
    await openAddonFolder(null);
    return true;
}
