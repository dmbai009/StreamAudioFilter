/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Typed access to the main process side. Everything the renderer needs from the
 * native engine goes through this one module, which also keeps the plugin usable
 * (with a clear message) when Vencord runs in a browser, where no native side
 * exists at all.
 */

import { PluginNative } from "@utils/types";

import type { AppInfo, EngineStartOptions, EngineStats, MockApp, NativeEnvironment, RuleSetPayload, StartResult, SupportInfo } from "./types";

type NativeApi = PluginNative<typeof import("./native")>;

/**
 * VencordNative is a global that only exists in the desktop client; in the
 * browser build it is a stub, so both cases are handled here and the UI says
 * "unavailable" instead of throwing.
 */
const helpers = (typeof VencordNative === "undefined" ? null : VencordNative.pluginHelpers) as Record<string, NativeApi> | null;
const native = helpers?.StreamAudioFilter ?? null;

export const IS_NATIVE_AVAILABLE = native != null;

function unavailable<T>(fallback: T, what: string): Promise<T> {

    console.warn(`[StreamAudioFilter] ${what} is unavailable: the native part only works in the desktop Discord client`);
    return Promise.resolve(fallback);
}

export const Native = {
    getEnvironment(): Promise<NativeEnvironment> {
        if (!native) {
            return Promise.resolve({
                platform: "browser",
                arch: "",
                isWindows: false,
                addon: { loaded: false, path: "", version: "", error: "the native part is unavailable in the browser", triedPaths: [], napi: 0 },
                support: { supported: false, os: "browser", build: 0, reason: "per application audio filtering needs the desktop Discord client", detail: "", checks: [], platform: "browser" },
                dataDir: "",
                discordExecutable: "",
                mock: false
            });
        }
        return native.getEnvironment();
    },

    refreshApps(): Promise<StartResult> {
        if (!native) return unavailable<StartResult>({ ok: false }, "refreshing the list");
        return native.refreshApps();
    },

    async listApps(): Promise<AppInfo[]> {
        if (!native) return unavailable<AppInfo[]>([], "the application list");
        const apps = await native.listApps();
        // The engine reports the peak level as an integer 0..1000 to keep the
        // JSON small; the UI works with 0..1.
        return apps.map(app => ({ ...app, peak: (app.peak ?? 0) / 1000 }));
    },

    getStats(): Promise<EngineStats | null> {
        if (!native) return unavailable<EngineStats | null>(null, "the engine state");
        return native.getStats();
    },

    getAppIcon(exePath: string): Promise<string | null> {
        if (!native) return Promise.resolve(null);
        return native.getAppIcon(exePath);
    },

    readPcm(frames: number): Promise<Uint8Array> {
        if (!native) return Promise.resolve(new Uint8Array(0));
        return native.readPcm(frames);
    },

    probeSupport(backend = "auto"): Promise<SupportInfo> {
        if (!native) {
            return Promise.resolve({
                supported: false,
                os: "browser",
                build: 0,
                reason: "the native part is unavailable",
                detail: "",
                checks: [],
                platform: "browser"
            });
        }
        return native.probeSupport(backend);
    },

    setMockApps(apps: MockApp[]): Promise<StartResult> {
        if (!native) return unavailable<StartResult>({ ok: false }, "the test mode");
        return native.setMockApps(apps);
    },

    openAddonFolder(): Promise<void> {
        if (!native) return unavailable(undefined, "opening the folder");
        return native.openAddonFolder();
    },

    confirmBuildInstructions(): Promise<boolean> {
        if (!native) return unavailable(false, "the build instructions");
        return native.confirmAndRevealInstructions();
    }
};

export const Engine = {
    start(options: EngineStartOptions): Promise<StartResult> {
        if (!native) return Promise.resolve({ ok: false, error: "the native part is unavailable in the browser" });
        return native.startEngine(options);
    },

    stop(): Promise<StartResult> {
        if (!native) return Promise.resolve({ ok: true });
        return native.stopEngine();
    },

    setRules(rules: RuleSetPayload): Promise<StartResult> {
        if (!native) return Promise.resolve({ ok: false, error: "the native part is unavailable" });
        return native.setRules(rules);
    }
};
