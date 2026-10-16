/*
 * StreamAudioFilter — packs the installation data embedded in saf-setup.exe.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   node scripts/pack-payload.mjs <payload dir> <payload.bin>
 *
 * <payload dir> contains dist\ (Vencord built with the plugin),
 * saf-audio-host.exe and saf-version.txt. Before packing, the versions of all
 * parts are checked against each other, so an installer can never be built
 * from parts of different releases.
 *
 * Format: "SAFPAYLOAD1\n", one "<size>\t<path>\n" line per file, an empty
 * line, then the file contents in the same order.
 */

import { readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const [dir, out] = process.argv.slice(2);
if (!dir || !out) {
    console.error("usage: node scripts/pack-payload.mjs <payload dir> <payload.bin>");
    process.exit(2);
}

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const fail = message => { console.error(`pack-payload: ${message}`); process.exit(1); };

const pluginVersion = /PLUGIN_VERSION = "([^"]+)"/.exec(readFileSync(join(root, "version.ts"), "utf8"))?.[1];
const protocol = /PROTOCOL_VERSION = (\d+)/.exec(readFileSync(join(root, "version.ts"), "utf8"))?.[1];
const native = readFileSync(join(root, "native", "src", "version.h"), "utf8");
const nativeVersion = /SAF_VERSION "([^"]+)"/.exec(native)?.[1];
const nativeProtocol = /SAF_PROTOCOL (\d+)/.exec(native)?.[1];
if (pluginVersion !== nativeVersion || protocol !== nativeProtocol) {
    fail(`version.ts (${pluginVersion}, protocol ${protocol}) and native/src/version.h (${nativeVersion}, protocol ${nativeProtocol}) differ`);
}

const manifest = Object.fromEntries(readFileSync(join(dir, "saf-version.txt"), "utf8").split(/\r?\n/).filter(Boolean).map(l => l.split("=")));
if (manifest.version !== pluginVersion) fail(`saf-version.txt says ${manifest.version}, the sources say ${pluginVersion}`);

const renderer = readFileSync(join(dir, "dist", "renderer.js"), "utf8");
if (!renderer.includes(`"${pluginVersion}"`) || !renderer.includes("forceHookSoundshare")) fail("dist/renderer.js does not contain this version of the plugin");

const files = [];
function walk(base) {
    for (const name of readdirSync(base)) {
        const full = join(base, name);
        if (statSync(full).isDirectory()) walk(full);
        else if (!name.endsWith(".map")) files.push(full);
    }
}
walk(join(dir, "dist"));
files.push(join(dir, "saf-audio-host.exe"), join(dir, "saf-version.txt"));

const contents = files.map(file => readFileSync(file));
const header = "SAFPAYLOAD1\n" + files.map((file, i) => `${contents[i].length}\t${relative(dir, file).replaceAll("\\", "/")}\n`).join("") + "\n";
writeFileSync(out, Buffer.concat([Buffer.from(header, "utf8"), ...contents]));
console.log(`pack-payload: ${files.length} files, ${(statSync(out).size / 1048576).toFixed(1)} MB, version ${pluginVersion}`);
