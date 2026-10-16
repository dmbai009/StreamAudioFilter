/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — the button next to Discord's microphone and headphone
 * buttons, and the quick panel it opens. The quick panel is the compact form
 * of the settings page: status, problems, the list and its switches.
 */

import ErrorBoundary from "@components/ErrorBoundary";
import { findComponentByCodeLazy } from "@webpack";
import { Popout, useEffect, useRef, useState } from "@webpack/common";

import { store, useStoreState } from "../apps/store";
import { t } from "../i18n";
import { settings } from "../settings";
import { FilterControls } from "./FilterControls";
import { Icon, iconKindOf } from "./icons";

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

// Discord's own account panel button (the row with mute/deafen).
const PanelButton = findComponentByCodeLazy(".GREEN,positionKeyStemOverride:") as PanelButtonComponent;

function QuickPanel() {
    useEffect(() => store.openPanel("popout"), []);
    return (
        <div className={cl("popout")}>
            <div className={cl("popout-head")}>
                <Icon kind={iconKindOf(store.getSnapshot().share.audio)} size={18} />
                <span className={cl("popout-title")}>{t("title")}</span>
            </div>
            <div className={cl("popout-body")}>
                <FilterControls compact />
            </div>
        </div>
    );
}

export const AccountPanelButton = ErrorBoundary.wrap(function AccountPanelButton(props: { nameplate?: unknown; }) {
    const { showAccountPanelButton } = settings.use(["showAccountPanelButton"]);
    const state = useStoreState();
    const [show, setShow] = useState(false);
    const ref = useRef<HTMLButtonElement | null>(null);

    if (!showAccountPanelButton) return null;

    const kind = state.masterMuted ? "muted" : iconKindOf(state.share.audio);
    const tooltip = state.masterMuted ? t("mute.active") : state.share.audio === "idle" ? t("button.tooltip") : `${t("title")}: ${t(`share.${state.share.audio}`)}`;

    return (
        <Popout
            position="top"
            align="left"
            animation={Popout.Animation.NONE}
            shouldShow={show}
            onRequestClose={() => setShow(false)}
            targetElementRef={ref}
            renderPopout={() => <QuickPanel />}
        >
            {(_, { isShown }) => (
                <PanelButton
                    ref={ref}
                    onClick={() => setShow(value => !value)}
                    tooltip={isShown ? null : tooltip}
                    icon={() => <Icon kind={kind} />}
                    selected={isShown}
                    role="button"
                    plated={props?.nameplate != null}
                />
            )}
        </Popout>
    );
}, { noop: true });
