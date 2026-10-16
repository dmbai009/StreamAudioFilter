/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import assert from "node:assert/strict";
import * as fs from "node:fs";
import { stripTypeScriptTypes } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import vm from "node:vm";

test("owned preferences preserve both keys, reset markers and unrelated files", async () => {
    const dir = fs.mkdtempSync(join(tmpdir(), "saf-preferences-"));
    try {
        const source = fs.readFileSync(new URL("../native.ts", import.meta.url), "utf8");
        const functions = source.slice(source.indexOf("function dataDir()"), source.indexOf("function hostDirectory()"));
        const context = vm.createContext({ ...fs, join, DATA_DIR: dir, FOLDER_NAME: "streamAudioFilter" });
        vm.runInContext(stripTypeScriptTypes(functions.replaceAll("export ", "")), context);
        const { loadPreferences, savePreferences } = context;
        fs.writeFileSync(join(dir, "other-plugin.json"), "unchanged");
        assert.equal(JSON.stringify(await loadPreferences()), "{}");
        await Promise.all([savePreferences(null, "rules", { mode: "onlySelected" }), savePreferences(null, "position", { x: 10, y: 20 })]);
        const file = join(dir, "streamAudioFilter", "preferences.json");
        assert.deepEqual(JSON.parse(fs.readFileSync(file, "utf8")), { rules: { mode: "onlySelected" }, position: { x: 10, y: 20 } });
        fs.writeFileSync(file, '{"reset":true}');
        await savePreferences(null, "rules", { mode: "allExcept" });
        assert.equal((await loadPreferences()).reset, true);
        await assert.rejects(savePreferences(null, "../other-plugin.json", {}));
        assert.equal(fs.readFileSync(join(dir, "other-plugin.json"), "utf8"), "unchanged");
        fs.writeFileSync(file, "broken");
        await assert.rejects(savePreferences(null, "rules", {}));
        assert.equal(fs.readFileSync(file, "utf8"), "broken", "corrupt preferences are never silently overwritten");
    } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});
