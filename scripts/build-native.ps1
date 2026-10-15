<#
    StreamAudioFilter — native module build script (Windows 10/11, x64)

    Builds native/build/Release/streamaudiofilter.node from the sources in this
    repository with the compiler that is already installed on your machine.

    What this script does NOT do:
      * it never downloads or runs an executable from the internet;
      * it does not install anything system wide;
      * it does not need administrator rights;
      * it does not touch audio devices or system sound settings.

    What it needs:
      * Node.js (18 or newer) — https://nodejs.org/  (only used as the build
        host; the module uses Node-API and therefore also works inside Discord's
        Electron regardless of its Node version)
      * Visual Studio Build Tools with the workload "Desktop development with
        C++" (or a full Visual Studio installation). If nothing is installed,
        this script prints the link instead of installing it for you.

    Usage:
        powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
        powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1 -Yes
        powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1 -Destination "$env:APPDATA\Vencord\streamAudioFilter\native"
#>

[CmdletBinding()]
param(
    # Skip the confirmation prompt (for scripted use).
    [switch] $Yes,

    # Where the built module is copied to. Empty = only build, print the path.
    [string] $Destination = "",

    # Build configuration, do not change unless you know why.
    [string] $Configuration = "Release",

    # Use the Electron headers of Discord instead of the local Node headers.
    # Only needed for very old Node versions; Node-API makes this optional.
    [switch] $ElectronHeaders
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$nativeDir = Join-Path $root "native"
$output = Join-Path $nativeDir "build\$Configuration\streamaudiofilter.node"

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Write-Warn($text) { Write-Host "!! $text" -ForegroundColor Yellow }
function Write-Ok($text) { Write-Host "ok $text" -ForegroundColor Green }

Write-Host "StreamAudioFilter — native module build" -ForegroundColor White
Write-Host "sources: $nativeDir"

if (-not (Test-Path (Join-Path $nativeDir "binding.gyp"))) {
    throw "binding.gyp not found in $nativeDir — run this script from the repository."
}

# ----------------------------------------------------------------- Node.js ---

Write-Step "Looking for Node.js"
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Warn "Node.js was not found."
    Write-Host @"
Install the LTS version from https://nodejs.org/ (a normal installer, no admin
rights needed for a per-user installation), reopen this window and run the script
again. Node.js is only used to compile the module; it is not needed afterwards.
"@
    exit 2
}
$nodeVersion = (& node -v).Trim()
Write-Ok "node $nodeVersion"

# --------------------------------------------------------------- MSVC check ---

Write-Step "Looking for a C++ compiler (Visual Studio Build Tools)"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$hasMsvc = $false
if (Test-Path $vswhere) {
    $installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)
    if ($installPath) { $hasMsvc = $true; Write-Ok "MSVC found: $installPath" }
}
if (-not $hasMsvc) {
    Write-Warn "No Visual Studio C++ build tools found."
    Write-Host @"
Install "Build Tools for Visual Studio" and select the workload
"Desktop development with C++":
    https://visualstudio.microsoft.com/visual-cpp-build-tools/

Alternative without Visual Studio: install the MSYS2 / MinGW-w64 toolchain and
build with 'node-gyp rebuild -- -Duse_mingw=true', but the officially supported
and tested path is MSVC.
"@
    exit 3
}

# ------------------------------------------------------------ confirmation ---

$gypArgs = @("node-gyp", "rebuild", "--verbose", "-C", $nativeDir, "--arch=x64")
if ($ElectronHeaders) {
    $gypArgs += @("--dist-url=https://electronjs.org/headers")
}

Write-Host ""
Write-Host "The following command will be executed:" -ForegroundColor White
Write-Host "    npx --yes $($gypArgs -join ' ')"
Write-Host @"
npx fetches the build tool 'node-gyp' from the npm registry (a few hundred
kilobytes of JavaScript) because it is not part of Node.js itself. No other
download happens: the compiler is the one installed on this machine, the sources
are the ones in this repository.
"@
if (-not $Yes) {
    $answer = Read-Host "Continue? [y/N]"
    if ($answer -notmatch '^(y|Y|yes|Yes)$') {
        Write-Host "Cancelled."
        exit 1
    }
}

# ------------------------------------------------------------------- build ---

Write-Step "Building (this takes about a minute)"
Push-Location $root
try {
    & npx @gypArgs
    $exit = $LASTEXITCODE
}
finally {
    Pop-Location
}

if ($exit -ne 0 -or -not (Test-Path $output)) {
    Write-Warn "Build failed (exit code $exit)."
    Write-Host "The compiler output above contains the reason. Common causes:"
    Write-Host "  * the C++ workload is not installed in Visual Studio;"
    Write-Host "  * a path with non-ASCII characters (move the repository, e.g. to C:\StreamAudioFilter);"
    Write-Host "  * an old node-gyp in the npm cache: run 'npx --yes node-gyp@latest rebuild' manually."
    exit 1
}

Write-Ok "built: $output"

# --------------------------------------------------------------- smoke test ---

Write-Step "Checking that the module loads and reports the audio status"
$probe = @"
const path = require('path');
const addon = require(path.resolve(process.argv[1]));
const info = JSON.parse(addon.probe('auto'));
console.log('version      :', JSON.parse(addon.version()).version);
console.log('system       :', info.os, 'build', info.build);
console.log('filtering    :', info.supported ? 'supported' : 'NOT supported');
console.log('reason       :', info.reason);
if (info.detail) console.log('detail       :', info.detail);
for (const check of info.checks) console.log('  -', check);
"@
$probeFile = Join-Path $env:TEMP "streamaudiofilter-probe.js"
Set-Content -Path $probeFile -Value $probe -Encoding UTF8
& node $probeFile $output
Remove-Item $probeFile -ErrorAction SilentlyContinue

# ------------------------------------------------------------------ deploy ---

if ($Destination -ne "") {
    Write-Step "Copying the module to $Destination"
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item $output (Join-Path $Destination "streamaudiofilter.node") -Force
    Write-Ok "installed: $(Join-Path $Destination 'streamaudiofilter.node')"
}

Write-Host ""
Write-Host "Done. Next steps:" -ForegroundColor White
Write-Host "  1. Put streamaudiofilter.node where the plugin can find it, by default:"
Write-Host "       %APPDATA%\Vencord\streamAudioFilter\native\streamaudiofilter.node"
Write-Host "     (the plugin's diagnostics section shows the exact folder and can open it;"
Write-Host "      a different location can be set in the plugin settings)"
Write-Host "  2. Restart Discord, enable StreamAudioFilter in Vencord's plugin list."
Write-Host "  3. Open the plugin panel and press 'Refresh list'."
