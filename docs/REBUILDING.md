# Rebuilding with a newer Vencord

Use this when a Discord update breaks an older StreamAudioFilter bundle and you
want to build the plugin with a newer official Vencord version. This is a Windows
source-build procedure. The release installer installs a prebuilt bundle; it does
not download or compile Vencord on your machine.

Custom plugins must be included when Vencord is built. Replacing only the audio
host or copying plugin source files into an installed Vencord `dist` does not add
the plugin. See Vencord's [custom-plugin guide](https://docs.vencord.dev/installing/custom-plugins/).

## Requirements

- Windows 10/11, Git and Node.js 22 or newer. Use a newer Node.js version if the
  selected Vencord release requires it.
- Visual Studio 2019/2022 or Build Tools with **Desktop development with C++** and
  the Windows SDK. The script builds the audio host and installer as well.
- Internet access to download Vencord sources and npm dependencies.

The build uses a separate Vencord source directory. Do not point it at a checkout
with your own edits: the script switches versions with `checkout --force` and
replaces its `src/userplugins/StreamAudioFilter` directory.

## 1. Get StreamAudioFilter sources

Open PowerShell in a folder where you keep source projects:

```powershell
git clone https://github.com/dmbai009/StreamAudioFilter.git
if ($LASTEXITCODE -ne 0) { throw "Could not clone StreamAudioFilter" }
Set-Location StreamAudioFilter
```

If you already have a clean checkout, open it and run `git pull --ff-only` instead.
Do not discard local edits to make an update succeed.

## 2. Choose an official Vencord tag

The following reads official tags and selects the highest numbered stable tag.
It ignores moving build tags and prereleases, and prints the selected version so
you can record exactly what you built:

```powershell
$safVencordTags = git ls-remote --tags --refs https://github.com/Vendicated/Vencord.git 'v*'
if ($LASTEXITCODE -ne 0) { throw "Could not read official Vencord tags" }
$safVencordRef = $safVencordTags | ForEach-Object {
    if ($_ -match 'refs/tags/(v\d+\.\d+\.\d+)$') { $Matches[1] }
} | Sort-Object { [version]($_ -replace '^v', '') } | Select-Object -Last 1
if (-not $safVencordRef) { throw "No numbered Vencord release tag found" }
Write-Host "Selected Vencord: $safVencordRef"
```

You can choose a particular tag explicitly instead:

```powershell
$safVencordRef = 'v1.15.17'
```

Vencord v1.15.17 was checked with Discord Stable 1.0.9268, web build 673869, on
2026-11-16. This is a recorded startup check, not a promise that this tag will
remain compatible with future Discord builds. The newest tag is also not a
guarantee of compatibility; newer fixes may still exist only on upstream's main
branch. This script's supported update path uses release tags.

## 3. Build and install

Quit Discord completely, including its tray icon, then run from the
StreamAudioFilter repository in the same PowerShell window:

```powershell
$safVencordDir = Join-Path $env:LOCALAPPDATA 'StreamAudioFilter-build\Vencord'
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\install.ps1 -VencordRef $safVencordRef -VencordDir $safVencordDir
if ($LASTEXITCODE -ne 0) { throw "Build or installation failed; read the error above" }
```

This builds plain Vencord for removal, then Vencord with the current plugin, and
installs the matching audio host through `saf-setup.exe`. It preserves application
rules and other plugins' settings and keeps a Vencord backup. It replaces the
installed Vencord distribution, so other custom plugins must also be included in
your source build if you need their code to remain available.

If multiple Discord variants are installed, add an explicit target to the build
command, for example `-Discord "$env:LOCALAPPDATA\DiscordPTB"`. The normal Stable
target is `-Discord "$env:LOCALAPPDATA\Discord"`.

## 4. Check the result

Start Discord and confirm that it finishes loading. Open Settings → Vencord →
Plugins → StreamAudioFilter and inspect the diagnostic report. Then follow the
[real-machine test plan](TESTPLAN.md#real-machine): share a whole screen with
sound, allow and block different applications, and have a second client confirm
what is audible. Successful compilation and startup do not verify stream audio.

If compilation fails with missing Vencord exports or type errors, the plugin needs
adapting to that Vencord version. If Discord starts but the diagnostic report says
patches are missing, the plugin needs adapting to Discord's current code. Keep the
errors and selected Vencord tag when reporting the problem.

## Updating again or recovering

For the next rebuild, update this repository, select the desired official tag
again, and rerun the same command. Do not use an older prebuilt installer after
rebuilding: it contains its own Vencord version and can replace your newer build.

To remove the plugin, quit Discord and use **Uninstall** in the StreamAudioFilter
installer, leaving **Keep my settings** enabled. When available, the saved Vencord
build is restored. If it is also too old for Discord, update official Vencord
after removing the custom bundle. See [updates and recovery](UPDATES.md).
