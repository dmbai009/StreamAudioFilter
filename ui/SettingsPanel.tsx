/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — the full panel inside Vencord's settings page for this
 * plugin. While it is open the floating panel hides itself, so the two never
 * cover each other.
 */

import { useEffect } from "@webpack/common";

import { store, useStoreState } from "../apps/store";
import { Diagnostics } from "../components/Diagnostics";
import { t } from "../i18n";
import { FilterControls } from "./FilterControls";

const cl = (name: string) => `vc-saf-${name}`;

export function SettingsPanel() {
    const state = useStoreState();
    useEffect(() => store.openPanel("settings"), []);

    return (
        <div className={cl("settings")}>
            <p className={cl("tagline")}>{t("tagline")}</p>
            <FilterControls />
            <p className={cl("note")}>{t("settings.mutedNote")}</p>
            <Diagnostics state={state} />
        </div>
    );
}
