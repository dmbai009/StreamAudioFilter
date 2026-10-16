/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Types shared between the renderer (index.tsx and everything below it) and the
 * main process side (native.ts). This file must stay free of imports so both
 * sides can use it.
 */

/** Mode A: everything is transmitted unless it was switched off. */
export type FilterMode = "allExcept" | "onlySelected";

/** What the UI sends to the audio host. */
export interface RuleSetPayload {
    /** 0 = allExcept, 1 = onlySelected (matches saf::RuleSet::Mode). */
    mode: 0 | 1;
    overrides: Record<string, boolean>;
    /**
     * Capture gate: false while the plugin does not provide the audio of a
     * share. The host then only lists applications and captures nothing.
     */
    active: boolean;
}

/** One application, as reported by the audio host. */
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
    /** Effective decision after mode + overrides, while the gate is open. */
    transmitting: boolean;
    /** A capture of this application is part of the mix. */
    captured: boolean;
    /** Peak level 0..1 (capture level, or the Windows session meter). */
    peak: number;
    latencyMs: number;
    format: string;
    error: string;
    notCapturableReason: string;
    /** Set when a transmitted application can not be sent (process tree conflict). */
    blockedReason: string;
    /** Set when the application is already contained in another application's capture. */
    coveredBy: string;
    starvedFrames: number;
    /** transmitting | covered | excluded | blocked | silent | error | unsupported */
    state: string;
    /** Independent instances (process trees) of this application. */
    instances: number;
    /** Output devices the application plays on. */
    devices: string[];
    /** Windows says the application plays, but its capture is pure silence. */
    silentCapture: boolean;
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
    starvedFrames: number;
    limitedBlocks: number;
    clampedSamples: number;
    activeStreams: number;
    warnings: string[];
}

/** The host's output: the muted session Discord takes the share audio from. */
export interface OutputStats {
    running: boolean;
    kind: string;
    device: string;
    format: string;
    error: string;
    renderedFrames: number;
    /** Peak 0..1 of the last rendered second: what the viewers receive. */
    peak: number;
    sessionMuted: boolean;
    remutes: number;
    /** Discord's soundshare hook is loaded into the host: Discord reads this audio. */
    discordAttached: boolean;
    restarts: number;
    /** The render process Discord is pointed at. */
    pid: number;
    underrunFrames: number;
}

export interface HostStats {
    engine: EngineStats | null;
    output: OutputStats | null;
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

/** The audio host process and how it was found. */
export interface HostInfo {
    found: boolean;
    running: boolean;
    path: string;
    version: string;
    protocol: number;
    pid: number;
    error: string;
    /** Every path that was tried, in order. */
    triedPaths: string[];
}

export interface NativeEnvironment {
    platform: string;
    arch: string;
    isWindows: boolean;
    host: HostInfo;
    support: SupportInfo;
    /** Where the host executable is expected. */
    hostDir: string;
    /** Version of the plugin's TypeScript part (must match the host). */
    pluginVersion: string;
    /** Host protocol this plugin speaks. */
    pluginProtocol: number;
    /** True while the scripted test backend is in use. */
    mock: boolean;
}

export interface StartResult {
    ok: boolean;
    /** setOutput: the render process, i.e. the pid Discord must capture. */
    pid?: number;
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
    version: number;
}

export interface AppListEntry {
    appKey: string;
    title: string;
    exeName: string;
    /** current = has audio right now, idle = running without audio, closed = saved rule only */
    section: "current" | "idle" | "closed";
    /** The switch: true = the viewers may hear this application. */
    allowed: boolean;
    /** The switch value comes from the mode, not from a saved choice. */
    byDefault: boolean;
    /** Re-plays other applications' audio (blocked by default). */
    router: boolean;
    capturable: boolean;
    ephemeral: boolean;
    state: string;
    captured: boolean;
    peak: number;
    instances: number;
    devices: string[];
    silentCapture: boolean;
    format: string;
    latencyMs: number;
    error: string;
    notCapturableReason: string;
    blockedReason: string;
    coveredBy: string;
    exePath: string;
}

/**
 * What happens to the audio of the user's own screen share, from the point of
 * view of the viewers. Every value means something different for privacy, so
 * the UI must be able to tell all of them apart:
 *
 *   idle         no own share
 *   noAudio      sharing without audio (Discord's "Sound" switch is off)
 *   window       sharing a window: Discord sends that application's own audio;
 *                the plugin does not take part
 *   off          filtering switched off by the user: Discord sends all system audio
 *   starting     the plugin took over the share audio, waiting for Discord to attach
 *   filtered     confirmed: Discord reads the plugin's filtered mix
 *   attachFailed the plugin took over, but Discord did not attach to the mix
 *                (the viewers most likely hear nothing)
 *   muted        filtering wanted but impossible; the share has no audio (fail closed)
 *   unfiltered   filtering impossible, and the user allowed plain system audio
 *   uncontrolled a share started without passing through the plugin (Discord changed):
 *                the plugin does not know or control what the viewers hear
 */
export type ShareAudio =
    | "idle"
    | "noAudio"
    | "window"
    | "off"
    | "starting"
    | "filtered"
    | "attachFailed"
    | "muted"
    | "unfiltered"
    | "uncontrolled";

export interface ShareStatus {
    audio: ShareAudio;
    /** Technical reason for the state (shown in the details). */
    reason: string;
    /** When the own share started (ms), 0 when idle. */
    since: number;
}

export interface DiagnosticsReport {
    generatedAt: string;
    pluginVersion: string;
    environment: NativeEnvironment | null;
    share: ShareStatus;
    stats: HostStats | null;
    apps: AppInfo[];
    problems: string[];
    patch: { pidHook: boolean; modeHook: boolean; };
}
