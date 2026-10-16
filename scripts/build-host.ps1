<#
.SYNOPSIS
    Builds saf-audio-host.exe (and, with -Setup, saf-setup.exe) with MSVC.

.DESCRIPTION
    StreamAudioFilter — native build. Needs Visual Studio 2019/2022 (or the
    Build Tools) with the "Desktop development with C++" workload. Nothing is
    downloaded. The same script is used by CI, so a local build and a release
    build are made the same way.

    Output: native\build\host\saf-audio-host.exe
            native\build\setup\saf-setup.exe  (with -Setup and -PayloadDir)

.EXAMPLE
    .\scripts\build-host.ps1
.EXAMPLE
    .\scripts\build-host.ps1 -Setup -PayloadDir C:\work\payload
#>
[CmdletBinding()]
param(
    [switch]$Setup,
    # Folder with dist\ (Vencord build) and saf-audio-host.exe, embedded into saf-setup.exe.
    [string]$PayloadDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$native = Join-Path $root "native"

function Find-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "Visual Studio (C++ build tools) was not found: vswhere.exe is missing." }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) { throw "Visual Studio is installed, but without the C++ tools (workload 'Desktop development with C++')." }
    return Join-Path $path "VC\Auxiliary\Build\vcvars64.bat"
}

function Invoke-Cl([string]$Arguments, [string]$Log) {
    $vcvars = Find-VsDevCmd
    cmd /c "`"$vcvars`" >nul && cd /d `"$native`" && cl $Arguments > `"$Log`" 2>&1"
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Get-Content $Log | Select-String -Pattern "error|warning C" | ForEach-Object { Write-Host $_ }
        throw "the compiler failed (exit $code), full log: $Log"
    }
}

$common = "/nologo /std:c++17 /EHsc /utf-8 /W4 /wd4100 /O2 /MT /D_USE_MATH_DEFINES /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00"

# ---------------------------------------------------------------- audio host
$hostDir = Join-Path $native "build\host"
New-Item -ItemType Directory -Force $hostDir | Out-Null
$hostSources = @(
    "src\host\host_main.cc", "src\null_sink.cc", "src\win\render_sink.cc", "src\win\remote_sink.cc",
    "src\app_identity.cc", "src\capture_group.cc", "src\engine.cc", "src\hr_advice.cc", "src\platform.cc",
    "src\dsp\convert.cc", "src\dsp\mixer.cc", "src\dsp\resampler.cc", "src\dsp\ring_buffer.cc", "src\mock\mock_backend.cc",
    "src\win\process_loopback.cc", "src\win\session_watcher.cc", "src\win\windows_util.cc"
) -join " "
$hostLibs = "ole32.lib oleaut32.lib uuid.lib version.lib advapi32.lib mmdevapi.lib ksuser.lib user32.lib"
Invoke-Cl "$common /Fo:build\host\ /Fe:build\host\saf-audio-host.exe $hostSources /link $hostLibs" (Join-Path $hostDir "build.log")
Write-Host "built: $(Join-Path $hostDir 'saf-audio-host.exe')"

# --------------------------------------------------------------------- setup
if ($Setup) {
    $setupDir = Join-Path $native "build\setup"
    New-Item -ItemType Directory -Force $setupDir | Out-Null
    $payload = Join-Path $setupDir "payload.bin"
    if ($PayloadDir) {
        & node (Join-Path $root "scripts\pack-payload.mjs") $PayloadDir $payload
        if ($LASTEXITCODE -ne 0) { throw "packing the payload failed" }
    } else {
        # No embedded data: this saf-setup.exe is used with --payload-dir only
        # (scripts/install.ps1 does that with a Vencord build made from source).
        [System.IO.File]::WriteAllText($payload, "SAFPAYLOAD1`n`n")
    }

    $vcvars = Find-VsDevCmd
    $rc = Join-Path $setupDir "setup.rc"
    "1 RCDATA `"payload.bin`"`r`n1 24 `"..\..\src\setup\setup.manifest`"" | Set-Content -Encoding ascii $rc
    cmd /c "`"$vcvars`" >nul && cd /d `"$setupDir`" && rc /nologo /fo setup.res setup.rc"
    if ($LASTEXITCODE -ne 0) { throw "the resource compiler failed" }

    Invoke-Cl "$common /Fo:build\setup\ /Fe:build\setup\saf-setup.exe src\setup\setup_main.cc src\setup\installer.cc build\setup\setup.res /link user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib advapi32.lib /SUBSYSTEM:WINDOWS" (Join-Path $setupDir "build.log")
    Write-Host "built: $(Join-Path $setupDir 'saf-setup.exe')"
}
