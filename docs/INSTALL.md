# Installation

## Installer (recommended)

1. Download `StreamAudioFilter-Setup.exe` from the releases page.
2. Quit Discord completely: tray icon → Quit Discord.
3. Run the installer, choose your Discord (Stable, PTB, Canary are detected), press **Install**.
4. Start Discord. The plugin is enabled; the button next to the microphone opens its panel.

The installer:

- puts Vencord with the plugin into `%APPDATA%\Vencord\dist` and the audio host into
  `%APPDATA%\Vencord\streamAudioFilter\native\<version>`;
- injects Vencord into Discord only if it is not injected yet (same method as Vencord's installer);
- keeps an existing Vencord build as `dist.saf-backup` and restores it on uninstall;
- preserves other plugins' settings; uninstall can keep or reset StreamAudioFilter's own settings;
- checks that the plugin and the audio host come from the same release;
- undoes every step if a later one fails;
- needs no administrator rights and downloads nothing.

The installer shows installed and available versions. Run it again to **Update**, **Reinstall** the
same version (e.g. after a Discord update removed Vencord), or **Uninstall**. Downgrades are refused.
Removal restores the original Vencord build, or plain Vencord when no original existed. If restoration
files are missing, reinstall first; the installer refuses to leave Discord with a broken loader.

**Keep my settings when uninstalling** is checked by default. Uncheck it to reset this plugin's
settings and rules. Since 0.3.0 rules and floating position live in the plugin's own `preferences.json`.
Earlier IndexedDB rules/localStorage position migrate on the next plugin start. Reset marks those
legacy entries as discarded immediately and deletes only the plugin's keys on its next start;
it never deletes Discord's shared database. Restart Discord after changing the installation.

Command line (same code): `saf-setup.exe --list | --status | --install | --update | --reinstall`;
installation commands accept `--discord <folder>` and `--payload-dir <folder>`.
Removal: `saf-setup.exe --uninstall [--remove-settings]`.

### Discord stays on the loading screen

The old bundle based on Vencord v1.15.11 was observed to stop Discord 1.0.9268
(web build 673869) at startup with `Uncaught TypeError: i.A is not a constructor`.
The replacement bundle uses Vencord v1.15.17; Discord and StreamAudioFilter both
started successfully with it on the affected machine. Use the rebuilt installer;
reinstalling the old bundle brings the incompatible Vencord code back.

If you need to recover before obtaining a rebuilt installer, quit Discord and
uninstall StreamAudioFilter with settings preserved. The installer restores the
previous Vencord build from `dist.saf-backup` when available.

## Manual

From the same release: `streamaudiofilter-vencord-dist.zip` and `saf-audio-host.exe`.

1. Quit Discord. Have Vencord injected (official installer from vencord.dev).
2. Replace `%APPDATA%\Vencord\dist\*` with the zip's `Vencord-dist\*`.
3. Put `saf-audio-host.exe` into `%APPDATA%\Vencord\streamAudioFilter\native\<version>\` (e.g. `native\0.2.0\`).
4. Start Discord.

## From source

Needs git, Node.js 22+, Visual Studio 2019/2022 (or Build Tools) with "Desktop development with C++".

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1            # build + install
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Uninstall
```

It builds the audio host and the installer (`scripts/build-host.ps1`), Vencord v1.15.17 with the plugin,
and installs with `saf-setup.exe --payload-dir` — the same code as the release installer.

Development checks: `scripts/run-native-tests.sh`, `node tests/host.test.mjs <host>`,
`VENCORD_DIR=<Vencord checkout> dev/typecheck.sh`.
