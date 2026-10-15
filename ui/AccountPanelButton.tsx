/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * The button next to Discord's microphone and headphone buttons (bottom left of
 * the window). It opens a compact popout with the same controls as the settings
 * page, so the transmitted set can be changed while sharing without leaving the
 * conversation.
 *
 * Discord's own button component is looked up by its code — this is the only way
 * to sit in that row without touching any layout of the client. When the lookup
 * fails (a new Discord build), the plugin falls back to Vencord's own button and
 * keeps working.
 */

import { Button } from "@components/Button";
import ErrorBoundary from "@components/ErrorBoundary";
import { Span } from "@components/Span";
import { findComponentByCodeLazy } from "@webpack";
import { Popout, useRef, useState } from "@webpack/common";

import { store, useStoreState } from "../apps/store";
import { settings } from "../settings";
import { FilterControls } from "./FilterControls";
import { Icon } from "./icons";

const cl = (name: string) => `vc-saf-${name}`;

type PanelButtonComponent = React.ComponentType<{
    ref?: React.Ref<HTMLButtonElement>;
    onClick?: () => void;
    tooltip?: string | null;
    icon?: () => React.ReactNode;
    selected?: boolean;
    role?: string;
    plated?: boolean;
}>;

let panelButton: PanelButtonComponent | null = null;

function resolvePanelButton(): PanelButtonComponent | null {
    if (panelButton) return panelButton;
    try {
        // Discord's account panel button (the row with mute/deafen).
        panelButton = findComponentByCodeLazy(".GREEN,positionKeyStemOverride:") as PanelButtonComponent;
    } catch {
        panelButton = null;
    }
    return panelButton;
}

export const AccountPanelButton = ErrorBoundary.wrap(function AccountPanelButton(props: { nameplate?: unknown; }) {
    const { showAccountPanelButton } = settings.use(["showAccountPanelButton"]);
    const state = useStoreState();
    const [show, setShow] = useState(false);
    const ref = useRef<HTMLButtonElement | null>(null);

    if (!showAccountPanelButton) return null;

    const kind = state.stream.filtering ? "on" : state.stream.active ? "warn" : "off";
    const list = store.entries();
    const captured = list.filter(entry => entry.section === "current" && entry.transmitting).length;
    const tooltip = state.stream.active
        ? `Звук трансляции: передаётся ${captured} прил.`
        : "Звук трансляции: настройка приложений для демонстрации экрана";

    const PanelButton = resolvePanelButton();

    const popout = (
        <div className={cl("popout")}>
            <FilterControls compact />
            <Span size="xs" className={cl("muted")}>
                Полная панель и диагностика: Настройки → Плагины → StreamAudioFilter.
            </Span>
        </div>
    );

    return (
        <Popout
            position="top"
            align="left"
            animation={Popout.Animation.NONE}
            shouldShow={show}
            onRequestClose={() => setShow(false)}
            targetElementRef={ref}
            renderPopout={() => popout}
        >
            {(_, { isShown }) => PanelButton
                ? (
                    <PanelButton
                        ref={ref}
                        onClick={() => setShow(value => !value)}
                        tooltip={isShown ? null : tooltip}
                        icon={() => <Icon kind={kind} />}
                        selected={isShown}
                        role="switch"
                        plated={props?.nameplate != null}
                    />
                )
                : (
                    <Button
                        ref={ref}
                        size="iconOnly"
                        variant="secondary"
                        onClick={() => setShow(value => !value)}
                        aria-label={tooltip}
                        title={tooltip}
                    >
                        <Icon kind={kind} />
                    </Button>
                )}
        </Popout>
    );
}, { noop: true });
