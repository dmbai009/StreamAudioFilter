/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Renderer state: rules, live application list, host state and — the part
 * that must never lie — what the viewers of the user's own share receive.
 *
 * One store instance is shared by every panel, so all entry points show the
 * same state and only one poll loop exists.
 *
 * Lifecycle of the audio host's work:
 *   * engine (application list): while a panel is open, while in a voice
 *     channel, or while sharing; stopped 30 s after the last need ends.
 *   * output (the muted session Discord reads): while in a voice channel, so
 *     it already exists when Discord attaches at the start of a share.
 *   * capture gate (rules.active): only while the plugin provides the audio of
 *     the current share. Nothing is captured at any other time.
 */

import { React } from "@webpack/common";

import { DataStore } from "../dataStore";
import { t } from "../i18n";
import { type Backend, Native } from "../nativeBridge";
import { settings } from "../settings";
import { decideSoundshare, type GoLiveAction, readGoLiveAction } from "../stream/soundshare";
import type { AppInfo, AppListEntry, DiagnosticsReport, HostStats, NativeEnvironment, RulesState, ShareAudio, ShareStatus } from "../types";
import { PLUGIN_VERSION } from "../version";
import { buildAppList, isAudioRouter, normaliseState, resetRules, setMode as setModeInState, setTransmit as setTransmitInState, toPayload } from "./rules";

const STORE_KEY = "StreamAudioFilter_rules";
const POLL_INTERVAL_MS = 1000;
const IDLE_SHUTDOWN_MS = 30000;
/** How long Discord may take to attach to the host before the share is reported as failed. */
const ATTACH_TIMEOUT_MS = 8000;
/** A share whose soundshare decision is older than this did not pass through the plugin. */
const DECISION_WINDOW_MS = 30000;

export type PanelKind = "settings" | "popout" | "floating";

export interface Problem {
    key: string;
    level: "error" | "warning" | "info";
    text: string;
}

export interface StoreState {
    environment: NativeEnvironment | null;
    rules: RulesState;
    apps: AppInfo[];
    stats: HostStats | null;
    icons: Record<string, string | null>;
    share: ShareStatus;
    panels: Record<PanelKind, number>;
    inVoice: boolean;
    engineRunning: boolean;
    /** Error of the last user action (cleared by the next successful one). */
    actionError: string;
    patch: { pidHook: boolean; modeHook: boolean; };
}

const IDLE_SHARE: ShareStatus = { audio: "idle", reason: "", since: 0 };

function initialState(): StoreState {
    return {
        environment: null,
        rules: { mode: "allExcept", rules: {}, version: 0 },
        apps: [],
        stats: null,
        icons: {},
        share: IDLE_SHARE,
        panels: { settings: 0, popout: 0, floating: 0 },
        inVoice: false,
        engineRunning: false,
        actionError: "",
        patch: { pidHook: false, modeHook: false }
    };
}

class Store {
    private state: StoreState = initialState();
    private listeners = new Set<() => void>();
    private pollTimer: ReturnType<typeof setInterval> | null = null;
    private idleTimer: ReturnType<typeof setTimeout> | null = null;
    private refreshing = false;
    private loaded = false;

    // Synchronous state for the patched Discord code (it can not await).
    private hostPid = 0;
    /**
     * The host's render process: the pid Discord takes the share audio from.
     * Kept apart from the capturing host process, because Discord hooks the
     * process it reads and captures inside that process deliver silence.
     */
    private renderPid = 0;
    private hostProblem = "";
    private redirectActive = false;
    private lastDecisionAt = 0;
    private lastRenderedFrames = -1;
    private lastRemutes = 0;
    private outputWanted = false;

    // ------------------------------------------------------------------ plumbing

    subscribe = (listener: () => void) => {
        this.listeners.add(listener);
        return () => { this.listeners.delete(listener); };
    };

    getSnapshot = () => this.state;

