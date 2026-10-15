/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * The full panel. It is rendered inside Vencord's settings page for this plugin
 * (Settings → Plugins → StreamAudioFilter) and is the place where everything is
 * visible: state, application list, and the diagnostics that explain failures.
 */

import { Divider } from "@components/Divider";
import { Flex } from "@components/Flex";
import { Heading } from "@components/Heading";
import { Span } from "@components/Span";
import { useEffect } from "@webpack/common";

import { store, useStoreState } from "../apps/store";
import { Diagnostics } from "../components/Diagnostics";
import { FilterControls } from "./FilterControls";

const cl = (name: string) => `vc-saf-${name}`;

export function SettingsPanel() {
    const state = useStoreState();

    // Keep the engine (and therefore the application list) alive while the
    // panel is open, and release it a little after the last panel is closed.
    useEffect(() => store.openPanel(), []);

    return (
        <div className={cl("panel")}>
            <Flex flexDirection="column" gap="0.25em">
                <Heading tag="h4">Stream audio</Heading>
                <Span size="xs" className={cl("muted")}>
                    Choose which applications the viewers hear while you share your screen.
                    Your own audio, the microphone and the voice call are never affected.
                </Span>
            </Flex>

            <Divider className={cl("divider")} />

            <FilterControls />

            <Divider className={cl("divider")} />

            <Diagnostics
                environment={state.environment}
                stats={state.stats}
                stream={state.stream}
                warnings={[state.lastWarning, ...(state.stats?.warnings ?? [])].filter(Boolean)}
            />
        </div>
    );
}
