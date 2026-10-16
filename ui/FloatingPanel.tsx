/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — optional floating panel (setting "showFloatingPanel").
 *
 * It starts as a small status pill and only expands when the user asks. It
 * hides itself while the settings page or the quick panel is open, so the
 * plugin never shows two full interfaces at once. It is dragged by its title,
 * stays inside the window when the window or the zoom level changes, and its
 * position is remembered per device (localStorage).
 */

import ErrorBoundary from "@components/ErrorBoundary";
import { useEffect, useRef, useState } from "@webpack/common";

import { store, useStoreState } from "../apps/store";
import { t } from "../i18n";
import { Native } from "../nativeBridge";
import { settings } from "../settings";
import { FilterControls, ProblemsSummary, ShareStatusCard } from "./FilterControls";
import { Icon, iconKindOf } from "./icons";

const cl = (name: string) => `vc-saf-${name}`;
const POSITION_KEY = "StreamAudioFilter_floatingPosition";
const MARGIN = 8;

interface Position { x: number; y: number; }

function loadPosition(): Position | null {
    try {
        const value = JSON.parse(localStorage.getItem(POSITION_KEY) ?? "null");
        return value && typeof value.x === "number" && typeof value.y === "number" ? value : null;
    } catch {
        return null;
    }
}

function savePosition(position: Position) {
    void Native.savePreferences("position", position).catch(console.error);
}

function clamp(position: Position, element: HTMLElement | null): Position {
    const width = element?.offsetWidth ?? 240;
    const height = element?.offsetHeight ?? 40;
    return {
        x: Math.min(Math.max(position.x, MARGIN), Math.max(MARGIN, window.innerWidth - width - MARGIN)),
        y: Math.min(Math.max(position.y, MARGIN), Math.max(MARGIN, window.innerHeight - height - MARGIN))
    };
}

export const FloatingPanel = ErrorBoundary.wrap(function FloatingPanel() {
    const state = useStoreState();
    const [expanded, setExpanded] = useState(false);
    const [position, setPosition] = useState<Position | null>(null);
    const panelRef = useRef<HTMLDivElement | null>(null);
    const dragRef = useRef<{ dx: number; dy: number; } | null>(null);

    useEffect(() => {
        let active = true;
        void Native.loadPreferences().then(async preferences => {
            const saved = preferences.position ?? (preferences.reset ? null : loadPosition());
            if (saved) {
                await Native.savePreferences("position", saved);
                if (active && !dragRef.current) setPosition(saved);
            } else if (active && preferences.reset) setPosition(null);
            localStorage.removeItem(POSITION_KEY);
        }).catch(console.error);
        return () => { active = false; };
    }, []);

    // Keep the list polling only while the panel is expanded.
    useEffect(() => (expanded ? store.openPanel("floating") : undefined), [expanded]);

    useEffect(() => {
        const onMove = (event: MouseEvent) => {
            const drag = dragRef.current;
            if (!drag) return;
            event.preventDefault();
            setPosition(clamp({ x: event.clientX - drag.dx, y: event.clientY - drag.dy }, panelRef.current));
        };
        const onUp = () => {
            if (!dragRef.current) return;
            dragRef.current = null;
            setPosition(current => {
                if (current) savePosition(current);
                return current;
            });
        };
        const onResize = () => setPosition(current => current && clamp(current, panelRef.current));
        window.addEventListener("mousemove", onMove);
        window.addEventListener("mouseup", onUp);
        window.addEventListener("resize", onResize);
        return () => {
            window.removeEventListener("mousemove", onMove);
            window.removeEventListener("mouseup", onUp);
            window.removeEventListener("resize", onResize);
        };
    }, []);

    // After expanding/collapsing the size changes: keep it inside the window.
    useEffect(() => { setPosition(current => current && clamp(current, panelRef.current)); }, [expanded]);

    if (state.panels.settings > 0 || state.panels.popout > 0) return null;

    const style = position ? { left: position.x, top: position.y } : { right: 16, bottom: 72 };

    return (
        <div ref={panelRef} className={cl("floating")} style={style} data-expanded={expanded ? "true" : "false"}>
            <div
                className={cl("floating-head")}
                onMouseDown={event => {
                    if ((event.target as HTMLElement).closest("button")) return;
                    const rect = panelRef.current!.getBoundingClientRect();
                    dragRef.current = { dx: event.clientX - rect.left, dy: event.clientY - rect.top };
                }}
            >
                <Icon kind={state.masterMuted ? "muted" : iconKindOf(state.share.audio)} size={18} />
                <span className={cl("floating-title")}>
                    {state.masterMuted && state.share.audio === "filtered" ? t("mute.active") : state.share.audio === "idle" ? `${t("title")} · ${t("floating.noShare")}` : t(`share.${state.share.audio}`)}
                </span>
                <button className={cl("icon-button")} title={expanded ? t("action.collapse") : t("action.expand")} onClick={() => setExpanded(!expanded)}>
                    {expanded ? "▾" : "▴"}
                </button>
                <button className={cl("icon-button")} title={t("action.close")} onClick={() => { settings.store.showFloatingPanel = false; }}>
                    ✕
                </button>
            </div>
            {expanded
                ? <div className={cl("floating-body")}><FilterControls compact /></div>
                : (state.share.audio !== "idle" && state.share.audio !== "filtered") && (
                    <div className={cl("floating-body")}>
                        <ShareStatusCard state={state} compact />
                        <ProblemsSummary state={state} />
                    </div>
                )}
        </div>
    );
}, { noop: true });
