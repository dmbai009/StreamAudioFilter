/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — technical details for when something does not work,
 * and a one click report that contains everything needed to diagnose it.
 * Collapsed by default; nothing here is needed for normal use. The report is
 * only copied to the clipboard: nothing is sent anywhere.
 */

import { Button } from "@components/Button";
import { copyWithToast } from "@utils/discord";
import { useState } from "@webpack/common";

import { store, type StoreState } from "../apps/store";
import { t } from "../i18n";
import { Native } from "../nativeBridge";

const cl = (name: string) => `vc-saf-${name}`;

function Row({ label, value }: { label: string; value: string; }) {
    return (
        <div className={cl("diag-row")}>
            <span className={cl("diag-label")}>{label}</span>
            <span className={cl("diag-value")}>{value || "—"}</span>
        </div>
    );
}

export function Diagnostics({ state }: { state: StoreState; }) {
    const [open, setOpen] = useState(false);
    const [probe, setProbe] = useState("");
    const env = state.environment;
    const engine = state.stats?.engine;
    const output = state.stats?.output;

    async function runProbe() {
        setProbe("…");
        const result = await Native.probeSupport();
        setProbe(result ? [result.reason, result.detail, ...result.checks.map(check => `• ${check}`)].filter(Boolean).join("\n") : "—");
    }

    return (
        <div className={cl("diagnostics")}>
            <div className={cl("diag-head")}>
                <button className={cl("disclosure")} onClick={() => setOpen(!open)} aria-expanded={open}>
                    {open ? "▾" : "▸"} {t("diag.title")}
                </button>
                <Button size="small" variant="secondary" onClick={() => void copyWithToast(JSON.stringify(store.buildReport(), null, 2), t("toast.copied"))}>
                    {t("action.copyReport")}
                </Button>
            </div>

            {open && (
                <div className={cl("diag-body")}>
                    <Row label={t("diag.version")} value={`plugin ${env?.pluginVersion ?? "?"} · host ${env?.host.version || "—"} · protocol ${env?.host.protocol || "—"}/${env?.pluginProtocol ?? "?"}`} />
                    <Row label={t("diag.platform")} value={env?.support.os ?? ""} />
                    <Row label={t("diag.host")} value={env ? (env.host.running ? `pid ${env.host.pid} · ${env.host.path}` : env.host.error) : ""} />
                    <Row label={t("diag.patch")} value={`soundshare pid: ${state.patch.pidHook ? "ok" : "—"} · capture mode: ${state.patch.modeHook ? "ok" : "—"}`} />
                    <Row label={t("diag.share")} value={`${state.share.audio}${state.share.reason ? ` (${state.share.reason})` : ""}`} />
                    <Row label={t("diag.capture")} value={engine ? `${engine.captureBackend} · ${engine.activeStreams} app(s) · ${engine.sampleRate} Hz` : ""} />
                    <Row
                        label={t("diag.output")}
                        value={output ? `${output.running ? "running" : "stopped"} · ${output.device} · ${output.format} · muted: ${output.sessionMuted} · Discord attached: ${output.discordAttached} · level ${output.peak.toFixed(3)}` : ""}
                    />
                    <Row
                        label={t("diag.counters")}
                        value={engine ? `starved ${engine.starvedFrames} · limiter ${engine.limitedBlocks} · clamped ${engine.clampedSamples} · output restarts ${output?.restarts ?? 0} · re-mutes ${output?.remutes ?? 0}` : ""}
                    />
                    <div className={cl("diag-actions")}>
                        <Button size="small" variant="secondary" onClick={() => void runProbe()}>{t("action.checkSupport")}</Button>
                        <Button size="small" variant="secondary" onClick={() => void store.restartHost()}>{t("action.restartHost")}</Button>
                        <Button size="small" variant="secondary" onClick={() => void Native.openHostFolder()}>{t("action.openFolder")}</Button>
                    </div>
                    {probe && <pre className={cl("diag-pre")}>{probe}</pre>}
                </div>
            )}
        </div>
    );
}
