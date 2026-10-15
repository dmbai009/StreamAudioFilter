# StreamAudioFilter — one command installer for Windows
#
# Turns a plain Windows machine into "Discord with Vencord + StreamAudioFilter"
# and can be re-run later to update. It deliberately does not download any third
# party executable without asking: every download it wants to make is printed
# first and can be declined (then it stops and says what is missing instead of
# improvising).
#
# Usage (from the folder this repository was unpacked into):
#
#   powershell -ExecutionPolicy Bypass -File scripts\install.ps1
#
# Common variants:
#   ... -Prebuilt latest                                   no build at all (no Node.js either)
#   ... -Prebuilt v0.1.1                                   the same, pinned to one release
#   ... -AddonPath C:\Downloads\streamaudiofilter.node     use a prebuilt addon
#   ... -DownloadAddon latest                              fetch it from the newest release
#   ... -BuildAddon                                        compile it here (needs VS Build Tools)
#   ... -Update                                            refresh Vencord and rebuild
#   ... -Reinject                                          Discord updated: patch it again
#   ... -Yes                                               no questions (see the printed URLs)
#
# Why a build is unavoidable: Vencord does not load plugins from a folder at
# runtime, it bundles them into its own build (`pnpm build` reads
# src\userplugins). There is no plugins directory inside an installed client, so
# a user plugin can only enter through a build of Vencord. What this script
# removes is all the manual work around that build, and the guesswork about what
# went wrong.

