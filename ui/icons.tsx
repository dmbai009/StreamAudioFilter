/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — the speaker icon with a status mark:
 *   on    the viewers receive the filtered audio (confirmed)
 *   warn  something needs attention (see the panel)
 *   off   idle: no share, or the plugin is not involved
 */

import type { ShareAudio } from "../types";

export type IconKind = "on" | "warn" | "off";

export function iconKindOf(audio: ShareAudio): IconKind {
    if (audio === "filtered") return "on";
    if (audio === "idle" || audio === "window" || audio === "noAudio" || audio === "off") return "off";
    return "warn";
}

export function Icon({ kind, size = 20, className }: { kind: IconKind; size?: number; className?: string; }) {
    return (
        <svg width={size} height={size} viewBox="0 0 24 24" className={className} aria-hidden="true">
            <path
                fill="currentColor"
                d="M4.5 9.5h3.2l4.3-4.1a1 1 0 0 1 1.7.74v11.72a1 1 0 0 1-1.7.74L7.7 14.5H4.5a1 1 0 0 1-1-1v-3a1 1 0 0 1 1-1Z"
            />
            <path
                fill="currentColor"
                d="M16.1 8.6a.9.9 0 0 1 1.27.08 5.6 5.6 0 0 1 0 6.64.9.9 0 1 1-1.35-1.18 3.8 3.8 0 0 0 0-4.28.9.9 0 0 1 .08-1.26Z"
            />
            <path
                fill="currentColor"
                d="M18.6 5.6a.9.9 0 0 1 1.26.1 9.4 9.4 0 0 1 0 12.6.9.9 0 1 1-1.36-1.17 7.6 7.6 0 0 0 0-10.26.9.9 0 0 1 .1-1.27Z"
            />
            {kind === "on" && (
                <circle cx="19.6" cy="5" r="3.6" fill="var(--status-positive)" stroke="var(--background-base-lowest)" strokeWidth="1.6" />
            )}
            {kind === "warn" && (
                <circle cx="19.6" cy="5" r="3.6" fill="var(--status-warning)" stroke="var(--background-base-lowest)" strokeWidth="1.6" />
            )}
        </svg>
    );
}
