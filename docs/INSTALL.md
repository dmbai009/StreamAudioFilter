# Installation

The plugin has two parts:

* **the TypeScript part** — an ordinary Vencord user plugin (`index.tsx` and the folders next to it);
* **the native addon** — `streamaudiofilter.node`, built from the `native/` folder on your own machine,
  because it talks to WASAPI directly.

The first part is installed by copying, the second one by building. Nothing is downloaded or installed
automatically.

## Quick install (one command)

On Windows, from the folder this repository was unpacked into:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -DownloadAddon v0.1.0
```

Run it from this repository, or download just `scripts/install.ps1` and run it anywhere: in that case
it fetches the plugin sources itself (a plain source archive of this repository, after asking).

The script

* checks Node.js (22 or newer, <https://nodejs.org>) and stops with a link if it is missing;
* fetches the Vencord source (a shallow `git clone`, or GitHub's source zip when git is absent);
* installs its dependencies, copies this plugin into `src\userplugins\StreamAudioFilter`, builds
  Vencord, and verifies that the built bundle really contains the plugin;
* puts the native addon into `%APPDATA%\Vencord\streamAudioFilter\native\` — with
  `-DownloadAddon v0.1.0` it comes from the release, `-BuildAddon` compiles it locally,
  `-AddonPath <file>` uses a file you already have. Without any of them the plugin is installed but
  fails closed: it removes the share audio and says why;
* patches Discord with your build. That step downloads the official `VencordInstallerCli.exe` from
  github.com/Vencord/Installer (about 5 MB) and the script asks before doing it.

Then: start Discord, **Settings → Plugins → enable StreamAudioFilter**, open its panel and press
"Check support".

Every download is printed before it happens and the script stops if you decline, so nothing is
fetched behind your back. It needs no administrator rights and touches no audio device or sound
setting. If PowerShell refuses to run the file (Windows marks files that came from the internet),
unblock the folder once:

```powershell
Get-ChildItem -Recurse | Unblock-File
```

Useful switches:

| Switch | What it does |
|---|---|
| `-Update` | refresh the Vencord source (`git pull`) and rebuild — keeps your plugins |
| `-Reinject` | only patch Discord again, used after a Discord update removed the patch |
| `-VencordDir <path>` | where the Vencord source tree lives or should be created |
| `-SkipInstall` | skip `pnpm install` (the dependencies are already there) |
| `-Yes` | answer every question with yes; the printed download URLs stay |

What it downloads, and only after printing the URL and asking: the Vencord source (git clone or a
source zip), the npm packages Vencord needs, the official `VencordInstallerCli.exe`, and — depending
on the switches — the prebuilt native addon or the plugin sources. If you decline any of them, the
script stops and says what is missing instead of working around it.

### Why "just drop the plugin file somewhere" cannot work

Vencord does not read plugins from a folder at runtime: `pnpm build` scans `src\plugins` and
`src\userplugins` and bundles all of them into the JavaScript that Discord then loads. A user plugin
therefore only enters through a build of Vencord. A hand-installed `dist` would not help either —
Vencord's updater replaces it, and the main-process half of a plugin (`native.ts`, which talks to
the compiled addon) lives inside that same bundle. Only the native addon genuinely behaves like a
single file you drop in the right folder.

### After the first installation

* a Discord update removes the patch → `... -Reinject`;
* a Vencord or plugin update → `... -Update`;
* Vencord's own updater (Settings → Vencord → Updater) also keeps the plugin, because it pulls this
  source tree and rebuilds it.

## Manual installation

The script above does exactly this, step by step, so these instructions are also the way to do it
by hand or to understand what went wrong.

## 0. What has to be installed

| Component | Why | How to check |
|---|---|---|
| Windows 10 version **2004 (build 19041)** or newer, or Windows 11 | capturing the audio of individual applications | `[Environment]::OSVersion.Version.Build` → must be ≥ 19041 |
| Discord Desktop (the normal client, not Vesktop) | the target client | — |
| Node.js 22+ | building Vencord (`install.ps1`); 18+ is enough when you only build the addon | `node -v` |
| Visual Studio Build Tools with the "Desktop development with C++" workload | the MSVC compiler | see below |
| Git (recommended), pnpm (part of corepack or fetched by npm) | building Vencord, updating it later | `git --version`, `pnpm -v` |

If you do not need to build anything (for example because you already have a ready
`streamaudiofilter.node` from your own build on another machine), steps 1, 5 and 6 are enough.

Checking the compiler:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest `
    -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
