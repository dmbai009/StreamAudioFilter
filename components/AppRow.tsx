/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — one application in the list: icon, name, executable,
 * a status that only claims what is true, and the switch.
 *
 * The switch always means "the viewers may hear this application". The status
 * says "sent" only while the share audio is confirmed to come from the plugin
 * and this application is really part of the mix.
 */

import { Switch } from "@components/Switch";
import { Tooltip } from "@webpack/common";

import { type I18nKey, t } from "../i18n";
import type { AppListEntry, ShareAudio } from "../types";

const cl = (name: string) => `vc-saf-${name}`;

type Tone = "ok" | "warn" | "danger" | "muted" | "neutral";

function statusOf(entry: AppListEntry, share: ShareAudio): { text: string; tone: Tone; hint: string; } {
    const make = (key: I18nKey, tone: Tone, hint = "", params?: Record<string, string>) => ({ text: t(key, params), tone, hint });

    if (entry.section === "closed") return make("state.closed", "muted");
    if (!entry.capturable) return make("state.unsupported", "muted", entry.notCapturableReason);
    if (!entry.allowed) return make("state.excluded", "muted", entry.router && entry.byDefault ? t("row.router") : t("row.switch.off"));
    if (entry.state === "blocked") return make("state.blocked", "danger", entry.blockedReason);
    if (entry.state === "error") return make("state.error", "danger", entry.error);
    if (entry.silentCapture) return make("state.silentCapture", "warn", t("problem.silentCapture", { app: entry.title, device: entry.devices[0] ?? "?" }));
    if (entry.state === "covered") return make("state.covered", "neutral", t("state.covered.hint", { app: entry.coveredBy }), { app: entry.coveredBy });
    if (share === "filtered" && entry.section === "current") {
        return entry.captured
            ? make("state.sending", "ok", t("state.sending.hint"))
            : make("state.waiting", "neutral", t("state.waiting.hint"));
    }
    return make("state.ready", "neutral", t("state.ready.hint"));
}

function Meter({ level }: { level: number; }) {
    return (
        <span className={cl("meter")} aria-hidden="true">
            <span className={cl("meter-fill")} style={{ transform: `scaleX(${Math.max(0, Math.min(1, level))})` }} />
        </span>
    );
}

export function AppRow({ entry, icon, share, onChange }: {
    entry: AppListEntry;
    icon: string | null | undefined;
    share: ShareAudio;
    onChange(allowed: boolean): void;
}) {
    const status = statusOf(entry, share);
    const sub = [entry.exeName, entry.instances > 1 ? t("row.instances", { n: entry.instances }) : ""].filter(Boolean).join(" · ");

    return (
        <div className={cl("row")} data-section={entry.section} data-allowed={entry.allowed ? "true" : "false"}>
            <div className={cl("row-icon")}>
                {icon
                    ? <img src={icon} alt="" width={24} height={24} />
                    : <span className={cl("row-icon-fallback")}>{entry.title.slice(0, 1).toUpperCase()}</span>}
            </div>

            <div className={cl("row-main")}>
                <div className={cl("row-title")}>
                    <span className={cl("row-name")} title={entry.title}>{entry.title}</span>
                    {entry.byDefault && entry.section !== "closed" && (
                        <Tooltip text={t("row.default.hint")}>
                            {props => <span {...props} className={cl("tag")}>{t("row.default")}</span>}
                        </Tooltip>
                    )}
                    {entry.ephemeral && (
                        <Tooltip text={t("row.notSaved.hint")}>
                            {props => <span {...props} className={cl("tag")}>{t("row.notSaved")}</span>}
                        </Tooltip>
                    )}
                </div>
                {sub && <span className={cl("row-sub")} title={entry.exePath || sub}>{sub}</span>}
                {entry.section === "current" && entry.capturable && <Meter level={entry.peak} />}
            </div>

            <Tooltip text={status.hint || status.text}>
                {props => <span {...props} className={cl("chip")} data-tone={status.tone}>{status.text}</span>}
            </Tooltip>

            <Tooltip text={entry.allowed ? t("row.switch.on") : t("row.switch.off")}>
                {props => (
                    <span {...props} className={cl("row-switch")}>
                        <Switch checked={entry.allowed} onChange={onChange} disabled={!entry.capturable && entry.section !== "closed"} />
                    </span>
                )}
            </Tooltip>
        </div>
    );
}
