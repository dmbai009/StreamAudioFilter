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
    if (!state.environment) return "Проверка системы…";
    if (!state.environment.addon.loaded) return "Нативный модуль не найден";

    if (state.stream.filtering) {
        const captured = state.apps.filter(app => app.captured).length;
        return `Трансляция идёт: звук ${captured === 0 ? "ещё не захвачен" : `передаётся для ${captured} прил.`}`;
    }
    if (state.stream.active) return "Трансляция активна, фильтрация не применена";

    if (!state.environment.support.supported) return "Фильтрация недоступна в этой системе";
    if (!state.engineRunning) return "Готов к работе";
    return "Наблюдение за приложениями";
}

function ModeSelector() {
    const { rules } = useStoreState();

    return (
        <Flex flexDirection="row" gap="0.5em" className={cl("modes")}>
            <Tooltip text="Звук передаётся всем приложениям, кроме выключенных вами. Новые приложения включаются автоматически.">
                {props => (
                    <Button
                        {...props}
                        size="small"
                        variant={rules.mode === "allExcept" ? "primary" : "secondary"}
                        onClick={() => void store.setMode("allExcept")}
                    >
                        Всё, кроме выбранного
                    </Button>
                )}
            </Tooltip>
            <Tooltip text="Передаётся только то, что вы включили вручную. Новые приложения молчат, пока их не разрешат.">
                {props => (
                    <Button
                        {...props}
                        size="small"
                        variant={rules.mode === "onlySelected" ? "primary" : "secondary"}
                        onClick={() => void store.setMode("onlySelected")}
                    >
                        Только выбранные
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
                        {state.environment?.support.os ?? "определение системы…"}
                        {state.stats ? ` · захват: ${state.stats.captureBackend || state.stats.backend} · ${state.stats.sampleRate} Гц` : ""}
                        {state.stream.active ? ` · задержка буфера ~${state.stream.pipelinedMs} мс` : ""}
                    </Span>
                </Flex>
                {state.loading && <Span size="xs" className={cl("muted")}>обновление…</Span>}
            </Flex>

            <div className={cl("toolbar")}>
                <ModeSelector />
                <Flex flexDirection="row" gap="0.4em">
                    <Button size="small" variant="secondary" onClick={() => void store.refresh(true)}>
                        Обновить список
                    </Button>
                    <Button size="small" variant="secondary" onClick={() => void store.resetFilters()}>
                        Сбросить фильтры
                    </Button>
                </Flex>
            </div>

            {state.environment && !state.environment.addon.loaded && (
                <Card variant="danger">
                    <Span size="sm">
                        Нативный модуль не загружен, поэтому звук не фильтруется. Подробности и кнопка
                        «Открыть каталог» — в разделе «Диагностика» ниже.
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

            {problems.length > 0 && (
                <Card variant="warning">
                    <Flex flexDirection="column" gap="0.25em">
                        {problems.map(problem => <Span key={problem} size="xs">{problem}</Span>)}
                    </Flex>
                </Card>
            )}

            <ListSection
                title="Сейчас воспроизводят звук"
                entries={active}
                icons={state.icons}
                empty="Пока ничего не воспроизводит звук."
            />
            {idle.length > 0 && (
                <ListSection
                    title="Запущены, но молчат"
                    entries={idle}
                    icons={state.icons}
                    empty=""
                />
            )}
            {closed.length > 0 && (
                <ListSection
                    title="Сохранённые правила (приложение закрыто)"
                    entries={closed}
                    icons={state.icons}
                    empty=""
                />
            )}

            <div className={cl("hint")}>
                <Span size="xs">
                    Не видишь нужное приложение? Запусти в нём воспроизведение звука и обнови список.
                </Span>
                <Span size="xs">
                    Настройка действует только на то, что слышат зрители: ваш собственный звук не меняется.
                    Приложения, звук которых невозможно перехватить, помечены как «недоступно».
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