```

Empty → install
[Build Tools for Visual Studio](https://visualstudio.microsoft.com/visual-cpp-build-tools/) with the
"Desktop development with C++" workload.

## 1. Get Vencord from source

The ready made Vencord installer ships compiled, which is why user plugins require your own build:

```powershell
git clone https://github.com/Vencord/Vencord
cd Vencord
corepack enable pnpm
pnpm install --frozen-lockfile
```

## 2. Put the plugin in place

```powershell
# from the folder this repository was downloaded/cloned into
xcopy /E /I /Y .\StreamAudioFilter  <path-to>\Vencord\src\userplugins\StreamAudioFilter
```

Important: the folder must be called `StreamAudioFilter`, it must live inside `src\userplugins`, and it
must contain `index.tsx` (it does). The folders `native`, `docs`, `scripts`, `dev` and `tests` do not
have to be copied into Vencord — the plugin does not need them, but they do no harm either.

## 3. Build the native addon

```powershell
cd <path-to>\StreamAudioFilter
powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
```

The script:

1. checks for Node.js and the compiler and prints links if something is missing;
2. prints the command it is about to run and asks for confirmation (it uses `npx node-gyp`, so npm only
   delivers the build tool; the compiler and the sources are yours, nothing executable is fetched from
   the internet);
3. builds `native/build/Release/streamaudiofilter.node`;
4. **verifies the addon**: it loads it into Node and prints the real result of the support check
   (version, Windows build, verdict, reason, the HRESULT of a refusal).

To put the finished addon where it belongs in one go:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1 -Yes `
    -Destination "$env:APPDATA\Vencord\streamAudioFilter\native"
```

## 4. Where the plugin looks for the addon

In order:

1. the path from the **"Path to streamaudiofilter.node"** setting (if it is set);
2. `%APPDATA%\Vencord\streamAudioFilter\native\streamaudiofilter.node`
   (and `…\native\build\Release\streamaudiofilter.node`);
3. a path next to the built `dist` (for a Vencord source tree);
4. any `*.node` in `%APPDATA%\Vencord\streamAudioFilter`.

The **"Open folder"** button in the diagnostics section creates and opens the right folder, and the
diagnostics themselves show **every** path that was tried plus the reason the load failed, if the addon
did not fit.

## 5. Build and inject Vencord

```powershell
pnpm build
pnpm inject     # or run VencordInstaller and pick your Discord installation
```

Because the plugin adds a patch (the button next to the microphone), Discord has to be restarted after
the plugin is enabled — Vencord warns about that itself.

## 6. First run and check

1. Open **Settings → Plugins** and enable **StreamAudioFilter**.
2. Open **Settings → Plugins → StreamAudioFilter → the plugin panel**.
3. Press **"Check support"** in the diagnostics section:
   * "supported: yes" — everything is ready;
   * "supported: no" with a code and a text — either the system cannot do process loopback (see
     `LIMITATIONS.md`) or the addon did not load (then every path that was tried is listed next to it).
4. Start some music in an application and press **"Refresh list"** — the application appears in the
   "Currently playing audio" section.
5. Start a screen share with audio and make sure the panel says "filtering active" and that the viewers
   hear what you expect.

## Common problems

| Symptom | Cause and solution |
|---|---|
| "The native addon was not found" | the addon was not built or lies somewhere else. The panel lists every path it tried; press "Open folder" and put the file there |
| The addon is found but does not load | almost always a bitness mismatch or a missing Node-API: build it again with the command from step 3 (an x64 build) |
| "Filtering is not available on this system" | Windows 10 older than version 2004 (build < 19041): the operating system itself has no per process capture. Update Windows. From 2004 on, the check is a real activation attempt and the diagnostics show its result or its error code |
| A share is running, but the panel says "filtering not applied" | Discord did not ask for the audio ("Share audio" is off), or the request went through a different API. The latter signals an incompatibility with this Discord version: please send the contents of the diagnostics section |
| The viewers hear silence although the panel shows that audio is being sent | check that "Audio" is selected for the share in the Discord mixer and that the application is not muted or at volume 0 in the Windows volume mixer |
| The panel says "Stream audio is off: filtering could not be applied" and the viewers hear no share audio | this is the leak protection: the filter did not work, so the unfiltered audio was not sent. The reason is in the same card and in the diagnostics section. If you cannot live with that, enable the "Send plain system audio when filtering is unavailable" setting |
| The panel says "Sending unfiltered system audio" | that setting is on and filtering is currently unavailable — the viewers hear every application |
| Clicks or dropouts | raise the "audio buffer headroom" in the plugin settings to 90–120 ms |
| `install.ps1` is blocked by PowerShell | the file came from the internet: run `Get-ChildItem -Recurse | Unblock-File` in the repository folder and try again |
| `install.ps1` says the plugin is not inside the build | `src\userplugins\StreamAudioFilter\index.tsx` is missing — copy the repository folder again and re-run; the check exists so that a silent "installed but absent" cannot happen |
| Discord runs but Vencord is gone after an update | Discord replaced its files: run `... -Reinject` (or `cd <VencordDir>; pnpm inject`) |
