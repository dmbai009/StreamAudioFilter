/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Small original icons drawn for the plugin. Three states: audio is being
 * filtered (green dot inside), a stream is running but nothing is filtered
 * (warning), and idle.
 */

export type IconKind = "on" | "warn" | "off";

export function Icon({ kind, size = 20, className }: { kind: IconKind; size?: number; className?: string; }) {
    const color = kind === "on" ? "var(--status-positive, #248046)" : kind === "warn" ? "var(--status-warning, #f0b232)" : "currentColor";

    return (
        <svg width={size} height={size} viewBox="0 0 24 24" className={className} aria-hidden="true">
            {/* speaker */}
            <path
                fill="currentColor"
                d="M4.5 9.5h3.2l4.3-4.1a1 1 0 0 1 1.7.74v11.72a1 1 0 0 1-1.7.74L7.7 14.5H4.5a1 1 0 0 1-1-1v-3a1 1 0 0 1 1-1Z"
            />
            {/* waves */}
            <path
                fill="currentColor"
                d="M16.1 8.6a.9.9 0 0 1 1.27.08 5.6 5.6 0 0 1 0 6.64.9.9 0 1 1-1.35-1.18 3.8 3.8 0 0 0 0-4.28.9.9 0 0 1 .08-1.26Z"
            />
            <path
                fill="currentColor"
                d="M18.6 5.6a.9.9 0 0 1 1.26.1 9.4 9.4 0 0 1 0 12.6.9.9 0 1 1-1.36-1.17 7.6 7.6 0 0 0 0-10.26.9.9 0 0 1 .1-1.27Z"
            />
            {kind === "off" && (
                <path fill="var(--status-danger, #da373c)" d="M3.3 2.3 21.7 20.7l-1.4 1.4L1.9 3.7Z" />
            )}
            {kind === "on" && (
                <circle cx="20.2" cy="4.4" r="3.4" fill={color} stroke="var(--background-base-low, #313338)" strokeWidth="1.4" />
            )}
            {kind === "warn" && (
                <path
                    fill={color}
                    d="M12 3.2c.5 0 .9.3 1.1.7l8 14.2c.2.4.2.9 0 1.3-.2.4-.7.6-1.1.6H4c-.4 0-.9-.2-1.1-.6-.2-.4-.2-.9 0-1.3l8-14.2c.2-.4.6-.7 1.1-.7Zm0 5.4a.9.9 0 0 0-.9 1v3.6a.9.9 0 0 0 1.8 0v-3.6a.9.9 0 0 0-.9-1Zm0 6.1a1 1 0 1 0 0 2 1 1 0 0 0 0-2Z"
                />
            )}
        </svg>
    );
}

/** Wrapper that keeps an icon clickable-looking inside Discord's panel rows. */
export function PanelIcon({ kind }: { kind: IconKind; }) {
    return <Icon kind={kind} size={20} className="vc-saf-panel-icon" />;
}
