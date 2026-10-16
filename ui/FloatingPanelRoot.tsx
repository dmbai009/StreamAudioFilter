/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — mounts the floating panel. It lives outside Discord's
 * React tree, so it does not inherit the theme classes that define Discord's
 * colour variables: the theme classes of <html> are mirrored onto the root
 * (and kept in sync when the theme changes) so the panel is never transparent
 * and always matches the light or dark theme.
 *
 * Mounting twice is a no-op: there is at most one floating panel.
 */

import { createRoot } from "@webpack/common";

import { FloatingPanel } from "./FloatingPanel";

let container: HTMLDivElement | null = null;
let root: ReturnType<typeof createRoot> | null = null;
let observer: MutationObserver | null = null;

function themeClasses(): string {
    const sources = [document.documentElement, document.body];
    const classes = new Set<string>();
    for (const element of sources) {
        for (const name of Array.from(element?.classList ?? [])) {
            if (name.startsWith("theme-") || name === "visual-refresh" || name.startsWith("density-")) classes.add(name);
        }
    }
    return Array.from(classes).join(" ");
}

function syncTheme() {
    if (container) container.className = `vc-saf-floating-root ${themeClasses()}`.trim();
}

export function mountFloatingPanel(): void {
    if (root || typeof document === "undefined") return;

    container = document.createElement("div");
    syncTheme();
    document.body.appendChild(container);

    observer = new MutationObserver(syncTheme);
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ["class"] });
    observer.observe(document.body, { attributes: true, attributeFilter: ["class"] });

    root = createRoot(container);
    root.render(<FloatingPanel />);
}

export function unmountFloatingPanel(): void {
    observer?.disconnect();
    observer = null;
    root?.unmount();
    root = null;
    container?.remove();
    container = null;
}
