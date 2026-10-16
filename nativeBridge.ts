/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Typed access to the main process side (native.ts). In the browser build of
 * Vencord there is no native side: every call then answers "unavailable"
 * instead of throwing.
 */

import { PluginNative } from "@utils/types";

import type { AppInfo, HostInfo, HostStats, MockApp, NativeEnvironment, RuleSetPayload, StartResult, SupportInfo } from "./types";
import { PLUGIN_VERSION, PROTOCOL_VERSION } from "./version";

type NativeApi = PluginNative<typeof import("./native")>;

const helpers = (typeof VencordNative === "undefined" ? null : VencordNative.pluginHelpers) as Record<string, NativeApi> | null;
const native = helpers?.StreamAudioFilter ?? null;

export const IS_NATIVE_AVAILABLE = native != null;

const NO_HOST: HostInfo = { found: false, running: false, path: "", version: "", protocol: 0, pid: 0, error: "per application audio needs the Discord desktop app", triedPaths: [] };
const NOT_AVAILABLE: StartResult = { ok: false, error: NO_HOST.error };

export type Backend = "auto" | "mock";

export const Native = {
    getEnvironment(backend: Backend): Promise<NativeEnvironment> {
        if (!native) {
            return Promise.resolve({
                platform: "browser",
                arch: "",
                isWindows: false,
                host: NO_HOST,
                support: { supported: false, os: "browser", build: 0, reason: NO_HOST.error, detail: "", checks: [], platform: "browser" },
                hostDir: "",
                pluginVersion: PLUGIN_VERSION,
                pluginProtocol: PROTOCOL_VERSION,
                mock: false
            });
        }
        return native.getEnvironment(backend);
    },
    ensureHost: (backend: Backend): Promise<HostInfo> => native ? native.ensureHost(backend) : Promise.resolve(NO_HOST),
    restartHost: (backend: Backend): Promise<HostInfo> => native ? native.restartHost(backend) : Promise.resolve(NO_HOST),
    shutdownHost: (): Promise<void> => native ? native.shutdownHost() : Promise.resolve(),
    startEngine: (maxStreams: number, backend: Backend): Promise<StartResult> => native ? native.startEngine(maxStreams, backend) : Promise.resolve(NOT_AVAILABLE),
    stopEngine: (): Promise<StartResult> => native ? native.stopEngine() : Promise.resolve({ ok: true }),
    setRules: (rules: RuleSetPayload): Promise<StartResult> => native ? native.setRules(rules) : Promise.resolve(NOT_AVAILABLE),
    setOutput: (on: boolean): Promise<StartResult> => native ? native.setOutput(on) : Promise.resolve(NOT_AVAILABLE),
    setMasterMuted: (muted: boolean): Promise<StartResult> => native ? native.setMasterMuted(muted) : Promise.resolve(NOT_AVAILABLE),
    loadPreferences: () => native ? native.loadPreferences() : Promise.resolve({} as { rules?: unknown; position?: { x: number; y: number; }; reset?: boolean; }),
    savePreferences: (key: "rules" | "position", value: unknown): Promise<void> => native ? native.savePreferences(key, value) : Promise.reject(new Error(NO_HOST.error)),
    refreshApps: (): Promise<StartResult> => native ? native.refreshApps() : Promise.resolve(NOT_AVAILABLE),
    async listApps(): Promise<AppInfo[]> {
        if (!native) return [];
        const apps = await native.listApps();
        // The host reports levels as integers 0..1000 to keep its JSON small.
        return apps.map(app => ({ ...app, peak: (app.peak ?? 0) / 1000 }));
    },
    async getStats(): Promise<HostStats | null> {
        if (!native) return null;
        const stats = await native.getStats();
        if (stats?.output) stats.output = { ...stats.output, peak: (stats.output.peak ?? 0) / 1000 };
        return stats;
    },
    probeSupport: (): Promise<SupportInfo | null> => native ? native.probeSupport() : Promise.resolve(null),
    setMockApps: (apps: MockApp[]): Promise<StartResult> => native ? native.setMockApps(apps) : Promise.resolve(NOT_AVAILABLE),
    getAppIcon: (exePath: string): Promise<string | null> => native ? native.getAppIcon(exePath) : Promise.resolve(null),
    openHostFolder: (): Promise<void> => native ? native.openHostFolder() : Promise.resolve()
};
