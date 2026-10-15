/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * The controls every entry point shows: mode, application list, counters and
 * problems. The settings page, the popout next to the microphone buttons and
 * the floating panel all render this one component, so the state can never
 * diverge between them.
 */

import { Button } from "@components/Button";
import { Card } from "@components/Card";
import { Divider } from "@components/Divider";
import { Flex } from "@components/Flex";
import { Span } from "@components/Span";
import { Tooltip } from "@webpack/common";

import { store, useStoreState } from "../apps/store";
import { AppRow } from "../components/AppRow";
import type { AppListEntry } from "../types";

const cl = (name: string) => `vc-saf-${name}`;

function statusLine(state: ReturnType<typeof useStoreState>): string {
    if (!state.environment) return "Checking the system…";
    if (!state.environment.addon.loaded) return "The native addon was not found";

    // The audio state is what the viewers get, which is what the user cares
    // about — and the only state that may not lie about filtering.
    if (state.stream.audio === "muted") return "Stream audio is off: filtering could not be applied";
    if (state.stream.audio === "unfiltered" && state.stream.active) return "Sending unfiltered system audio";
    if (state.stream.audio === "filtered") {
        const captured = state.apps.filter(app => app.captured).length;
        return `Sharing: audio ${captured === 0 ? "not captured yet" : `sent for ${captured} apps`}`;
    }

    if (state.stream.filtering) {
        const captured = state.apps.filter(app => app.captured).length;
        return `Sharing: audio ${captured === 0 ? "not captured yet" : `sent for ${captured} apps`}`;
    }
    if (state.stream.active) return "Sharing, but filtering was not applied";

    if (!state.environment.support.supported) return "Filtering is not available on this system";
    if (!state.engineRunning) return "Ready";
    return "Watching applications";
}

function ModeSelector() {
    const { rules } = useStoreState();

    return (
        <Flex flexDirection="row" gap="0.5em" className={cl("modes")}>
            <Tooltip text="Audio is sent for every application except the ones you exclude. New applications are included automatically.">
                {props => (
                    <Button
                        {...props}
                        size="small"
                        variant={rules.mode === "allExcept" ? "primary" : "secondary"}
                        onClick={() => void store.setMode("allExcept")}
                    >
                        Everything except excluded
                    </Button>
                )}
            </Tooltip>
            <Tooltip text="Only the applications you include are sent. New applications stay silent until you allow them.">
                {props => (
                    <Button
                        {...props}
                        size="small"
                        variant={rules.mode === "onlySelected" ? "primary" : "secondary"}
                        onClick={() => void store.setMode("onlySelected")}
                    >
                        Only included
                    </Button>
                )}
            </Tooltip>
        </Flex>
    );
}

