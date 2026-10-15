/*
 * StreamAudioFilter — per application audio filtering for screen shares
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Typed access to the main process side. Everything the renderer needs from the
 * native engine goes through this one module, which also keeps the plugin usable
 * (with a clear message) when Vencord runs in a browser, where no native side
 * exists at all.
 */

import { PluginNative } from "@utils/types";
import { VencordNative } from "@webpack/common";

import type { AppInfo, EngineStartOptions, EngineStats, MockApp, NativeEnvironment, RuleSetPayload, StartResult, SupportInfo } from "./types";

type NativeApi = PluginNative<typeof import("./native")>;

const helpers = (VencordNative as any)?.pluginHelpers ?? {};
const native = (helpers.StreamAudioFilter ?? null) as NativeApi | null;

export const IS_NATIVE_AVAILABLE = native != null;

function unavailable<T>(fallback: T, what: string): Promise<T> {
    // eslint-disable-next-line no-console
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

    listApps(): Promise<AppInfo[]> {
        if (!native) return unavailable<AppInfo[]>([], "список приложений");
        return native.listApps();
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