    private set(patch: Partial<StoreState>) {
        this.state = { ...this.state, ...patch };
        for (const listener of this.listeners) listener();
    }

    private setShare(audio: ShareAudio, reason = "") {
        const since = audio === "idle" ? 0 : this.state.share.since || Date.now();
        if (this.state.share.audio === audio && this.state.share.reason === reason) return;
        this.set({ share: { audio, reason, since } });
    }

    private backend(): Backend {
        return settings.store.testBackend ? "mock" : "auto";
    }

    // ------------------------------------------------------------------- startup

    async init(patch: StoreState["patch"]): Promise<void> {
        this.set({ patch });
        await this.loadRules();
        await this.refreshHost();
        // The capability probe performs a real activation: not on the hot path.
        void this.refreshEnvironment();
    }

    async shutdown(): Promise<void> {
        this.stopPolling();
        if (this.idleTimer) clearTimeout(this.idleTimer);
        this.redirectActive = false;
        this.hostPid = 0;
        await Native.shutdownHost();
        this.state = initialState();
        this.loaded = false;
    }

    private async loadRules(): Promise<void> {
        const raw = await DataStore.get(STORE_KEY);
        this.set({ rules: normaliseState(raw) });
        this.loaded = true;
        await this.persist();
    }

    private async persist(): Promise<void> {
        await DataStore.set(STORE_KEY, this.state.rules);
    }

    async refreshHost(): Promise<void> {
        const info = await Native.ensureHost(this.backend());
        this.hostPid = info.running ? info.pid : 0;
        this.hostProblem = info.running ? "" : info.error;
        if (this.state.environment) this.set({ environment: { ...this.state.environment, host: info } });
    }

    async refreshEnvironment(): Promise<void> {
        const environment = await Native.getEnvironment(this.backend());
        this.hostPid = environment.host.running ? environment.host.pid : 0;
        this.hostProblem = environment.host.running ? "" : environment.host.error;
        this.set({ environment });
    }

    async restartHost(): Promise<void> {
        this.set({ engineRunning: false });
        await Native.restartHost(this.backend());
        await this.refreshEnvironment();
        await this.syncHost();
    }

    // ------------------------------------------------- the patched Discord code

    /**
     * Called synchronously from Discord's MEDIA_ENGINE_SET_GO_LIVE_SOURCE
     * handler with the pid Discord is about to capture. Returns the pid it
     * should capture instead. Must never throw: Discord's code runs around it.
     */
    soundshareSource(originalPid: number | null, action: GoLiveAction | undefined): number | null {
        try {
            if (!this.state.patch.pidHook) this.set({ patch: { ...this.state.patch, pidHook: true } });
            const { context, sourceId, sound } = readGoLiveAction(action);
            const decision = decideSoundshare({
                context,
                sourceId,
                sound,
                originalPid,
                filterEnabled: settings.store.filterEnabled,
                hostPid: this.hostPid ? this.renderPid : 0,
                hostProblem: this.hostProblem || (this.renderPid ? "" : "the audio output was not running yet; stop and start the share again"),
                fallbackAllowed: settings.store.fallbackUnfiltered
            });
            if (!decision) return originalPid;

            this.lastDecisionAt = Date.now();
            const wasRedirected = this.redirectActive;
            this.redirectActive = decision.redirect;
            if (decision.redirect && !wasRedirected) this.lastRenderedFrames = -1;
            this.setShare(decision.audio, decision.reason);
            void this.syncHost();
            return decision.pid;
        } catch (error) {
            // Fail closed: no audio rather than unknown audio.
            console.error("[StreamAudioFilter] soundshare decision failed", error);
            this.redirectActive = false;
            this.setShare("muted", String(error));
            return null;
        }
    }

    /** Discord asks whether to use its experimental (loopback) capture. */
    forceHookSoundshare(): boolean {
        if (!this.state.patch.modeHook) queueMicrotask(() => this.set({ patch: { ...this.state.patch, modeHook: true } }));
        return this.redirectActive;
    }

