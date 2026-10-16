/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — interface texts in English and Russian.
 * The language follows Discord's (the lang attribute Discord sets on <html>).
 * Technical names (WASAPI, error codes, file names) are never translated.
 */

const en = {
    title: "Stream audio",
    tagline: "Choose which applications the viewers of your screen share hear. You keep hearing everything; the microphone and the voice call are not affected.",

    // share states: headline + explanation
    "share.idle": "Not sharing",
    "share.idle.detail": "Rules apply as soon as you share your whole screen with audio.",
    "share.noAudio": "Sharing without audio",
    "share.noAudio.detail": "Discord's “Sound” switch is off for this share, so the viewers hear nothing.",
    "share.window": "Sharing a window",
    "share.window.detail": "Discord sends only the audio of the shared window's application. These rules apply to whole screen shares.",
    "share.off": "Filtering is off",
    "share.off.detail": "Filtering is switched off in the plugin settings: Discord sends all system audio.",
    "share.starting": "Connecting the filtered audio…",
    "share.starting.detail": "Discord is being pointed at the filtered audio. This takes a second or two.",
    "share.filtered": "Viewers hear only the allowed apps",
    "share.filtered.detail": "Discord reads the filtered audio. Changes below apply immediately.",
    "share.filteredSilent": "Viewers do not hear some allowed apps",
    "share.filteredSilent.detail": "Discord reads the filtered audio, but Windows delivers only silence for: {apps}. See the problems below.",
    "share.attachFailed": "Discord did not take the filtered audio",
    "share.attachFailed.detail": "The share was pointed at the filtered audio, but Discord did not connect to it: the viewers most likely hear no audio. Restart the share; if it keeps happening, copy the diagnostic report.",
    "share.muted": "Share audio is off",
    "share.muted.detail": "Filtering could not be applied, so no audio is sent at all — not even the excluded apps. Either fix the problem below, or allow plain system audio in the settings.",
    "share.unfiltered": "Viewers hear ALL apps",
    "share.unfiltered.detail": "Filtering could not be applied and plain system audio is sent, because “Send unfiltered audio when filtering fails” is on.",
    "share.uncontrolled": "Audio not controlled by the plugin",
    "share.uncontrolled.detail": "This share started without passing through the plugin (Discord may have changed). The viewers may hear every application. Turn the share's sound off if that matters, and copy the diagnostic report.",

    // toolbar / modes
    "mode.label": "Default for apps without a rule",
    "mode.allExcept": "Allowed",
    "mode.allExcept.hint": "New applications are sent unless you switch them off.",
    "mode.onlySelected": "Blocked",
    "mode.onlySelected.hint": "New applications stay silent until you switch them on.",
    "action.refresh": "Refresh",
    "action.reset": "Reset rules",
    "action.copyReport": "Copy diagnostic report",
    "action.openSettings": "All settings",
    "action.collapse": "Collapse",
    "action.expand": "Expand",
    "action.close": "Close",
    "action.checkSupport": "Check support",
    "action.restartHost": "Restart audio host",
    "action.openFolder": "Open folder",
    "toast.copied": "Diagnostic report copied",

    // list
    "list.playing": "Playing now",
    "list.idle": "Running, silent",
    "list.closed": "Saved rules",
    "list.empty": "No application is playing audio.",
    "list.hint": "Not in the list? Start playing audio in the application.",
    "row.switch.on": "Viewers may hear this application",
    "row.switch.off": "Viewers do not hear this application",
    "row.default": "default",
    "row.router": "Re-plays the audio of other applications. Sending it would send them too, including excluded ones, so it is blocked unless you allow it.",
    "row.default.hint": "No rule saved: follows the default above",
    "row.instances": "{n} instances",
    "row.notSaved": "rule not saved",
    "row.notSaved.hint": "The executable of this application is unknown, so a rule can not be remembered after it closes.",

    // row states (only claims that are true)
    "state.sending": "sent",
    "state.sending.hint": "Captured and part of the audio the viewers receive",
    "state.ready": "allowed",
    "state.ready.hint": "Will be sent when you share your screen",
    "state.excluded": "blocked",
    "state.covered": "via {app}",
    "state.covered.hint": "Already contained in the capture of {app}",
    "state.blocked": "conflict",
    "state.silentCapture": "no audio",
    "state.unsupported": "unavailable",
    "state.error": "error",
    "state.closed": "closed",
    "state.waiting": "waiting",
    "state.waiting.hint": "Allowed, but not captured yet",

    // problems
    "problems.title": "{n} problem(s)",
    "problems.show": "Show details",
    "problems.hide": "Hide details",
    "problem.hostMissing": "The audio host (saf-audio-host.exe) was not found",
    "problem.hostFailed": "The audio host is not running",
    "problem.unsupported": "Per application capture is not available on this system",
    "problem.silentCapture": "Windows delivers only silence for {app} (it plays through “{device}”), so viewers do not hear it. Check the application's volume in the Windows volume mixer; if it is fine, restart the share. Copy the diagnostic report if it stays silent.",
    "problem.blocked": "{app} is not sent: {reason}",
    "problem.patch": "The plugin could not hook into Discord's screen share code (Discord changed). Shares are not filtered.",
    "problem.sessionUnmuted": "The muted audio session of the plugin was unmuted in the Windows volume mixer and has been muted again (you would hear everything twice).",

    // diagnostics
    "diag.title": "Diagnostics",
    "diag.host": "Audio host",
    "diag.version": "Versions",
    "diag.platform": "System",
    "diag.capture": "Capture",
    "diag.output": "Output to Discord",
    "diag.share": "Share",
    "diag.counters": "Counters",
    "diag.patch": "Discord hooks",

    // settings page extras
    "settings.mutedNote": "In the Windows volume mixer the plugin appears as a muted session (“StreamAudioFilter: share audio”). Leave it muted: Discord reads it before the mute.",
    "floating.noShare": "not sharing",
    "button.tooltip": "Stream audio: choose what the viewers hear"
};

