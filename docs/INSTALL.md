# Installation

The plugin has two parts:

* **the TypeScript part** — an ordinary Vencord user plugin (`index.tsx` and the folders next to it);
* **the native addon** — `streamaudiofilter.node`, built from the `native/` folder on your own machine,
  because it talks to WASAPI directly.

The first part is installed by copying, the second one by building. Nothing is downloaded or installed
automatically.

## 0. What has to be installed

| Component | Why | How to check |
|---|---|---|
| Windows 10 version **2004 (build 19041)** or newer, or Windows 11 | capturing the audio of individual applications | `[Environment]::OSVersion.Version.Build` → must be ≥ 19041 |
| Discord Desktop (the normal client, not Vesktop) | the target client | — |
| Node.js 18+ | building the native addon and Vencord | `node -v` |
| Visual Studio Build Tools with the "Desktop development with C++" workload | the MSVC compiler | see below |
| Git, pnpm (part of corepack) | building Vencord | `git --version`, `pnpm -v` |

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