    // ------------------------------------------------------------- Discord events

    onVoiceChannel(channelId: string | null | undefined): void {
        const inVoice = !!channelId;
        if (inVoice === this.state.inVoice) return;
        this.set({ inVoice });
        void this.syncHost();
    }

    /** STREAM_CREATE / STREAM_START for the user's own stream. */
    onOwnStreamStarted(): void {
        if (Date.now() - this.lastDecisionAt > DECISION_WINDOW_MS && this.state.share.audio === "idle") {
            // A share exists, but Discord never asked the plugin about its
            // audio: the plugin does not know what the viewers hear.
            this.setShare("uncontrolled", "no soundshare decision was made for this share");
        }
        this.startPolling();
    }

    onOwnStreamEnded(): void {
        this.redirectActive = false;
        this.lastDecisionAt = 0;
        this.setShare("idle");
        void this.syncHost();
    }

    onSoundshareFailed(message: string): void {
        if (!this.redirectActive) return;
        this.setShare("attachFailed", message || "MEDIA_ENGINE_SOUNDSHARE_FAILED");
    }

    // ------------------------------------------------------------------- host sync

    private engineNeeded(): boolean {
        const { panels, inVoice, share } = this.state;
        return panels.settings + panels.popout + panels.floating > 0 || inVoice || share.audio !== "idle";
    }

    /** Brings the host in line with what is needed right now. */
    private async syncHost(): Promise<void> {
        if (!this.loaded) return;
        const needEngine = this.engineNeeded();
        const needOutput = this.state.inVoice || this.redirectActive;

        if (needEngine) {
            if (this.idleTimer) {
                clearTimeout(this.idleTimer);
                this.idleTimer = null;
            }
            if (!(await this.ensureEngine())) {
                if (this.redirectActive) this.setShare("attachFailed", this.state.actionError);
                return;
            }
            await this.applyRules();
            if (needOutput !== this.outputWanted) {
                this.outputWanted = needOutput;
                const result = await Native.setOutput(needOutput);
                this.renderPid = needOutput && result.ok ? result.pid ?? 0 : 0;
                if (!result.ok) this.set({ actionError: result.error ?? "the output could not be changed" });
            }
            if (this.redirectActive || this.panelsOpen() > 0) this.startPolling();
        } else {
            await this.applyRules();
            if (this.outputWanted) {
                this.outputWanted = false;
                this.renderPid = 0;
                await Native.setOutput(false);
            }
            this.scheduleIdleShutdown();
        }
    }

    private async ensureEngine(): Promise<boolean> {
        if (this.state.engineRunning) return true;
        if (!this.hostPid) await this.refreshHost();
        const result = await Native.startEngine(settings.store.maxStreams || 12, this.backend());
        if (!result.ok) {
            this.set({ engineRunning: false, actionError: result.error ?? "the audio engine could not be started" });
            return false;
        }
        this.outputWanted = false;
        this.renderPid = 0;
        this.set({ engineRunning: true, actionError: "" });
        return true;
    }

    private async applyRules(): Promise<void> {
        if (!this.state.engineRunning) return;
        const result = await Native.setRules(toPayload(this.state.rules, this.redirectActive, this.state.apps.map(app => app.appKey)));
        if (!result.ok && result.error) this.set({ actionError: result.error });
    }

    private scheduleIdleShutdown(): void {
        if (this.idleTimer) clearTimeout(this.idleTimer);
        this.idleTimer = setTimeout(() => {
            this.idleTimer = null;
            if (this.engineNeeded()) return;
            this.stopPolling();
            void Native.stopEngine();
            this.outputWanted = false;
            this.set({ engineRunning: false, apps: [], stats: null });
        }, IDLE_SHUTDOWN_MS);
    }

    // ---------------------------------------------------------------------- panels

