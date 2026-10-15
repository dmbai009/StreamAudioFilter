/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Mounts the floating panel into Discord's DOM. A React root lives outside the
 * settings page on purpose: the panel has to be reachable while a screen share
 * is running, when the settings page is not open.
 */

import { createRoot } from "@webpack/common";

import { FloatingPanel } from "./FloatingPanel";

let container: HTMLDivElement | null = null;
let root: ReturnType<typeof createRoot> | null = null;

export function mountFloatingPanel(): void {
    if (root || typeof document === "undefined") return;

    container = document.createElement("div");
    container.className = "vc-saf-floating-root";
    document.body.appendChild(container);

    root = createRoot(container);
    root.render(<FloatingPanel />);
}

export function unmountFloatingPanel(): void {
    if (root) {
        root.unmount();
        root = null;
    }
    if (container) {
        container.remove();
        container = null;
    }
}
