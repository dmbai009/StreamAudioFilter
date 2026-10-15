/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Plugin settings as shown in Vencord's plugin card. Only things a user may
 * actually want to change live here; the technical state (which applications
 * are transmitted) is kept with the rules and is edited in the panel.
 */

import { definePluginSettings } from "@api/Settings";
import { OptionType } from "@utils/types";

export const settings = definePluginSettings({
    filterEnabled: {
        type: OptionType.BOOLEAN,
        description: "Фильтровать звук демонстрации экрана (иначе передаётся обычный системный звук)",
        default: true
    },
    showAccountPanelButton: {
        type: OptionType.BOOLEAN,
        description: "Кнопка «Звук трансляции» рядом с кнопками микрофона внизу окна",
        default: true
    },
    showFloatingPanel: {
        type: OptionType.BOOLEAN,
        description: "Плавающая панель со списком приложений",
        default: true
    },
    collapseDuringStream: {
        type: OptionType.BOOLEAN,
        description: "Сворачивать плавающую панель, пока идёт трансляция",
        default: true
    },
    bufferMs: {
        type: OptionType.SLIDER,
        description: "Запас звукового буфера. Больше — устойчивее, меньше — ниже задержка",
        markers: [30, 45, 60, 90, 120, 180],
        default: 60,
        stickToMarkers: false
    },
    maxStreams: {
        type: OptionType.SLIDER,
        description: "Сколько приложений захватывать одновременно",
        markers: [4, 8, 12, 16, 24, 32],
        default: 12,
        stickToMarkers: true
    },
    nativeModulePath: {
        type: OptionType.STRING,
        description: "Путь к streamaudiofilter.node, если модуль собран в нестандартном месте",
        default: "",
        placeholder: "C:\\путь\\к\\streamaudiofilter.node"
    },
    testBackend: {
        type: OptionType.BOOLEAN,
        description: "[Проверка] Тестовый режим: вместо захвата звука используется генератор сигнала",
        default: false
    }
});
