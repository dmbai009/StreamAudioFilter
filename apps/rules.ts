/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Rule resolution. Deliberately pure (no imports, no Vencord, no DOM) so the
 * semantics of both modes can be unit tested.
 *
 * The switch of an application always means the same thing: on = the viewers
 * may hear it. The mode only decides the value for applications the user never
 * touched; switching the mode never rewrites a saved choice.
 */

import type { AppInfo, AppListEntry, AppRule, FilterMode, RuleSetPayload, RulesState } from "../types";

/**
 * 2: Discord itself is protected by the audio host (by process tree), so the
 *    old seeded rule keyed by Discord's versioned executable path is removed.
 */
export const RULES_VERSION = 2;

export function emptyState(mode: FilterMode = "allExcept"): RulesState {
    return { mode, rules: {}, version: RULES_VERSION };
}

/**
 * Applications that play the audio of *other* applications again (virtual
 * cable repeaters, mixers). Sending one of them would send everything it
 * re-plays, excluded applications included, so they are blocked unless the
 * user explicitly allows them — in both modes.
 */
const AUDIO_ROUTERS = [
    "audiorepeater.exe", "audiorepeater_ks.exe",
    "voicemeeter.exe", "voicemeeter_x64.exe", "voicemeeterpro.exe", "voicemeeterpro_x64.exe", "voicemeeter8.exe", "voicemeeter8x64.exe",
    "equalizerapo.exe", "soundswitch.exe", "nvidia broadcast.exe"
];

export function isAudioRouter(appKey: string): boolean {
    if (!appKey.startsWith("exe:")) return false;
    const name = appKey.slice(appKey.lastIndexOf("\\") + 1).toLowerCase();
    return AUDIO_ROUTERS.includes(name);
}

export function resolveTransmit(state: RulesState, appKey: string): boolean {
    const rule = state.rules[appKey];
    if (rule) return rule.transmit;
    if (isAudioRouter(appKey)) return false;
    return state.mode === "allExcept";
}

export function hasRule(state: RulesState, appKey: string): boolean {
    return Object.prototype.hasOwnProperty.call(state.rules, appKey);
}

/** Records an explicit decision and remembers how to display the entry later. */
export function setTransmit(state: RulesState, app: Pick<AppInfo, "appKey"> & Partial<Pick<AppInfo, "displayName" | "exePath">>, transmit: boolean, now = Date.now()): RulesState {
    const existing = state.rules[app.appKey];
    const rule: AppRule = {
        transmit,
        name: app.displayName || existing?.name,
        exePath: app.exePath || existing?.exePath,
        lastSeen: now
    };
    return { ...state, rules: { ...state.rules, [app.appKey]: rule } };
}

export function setMode(state: RulesState, mode: FilterMode): RulesState {
    return { ...state, mode };
}

/** Removes every explicit choice. */
export function resetRules(state: RulesState): RulesState {
    return { ...state, rules: {} };
}

/** Forgets a single application. */
export function forgetApp(state: RulesState, appKey: string): RulesState {
    const rules = { ...state.rules };
    delete rules[appKey];
    return { ...state, rules };
}

/**
 * @param liveKeys app keys currently known: audio routers among them get an
 *                 explicit "blocked" so the host applies the same default.
 */
export function toPayload(state: RulesState, active: boolean, liveKeys: string[] = []): RuleSetPayload {
    const overrides: Record<string, boolean> = {};
    for (const key of liveKeys) if (isAudioRouter(key)) overrides[key] = false;
    for (const [key, rule] of Object.entries(state.rules)) overrides[key] = rule.transmit;
    return { mode: state.mode === "onlySelected" ? 1 : 0, overrides, active };
}

/** True for the rule version 1 seeded for Discord's (versioned) executable. */
function isLegacyDiscordRule(key: string, rule: AppRule): boolean {
    return rule.name === "Discord (voice call)" && /\\discord(ptb|canary|development)?\\app-[^\\]+\\discord(ptb|canary|development)?\.exe$/i.test(key);
}

/** Loads a possibly broken/old persisted state. */
export function normaliseState(raw: unknown, fallbackMode: FilterMode = "allExcept"): RulesState {
    const state = emptyState(fallbackMode);
    if (!raw || typeof raw !== "object") return state;

    const candidate = raw as Partial<RulesState>;
    if (candidate.mode === "allExcept" || candidate.mode === "onlySelected") state.mode = candidate.mode;
    const storedVersion = typeof candidate.version === "number" ? candidate.version : 0;

    if (candidate.rules && typeof candidate.rules === "object") {
        for (const [key, value] of Object.entries(candidate.rules)) {
            if (!value || typeof value !== "object") continue;
            const rule = value as Partial<AppRule>;
            if (typeof rule.transmit !== "boolean") continue;
            const normalised: AppRule = {
                transmit: rule.transmit,
                name: typeof rule.name === "string" ? rule.name : undefined,
                exePath: typeof rule.exePath === "string" ? rule.exePath : undefined,
                lastSeen: typeof rule.lastSeen === "number" ? rule.lastSeen : undefined
            };
            if (storedVersion < 2 && isLegacyDiscordRule(key, normalised)) continue;
            state.rules[key] = normalised;
        }
    }
    state.version = RULES_VERSION;
    return state;
}

/**
 * Merges the live application list with the saved rules.
 *
 *   current — a session is active right now
 *   idle    — running, but not playing anything at the moment
 *   closed  — saved rule of an application that is not running
 */
export function buildAppList(live: AppInfo[], state: RulesState): AppListEntry[] {
    const entries: AppListEntry[] = [];
    const seen = new Set<string>();

    for (const app of live) {
        seen.add(app.appKey);
        entries.push({
            appKey: app.appKey,
            title: app.displayName || app.exeName || app.appKey,
            exeName: app.exeName,
            section: app.playing ? "current" : "idle",
            allowed: app.capturable && resolveTransmit(state, app.appKey),
            byDefault: app.capturable && !hasRule(state, app.appKey),
            router: isAudioRouter(app.appKey),
            capturable: app.capturable,
            ephemeral: app.ephemeral,
            state: app.state,
            captured: app.captured,
            peak: app.peak,
            instances: app.instances,
            devices: app.devices ?? [],
            silentCapture: !!app.silentCapture,
            format: app.format,
            latencyMs: app.latencyMs,
            error: app.error,
            notCapturableReason: app.notCapturableReason,
            blockedReason: app.blockedReason ?? "",
            coveredBy: app.coveredBy ?? "",
            exePath: app.exePath
        });
    }

    for (const [key, rule] of Object.entries(state.rules)) {
        if (seen.has(key)) continue;
        entries.push({
            appKey: key,
            title: rule.name || key,
            exeName: rule.exePath ? rule.exePath.split(/[\\/]/).pop() ?? "" : "",
            section: "closed",
            allowed: rule.transmit,
            byDefault: false,
            router: isAudioRouter(key),
            capturable: true,
            ephemeral: false,
            state: "closed",
            captured: false,
            peak: 0,
            instances: 0,
            devices: [],
            silentCapture: false,
            format: "",
            latencyMs: -1,
            error: "",
            notCapturableReason: "",
            blockedReason: "",
            coveredBy: "",
            exePath: rule.exePath || ""
        });
    }

    const weight = (entry: AppListEntry) => (entry.section === "current" ? 0 : entry.section === "idle" ? 1 : 2);
    entries.sort((a, b) => weight(a) - weight(b) || a.title.localeCompare(b.title));
    return entries;
}
