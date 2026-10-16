# StreamAudioFilter — install from source (for developers and advanced users)
#
# Most users want StreamAudioFilter-Setup.exe from the releases page instead:
# it needs no tools at all.
#
# This script builds everything from source and then installs it with
# saf-setup.exe, the same program and the same code as the release installer,
# so both ways behave identically (backup, rollback, uninstall).
#
# Needs: git, Node.js 20+ (pnpm is fetched through npx), Visual Studio 2019/2022
# or its Build Tools with "Desktop development with C++". Nothing is downloaded
# except Vencord's sources and its npm dependencies, and the script prints that
# before it happens.
#
# Usage (from the repository folder):
#   powershell -ExecutionPolicy Bypass -File scripts\install.ps1
#   ... -Discord "$env:LOCALAPPDATA\DiscordPTB"   a specific Discord
#   ... -Uninstall                                remove StreamAudioFilter again

[CmdletBinding()]
param(
    # Where the Vencord source tree lives / should be created.
    [string] $VencordDir = (Join-Path $env:USERPROFILE "Vencord"),
    # The Vencord release to build (the release bundle uses the same one).
    [string] $VencordRef = "v1.15.11",
    # A specific Discord folder (default: the first one found).
    [string] $Discord = "",
    [switch] $Uninstall
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot

# The plugin's files: kept equal to dev/typecheck.sh (CI checks both build it).
$PluginItems = @("types.ts", "native.ts", "nativeBridge.ts", "dataStore.ts", "settings.ts", "i18n.ts", "version.ts", "index.tsx",
    "apps", "components", "stream", "ui", "style.css")

function Step([string]$Text) { Write-Host "`n== $Text" -ForegroundColor Cyan }
function Need([string]$Command, [string]$Hint) {
    if (-not (Get-Command $Command -ErrorAction SilentlyContinue)) { throw "$Command was not found. $Hint" }
}
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
}

$setupExe = Join-Path $root "native\build\setup\saf-setup.exe"

Step "Native parts (audio host + installer)"
& (Join-Path $PSScriptRoot "build-host.ps1") -Setup

if ($Uninstall) {
    Step "Uninstall"
    Run $setupExe @("--uninstall")
    exit 0
}

Need git "Install Git for Windows: https://git-scm.com/download/win"
Need node "Install Node.js 20 or newer: https://nodejs.org"
$version = (Select-String -Path (Join-Path $root "version.ts") -Pattern 'PLUGIN_VERSION = "([^"]+)"').Matches[0].Groups[1].Value
$protocol = (Select-String -Path (Join-Path $root "version.ts") -Pattern 'PROTOCOL_VERSION = (\d+)').Matches[0].Groups[1].Value

Step "Vencord $VencordRef sources in $VencordDir"
if (-not (Test-Path (Join-Path $VencordDir ".git"))) {
    Write-Host "downloading: https://github.com/Vencord/Vencord ($VencordRef)"
    Run git @("clone", "--depth", "1", "--branch", $VencordRef, "https://github.com/Vencord/Vencord", $VencordDir)
} else {
    Run git @("-C", $VencordDir, "fetch", "--depth", "1", "origin", "tag", $VencordRef)
    Run git @("-C", $VencordDir, "checkout", "--force", $VencordRef)
}

Step "Build plain Vencord and the plugin distribution"
$target = [IO.Path]::GetFullPath((Join-Path $VencordDir "src\userplugins\StreamAudioFilter"))
$expectedParent = [IO.Path]::GetFullPath((Join-Path $VencordDir "src\userplugins"))
if ((Split-Path $target -Parent) -ne $expectedParent) { throw "Invalid plugin directory" }
if (Test-Path $target) { Remove-Item -LiteralPath $target -Recurse -Force }
$cleanDir = Join-Path $root "native\build\clean-dist"
New-Item -ItemType Directory -Force $cleanDir | Out-Null
$pnpm = (Get-Content (Join-Path $VencordDir "package.json") -Raw | ConvertFrom-Json).packageManager -replace '^pnpm@', ''
Push-Location $VencordDir
try {
    $env:CI = "true"
    Run npx @("--yes", "pnpm@$pnpm", "install", "--frozen-lockfile")
    Run npx @("--yes", "pnpm@$pnpm", "build", "--standalone")
    Copy-Item (Join-Path $VencordDir "dist\*") $cleanDir -Force
    New-Item -ItemType Directory -Force $target | Out-Null
    foreach ($item in $PluginItems) { Copy-Item -Recurse (Join-Path $root $item) $target }
    Run npx @("--yes", "pnpm@$pnpm", "build", "--standalone", "--disable-updater")
} finally {
    Pop-Location
}

Step "Install"
$payload = Join-Path $env:TEMP "saf-payload-$PID"
if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory -Force (Join-Path $payload "dist") | Out-Null
Copy-Item (Join-Path $VencordDir "dist\*") (Join-Path $payload "dist")
Copy-Item -Recurse $cleanDir $payload
Copy-Item (Join-Path $root "native\build\host\saf-audio-host.exe") $payload
Set-Content (Join-Path $payload "saf-version.txt") "version=$version`nprotocol=$protocol`nvencord=$VencordRef"
try {
    $arguments = @("--install", "--payload-dir", $payload)
    if ($Discord) { $arguments += @("--discord", $Discord) }
    Run $setupExe $arguments
} finally {
    Remove-Item -Recurse -Force $payload -ErrorAction SilentlyContinue
}
Write-Host "`nDone. Start Discord; StreamAudioFilter is enabled, its panel opens from the button next to the microphone." -ForegroundColor Green