export function FilterControls({ compact = false }: { compact?: boolean; }) {
    const state = useStoreState();
    const entries = store.entries();
    const active = entries.filter(entry => entry.section === "current");
    const idle = entries.filter(entry => entry.section === "idle");
    const closed = entries.filter(entry => entry.section === "closed");

    const problems = [
        state.lastError,
        state.lastWarning,
        ...(state.stats?.warnings ?? []),
        ...state.apps.filter(app => app.error).map(app => `${app.displayName}: ${app.error}`)
    ].filter(Boolean);

    return (
        <div className={cl("controls")} data-compact={compact ? "true" : "false"}>
            <Flex flexDirection="row" gap="0.6em" alignItems="center" justifyContent="space-between">
                <Flex flexDirection="column" gap="0.15em">
                    <Span size="md" weight="semibold">{statusLine(state)}</Span>
                    <Span size="xs" className={cl("muted")}>
                        {state.environment?.support.os ?? "detecting the system…"}
                        {state.stats ? ` · capture: ${state.stats.captureBackend || state.stats.backend} · ${state.stats.sampleRate} Hz` : ""}
                        {state.stream.active ? ` · buffer delay ~${state.stream.pipelinedMs} ms` : ""}
                    </Span>
                </Flex>
                {state.loading && <Span size="xs" className={cl("muted")}>refreshing…</Span>}
            </Flex>

            <div className={cl("toolbar")}>
                <ModeSelector />
                <Flex flexDirection="row" gap="0.4em">
                    <Button size="small" variant="secondary" onClick={() => void store.refresh(true)}>
                        Refresh list
                    </Button>
                    <Button size="small" variant="secondary" onClick={() => void store.resetFilters()}>
                        Reset filters
                    </Button>
                </Flex>
            </div>

            {state.environment && !state.environment.addon.loaded && (
                <Card variant="danger">
                    <Span size="sm">
                        The native addon is not loaded, so the audio is not filtered. The details and the
                        "Open folder" button are in the diagnostics section below.
                    </Span>
                </Card>
            )}

            {state.environment && !state.environment.support.supported && state.environment.addon.loaded && (
                <Card variant="warning">
                    <Flex flexDirection="column" gap="0.25em">
                        <Span size="sm">{state.environment.support.reason}</Span>
                        {state.environment.support.detail && <Span size="xs">{state.environment.support.detail}</Span>}
                    </Flex>
                </Card>
            )}

            {state.stream.audio === "muted" && (
                <Card variant="danger">
                    <Flex flexDirection="column" gap="0.25em">
                        <Span size="sm" weight="semibold">
                            The viewers do not hear the screen share audio: filtering could not be applied.
                        </Span>
                        {state.stream.audioReason && <Span size="xs">{state.stream.audioReason}</Span>}
                        <Span size="xs">
                            The plugin never falls back to unfiltered audio on its own: that would make the excluded applications audible.
                            The setting "Send plain system audio when filtering is unavailable" restores
                            the normal behaviour of Discord.
                        </Span>
                    </Flex>
                </Card>
            )}

            {state.stream.audio === "unfiltered" && state.stream.active && (
                <Card variant="warning">
                    <Flex flexDirection="column" gap="0.25em">
                        <Span size="sm" weight="semibold">Unfiltered system audio is being sent.</Span>
                        {state.stream.audioReason && <Span size="xs">{state.stream.audioReason}</Span>}
                    </Flex>
                </Card>
            )}

            {problems.length > 0 && (
                <Card variant="warning">
                    <Flex flexDirection="column" gap="0.25em">
                        {problems.map(problem => <Span key={problem} size="xs">{problem}</Span>)}
                    </Flex>
                </Card>
            )}

            <ListSection
                title="Currently playing audio"
                entries={active}
                icons={state.icons}
                empty="Nothing is playing audio yet."
            />
            {idle.length > 0 && (
                <ListSection
                    title="Running, but silent"
                    entries={idle}
                    icons={state.icons}
                    empty=""
                />
            )}
            {closed.length > 0 && (
                <ListSection
                    title="Saved rules (application closed)"
                    entries={closed}
                    icons={state.icons}
                    empty=""
                />
            )}

            <div className={cl("hint")}>
                <Span size="xs">
                    Cannot see the application you need? Start playing audio in it, then refresh the list.
                </Span>
                <Span size="xs">
                    The setting only affects what the viewers hear: your own audio stays as it is.
                    Applications whose audio cannot be captured are marked as "unavailable".
                </Span>
            </div>
        </div>
    );
}

function ListSection(props: { title: string; entries: AppListEntry[]; icons: Record<string, string | null>; empty: string; }) {
    if (props.entries.length === 0) {
        return (
            <div className={cl("section")}>
                <Span size="xs" className={cl("section-title")}>{props.title}</Span>
                <Span size="xs" className={cl("muted")}>{props.empty}</Span>
            </div>
        );
    }

    return (
        <div className={cl("section")}>
            <Flex flexDirection="row" gap="0.5em" alignItems="center">
                <Span size="xs" className={cl("section-title")}>{props.title}</Span>
                <Divider className={cl("section-divider")} />
                <Span size="xs" className={cl("muted")}>{props.entries.length}</Span>
            </Flex>
            {props.entries.map(entry => (
                <AppRow
                    key={entry.appKey}
                    entry={entry}
                    icon={entry.exePath ? props.icons[entry.exePath] : null}
                    onChange={transmit => void store.setTransmit({ appKey: entry.appKey, displayName: entry.title, exePath: entry.exePath }, transmit)}
                />
            ))}
        </div>
    );
}
