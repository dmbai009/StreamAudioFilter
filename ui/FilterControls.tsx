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

export function ShareStatusCard({ state, compact }: { state: StoreState; compact?: boolean; }) {
    const { audio, reason } = state.share;
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
                    share={state.share.audio}
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