    private panelsOpen(): number {
        const { panels } = this.state;
        return panels.settings + panels.popout + panels.floating;
    }

    /** Registers an open panel; returns the function that unregisters it. */
    openPanel(kind: PanelKind): () => void {
        this.set({ panels: { ...this.state.panels, [kind]: this.state.panels[kind] + 1 } });
        void this.syncHost().then(() => this.refresh());
        this.startPolling();

        let closed = false;
        return () => {
            if (closed) return;
            closed = true;
            this.set({ panels: { ...this.state.panels, [kind]: Math.max(0, this.state.panels[kind] - 1) } });
            void this.syncHost();
        };
    }

    // ---------------------------------------------------------------------- rules

    async setMode(mode: RulesState["mode"]): Promise<void> {
        this.set({ rules: setModeInState(this.state.rules, mode) });
        await this.persist();
        await this.applyRules();
        await this.refresh();
    }

    async setAllowed(entry: Pick<AppListEntry, "appKey" | "title" | "exePath">, allowed: boolean): Promise<void> {
        this.set({ rules: setTransmitInState(this.state.rules, { appKey: entry.appKey, displayName: entry.title, exePath: entry.exePath }, allowed) });
        await this.persist();
        await this.applyRules();
        await this.refresh();
    }

    async resetRules(): Promise<void> {
        this.set({ rules: resetRules(this.state.rules) });
        await this.persist();
        await this.applyRules();
        await this.refresh();
    }

    // -------------------------------------------------------------------- polling

    private startPolling(): void {
        if (this.pollTimer) return;
        this.pollTimer = setInterval(() => {
            if (this.panelsOpen() === 0 && this.state.share.audio === "idle") {
                this.stopPolling();
                return;
            }
            void this.refresh();
        }, POLL_INTERVAL_MS);
    }

    private stopPolling(): void {
        if (!this.pollTimer) return;
        clearInterval(this.pollTimer);
        this.pollTimer = null;
    }

    async refresh(force = false): Promise<void> {
        if (this.refreshing || !this.state.engineRunning) return;
        this.refreshing = true;
        try {
            if (force) await Native.refreshApps();
            const [apps, stats] = await Promise.all([Native.listApps(), Native.getStats()]);
            if (stats === null && this.state.engineRunning) {
                // The host went away (crash, killed): start over on the next need.
                this.hostPid = 0;
                this.renderPid = 0;
                this.set({ engineRunning: false, stats: null });
                if (this.redirectActive) this.setShare("attachFailed", "the audio host stopped");
                void this.refreshHost().then(() => this.syncHost());
                return;
            }
            const newRouter = apps.some(app => isAudioRouter(app.appKey) && !this.state.apps.some(old => old.appKey === app.appKey));
            this.set({ apps, stats });
            if (newRouter) void this.applyRules();
            if (stats?.output) this.renderPid = stats.output.running ? stats.output.pid : 0;
            this.updateShareFromStats(stats);
            if (apps.length) void this.loadIcons(apps);
        } finally {
            this.refreshing = false;
        }
    }

    /** Confirms (or refutes) that Discord really reads the filtered audio. */
    private updateShareFromStats(stats: HostStats | null): void {
        const output = stats?.output;
        if (output && output.remutes > this.lastRemutes) this.lastRemutes = output.remutes;
        if (!this.redirectActive) return;

        const { audio, since } = this.state.share;
        const advancing = !!output && this.lastRenderedFrames >= 0 && output.renderedFrames > this.lastRenderedFrames;
        if (output) this.lastRenderedFrames = output.renderedFrames;

        const confirmed = !!output && output.running && output.discordAttached && advancing;
        if (confirmed) {
            if (audio !== "filtered") this.setShare("filtered");
            return;
        }
        if (audio === "filtered" && output && !output.running) {
            this.setShare("attachFailed", output.error || "the output of the audio host stopped");
        } else if (audio === "starting" && Date.now() - since > ATTACH_TIMEOUT_MS) {
            this.setShare("attachFailed", output?.discordAttached
                ? "the audio host is not producing audio"
                : "Discord did not attach to the audio host (DiscordHook64.dll is not loaded in it)");
        }
    }

