/*
 * StreamAudioFilter — per application audio filtering for screen shares
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Types shared between the renderer (index.tsx and everything below it) and the
 * main process side (native.ts). This file must stay free of imports so both
 * sides can use it.
 */

/** Mode A: everything is transmitted unless it was switched off. */
export type FilterMode = "allExcept" | "onlySelected";

/** What the UI sends to the native engine. */
export interface RuleSetPayload {
    /** 0 = allExcept, 1 = onlySelected (matches saf::RuleSet::Mode). */
    mode: 0 | 1;
    overrides: Record<string, boolean>;
    /**
     * False while no screen share is running: the engine keeps watching the
     * application list but captures nothing, so the plugin stays quiet in the
     * background.
     */
    active: boolean;
}

/** One application, as reported by the native engine. */
export interface AppInfo {
    appKey: string;
    displayName: string;
    exeName: string;
    exePath: string;
    /** A session of this application is currently active. */
    playing: boolean;
    capturable: boolean;
    /** The application could not be identified for a persistent rule. */
    ephemeral: boolean;
    /** Effective decision after mode + overrides. */
    transmitting: boolean;
    /** A capture stream currently exists. */
    captured: boolean;
    /** Peak level 0..1 of the last mixed block (or of the session meter). */
    peak: number;
    latencyMs: number;
    format: string;
    error: string;
    notCapturableReason: string;
    starvedFrames: number;
    /** transmitting | excluded | silent | error | unsupported */
    state: string;
}

export interface EngineStats {
    running: boolean;
    backend: string;
    captureBackend: string;
    sampleRate: number;
    channels: number;
    blockMs: number;
    blocksMixed: number;
    outputFrames: number;
    outputDroppedFrames: number;
    starvedFrames: number;
    limitedBlocks: number;
    clampedSamples: number;
    activeStreams: number;
    outputBufferedFrames: number;
    warnings: string[];
}

export interface SupportInfo {
    supported: boolean;
    os: string;
    build: number;
    reason: string;
    detail: string;
    checks: string[];
    platform: string;
}

export interface AddonInfo {
    loaded: boolean;
    path: string;
    version: string;
    error: string;
    /** Every path that was tried, in order. */
    triedPaths: string[];
    napi: number;
}

export interface NativeEnvironment {
    platform: string;
    arch: string;
    isWindows: boolean;
    addon: AddonInfo;
    support: SupportInfo;
    /** Where the built .node file should be placed. */
    dataDir: string;
    /** Path of the Discord executable, excluded by default to avoid echo. */
    discordExecutable: string;
    /** True while the scripted test backend is in use. */
    mock: boolean;
}

export interface EngineStartOptions {
    sampleRate: number;
    blockMs: number;
    maxStreams: number;
    outputBufferMs: number;
    backend?: "auto" | "wasapi" | "mock";
}

export interface StartResult {
    ok: boolean;
    error?: string;
    warning?: string;
}

/** Scripted application list for the test backend. */
export interface MockApp {
    appKey: string;
    displayName: string;
    exePath: string;
    exeName: string;
    pid: number;
    parentPid: number;
    playing: boolean;
    capturable: boolean;
    notCapturableReason?: string;
}

/** Persisted rule of one application. */
export interface AppRule {
    transmit: boolean;
    /** Display name, kept so closed applications stay recognisable. */
    name?: string;
    exePath?: string;
    lastSeen?: number;
}

export interface RulesState {
    mode: FilterMode;
    rules: Record<string, AppRule>;
    /** Set once the built in default exclusions have been seeded. */
    version: number;
}

export interface AppListEntry {
    appKey: string;
    title: string;
    subtitle: string;
    /** current = has audio right now, idle = running without audio, closed = saved rule only */
    section: "current" | "idle" | "closed";
    transmitting: boolean;
    capturable: boolean;
    ephemeral: boolean;
    state: string;
    peak: number;
    details: string;
    error: string;
    exePath: string;
}

export interface StreamStatus {
    active: boolean;
    filtering: boolean;
    /** Why filtering is inactive, when the user asked for it. */
    reason: string;
    startedAt: number;
    underruns: number;
    pipelinedMs: number;
    sampleRate: number;
}

export interface DiagnosticsReport {
    environment: NativeEnvironment;
    engine: EngineStats | null;
    apps: AppInfo[];
    stream: StreamStatus;
    lastError: string;
}
