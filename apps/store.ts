/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Renderer state: environment, rules, live application list, stream status.
 * One store instance is shared by every panel (settings page, popout, floating
 * panel, toolbox) so all entry points always show the same state — that is a
 * requirement, and it also means only one poll loop exists.
 */

import { React } from "@webpack/common";

import { createFilteredTrack, type FilteredTrack } from "../audio/sourceTrack";
import { DataStore } from "../dataStore";
import { Engine, Native } from "../nativeBridge";
import { settings } from "../settings";
import type { AppInfo, EngineStats, NativeEnvironment, RulesState, StreamStatus } from "../types";
import { buildAppList, normaliseState, resetRules, resolveTransmit, seedDefaults, setMode as setModeInState, setTransmit } from "./rules";

const STORE_KEY = "StreamAudioFilter_rules";
const SAMPLE_RATE = 48000;
const POLL_INTERVAL_MS = 1000;
const IDLE_SHUTDOWN_MS = 30000;

export interface StoreState {
    environment: NativeEnvironment | null;
    rules: RulesState;
    apps: AppInfo[];
    stats: EngineStats | null;
    icons: Record<string, string | null>;
    stream: StreamStatus;
    /** Panels currently visible; polling only runs while something is visible. */
    panelsOpen: number;
    engineRunning: boolean;
    loading: boolean;
    lastError: string;
    lastWarning: string;
}

function initialState(): StoreState {
    return {
        environment: null,
        rules: { mode: "allExcept", rules: {}, version: 0 },
        apps: [],
        stats: null,
        icons: {},
        stream: { active: false, filtering: false, reason: "", startedAt: 0, underruns: 0, pipelinedMs: 0, sampleRate: SAMPLE_RATE },
        panelsOpen: 0,
        engineRunning: false,
        loading: false,
        lastError: "",
        lastWarning: ""
    };
}

class Store {
    private state: StoreState = initialState();
    private listeners = new Set<() => void>();
    private pollTimer: NodeJS.Timeout | null = null;
    private idleTimer: NodeJS.Timeout | null = null;
    private track: FilteredTrack | null = null;
    private refreshing = false;
    private loadedRules = false;

    // ------------------------------------------------------------------ plumbing

    subscribe = (listener: () => void) => {
        this.listeners.add(listener);
        return () => this.listeners.delete(listener);
    };

    getSnapshot = () => this.state;

    private set(patch: Partial<StoreState>) {
        this.state = { ...this.state, ...patch };
        for (const listener of this.listeners) listener();
    }

    // --------------------------------------------------------------------- rules

    async loadRules(): Promise<RulesState> {
        let raw: unknown = null;
        try {
            raw = await DataStore.get(STORE_KEY);
        } catch (error) {
            this.set({ lastError: `не удалось прочитать сохранённые правила: ${(error as Error).message}` });
        }

        const environment = this.state.environment ?? await Native.getEnvironment().catch(() => null);
        if (environment && !this.state.environment) this.set({ environment });

        const state = seedDefaults(normaliseState(raw, this.defaultMode()), environment?.discordExecutable ?? "");
        this.loadedRules = true;
        this.set({ rules: state });
        void this.persist();
        return state;
    }

    private defaultMode() {
        return this.state.rules.mode;
    }

    private async persist(): Promise<void> {
        try {
            await DataStore.set(STORE_KEY, this.state.rules);
        } catch (error) {
            this.set({ lastError: `не удалось сохранить правила: ${(error as Error).message}` });
        }
    }

    /** Pushes the current rules to the engine (including the capture gate). */
    async applyRules(): Promise<void> {
        if (!this.state.engineRunning) return;
        const payload = {
            mode: this.state.rules.mode === "onlySelected" ? 1 as const : 0 as const,
            overrides: Object.fromEntries(Object.entries(this.state.rules.rules).map(([key, rule]) => [key, rule.transmit])),
            active: this.state.stream.filtering
        };
        const result = await Engine.setRules(payload);
        if (!result.ok && result.error) this.set({ lastError: result.error });
    }

