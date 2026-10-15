/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * One application in the list: icon, name, executable, audio activity and the
 * switch that decides whether the viewers hear it.
 */

import { Span } from "@components/Span";
import { Switch } from "@components/Switch";
import { Tooltip } from "@webpack/common";

import type { AppListEntry } from "../types";

const cl = (name: string) => `vc-saf-${name}`;

function ActivityMeter({ peak, active }: { peak: number; active: boolean; }) {
    const level = Math.max(0, Math.min(1, peak));
    const bars = 4;
    return (
        <div className={cl("meter")} aria-hidden="true">
            {Array.from({ length: bars }, (_, index) => (
                <i
                    key={index}
                    className={cl("meter-bar")}
                    data-on={active && level > (index + 1) / (bars + 1) ? "true" : "false"}
                />
            ))}
        </div>
    );
}

function StatusChip({ entry }: { entry: AppListEntry; }) {
    const state = !entry.capturable
        ? { text: "unavailable", tone: "danger" }
        : entry.error
            ? { text: "error", tone: "danger" }
            : entry.state === "excluded"
                ? { text: "excluded", tone: "muted" }
                : entry.section === "closed"
                    ? { text: "closed", tone: "muted" }
                    : entry.transmitting && entry.state === "transmitting"
                        ? { text: "sent", tone: "ok" }
                        : entry.transmitting
                            ? { text: "waiting for audio", tone: "warn" }
                            : { text: "no audio", tone: "muted" };

    return <span className={cl("chip")} data-tone={state.tone}>{state.text}</span>;
}

export function AppRow(props: {
    entry: AppListEntry;
    icon: string | null | undefined;
    onChange(transmit: boolean): void;
    onForget?(): void;
}) {
    const { entry, icon } = props;

    const title = (
        <div className={cl("row-title")}>
            <span className={cl("row-name")}>{entry.title}</span>
            {entry.ephemeral && (
                <Tooltip text="A rule for this process cannot be saved: the application does not report the path of its executable">
                    {props => <span className={cl("row-badge")} {...props}>not saved</span>}
                </Tooltip>
            )}
        </div>
    );

    return (
        <div className={cl("row")} data-state={entry.state}>
            <div className={cl("row-icon")}>
                {icon
                    ? <img src={icon} alt="" width={28} height={28} />
                    : <div className={cl("row-icon-fallback")}>{entry.title.slice(0, 1).toUpperCase()}</div>}
            </div>

            <div className={cl("row-main")}>
                {title}
                <div className={cl("row-sub")}>
                    <Span size="sm" className={cl("row-subtitle")}>{entry.subtitle}</Span>
                    {entry.details && <Span size="sm" className={cl("row-details")}>{entry.details}</Span>}
                </div>
                {entry.error && <Span size="sm" className={cl("row-error")}>{entry.error}</Span>}
            </div>

            <div className={cl("row-side")}>
                <ActivityMeter peak={entry.peak} active={entry.section === "current" && entry.capturable} />
                <StatusChip entry={entry} />
                <Tooltip text={entry.transmitting ? "The viewers hear this application" : "The viewers do not hear this application"}>
                    {tooltipProps => (
                        <span {...tooltipProps}>
                            <Switch checked={entry.transmitting} onChange={props.onChange} disabled={!entry.capturable && entry.section !== "closed"} />
                        </span>
                    )}
                </Tooltip>
            </div>
        </div>
    );
}
