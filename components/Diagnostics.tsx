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
        setProbeResult("проверка…");
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
                <Span size="sm" weight="medium">{open ? "Скрыть диагностику" : "Диагностика и подробности"}</Span>
            </button>

            {open && (
                <div className={cl("diagnostics-body")}>
                    <Row label="Платформа" value={environment?.support.os ?? "неизвестно"} />
                    <Row label="Захват звука" value={stats?.captureBackend ?? "движок не запущен"} />
                    <Row label="Список приложений" value={stats?.backend ?? "движок не запущен"} />
                    <Row label="Формат вывода" value={stats ? `${stats.sampleRate} Гц, ${stats.channels} кан.` : "—"} />
                    <Row label="Нативный модуль" value={environment?.addon.loaded ? `${environment.addon.path} (v${environment.addon.version}, N-API ${environment.addon.napi})` : "не загружен"} />
                    <Row label="Каталог для модуля" value={environment?.dataDir ?? "—"} />

                    {stats && <>
                        <Divider className={cl("divider")} />
                        <Row label="Захватывается приложений" value={String(stats.activeStreams)} />
                        <Row label="Микшировано блоков" value={String(stats.blocksMixed)} />
                        <Row label="Простой захватов" value={`${stats.starvedFrames} кадров`} />
                        <Row label="Переполнение буфера" value={`${stats.outputDroppedFrames} кадров`} />
                        <Row label="Ограничитель" value={`${stats.limitedBlocks} блоков`} />
                        <Row label="Защита от перегрузки" value={`${stats.clampedSamples} сэмплов`} />
                    </>}

                    <Row label="Состояние трансляции" value={stream.active ? (stream.filtering ? "фильтрация активна" : "фильтрация не применена") : "нет трансляции"} />
                    {stream.active && <Row label="Пропуски звука" value={`${stream.underruns}`} />}

                    {props.warnings.length > 0 && <>
                        <Divider className={cl("divider")} />
                        {props.warnings.map(warning => <Span key={warning} size="sm" className={cl("row-error")}>{warning}</Span>)}
                    </>}

                    {!environment?.addon.loaded && (
                        <div className={cl("diag-hint")}>
                            <Span size="sm">
                                Плагин не скачивает готовые бинарные файлы. Соберите нативный модуль сами:
                                {" "}<b>scripts/build-native.ps1</b>, затем положите результат в указанный каталог
                                (кнопка ниже откроет его) или укажите путь в настройках плагина.
                            </Span>
                            <div className={cl("diag-actions")}>
                                <Button size="small" variant="secondary" onClick={() => void Native.openAddonFolder()}>
                                    Открыть каталог
                                </Button>
                                <Button size="small" variant="secondary" onClick={() => void Native.confirmBuildInstructions()}>
                                    Инструкция по сборке
                                </Button>
                            </div>
                        </div>
                    )}

                    <Divider className={cl("divider")} />
                    <div className={cl("diag-actions")}>
                        <Button size="small" variant="secondary" onClick={() => void runProbe()}>
                            Проверить поддержку
                        </Button>
                    </div>
                    {probeResult && <pre className={cl("diag-pre")}>{probeResult}</pre>}
                </div>
            )}
        </div>
    );
}
