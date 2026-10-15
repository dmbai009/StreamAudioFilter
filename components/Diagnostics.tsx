/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * The part of the panel that explains what is going on when something does not
 * work: capability probe, native module, counters, warnings. Normal users never
 * need it, so it stays collapsed.
 */

import { Button } from "@components/Button";
import { Divider } from "@components/Divider";
import { Span } from "@components/Span";
import { useState } from "@webpack/common";

import { Native } from "../nativeBridge";
import type { EngineStats, NativeEnvironment, StreamStatus } from "../types";

const cl = (name: string) => `vc-saf-${name}`;

function Row({ label, value }: { label: string; value: string; }) {
    return (
        <div className={cl("diag-row")}>
            <Span size="sm" className={cl("diag-label")}>{label}</Span>
            <Span size="sm" className={cl("diag-value")}>{value}</Span>
        </div>
    );
}

export function Diagnostics(props: {
    environment: NativeEnvironment | null;
    stats: EngineStats | null;
    stream: StreamStatus;
    warnings: string[];
}) {
    const [open, setOpen] = useState(false);
    const [probeResult, setProbeResult] = useState<string>("");

    const { environment, stats, stream } = props;

    async function runProbe() {
        setProbeResult("checking…");
        const result = await Native.probeSupport();
        setProbeResult([
            result.reason,
            result.detail,
            ...result.checks.map(check => `• ${check}`)
        ].filter(Boolean).join("\n"));
    }

    return (
        <div className={cl("diagnostics")}>
            <button className={cl("diagnostics-toggle")} onClick={() => setOpen(!open)}>
                <Span size="sm" weight="medium">{open ? "Hide diagnostics" : "Diagnostics and details"}</Span>
            </button>

            {open && (
                <div className={cl("diagnostics-body")}>
                    <Row label="Platform" value={environment?.support.os ?? "unknown"} />
                    <Row label="Audio capture" value={stats?.captureBackend ?? "engine not running"} />
                    <Row label="Application list" value={stats?.backend ?? "engine not running"} />
                    <Row label="Output format" value={stats ? `${stats.sampleRate} Hz, ${stats.channels} ch.` : "—"} />
                    <Row label="Native addon" value={environment?.addon.loaded ? `${environment.addon.path} (v${environment.addon.version}, N-API ${environment.addon.napi})` : "not loaded"} />
                    <Row label="Addon folder" value={environment?.dataDir ?? "—"} />

                    {stats && <>
                        <Divider className={cl("divider")} />
                        <Row label="Applications captured" value={String(stats.activeStreams)} />
                        <Row label="Blocks mixed" value={String(stats.blocksMixed)} />
                        <Row label="Starved captures" value={`${stats.starvedFrames} frames`} />
                        <Row label="Buffer overruns" value={`${stats.outputDroppedFrames} frames`} />
                        <Row label="Limiter" value={`${stats.limitedBlocks} blocks`} />
                        <Row label="Overload protection" value={`${stats.clampedSamples} samples`} />
                    </>}

                    <Row label="Share state" value={stream.active ? (stream.filtering ? "filtering active" : "filtering not applied") : "no share"} />
                    <Row
                        label="Audio at the viewers"
                        value={
                            stream.audio === "filtered" ? "filtered"
                                : stream.audio === "unfiltered" ? "plain system audio (unfiltered)"
                                    : stream.audio === "muted" ? "off (filtering could not be applied)"
                                        : "share without audio"
                        }
                    />
                    {stream.audioReason && <Row label="Reason" value={stream.audioReason} />}
                    {stream.active && <Row label="Audio dropouts" value={`${stream.underruns}`} />}

                    {props.warnings.length > 0 && <>
                        <Divider className={cl("divider")} />
                        {props.warnings.map(warning => <Span key={warning} size="sm" className={cl("row-error")}>{warning}</Span>)}
                    </>}

                    {!environment?.addon.loaded && (
                        <div className={cl("diag-hint")}>
                            <Span size="sm">
                                The plugin does not download prebuilt binaries. Build the native addon yourself:
                                {" "}<b>scripts/build-native.ps1</b>, then put the result into the folder below
                                (the button opens it) or set the path in the plugin settings.
                            </Span>
                            <div className={cl("diag-actions")}>
                                <Button size="small" variant="secondary" onClick={() => void Native.openAddonFolder()}>
                                    Open folder
                                </Button>
                                <Button size="small" variant="secondary" onClick={() => void Native.confirmBuildInstructions()}>
                                    Build instructions
                                </Button>
                            </div>
                        </div>
                    )}

                    <Divider className={cl("divider")} />
                    <div className={cl("diag-actions")}>
                        <Button size="small" variant="secondary" onClick={() => void runProbe()}>
                            Check support
                        </Button>
                    </div>
                    {probeResult && <pre className={cl("diag-pre")}>{probeResult}</pre>}
                </div>
            )}
        </div>
    );
}
