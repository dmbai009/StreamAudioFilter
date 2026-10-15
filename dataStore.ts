/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 *
 * StreamAudioFilter — per application audio filtering for screen shares.
 * Thin wrapper around Vencord's key-value store. Rules are stored per
 * application, keyed by a stable application key, so they survive restarts and
 * do not depend on process ids.
 */

import * as VencordDataStore from "@api/DataStore";

export const DataStore = {
    async get<T>(key: string): Promise<T | undefined> {
        try {
            return await VencordDataStore.get<T>(key);
        } catch {
            return undefined;
        }
    },

    async set(key: string, value: unknown): Promise<void> {
        try {
            await VencordDataStore.set(key, value);
        } catch {
            // A failing store must never break audio filtering.
        }
    },

    async del(key: string): Promise<void> {
        try {
            await VencordDataStore.del(key);
        } catch { }
    }
};