type Key = keyof typeof en;

const ru: Record<Key, string> = {
    title: "Звук трансляции",
    tagline: "Выберите, какие приложения слышат зрители вашей демонстрации экрана. Вы по-прежнему слышите всё; микрофон и голосовой канал не затрагиваются.",

    "share.idle": "Трансляция не идёт",
    "share.idle.detail": "Правила применяются, когда вы демонстрируете весь экран со звуком.",
    "share.noAudio": "Трансляция без звука",
    "share.noAudio.detail": "Для этой трансляции в Discord выключен «Звук», зрители ничего не слышат.",
    "share.window": "Трансляция окна",
    "share.window.detail": "Discord передаёт только звук приложения, чьё окно транслируется. Эти правила действуют для трансляции всего экрана.",
    "share.off": "Фильтрация выключена",
    "share.off.detail": "Фильтрация выключена в настройках плагина: Discord передаёт весь системный звук.",
    "share.starting": "Подключение отфильтрованного звука…",
    "share.starting.detail": "Discord переключается на отфильтрованный звук. Это занимает секунду-две.",
    "share.filtered": "Зрители слышат только разрешённые приложения",
    "share.filtered.detail": "Discord получает отфильтрованный звук. Изменения ниже применяются сразу.",
    "share.filteredSilent": "Зрители не слышат часть разрешённых приложений",
    "share.filteredSilent.detail": "Discord получает отфильтрованный звук, но Windows отдаёт только тишину для: {apps}. Подробности — в проблемах ниже.",
    "share.attachFailed": "Discord не подключился к отфильтрованному звуку",
    "share.attachFailed.detail": "Трансляция переключена на отфильтрованный звук, но Discord к нему не подключился: скорее всего, зрители не слышат звука. Перезапустите трансляцию; если повторяется — скопируйте диагностический отчёт.",
    "share.muted": "Звук трансляции выключен",
    "share.muted.detail": "Фильтрацию применить не удалось, поэтому звук не передаётся вовсе — в том числе исключённых приложений. Устраните проблему ниже или разрешите обычный системный звук в настройках.",
    "share.unfiltered": "Зрители слышат ВСЕ приложения",
    "share.unfiltered.detail": "Фильтрацию применить не удалось, и передаётся обычный системный звук, так как включено «Передавать нефильтрованный звук при сбое».",
    "share.uncontrolled": "Звук не контролируется плагином",
    "share.uncontrolled.detail": "Трансляция запустилась в обход плагина (возможно, Discord обновился). Зрители могут слышать все приложения. Выключите звук трансляции, если это важно, и скопируйте диагностический отчёт.",

    "mode.label": "По умолчанию для приложений без правила",
    "mode.allExcept": "Разрешены",
    "mode.allExcept.hint": "Новые приложения передаются, пока вы их не выключите.",
    "mode.onlySelected": "Запрещены",
    "mode.onlySelected.hint": "Новые приложения не передаются, пока вы их не включите.",
    "action.refresh": "Обновить",
    "action.reset": "Сбросить правила",
    "action.copyReport": "Скопировать диагностический отчёт",
    "action.openSettings": "Все настройки",
    "action.collapse": "Свернуть",
    "action.expand": "Развернуть",
    "action.close": "Закрыть",
    "action.checkSupport": "Проверить поддержку",
    "action.restartHost": "Перезапустить аудиопроцесс",
    "action.openFolder": "Открыть папку",
    "toast.copied": "Диагностический отчёт скопирован",

    "list.playing": "Сейчас воспроизводят звук",
    "list.idle": "Запущены, без звука",
    "list.closed": "Сохранённые правила",
    "list.empty": "Ни одно приложение не воспроизводит звук.",
    "list.hint": "Нет нужного приложения? Включите в нём звук.",
    "row.switch.on": "Зрители могут слышать это приложение",
    "row.switch.off": "Зрители не слышат это приложение",
    "row.default": "по умолчанию",
    "row.router": "Повторно воспроизводит звук других приложений. Его передача передала бы и их, включая исключённые, поэтому он запрещён, пока вы его не разрешите.",
    "row.default.hint": "Правило не сохранено: действует значение по умолчанию",
    "row.instances": "экземпляров: {n}",
    "row.notSaved": "правило не сохранится",
    "row.notSaved.hint": "Исполняемый файл приложения неизвестен, поэтому правило не запомнится после его закрытия.",

    "state.sending": "передаётся",
    "state.sending.hint": "Захватывается и входит в звук, который получают зрители",
    "state.ready": "разрешено",
    "state.ready.hint": "Будет передаваться, когда вы начнёте трансляцию экрана",
    "state.excluded": "запрещено",
    "state.covered": "через {app}",
    "state.covered.hint": "Уже входит в захват приложения {app}",
    "state.blocked": "конфликт",
    "state.silentCapture": "нет звука",
    "state.unsupported": "недоступно",
    "state.error": "ошибка",
    "state.closed": "закрыто",
    "state.waiting": "ожидание",
    "state.waiting.hint": "Разрешено, но ещё не захватывается",

    "problems.title": "Проблем: {n}",
    "problems.show": "Показать подробности",
    "problems.hide": "Скрыть подробности",
    "problem.hostMissing": "Аудиопроцесс (saf-audio-host.exe) не найден",
    "problem.hostFailed": "Аудиопроцесс не запущен",
    "problem.unsupported": "Захват отдельных приложений недоступен в этой системе",
    "problem.silentCapture": "Windows отдаёт для {app} только тишину (оно играет через «{device}»), поэтому зрители его не слышат. Проверьте громкость приложения в микшере Windows; если она в порядке — перезапустите трансляцию. Если тишина остаётся, скопируйте диагностический отчёт.",
    "problem.blocked": "{app} не передаётся: {reason}",
    "problem.patch": "Плагин не смог подключиться к коду трансляции Discord (Discord изменился). Трансляции не фильтруются.",
    "problem.sessionUnmuted": "Заглушённую аудиосессию плагина включили в микшере громкости Windows; она снова заглушена (иначе вы слышали бы всё дважды).",

    "diag.title": "Диагностика",
    "diag.host": "Аудиопроцесс",
    "diag.version": "Версии",
    "diag.platform": "Система",
    "diag.capture": "Захват",
    "diag.output": "Вывод в Discord",
    "diag.share": "Трансляция",
    "diag.counters": "Счётчики",
    "diag.patch": "Подключение к Discord",

    "settings.mutedNote": "В микшере громкости Windows плагин виден как заглушённая сессия («StreamAudioFilter: share audio»). Не включайте её: Discord берёт звук до заглушения.",
    "floating.noShare": "нет трансляции",
    "button.tooltip": "Звук трансляции: выбрать, что слышат зрители"
};

export type I18nKey = Key;

export function language(): "ru" | "en" {
    const lang = typeof document !== "undefined" ? document.documentElement.lang || navigator.language : "en";
    return lang.toLowerCase().startsWith("ru") ? "ru" : "en";
}

export function t(key: Key, params?: Record<string, string | number>): string {
    let text = (language() === "ru" ? ru[key] : en[key]) ?? en[key] ?? key;
    if (params) for (const [name, value] of Object.entries(params)) text = text.split(`{${name}}`).join(String(value));
    return text;
}