    async setMode(mode: RulesState["mode"]): Promise<void> {
        this.set({ rules: setModeInState(this.state.rules, mode) });
        await this.persist();
        await this.applyRules();
        await this.refresh();
    }

    async setTransmit(app: Pick<AppInfo, "appKey"> & Partial<Pick<AppInfo, "displayName" | "exePath">>, transmit: boolean): Promise<void> {
        this.set({ rules: setTransmit(this.state.rules, app, transmit) });
        await this.persist();
        await this.applyRules();
        await this.refresh();
    }

    async resetFilters(): Promise<void> {
        const { environment } = this.state;
        this.set({ rules: seedDefaults(resetRules(this.state.rules), environment?.discordExecutable ?? "") });
        await this.persist();
        await this.applyRules();
        await this.refresh();
    }

    // ------------------------------------------------------------------- engine

    private backend(): "auto" | "wasapi" | "mock" {
        // The scripted back end exists to verify the whole pipeline without
        // WASAPI; it is never selected implicitly.
        return settings.store.testBackend ? "mock" : "auto";
    }

    /** Starts the engine in monitor mode (no capture) — used by the panels. */
    async ensureEngine(): Promise<boolean> {
        if (this.state.engineRunning) return true;
        if (!this.loadedRules) await this.loadRules();

        this.set({ loading: true });
        const result = await Engine.start({
            sampleRate: SAMPLE_RATE,
            blockMs: 10,
            maxStreams: settings.store.maxStreams || 12,
            outputBufferMs: 300,
            backend: this.backend()
        });
        this.set({ loading: false });

        if (!result.ok) {
            this.set({ engineRunning: false, lastError: result.error ?? "не удалось запустить аудиодвижок" });
            return false;
        }

        if (result.warning) this.set({ lastWarning: result.warning });
        this.set({ engineRunning: true });
        await this.applyRules();
        await this.refresh();
        return true;
    }

    async stopEngine(): Promise<void> {
        this.stopPolling();
        await Engine.stop();
        this.set({ engineRunning: false, apps: [], stats: null });
    }

    private scheduleIdleShutdown() {
        if (this.idleTimer) clearTimeout(this.idleTimer);
        if (this.state.stream.active || this.state.panelsOpen > 0) return;
        this.idleTimer = setTimeout(() => {
            if (this.state.stream.active || this.state.panelsOpen > 0) return;
            void this.stopEngine();
        }, IDLE_SHUTDOWN_MS);
    }

    // -------------------------------------------------------------------- stream

    /** Creates the filtered track for an intercepted screen share. */
    async startFilteredTrack(): Promise<MediaStreamTrack | null> {
        const environment = this.state.environment ?? await Native.getEnvironment().catch(() => null);
        if (environment && !this.state.environment) this.set({ environment });

        if (environment && !environment.addon.loaded) {
            this.set({ lastError: environment.addon.error });
            return null;
        }
        if (environment && !environment.support.supported && this.backend() !== "mock") {
            this.set({ lastError: environment.support.reason });
            return null;
        }

        this.set({ stream: { ...this.state.stream, filtering: true, active: true, startedAt: Date.now(), reason: "", underruns: 0 } });
        if (!(await this.ensureEngine())) {
            this.set({ stream: { ...this.state.stream, filtering: false, active: false, reason: this.state.lastError } });
            return null;
        }

        await this.applyRules();
        this.startPolling();

        try {
            const handle = await createFilteredTrack({
                sampleRate: SAMPLE_RATE,
                bufferedMs: settings.store.bufferMs || 60,
                pull: frames => Native.readPcm(frames),
                onStats: stats => this.set({
                    stream: { ...this.state.stream, underruns: stats.underruns, pipelinedMs: Math.round(stats.bufferedMs), sampleRate: stats.sampleRate }
                }),
                onError: message => this.set({ lastError: message })
            });
            this.track = handle;
            return handle.track;
        } catch (error) {
            const reason = `не удалось создать звуковой поток: ${(error as Error).message}`;
            this.set({ lastError: reason, stream: { ...this.state.stream, filtering: false, active: false, reason } });
            return null;
        }
    }

