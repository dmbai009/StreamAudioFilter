/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — the controls every entry point shows: one status line
 * about what the viewers hear, a short problem summary with details on
 * demand, the default for new applications, and the application list.
 */

import { Button } from "@components/Button";
import { useState } from "@webpack/common";

import { entriesOf, problemsOf, store, type StoreState, useStoreState } from "../apps/store";
import { AppRow } from "../components/AppRow";
import { type I18nKey, t } from "../i18n";
import type { AppListEntry, ShareAudio } from "../types";
import { Icon } from "./icons";

const cl = (name: string) => `vc-saf-${name}`;

const SHARE_TONE: Record<ShareAudio, "ok" | "info" | "neutral" | "warn" | "danger"> = {
    idle: "neutral",
    noAudio: "neutral",
    window: "neutral",
    off: "warn",
    starting: "info",
    filtered: "ok",
    attachFailed: "danger",
    muted: "warn",
    unfiltered: "danger",
    uncontrolled: "danger"
};

/**
 * "filtered" says Discord reads the plugin's audio. If an allowed application
 * is playing but its capture is pure silence, the viewers do not hear it: the
 * headline must not claim otherwise.
 */
function silentAllowedApps(state: StoreState) {
    return state.apps.filter(app => app.playing && app.captured && app.silentCapture).map(app => app.displayName);
}

export function ShareStatusCard({ state, compact }: { state: StoreState; compact?: boolean; }) {
    const { audio, reason } = state.share;
    if (state.masterMuted && audio === "filtered") {
        return (
            <div className={cl("status")} data-tone="warn" role="status">
                <div className={cl("status-title")}>{t("mute.active")}</div>
                <div className={cl("status-detail")}>{t("mute.detail")}</div>
            </div>
        );
    }
    const silent = audio === "filtered" ? silentAllowedApps(state) : [];
    if (silent.length) {
        return (
            <div className={cl("status")} data-tone="warn" role="status">
                <div className={cl("status-title")}>{t("share.filteredSilent")}</div>
                <div className={cl("status-detail")}>{t("share.filteredSilent.detail", { apps: silent.join(", ") })}</div>
            </div>
        );
    }
    return (
        <div className={cl("status")} data-tone={SHARE_TONE[audio]} role="status">
            <div className={cl("status-title")}>{t(`share.${audio}` as I18nKey)}</div>
            {!compact || SHARE_TONE[audio] === "danger" || SHARE_TONE[audio] === "warn"
                ? <div className={cl("status-detail")}>{t(`share.${audio}.detail` as I18nKey)}</div>
                : null}
            {reason && SHARE_TONE[audio] !== "ok" && <div className={cl("status-reason")}>{reason}</div>}
        </div>
    );
}

export function ProblemsSummary({ state }: { state: StoreState; }) {
    const [open, setOpen] = useState(false);
    const problems = problemsOf(state);
    if (!problems.length) return null;
    const worst = problems.some(p => p.level === "error") ? "danger" : problems.some(p => p.level === "warning") ? "warn" : "info";
    return (
        <div className={cl("problems")} data-tone={worst}>
            <button className={cl("problems-toggle")} onClick={() => setOpen(!open)} aria-expanded={open}>
                <span>{t("problems.title", { n: problems.length })}</span>
                <span className={cl("problems-action")}>{open ? t("problems.hide") : t("problems.show")}</span>
            </button>
            {open && (
                <ul className={cl("problems-list")}>
                    {problems.map(problem => <li key={problem.key} data-level={problem.level}>{problem.text}</li>)}
                </ul>
            )}
        </div>
    );
}

function ModeSelector({ mode }: { mode: StoreState["rules"]["mode"]; }) {
    const option = (value: typeof mode, label: I18nKey, hint: I18nKey) => (
        <button
            className={cl("segment")}
            data-selected={mode === value ? "true" : "false"}
            aria-pressed={mode === value}
            title={t(hint)}
            onClick={() => void store.setMode(value)}
        >
            {t(label)}
        </button>
    );
    return (
        <div className={cl("mode")}>
            <span className={cl("mode-label")}>{t("mode.label")}</span>
            <div className={cl("segments")} role="group">
                {option("allExcept", "mode.allExcept", "mode.allExcept.hint")}
                {option("onlySelected", "mode.onlySelected", "mode.onlySelected.hint")}
            </div>
        </div>
    );
}

function ListSection({ title, entries, state }: { title: string; entries: AppListEntry[]; state: StoreState; }) {
    if (!entries.length) return null;
    return (
        <section className={cl("section")}>
            <div className={cl("section-title")}>
                <span>{title}</span>
                <span className={cl("section-count")}>{entries.length}</span>
            </div>
            {entries.map(entry => (
                <AppRow
                    key={entry.appKey}
                    entry={entry}
                    icon={entry.exePath ? state.icons[entry.exePath] : null}
                    share={state.masterMuted && state.share.audio === "filtered" ? "noAudio" : state.share.audio}
                    onChange={allowed => void store.setAllowed(entry, allowed)}
                />
            ))}
        </section>
    );
}

export function FilterControls({ compact = false }: { compact?: boolean; }) {
    const state = useStoreState();
    const entries = entriesOf(state);
    const current = entries.filter(entry => entry.section === "current");
    const idle = entries.filter(entry => entry.section === "idle");
    const closed = entries.filter(entry => entry.section === "closed");

    return (
        <div className={cl("controls")} data-compact={compact ? "true" : "false"}>
            <button
                className={cl("master-mute")}
                aria-pressed={state.masterMuted}
                disabled={state.mutePending}
                title={t("mute.hint")}
                onClick={() => void store.toggleMasterMuted()}
            >
                <Icon kind={state.masterMuted ? "muted" : "off"} size={18} />
                <span>{t(state.masterMuted ? "mute.restore" : "mute.action")}</span>
            </button>
            {state.masterMuted && ["window", "off", "unfiltered", "uncontrolled"].includes(state.share.audio)
                && <div className={cl("hint")} role="status">{t("mute.bypass")}</div>}
            <ShareStatusCard state={state} compact={compact} />
            <ProblemsSummary state={state} />
            <ModeSelector mode={state.rules.mode} />

            <div className={cl("list")}>
                {current.length === 0 && <div className={cl("empty")}>{t("list.empty")}</div>}
                <ListSection title={t("list.playing")} entries={current} state={state} />
                <ListSection title={t("list.idle")} entries={idle} state={state} />
                {!compact && <ListSection title={t("list.closed")} entries={closed} state={state} />}
            </div>

            <div className={cl("footer")}>
                <span className={cl("hint")}>{t("list.hint")}</span>
                <div className={cl("footer-actions")}>
                    <Button size="small" variant="secondary" onClick={() => void store.refresh(true)}>{t("action.refresh")}</Button>
                    {!compact && <Button size="small" variant="secondary" onClick={() => void store.resetRules()}>{t("action.reset")}</Button>}
                </div>
            </div>
        </div>
    );
}