    private iconRequests = new Set<string>();

    private async loadIcons(apps: AppInfo[]): Promise<void> {
        const missing = apps.filter(app => app.exePath && !(app.exePath in this.state.icons) && !this.iconRequests.has(app.exePath));
        if (!missing.length) return;
        for (const app of missing) this.iconRequests.add(app.exePath);
        const results = await Promise.all(missing.map(app => Native.getAppIcon(app.exePath).catch(() => null)));
        const icons = { ...this.state.icons };
        missing.forEach((app, index) => { icons[app.exePath] = results[index]; });
        this.set({ icons });
    }

    // -------------------------------------------------------------------- report

    buildReport(): DiagnosticsReport {
        const { state } = this;
        return {
            generatedAt: new Date().toISOString(),
            pluginVersion: PLUGIN_VERSION,
            environment: state.environment,
            share: state.share,
            stats: state.stats,
            apps: state.apps,
            problems: problemsOf(state).map(problem => `[${problem.level}] ${problem.text}`),
            patch: state.patch,
            runtime: {
                inVoice: state.inVoice,
                engineRunning: state.engineRunning,
                renderPid: this.renderPid,
                redirectActive: this.redirectActive,
                panels: state.panels,
                actionError: state.actionError
            }
        };
    }
}

export const store = new Store();

/** Subscribes a component to the whole store state (identity is stable). */
export function useStoreState(): StoreState {
    return React.useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot);
}

/** The application list for the UI (memoised per state object). */
const entriesCache = new WeakMap<StoreState, AppListEntry[]>();
export function entriesOf(state: StoreState): AppListEntry[] {
    let entries = entriesCache.get(state);
    if (!entries) {
        entries = buildAppList(state.apps, state.rules);
        entriesCache.set(state, entries);
    }
    return entries;
}

/**
 * Every problem worth showing, derived from the current state (never
 * accumulated, so nothing is shown twice and nothing stale stays around).
 */
export function problemsOf(state: StoreState): Problem[] {
    const problems: Problem[] = [];
    const add = (key: string, level: Problem["level"], text: string) => {
        if (text && !problems.some(problem => problem.key === key)) problems.push({ key, level, text });
    };

    const env = state.environment;
    if (env && !env.host.found) add("host-missing", "error", `${t("problem.hostMissing")}: ${env.host.error}`);
    else if (env && !env.host.running) add("host-failed", "error", `${t("problem.hostFailed")}: ${env.host.error}`);
    else if (env && !env.support.supported) add("unsupported", "error", `${t("problem.unsupported")}: ${env.support.reason}`);
    if (!state.patch.modeHook || state.share.audio === "uncontrolled") add("patch", "error", t("problem.patch"));

    if (state.actionError) add("action", "error", state.actionError);
    const output = state.stats?.output;
    if (output?.error) add("output", "error", `${t("diag.output")}: ${output.error}`);
    if (output && output.remutes > 0) add("remuted", "info", t("problem.sessionUnmuted"));

    for (const app of state.apps) {
        if (app.silentCapture) add(`silent:${app.appKey}`, "warning", t("problem.silentCapture", { app: app.displayName, device: app.devices[0] ?? "?" }));
        if (app.state === "blocked" && app.blockedReason) add(`blocked:${app.appKey}`, "warning", t("problem.blocked", { app: app.displayName, reason: app.blockedReason }));
        if (app.state === "error" && app.error) add(`error:${app.appKey}`, "warning", `${app.displayName}: ${app.error}`);
    }
    for (const warning of state.stats?.engine?.warnings ?? []) add(`engine:${warning}`, "warning", warning);
    return problems;
}
