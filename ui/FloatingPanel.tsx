/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Small floating window with the same controls as the settings page.
 *
 * It exists because Discord's own "go live" window and the stream interface can
 * not be extended safely from a user plugin: their internals are not part of any
 * stable API. Instead of patching them blindly, the compact panel appears as soon
 * as Discord asks for the audio of a screen share, which is exactly the moment
 * the filter set usually needs adjusting.
 *
 * While a stream is running the panel collapses to a small badge (configurable),
 * so it does not cover the shared screen more than necessary.
 */

import ErrorBoundary from "@components/ErrorBoundary";
import { Flex } from "@components/Flex";
import { Span } from "@components/Span";
import { useEffect, useRef, useState } from "@webpack/common";
import type { CSSProperties } from "react";

import { store, useStoreState } from "../apps/store";
import { settings } from "../settings";
import { FilterControls } from "./FilterControls";
import { Icon } from "./icons";

const cl = (name: string) => `vc-saf-${name}`;

interface Position {
    x: number;
    y: number;
}

const DEFAULT_POSITION: Position = { x: -1, y: -1 };

function clampPosition(position: Position): Position {
    const width = window.innerWidth;
    const height = window.innerHeight;
    return {
        x: Math.min(Math.max(position.x, 8), Math.max(8, width - 360)),
        y: Math.min(Math.max(position.y, 8), Math.max(8, height - 80))
    };
}

export const FloatingPanel = ErrorBoundary.wrap(function FloatingPanel() {
    const state = useStoreState();
    const [collapsed, setCollapsed] = useState(false);
    const [visible, setVisible] = useState(false);
    const [position, setPosition] = useState<Position>(DEFAULT_POSITION);
    const dragRef = useRef<{ startX: number; startY: number; originX: number; originY: number; } | null>(null);

    // The panel is shown when the user asks for it, and whenever Discord starts
    // asking for the audio of a screen share.
    useEffect(() => {
        setVisible(true);
        return store.openPanel();
    }, []);

    useEffect(() => {
        if (state.stream.active) {
            setCollapsed(settings.store.collapseDuringStream ?? true);
            setVisible(true);
        }
    }, [state.stream.active]);

    useEffect(() => {
        function onMove(event: MouseEvent) {
            const drag = dragRef.current;
            if (!drag) return;
            event.preventDefault();
            setPosition(clampPosition({
                x: drag.originX + (event.clientX - drag.startX),
                y: drag.originY + (event.clientY - drag.startY)
            }));
        }
        function onUp() {
            dragRef.current = null;
        }

        window.addEventListener("mousemove", onMove);
        window.addEventListener("mouseup", onUp);
        return () => {
            window.removeEventListener("mousemove", onMove);
            window.removeEventListener("mouseup", onUp);
        };
    }, []);

    if (!visible) return null;

    const style: CSSProperties = {
        left: position.x >= 0 ? position.x : undefined,
        top: position.y >= 0 ? position.y : undefined,
        right: position.x >= 0 ? undefined : 24,
        bottom: position.y >= 0 ? undefined : 96
    };

    const captured = state.apps.filter(app => app.captured).length;

    return (
        <div className={cl("floating")} style={style} data-collapsed={collapsed ? "true" : "false"}>
            <div
                className={cl("floating-header")}
                onMouseDown={event => {
                    const rect = (event.currentTarget.parentElement as HTMLElement).getBoundingClientRect();
                    dragRef.current = {
                        startX: event.clientX,
                        startY: event.clientY,
                        originX: rect.left,
                        originY: rect.top
                    };
                    setPosition({ x: rect.left, y: rect.top });
                }}
            >
                <Flex flexDirection="row" gap="0.4em" alignItems="center">
                    <Icon kind={state.stream.filtering ? "on" : state.stream.active ? "warn" : "off"} />
                    <Span size="sm" weight="semibold">Stream audio</Span>
                    {collapsed && (
                        <Span size="xs" className={cl("muted")}>
                            {state.stream.active ? `${captured} cap.` : "no share"}
                        </Span>
                    )}
                </Flex>
                <Flex flexDirection="row" gap="0.2em" alignItems="center">
                    <button
                        className={cl("floating-btn")}
                        title={collapsed ? "Expand" : "Collapse"}
                        onClick={() => setCollapsed(!collapsed)}
                    >
                        {collapsed ? "▾" : "▴"}
                    </button>
                    <button
                        className={cl("floating-btn")}
                        title="Hide the panel (enable it again in the plugin settings)"
                        onClick={() => {
                            setVisible(false);
                            settings.store.showFloatingPanel = false;
                        }}
                    >
                        ✕
                    </button>
                </Flex>
            </div>

            {!collapsed && (
                <div className={cl("floating-body")}>
                    <FilterControls compact />
                </div>
            )}
        </div>
    );
}, { noop: true });
