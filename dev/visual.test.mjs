/*
 * Vencord, a Discord client mod
 * Copyright (c) 2026 Vendicated and contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Real FilterControls/AppRow/store, with Discord primitives and native IPC
// replaced by fixtures. Also checks keyboard interaction and rule preservation.
// SAF_TOOLCHAIN=<Vencord checkout> SAF_UI_TOOLS=<react/react-dom/playwright dir>
// node dev/visual.test.mjs
import assert from "node:assert/strict";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const toolchain = createRequire(join(resolve(process.env.SAF_TOOLCHAIN), "package.json"));
const uiTools = createRequire(join(resolve(process.env.SAF_UI_TOOLS), "package.json"));
const { build } = toolchain("esbuild");
const { chromium } = process.env.SAF_PLAYWRIGHT ? createRequire(join(resolve(process.env.SAF_PLAYWRIGHT), "package.json"))("playwright") : uiTools("playwright");
const out = join(root, "native", "build", "visual");
mkdirSync(out, { recursive: true });
const entry = `import React from 'react'; import {createRoot} from 'react-dom/client';
import {FilterControls} from './ui/FilterControls'; import {store} from './apps/store';
const root=createRoot(document.getElementById('root'));
globalThis.mutes=[];
globalThis.fixture=(audio='filtered')=>{
  store.set({engineRunning:true,masterMuted:false,mutePending:false,actionError:'',
    patch:{pidHook:true,modeHook:true},share:{audio,reason:'',since:Date.now()},
    rules:{mode:'allExcept',version:2,rules:{'exe:music':{transmit:false}}},
    apps:[{appKey:'exe:game',displayName:'A game with a very long application name',exeName:'game.exe',exePath:'',playing:true,capturable:true,captured:true,peak:0.5,state:'transmitting',devices:[],instances:1},
    {appKey:'exe:music',displayName:'Music',exeName:'music.exe',exePath:'',playing:true,capturable:true,captured:false,peak:0,state:'excluded',devices:[],instances:1}]});
  root.render(<FilterControls compact/>);
}; globalThis.snapshot=()=>store.getSnapshot(); globalThis.fixture();`;
const result = await build({
    stdin: { contents: entry, resolveDir: root, loader: "tsx" },
    bundle: true, write: false, jsx: "automatic", format: "iife",
    nodePaths: [join(resolve(process.env.SAF_UI_TOOLS), "node_modules")],
    plugins: [{ name: "discord-fixtures", setup(b) {
        b.onResolve({ filter: /^@webpack\/common$|^@components\/(Button|Switch)$/ }, () => ({ path: "primitives", namespace: "fixture" }));
        b.onResolve({ filter: /nativeBridge$/ }, () => ({ path: "native", namespace: "fixture" }));
        b.onResolve({ filter: /settings$/ }, () => ({ path: "settings", namespace: "fixture" }));
        b.onResolve({ filter: /dataStore$/ }, () => ({ path: "data", namespace: "fixture" }));
        b.onResolve({ filter: /^react$|^react-dom\/client$|^react\/jsx-runtime$/ }, args => ({ path: uiTools.resolve(args.path) }));
        b.onLoad({ filter: /.*/, namespace: "fixture" }, ({ path }) => ({ loader: "jsx", contents: path === "primitives"
            ? `import React from 'react'; export * from 'react'; export {React}; export const Tooltip=({children})=>children({}); export const Button=({children,size,variant,...p})=><button {...p}>{children}</button>; export const Switch=({checked,onChange,disabled})=><button role="switch" aria-checked={checked} disabled={disabled} onClick={()=>onChange(!checked)}>{checked?'●':'○'}</button>;`
            : path === "native" ? `export const IS_NATIVE_AVAILABLE=true; export const Native={savePreferences:async()=>{},setRules:async()=>({ok:true}),listApps:async()=>globalThis.snapshot().apps,getStats:async()=>({engine:{},output:{running:true,pid:100}}),setMasterMuted:async muted=>{globalThis.mutes.push(muted);return {ok:true}}};`
                : path === "settings" ? `export const settings={store:{filterEnabled:true,maxStreams:12,testBackend:false}};`
                    : `export const DataStore={get:async()=>undefined,set:async()=>{},del:async()=>{}};` }));
    } }]
});
writeFileSync(join(out, "fixture.js"), result.outputFiles[0].contents);
const css = readFileSync(join(root, "style.css"), "utf8");
const browser = await chromium.launch({ headless: true, executablePath: process.env.SAF_CHROMIUM });
let checked = 0;
try {
    for (const [theme, bg, fg, danger] of [["dark", "#313338", "#f2f3f5", "#ff8a8e"], ["light", "#fff", "#17181b", "#ad1420"], ["midnight", "#111214", "#f2f3f5", "#ff8a8e"], ["ash", "#41434a", "#fff", "#ff9da0"]]) {
        const page = await browser.newPage({ viewport: { width: 900, height: 850 } });
        const errors = []; page.on("pageerror", e => errors.push(e.message));
        for (const lang of ["en", "ru"]) for (const width of [260, 380, 700]) {
            await page.setContent(`<html lang="${lang}"><style>:root{--text-default:${fg};--text-strong:${fg};--text-muted:${fg};--background-surface-high:${bg};--border-subtle:#80808055;--text-feedback-critical:${danger};--background-feedback-critical:#da373c22;--status-positive:#248046;--status-warning:#e8aa32;--background-feedback-warning:#e8aa3222;--background-feedback-positive:#24804622}*{box-sizing:border-box}body{font-family:Arial;background:${bg};color:${fg}}#root{width:${width}px;padding:12px}button{font:inherit;color:inherit;background:transparent;border:1px solid #80808077;border-radius:4px;padding:4px}${css}</style><div id="root"></div></html>`);
            await page.addScriptTag({ content: result.outputFiles[0].text });
            const button = page.locator(".vc-saf-master-mute");
            await button.waitFor();
            const rules = await page.evaluate(() => JSON.stringify(globalThis.snapshot().rules.rules));
            await button.click(); await page.waitForFunction(() => globalThis.snapshot().masterMuted);
            assert.equal(await button.getAttribute("aria-pressed"), "true");
            assert.equal(await page.locator('.vc-saf-status[data-tone="warn"]').count(), 1);
            await page.locator(".vc-saf-segment").last().click();
            assert.equal(await page.evaluate(() => globalThis.snapshot().masterMuted), true);
            assert.equal(await page.evaluate(() => JSON.stringify(globalThis.snapshot().rules.rules)), rules);
            const fits = await page.locator("#root").evaluate(e => e.scrollWidth <= e.clientWidth);
            assert.ok(fits, `overflow ${theme}/${lang}/${width}`);
            await page.screenshot({ path: join(out, `${theme}-${lang}-${width}-muted.png`) });
            await button.focus(); await page.keyboard.press("Space");
            await page.waitForFunction(() => !globalThis.snapshot().masterMuted);
            assert.deepEqual(await page.evaluate(() => globalThis.mutes), [true, false]);
            await page.evaluate(() => globalThis.fixture("window")); await button.click();
            await page.waitForFunction(() => globalThis.snapshot().masterMuted);
            assert.equal(await page.locator('.vc-saf-hint[role="status"]').count(), 1);
            assert.deepEqual(errors, []);
            checked++;
        }
        await page.close();
    }
} finally { await browser.close(); }
console.log(`${checked} visual/interaction states passed; screenshots: ${out}`);
