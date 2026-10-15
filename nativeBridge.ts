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
 * "недоступно" instead of throwing.
 */
const helpers = (typeof VencordNative === "undefined" ? null : VencordNative.pluginHelpers) as Record<string, NativeApi> | null;
const native = helpers?.StreamAudioFilter ?? null;

export const IS_NATIVE_AVAILABLE = native != null;

function unavailable<T>(fallback: T, what: string): Promise<T> {

    console.warn(`[StreamAudioFilter] ${what} недоступно: нативная часть работает только в настольном Discord`);
    return Promise.resolve(fallback);
}

export const Native = {
    getEnvironment(): Promise<NativeEnvironment> {
        if (!native) {
            return Promise.resolve({
                platform: "browser",
                arch: "",
                isWindows: false,
                addon: { loaded: false, path: "", version: "", error: "нативная часть недоступна в браузере", triedPaths: [], napi: 0 },
                support: { supported: false, os: "браузер", build: 0, reason: "фильтрация звука по приложениям требует настольного Discord", detail: "", checks: [], platform: "browser" },
                dataDir: "",
                discordExecutable: "",
                mock: false
            });
        }
        return native.getEnvironment();
    },

    async listApps(): Promise<AppInfo[]> {
        if (!native) return unavailable<AppInfo[]>([], "список приложений");
        const apps = await native.listApps();
        // The engine reports the peak level as an integer 0..1000 to keep the
        // JSON small; the UI works with 0..1.
        return apps.map(app => ({ ...app, peak: (app.peak ?? 0) / 1000 }));
    },

    getStats(): Promise<EngineStats | null> {
        if (!native) return unavailable<EngineStats | null>(null, "состояние движка");
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
                os: "браузер",
                build: 0,
                reason: "нативная часть недоступна",
                detail: "",
                checks: [],
                platform: "browser"
            });
        }
        return native.probeSupport(backend);
    },

    setMockApps(apps: MockApp[]): Promise<StartResult> {
        if (!native) return unavailable<StartResult>({ ok: false }, "тестовый режим");
        return native.setMockApps(apps);
    },

    openAddonFolder(): Promise<void> {
        if (!native) return unavailable(undefined, "открытие папки");
        return native.openAddonFolder();
    },

    confirmBuildInstructions(): Promise<boolean> {
        if (!native) return unavailable(false, "инструкция по сборке");
        return native.confirmAndRevealInstructions();
    }
};

export const Engine = {
    start(options: EngineStartOptions): Promise<StartResult> {
        if (!native) return Promise.resolve({ ok: false, error: "нативная часть недоступна в браузере" });
        return native.startEngine(options);
    },

    stop(): Promise<StartResult> {
        if (!native) return Promise.resolve({ ok: true });
        return native.stopEngine();
    },

    setRules(rules: RuleSetPayload): Promise<StartResult> {
        if (!native) return Promise.resolve({ ok: false, error: "нативная часть недоступна" });
        return native.setRules(rules);
    }
};