[CmdletBinding()]
param(
    # Where the Vencord source tree lives / should be created.
    [string] $VencordDir = (Join-Path $env:USERPROFILE "Vencord"),

    # Refresh an existing tree (git pull) before rebuilding.
    [switch] $Update,

    # Only patch Discord again (after a Discord update) and exit.
    [switch] $Reinject,

    # Install without building anything: download the prebuilt Vencord + plugin
    # archive of that release (a tag, or "latest"). Needs no Node.js, no compiler,
    # no git -- just PowerShell and an internet connection.
    [string] $Prebuilt = "",

    # Where the native addon comes from: all optional, at most one is used.
    [string] $AddonPath = "",
    [string] $DownloadAddon = "",   # a release tag, or "latest"
    [switch] $BuildAddon,

    # Skip `pnpm install` (only useful when node_modules is already complete).
    [switch] $SkipInstall,

    # Answer every question with yes. The script still prints what it downloads.
    [switch] $Yes
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$RepoUrl = "https://github.com/dmbai009/StreamAudioFilter"
$VencordUrl = "https://github.com/Vencord/Vencord"
$PnpmVersion = "11.9.0"          # the version Vencord pins in package.json
$AddonDir = Join-Path $env:APPDATA "Vencord\streamAudioFilter\native"
$AddonName = "streamaudiofilter.node"

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

function Write-Head($text) {
    Write-Host ""
    Write-Host "== $text" -ForegroundColor Cyan
}

function Write-Info($text) {
    Write-Host "   $text"
}

function Write-Ok($text) {
    Write-Host "   $text" -ForegroundColor Green
}

function Write-Note($text) {
    Write-Host "   $text" -ForegroundColor Yellow
}

function Fail($text) {
    Write-Host ""
    Write-Host "!! $text" -ForegroundColor Red
    exit 1
}

function Have($command) {
    return [bool] (Get-Command $command -ErrorAction SilentlyContinue)
}

function Confirm($question) {
    if ($Yes) {
        Write-Info "$question -> yes (-Yes)"
        return $true
    }
    $answer = Read-Host "   $question [y/N]"
    return ($answer -eq "y" -or $answer -eq "Y" -or $answer -eq "yes")
}

# Runs an external command and fails loudly with its exit code.
function Invoke-Tool($file, [string[]] $arguments, $workDir = "") {
    if (-not $arguments) {
        $arguments = @()
    }
    $pretty = "$file $($arguments -join ' ')"
    Write-Info $pretty
    $global:LASTEXITCODE = 0
    if ($workDir) {
        Push-Location $workDir
    }
    try {
        & $file @arguments
        $code = $LASTEXITCODE
    } finally {
        if ($workDir) {
            Pop-Location
        }
    }
    if ($code -ne 0) {
        Fail "`"$pretty`" failed with exit code $code"
    }
}

# pnpm, either the real thing or through npm (Node 25 no longer bundles corepack).
function Get-PnpmCommand {
    if (Have "pnpm") {
        return @("pnpm")
    }
    if (Have "npx") {
        return @("npx", "--yes", "pnpm@$PnpmVersion")
    }
    Fail "Neither pnpm nor npx was found. Both come with Node.js: reinstall Node.js including npm."
}

function Invoke-Pnpm([string[]] $arguments, $workDir) {
    $pnpm = Get-PnpmCommand
    $all = @()
    if ($pnpm.Count -gt 1) {
        $all += $pnpm[1..($pnpm.Count - 1)]
    }
    $all += $arguments
    Invoke-Tool $pnpm[0] $all $workDir
}

function Get-RemoteFile($url, $destination) {
    if (Have "curl.exe") {
        Invoke-Tool "curl.exe" @("-L", "--fail", "--retry", "2", "--silent", "--show-error", "-o", $destination, $url)
        return
    }
    try {
        (New-Object System.Net.WebClient).DownloadFile($url, $destination)
    } catch {
        Fail "could not download $url : $($_.Exception.Message)"
    }
}

# Returns an empty string when the file looks like a Windows node addon.
function Test-AddonFile($path) {
    if (-not (Test-Path $path)) {
        return "does not exist"
    }
    $item = Get-Item $path
    if ($item.Length -lt 100000) {
        return "suspiciously small ($($item.Length) bytes) — that is not the compiled addon"
    }
    $head = [byte[]] @(0, 0)
    $stream = [System.IO.File]::OpenRead($path)
    try {
        $stream.Read($head, 0, 2) | Out-Null
    } finally {
        $stream.Close()
    }
    if ($head[0] -ne 0x4D -or $head[1] -ne 0x5A) {
        return "not a Windows binary (no MZ header)"
    }
    return ""
}

# "latest" means the newest published release; any other value is a tag.
function Get-ReleaseUrl($tag, $asset) {
    if ($tag -eq "latest") {
        return "$RepoUrl/releases/latest/download/$asset"
    }
    return "$RepoUrl/releases/download/$tag/$asset"
}

function Install-Addon($source) {
    $problem = Test-AddonFile $source
    if ($problem) {
        Fail "the addon at $source was rejected: $problem"
    }
    New-Item -ItemType Directory -Force -Path $AddonDir | Out-Null
    $target = Join-Path $AddonDir $AddonName
    Copy-Item -Force $source $target
    $hash = (Get-FileHash $target -Algorithm SHA256).Hash
    Write-Ok "native addon installed: $target"
    Write-Info "SHA-256 $hash"
}

# Is Vencord already injected into Discord? The installer keeps the original
# app.asar as _app.asar next to it, which is a cheap and reliable marker.
function Test-DiscordPatched {
    foreach ($base in @($env:LOCALAPPDATA, $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        foreach ($branch in @("Discord", "DiscordPTB", "DiscordCanary", "DiscordDevelopment")) {
            $dir = Join-Path $base $branch
            if (-not (Test-Path $dir)) { continue }
            $apps = Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like "app-*" }
            foreach ($app in $apps) {
                if (Test-Path (Join-Path $app.FullName "resources\_app.asar")) {
                    return $true
                }
            }
        }
    }
    return $false
}

function Invoke-Inject {
    if (-not (Test-Path (Join-Path $VencordDir "node_modules"))) {
        Fail "Vencord is not installed in $VencordDir (no node_modules). Run the script without -Reinject first."
    }
    Write-Head "Patch Discord"
    Write-Info "This runs Vencord's own inject command, which downloads the official"
    Write-Info "VencordInstallerCli.exe (about 5 MB) from github.com/Vencord/Installer and runs it."
    Write-Info "Discord has to be closed while this happens."

    if (Get-Process -Name "discord" -ErrorAction SilentlyContinue) {
        Write-Note "Discord is running. Quit it first (tray icon -> right click -> Quit Discord),"
        Write-Note "otherwise the patch cannot replace the files it needs."
        if (-not (Confirm "Discord is closed and I may continue?")) {
            Fail "Close Discord completely and run this script again."
        }
    }

    if (-not (Confirm "Download and run the official Vencord installer CLI now?")) {
        Write-Note "Skipped. Patch it later with:  cd `"$VencordDir`"; pnpm inject"
        return
    }
    Invoke-Pnpm @("inject") $VencordDir
    Write-Ok "Discord is patched with your build"
}

# The no-build route: prebuilt Vencord + plugin archive plus the addon, both
# from a release of this repository. Nothing is compiled, nothing but those two
# files is downloaded, and every download is confirmed first.
function Install-Prebuilt($tag) {
    $bundleName = "streamaudiofilter-vencord-dist.zip"
    $distDir = Join-Path $env:APPDATA "Vencord\dist"

    Write-Head "Prebuilt bundle ($tag)"
    Write-Info "This route needs no Node.js, no compiler and no git."

    $bundleUrl = Get-ReleaseUrl $tag $bundleName
    $addonUrl = Get-ReleaseUrl $tag $AddonName
    Write-Info "download: $bundleUrl"
    Write-Info "          $addonUrl"
    Write-Info "(both built by CI for release '$tag'; the release page lists their SHA-256)"
    if (-not (Confirm "Download the prebuilt Vencord and the native addon now?")) {
        Fail "Nothing to install without them. The files can also be downloaded by hand from $RepoUrl/releases."
    }

    $zip = Join-Path $env:TEMP $bundleName
    Get-RemoteFile $bundleUrl $zip
    $unpacked = Join-Path $env:TEMP "streamaudiofilter-bundle"
    if (Test-Path $unpacked) {
        Remove-Item -Recurse -Force $unpacked
    }
    Expand-Archive -Path $zip -DestinationPath $unpacked
    $payload = Join-Path $unpacked "Vencord-dist"
    if (-not (Test-Path (Join-Path $payload "patcher.js"))) {
        Fail "the archive does not contain Vencord-dist\patcher.js — please report this."
    }

    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    Copy-Item -Recurse -Force (Join-Path $payload "*") $distDir
    Write-Ok "Vencord with the plugin is in $distDir"

    # Same verification as the build route: do not trust the archive blindly.
    foreach ($part in @("patcher.js", "renderer.js")) {
        $file = Join-Path $distDir $part
        if (-not (Test-Path $file)) {
            Fail "$file is missing after unpacking the bundle."
        }
    }
    if (-not ((Select-String -Path (Join-Path $distDir "renderer.js") -Pattern "StreamAudioFilter" -Quiet) -and
              (Select-String -Path (Join-Path $distDir "patcher.js") -Pattern "StreamAudioFilter" -Quiet))) {
        Fail "the unpacked Vencord does not contain the plugin — please report this."
    }
    Write-Ok "checked: the plugin is inside the unpacked Vencord"

    $addonTemp = Join-Path $env:TEMP $AddonName
    Get-RemoteFile $addonUrl $addonTemp
    Install-Addon $addonTemp
    Remove-Item -Force $addonTemp, $zip

    if (-not (Test-DiscordPatched)) {
        Write-Host ""
        Write-Note "Discord does not look patched yet (no _app.asar next to app.asar)."
        Write-Note "Run the official Vencord installer once — https://vencord.dev/download —"
        Write-Note "and start it again afterwards. Vencord then loads exactly these files."
    } else {
        Write-Ok "Discord is already patched, it will load this Vencord"
    }

    Write-Host ""
    Write-Host "Next:"
    Write-Host "  1. Start (or restart) Discord."
    Write-Host "  2. Settings -> Plugins -> enable StreamAudioFilter."
    Write-Host "  3. Open its panel and press `"Check support`" in the diagnostics."
    Write-Host ""
    Write-Host "Remember: Vencord's own updater is switched off inside this bundle, so an"
    Write-Host "update never removes the plugin. For a newer Vencord, download a newer"
    Write-Host "release of this repository and run this command again."
}

# --------------------------------------------------------------------------
# 0. What happens now
# --------------------------------------------------------------------------

Write-Host "StreamAudioFilter installer" -ForegroundColor Magenta
Write-Host "Builds Vencord with the plugin and patches Discord with it."
Write-Host "No administrator rights, no audio device or sound setting is touched."

if (-not $PSBoundParameters.ContainsKey("VencordDir")) {
    Write-Info "Vencord source folder: $VencordDir  (change it with -VencordDir <path>)"
}

$repoRoot = Split-Path -Parent $PSScriptRoot

if ($Prebuilt) {
    Install-Prebuilt $Prebuilt
    exit 0
}

if (-not (Test-Path (Join-Path $repoRoot "index.tsx"))) {
    # The script was copied out of the repository (that is a supported way to
    # use it): the plugin sources are fetched as a plain source archive.
    $zipUrl = "$RepoUrl/archive/refs/heads/main.zip"
    Write-Note "The plugin sources are not next to this script."
    Write-Info "download: $zipUrl"
    Write-Info "(the source archive of this repository; the script asks before fetching it)"
    if (-not (Confirm "Download the plugin sources now?")) {
        Fail "Without the plugin sources there is nothing to install. Unpack the repository and run scripts\install.ps1 from it instead."
    }
    $zip = Join-Path $env:TEMP "streamaudiofilter-src.zip"
    $tmp = Join-Path $env:TEMP "streamaudiofilter-src"
    Get-RemoteFile $zipUrl $zip
    if (Test-Path $tmp) {
        Remove-Item -Recurse -Force $tmp
    }
    Expand-Archive -Path $zip -DestinationPath $tmp
    $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
    $repoRoot = $inner.FullName
    if (-not (Test-Path (Join-Path $repoRoot "index.tsx"))) {
        Fail "the downloaded archive does not contain index.tsx — please report this."
    }
    Write-Ok "plugin sources are in $repoRoot"
}

# --------------------------------------------------------------------------
# 1. Node.js
# --------------------------------------------------------------------------

Write-Head "Node.js"
if (-not (Have "node")) {
    Fail @"
Node.js was not found. Vencord requires Node.js 22 or newer.
Install it from https://nodejs.org (the LTS version), reopen PowerShell and run this script again.
This script downloads no installers itself; that one is your choice.
"@
}

$nodeVersion = (& node -v) -replace "^v", ""
$nodeMajor = [int] ($nodeVersion -split "\.")[0]
Write-Ok "node $nodeVersion"
if ($nodeMajor -lt 22) {
    Fail "Node.js $nodeVersion is too old: Vencord needs 22 or newer (https://nodejs.org)."
}

# --------------------------------------------------------------------------
# 2. Patch Discord again and stop (used after a Discord update)
# --------------------------------------------------------------------------

if ($Reinject) {
    Invoke-Inject
    Write-Host ""
    Write-Host "Done: start Discord." -ForegroundColor Green
    exit 0
}

# --------------------------------------------------------------------------
# 3. Vencord source tree
# --------------------------------------------------------------------------

Write-Head "Vencord source"
$distDir = Join-Path $VencordDir "dist"
$haveTree = Test-Path (Join-Path $VencordDir "package.json")

if ($haveTree) {
    Write-Ok "found an existing build tree: $VencordDir"
    if ($Update) {
        if ((Test-Path (Join-Path $VencordDir ".git")) -and (Have "git")) {
            Write-Info "git pull (your own additions, this plugin among them, stay in place)"
            Invoke-Tool "git" @("pull", "--ff-only") $VencordDir
        } else {
            Write-Note "not a git clone: skipping the update, the current sources will be rebuilt"
        }
    }
} else {
    Write-Info "Vencord has not been built here yet:"
    Write-Info "  source: $VencordUrl"
    Write-Info "  target: $VencordDir"
    if (-not (Confirm "Get the Vencord source now?")) {
        Fail "Without the Vencord source there is nothing to build."
    }

    if (Have "git") {
        $parent = Split-Path -Parent $VencordDir
        if ($parent -and -not (Test-Path $parent)) {
            New-Item -ItemType Directory -Force -Path $parent | Out-Null
        }
        # A shallow clone is enough to build, and later git pull keeps working.
        Invoke-Tool "git" @("clone", "--depth", "1", $VencordUrl, $VencordDir)
    } else {
        # Without git, GitHub's source archive is used: a plain zip, no executable.
        Write-Note "git was not found, using the source zip of the main branch instead."
        Write-Note "Vencord's built-in updater needs git; installing Git is recommended."
        $zip = Join-Path $env:TEMP "vencord-src.zip"
        $url = "https://codeload.github.com/Vencord/Vencord/zip/refs/heads/main"
        Write-Info "download: $url"
        Get-RemoteFile $url $zip
        $tmp = Join-Path $env:TEMP "vencord-src"
        if (Test-Path $tmp) {
            Remove-Item -Recurse -Force $tmp
        }
        Expand-Archive -Path $zip -DestinationPath $tmp
        $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
        New-Item -ItemType Directory -Force -Path $VencordDir | Out-Null
        Copy-Item -Recurse -Force (Join-Path $inner.FullName "*") $VencordDir
        Remove-Item -Recurse -Force $tmp, $zip
    }
    Write-Ok "Vencord source is in $VencordDir"
}

# --------------------------------------------------------------------------
# 4. Dependencies
# --------------------------------------------------------------------------

if ($SkipInstall -and (Test-Path (Join-Path $VencordDir "node_modules"))) {
    Write-Head "Dependencies"
    Write-Ok "skipped (-SkipInstall)"
} else {
    Write-Head "Dependencies (a few minutes on the first run)"
    if (Have "corepack") {
        & corepack enable 2>$null | Out-Null
    }
    Invoke-Pnpm @("install", "--frozen-lockfile") $VencordDir
    Write-Ok "dependencies installed"
}

# --------------------------------------------------------------------------
# 5. The plugin sources
# --------------------------------------------------------------------------

Write-Head "Plugin sources"
$pluginTarget = Join-Path $VencordDir "src\userplugins\StreamAudioFilter"

if (Test-Path $pluginTarget) {
    Write-Info "replacing the previous copy in $pluginTarget"
    Remove-Item -Recurse -Force $pluginTarget
}
New-Item -ItemType Directory -Force -Path $pluginTarget | Out-Null

# robocopy keeps the layout; the excluded folders must never be copied.
$exclude = @("node_modules", "build", "ci-out", ".git")
$robocopyArgs = @($repoRoot, $pluginTarget, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/XD")
$robocopyArgs += ($exclude | ForEach-Object { Join-Path $repoRoot $_ })
& robocopy @robocopyArgs | Out-Null
if ($LASTEXITCODE -ge 8) {
    Fail "copying the plugin failed (robocopy exit code $LASTEXITCODE)"
}
if (-not (Test-Path (Join-Path $pluginTarget "index.tsx"))) {
    Fail "the copy is incomplete: index.tsx is missing in $pluginTarget"
}
Write-Ok "copied to $pluginTarget"

# --------------------------------------------------------------------------
# 6. Build Vencord with the plugin
# --------------------------------------------------------------------------

Write-Head "Build"
if (-not (Test-Path (Join-Path $VencordDir "scripts\build\build.mjs"))) {
    Fail "the Vencord tree in $VencordDir looks incomplete (scripts\build\build.mjs is missing)."
}
Invoke-Tool "node" @("scripts/build/build.mjs") $VencordDir

$renderer = Join-Path $distDir "vencordDesktopRenderer.js"
if (-not (Test-Path $renderer)) {
    Fail "the build reported success but $renderer is missing — please send the output above."
}
if (-not (Select-String -Path $renderer -Pattern "StreamAudioFilter" -Quiet)) {
    Fail @"
the build finished, but the plugin is not inside it: $renderer does not mention StreamAudioFilter.
That means the copy in src\userplugins was not picked up — check that
  $pluginTarget\index.tsx
still exists and that nothing else removed it, then run this script again.
"@
}
Write-Ok "built and verified: $renderer contains the plugin"

# --------------------------------------------------------------------------
# 7. The native audio addon
# --------------------------------------------------------------------------

Write-Head "Native audio addon"
$installed = Join-Path $AddonDir $AddonName
$skipAddon = $false

if ($AddonPath) {
    Write-Info "using the file passed with -AddonPath"
    Install-Addon $AddonPath
} elseif ($DownloadAddon) {
    $url = Get-ReleaseUrl $DownloadAddon $AddonName
    Write-Info "download: $url"
    Write-Info "(built by CI for release '$DownloadAddon'; the release page lists its SHA-256)"
    if (-not (Confirm "Download the prebuilt addon from GitHub?")) {
        $skipAddon = $true
    } else {
        $temp = Join-Path $env:TEMP $AddonName
        Get-RemoteFile $url $temp
        Install-Addon $temp
        Remove-Item -Force $temp
    }
} elseif ($BuildAddon) {
    Write-Info "compiling with MSVC (needs Node.js and the C++ build tools)"
    Invoke-Tool "powershell" @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $repoRoot "scripts\build-native.ps1"), "-Yes", "-Destination", $AddonDir) $repoRoot
} elseif (Test-Path (Join-Path $repoRoot "native\build\Release\$AddonName")) {
    Write-Info "using the addon built earlier inside this repository"
    Install-Addon (Join-Path $repoRoot "native\build\Release\$AddonName")
} elseif (Test-Path $installed) {
    Write-Ok "an addon is already in place: $installed"
} else {
    $skipAddon = $true
}

if ($skipAddon) {
    Write-Note "No native addon was installed."
    Write-Note "The plugin will appear and work as a switch, but it will NOT filter audio:"
    Write-Note "with filtering on it removes the share audio and explains why (fail closed)."
    Write-Note "Add it later in any of these ways:"
    Write-Note "  * copy $AddonName to  $AddonDir"
    Write-Note "  * re-run this script with  -DownloadAddon latest"
    Write-Note "  * re-run this script with  -BuildAddon"
}

# --------------------------------------------------------------------------
# 8. Patch Discord
# --------------------------------------------------------------------------

Invoke-Inject

# --------------------------------------------------------------------------
# 9. Done
# --------------------------------------------------------------------------

Write-Host ""
Write-Host "Installed." -ForegroundColor Green
Write-Host ""
Write-Host "Next:"
Write-Host "  1. Start Discord (Vencord will say that a restart is needed)."
Write-Host "  2. Settings -> Plugins -> enable StreamAudioFilter."
Write-Host "  3. Open its panel and press `"Check support`" in the diagnostics."
Write-Host ""
Write-Host "Later:"
Write-Host "  * a Discord update removes the patch: run this script with -Reinject"
Write-Host "  * a plugin or Vencord update:          run this script with -Update"
Write-Host "  * Vencord's own updater (Settings -> Vencord -> Updater) also keeps the"
Write-Host "    plugin: it pulls this source tree and rebuilds it."
