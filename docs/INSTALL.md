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
- never changes Vencord's settings (the plugin's rules live there too);
- checks that the plugin and the audio host come from the same release;
- undoes every step if a later one fails;
- needs no administrator rights and downloads nothing.

Run it again to **repair** (after a Discord update removed Vencord) or to **uninstall**.

Command line (same code): `saf-setup.exe --list | --status | --install [--discord <folder>] | --uninstall`.

## Manual

From the same release: `streamaudiofilter-vencord-dist.zip` and `saf-audio-host.exe`.

1. Quit Discord. Have Vencord injected (official installer from vencord.dev).
2. Replace `%APPDATA%\Vencord\dist\*` with the zip's `Vencord-dist\*`.
3. Put `saf-audio-host.exe` into `%APPDATA%\Vencord\streamAudioFilter\native\<version>\` (e.g. `native\0.2.0\`).
4. Start Discord.

## From source

Needs git, Node.js 20+, Visual Studio 2019/2022 (or Build Tools) with "Desktop development with C++".

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1            # build + install
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Uninstall
```

It builds the audio host and the installer (`scripts/build-host.ps1`), Vencord v1.15.11 with the plugin,
and installs with `saf-setup.exe --payload-dir` — the same code as the release installer.

Development checks: `scripts/run-native-tests.sh`, `node tests/host.test.mjs <host>`,
`VENCORD_DIR=<Vencord checkout> dev/typecheck.sh`.
