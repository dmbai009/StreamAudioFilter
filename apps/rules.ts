/*
 * StreamAudioFilter — per application audio filtering for screen shares
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Rule resolution. Deliberately pure (no imports, no Vencord, no DOM) so the
 * semantics of both filtering modes can be unit tested.
 */

import type { AppInfo, AppListEntry, AppRule, FilterMode, RuleSetPayload, RulesState } from "../types";

export const RULES_VERSION = 1;

export function emptyState(mode: FilterMode = "allExcept"): RulesState {
    return { mode, rules: {}, version: RULES_VERSION };
}

/**
 * Resolves whether an application is transmitted.
 *
 * The single source of truth is the user's explicit choice per application; the
 * mode only decides what happens to applications that were never touched. That
 * is why switching modes never clears anything.
 */
export function resolveTransmit(state: RulesState, appKey: string): boolean {
    const rule = state.rules[appKey];
    if (rule) return rule.transmit;
    return state.mode === "allExcept";
}

/** Records an explicit decision and remembers how to display the entry later. */
export function setTransmit(state: RulesState, app: Pick<AppInfo, "appKey" | "displayName" | "exePath">, transmit: boolean, now = Date.now()): RulesState {
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

/** Removes every explicit choice (the built in defaults are seeded again). */
export function resetRules(state: RulesState): RulesState {
    return { ...state, rules: {} };
}

/** Forgets a single application. */
export function forgetApp(state: RulesState, appKey: string): RulesState {
    const rules = { ...state.rules };
    delete rules[appKey];
    return { ...state, rules };
}

export function toPayload(state: RulesState, active: boolean): RuleSetPayload {
    const overrides: Record<string, boolean> = {};
    for (const [key, rule] of Object.entries(state.rules)) overrides[key] = rule.transmit;
    return { mode: state.mode === "onlySelected" ? 1 : 0, overrides, active };
}

/**
 * Built in default exclusions.
 *
 * Discord's own voice chat is excluded by default in mode A: it is the one
 * application whose audio must not be sent back to the people producing it.
 * The decision is stored like any other rule, so the user can undo it and can
 * see why it exists.
 */
export function seedDefaults(state: RulesState, discordExecutable: string, now = Date.now()): RulesState {
    if (state.version >= RULES_VERSION && Object.keys(state.rules).length > 0) return state;
    if (!discordExecutable) return { ...state, version: RULES_VERSION };

    const key = `exe:${discordExecutable.replace(/\//g, "\\").toLowerCase()}`;
    if (state.rules[key] && state.version >= RULES_VERSION) return { ...state, version: RULES_VERSION };

    return {
        ...state,
        version: RULES_VERSION,
        rules: {
            ...state.rules,
            [key]: {
                transmit: false,
                name: "Discord (голосовой канал)",
                exePath: discordExecutable,
                lastSeen: now
            }
        }
    };
}

/** Loads a possibly broken/old persisted state. */
export function normaliseState(raw: unknown, fallbackMode: FilterMode = "allExcept"): RulesState {
    const state = emptyState(fallbackMode);
    if (!raw || typeof raw !== "object") return state;

    const candidate = raw as Partial<RulesState>;
    if (candidate.mode === "allExcept" || candidate.mode === "onlySelected") state.mode = candidate.mode;
    if (typeof candidate.version === "number") state.version = candidate.version;

    if (candidate.rules && typeof candidate.rules === "object") {
        for (const [key, value] of Object.entries(candidate.rules)) {
            if (!value || typeof value !== "object") continue;
            const rule = value as Partial<AppRule>;
            if (typeof rule.transmit !== "boolean") continue;
            state.rules[key] = {
                transmit: rule.transmit,
                name: typeof rule.name === "string" ? rule.name : undefined,
                exePath: typeof rule.exePath === "string" ? rule.exePath : undefined,
                lastSeen: typeof rule.lastSeen === "number" ? rule.lastSeen : undefined
            };
        }
    }
    return state;
}

/**
 * Merges the live application list with the saved rules.
 *
 * Sections (requirement: applications with audio first, saved rules for closed
 * applications separately):
 *   current — a session is active right now
 *   idle    — running, but not playing anything at the moment
 *   closed  — saved rule of an application that is not running
 */
export function buildAppList(live: AppInfo[], state: RulesState, discordExecutable = ""): AppListEntry[] {
    const entries: AppListEntry[] = [];
    const seen = new Set<string>();

    for (const app of live) {
        seen.add(app.appKey);
        const transmitting = resolveTransmit(state, app.appKey);
        const details: string[] = [];
        if (app.exeName) details.push(app.exeName);
        if (app.format) details.push(app.format);
        if (app.captured && app.latencyMs >= 0) details.push(`задержка ~${app.latencyMs} мс`);

        entries.push({
            appKey: app.appKey,
            title: displayNameOf(app),
            subtitle: subtitleOf(app),
            section: app.playing ? "current" : "idle",
            transmitting: transmitting && app.capturable,
            capturable: app.capturable,
            ephemeral: app.ephemeral,
            state: app.state,
            peak: app.peak,
            details: details.join(" · "),
            error: app.error || app.notCapturableReason,
            exePath: app.exePath
        });
    }

    for (const [key, rule] of Object.entries(state.rules)) {
        if (seen.has(key)) continue;
        const isDiscord = discordExecutable && rule.exePath
            && rule.exePath.replace(/\//g, "\\").toLowerCase() === discordExecutable.replace(/\//g, "\\").toLowerCase();
        entries.push({
            appKey: key,
            title: rule.name || "Приложение",
            subtitle: "Приложение закрыто",
            section: "closed",
            transmitting: rule.transmit,
            capturable: true,
            ephemeral: false,
            state: "closed",
            peak: 0,
            details: isDiscord ? "по умолчанию исключено, чтобы голосовой канал не попадал в трансляцию" : "",
            error: "",
            exePath: rule.exePath || ""
        });
    }

    const weight = (entry: AppListEntry) => (entry.section === "current" ? 0 : entry.section === "idle" ? 1 : 2);
    entries.sort((a, b) => {
        const bySection = weight(a) - weight(b);
        if (bySection !== 0) return bySection;
        // Louder applications first inside the active section.
        if (a.section === "current" && Math.abs(b.peak - a.peak) > 0.02) return b.peak - a.peak;
        return a.title.localeCompare(b.title);
    });

    return entries;
}

function displayNameOf(app: AppInfo): string {
    return app.displayName || app.exeName || app.appKey;
}

function subtitleOf(app: AppInfo): string {
    if (!app.capturable) return "недоступно для фильтрации";
    if (app.error) return "ошибка захвата";
    if (app.state === "excluded") return "исключено из трансляции";
    if (!app.playing) return "сейчас не воспроизводит звук";
    if (app.captured) return "передаётся зрителям";
    return "звук не захвачен";
}