    /**
     * Discord reports a running stream. Used for the status display, and to
     * detect the case where a stream exists but our audio hook never fired.
     */
    markStreamStarted(): void {
        if (this.state.stream.active) return;
        this.set({ stream: { ...this.state.stream, active: true, startedAt: Date.now() } });
        this.startPolling();
    }

    /** Called when the share ends (or the plugin is stopped). */
    stopFiltering(reason = ""): void {
        if (this.track) {
            this.track.stop();
            this.track = null;
        }
        this.set({ stream: { active: false, filtering: false, reason, startedAt: 0, underruns: 0, pipelinedMs: 0, sampleRate: SAMPLE_RATE } });
        void this.applyRules();
        this.scheduleIdleShutdown();
    }

    onSessionEnd(): void {
        this.stopFiltering();
    }

    // ------------------------------------------------------------------ polling

    /** Registers an open panel; polling runs while at least one is open. */
    openPanel(): () => void {
        this.set({ panelsOpen: this.state.panelsOpen + 1 });
        void this.ensureEngine().then(() => this.refresh());
        this.startPolling();

        let closed = false;
        return () => {
            if (closed) return;
            closed = true;
            this.set({ panelsOpen: Math.max(0, this.state.panelsOpen - 1) });
            if (this.state.panelsOpen === 0) this.scheduleIdleShutdown();
        };
    }

    private startPolling(): void {
        if (this.pollTimer) return;
        this.pollTimer = setInterval(() => {
            if (this.state.panelsOpen === 0 && !this.state.stream.active) {
                this.stopPolling();
                return;
            }
            void this.refresh();
        }, POLL_INTERVAL_MS);
    }

    private stopPolling(): void {
        if (this.pollTimer) {
            clearInterval(this.pollTimer);
            this.pollTimer = null;
        }
    }

    /**
     * Re-reads the application list and the engine statistics.
     *
     * @param force ask the native engine to enumerate again instead of returning
     *              the list it already has (used by "Обновить список")
     */
    async refresh(force = false): Promise<void> {
        if (this.refreshing) return;
        if (!this.state.engineRunning) return;
        this.refreshing = true;
        if (force) await Native.refreshApps();

        try {
            const [apps, stats] = await Promise.all([Native.listApps(), Native.getStats()]);
            if (apps.length) void this.loadIcons(apps);
            this.set({ apps, stats: stats ?? null });
        } finally {
            this.refreshing = false;
        }
    }

    private iconRequests = new Set<string>();

    private async loadIcons(apps: AppInfo[]): Promise<void> {
        const icons = { ...this.state.icons };
        let changed = false;

        for (const app of apps) {
            if (!app.exePath || app.exePath in icons || this.iconRequests.has(app.exePath)) continue;
            this.iconRequests.add(app.exePath);
            try {
                icons[app.exePath] = await Native.getAppIcon(app.exePath);
                changed = true;
            } catch {
                icons[app.exePath] = null;
            }
        }

        if (changed) this.set({ icons });
    }

    // ------------------------------------------------------------------ helpers

    /** Entries for the UI, merged from live sessions and saved rules. */
    entries() {
        return buildAppList(this.state.apps, this.state.rules, this.state.environment?.discordExecutable ?? "");
    }

    transmitState(appKey: string): boolean {
        return resolveTransmit(this.state.rules, appKey);
    }

    clearError(): void {
        this.set({ lastError: "" });
    }

    setLastError(message: string): void {
        this.set({ lastError: message });
    }

    setLastWarning(message: string): void {
        this.set({ lastWarning: message });
    }

    setEnvironment(environment: NativeEnvironment): void {
        this.set({ environment });
    }
}

export const store = new Store();

/** Subscribes a component to the whole store state (identity is stable). */
export function useStoreState(): StoreState {
    return React.useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot);
}
