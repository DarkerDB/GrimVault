<#
.SYNOPSIS
   Configure + build + run GrimVault locally in dev mode (debug, no signing,
   no updates, resources pulled from the repo).

.PARAMETER Preset
   CMake preset (default: windows-msvc-debug).

.PARAMETER Build
   Force a clean reconfigure before building.

.PARAMETER NoRun
   Skip the launch step. Useful when you just want to verify the build.

.PARAMETER NoDebug
   Launch without --debug (no verbose logs or OCR stage dumps). Diagnostic
   borders are separately opt-in via --debug=highlight:objects,highlight:game.

.PARAMETER DetectOnly
   Stop after tooltip detection (no OCR / lookup / augment). The full
   pipeline is the default.

.PARAMETER Full
   Deprecated compatibility switch. The full pipeline is now the default.

.EXAMPLE
   pwsh tools/dev-run.ps1
   pwsh tools/dev-run.ps1 -Build
   pwsh tools/dev-run.ps1 -Preset windows-msvc-test -NoRun
#>

[CmdletBinding ()]
param (
   # RelWithDebInfo by default: the Debug preset links debug OpenCV, which
   # makes detection ~10x slower (350ms/frame) — useless for overlay work.
   [string] $Preset = "windows-msvc-test",
   [ValidateSet ("dev", "qa", "prod")]
   [string] $Env    = "dev",
   [switch] $Build,
   [switch] $NoRun,
   [switch] $NoDebug,
   [switch] $DetectOnly,
   [switch] $Full,

   # Anything unrecognized is handed to grimvault.exe verbatim, so app
   # flags work directly:  pwsh tools/dev-run.ps1 --fcr=60
   [Parameter (ValueFromRemainingArguments)]
   [string[]] $AppArgs = @()
)

$ErrorActionPreference = "Stop"

# Keep UTF-8 OCR text intact in both Windows PowerShell 5.1 and pwsh. Without
# this, valid strings such as "í" are rendered as the OEM-codepage mojibake
# "├¡" even though the log file itself contains correct UTF-8.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

# Invoked from a WSL shell via Linux pwsh: this is a Windows build, so
# re-exec under Windows PowerShell through the W: mapping (see DEV.md).
if ($IsLinux) {
   $winScript = if ($PSCommandPath -like '/mnt/*') { wslpath -w $PSCommandPath }
                else                               { 'W:' + ($PSCommandPath -replace '/', '\') }

   $fwd = @(foreach ($kv in $PSBoundParameters.GetEnumerator()) {
      if ($kv.Value -is [System.Management.Automation.SwitchParameter]) {
         if ($kv.Value) { "-$($kv.Key)" }
      } elseif ($kv.Value -is [System.Array]) {
         # Remaining-args passthrough: re-emit bare so the Windows-side
         # binder collects them into the same parameter again.
         foreach ($item in $kv.Value) { "$item" }
      } else {
         "-$($kv.Key)"; "$($kv.Value)"
      }
   })

   # Snap-confined Linux pwsh has no Windows PATH; resolve the host binary
   # explicitly (pwsh 7 preferred, Windows PowerShell 5.1 fallback).
   $hostPs = @(
      (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
      "/mnt/c/Program Files/PowerShell/7/pwsh.exe"
      "/mnt/c/Users/$env:USER/AppData/Local/Microsoft/WindowsApps/pwsh.exe"
      "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
   ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

   if (-not $hostPs) {
      Write-Host "FAIL:  no Windows PowerShell reachable from WSL" -ForegroundColor Red
      exit 1
   }

   & $hostPs -NoProfile -File $winScript @fwd
   exit $LASTEXITCODE
}

$root = (Resolve-Path "$PSScriptRoot\..").Path

# Source lives in WSL, reached via the W: mapping of \\wsl.localhost\Ubuntu
# (see DEV.md). The build tree must stay on a local Windows drive: MSVC and
# Qt tooling spawn cmd.exe (no UNC cwd), and object/PDB writes over the 9P
# bridge are slow and flaky.
$remote = ($root -like '\\*') -or
          ((Get-PSDrive -Name (Split-Path -Qualifier $root).TrimEnd(':')).DisplayRoot -like '\\*')
$out = if ($remote) { "$env:LOCALAPPDATA\GrimVault\build\$Preset" }
       else         { Join-Path $root "build\$Preset" }

function Fail ([string] $msg) { Write-Host "FAIL:  $msg" -ForegroundColor Red; exit 1 }
function Ok   ([string] $msg) { Write-Host "OK     $msg" -ForegroundColor Green }
function Inf  ([string] $msg) { Write-Host "       $msg" -ForegroundColor DarkGray }

# Re-read PATH from the registry so winget-installed tools (cmake, ninja, git,
# pwsh) are visible in this same session. Without this, freshly-installed tools
# would only appear in new shells.
$env:PATH = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [Environment]::GetEnvironmentVariable("Path", "User")

# Same for VCPKG_ROOT (the only env var dev-setup writes persistently).
if (-not $env:VCPKG_ROOT) {
   $env:VCPKG_ROOT = [Environment]::GetEnvironmentVariable("VCPKG_ROOT", "User")
}

if ($remote) {
   if ($root -like '\\*') {
      Fail @"
Run from the W: drive mapping, not the raw UNC path. Qt's build tooling
(cmd-wrapped qmlimportscanner) needs a drive-lettered source directory:
   net use W: \\wsl.localhost\Ubuntu /persistent:yes
   pwsh W:\home\ethan\.katforge\realms\grimvault\tools\dev-run.ps1
"@
   }
   Inf "source on network drive ($root); build tree -> $out"
}

# Auto-source the VS Developer environment if cl.exe isn't on PATH yet.
# Lets dev-run.ps1 work from any PowerShell, not just "Developer PowerShell
# for VS 2022". Tries vswhere first, then falls back to brute-force search
# of known VS install roots.
function Find-VcVars {
   # 1. vswhere (preferred, comes with any VS install)
   $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
   if (Test-Path $vswhere) {
      $vsPath = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
      if ($vsPath) {
         $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
         if (Test-Path $candidate) { return $candidate }
      }
   }

   # 2. Brute force: any vcvars64.bat under the standard VS roots
   foreach ($root in @(
      "${env:ProgramFiles(x86)}\Microsoft Visual Studio",
      "$env:ProgramFiles\Microsoft Visual Studio"
   )) {
      if (-not (Test-Path $root)) { continue }
      $hit = Get-ChildItem -Path $root -Filter vcvars64.bat -Recurse -ErrorAction SilentlyContinue |
             Select-Object -First 1
      if ($hit) { return $hit.FullName }
   }

   return $null
}

function Initialize-DevEnv {
   if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return $true }

   $vcvars = Find-VcVars
   if (-not $vcvars) {
      Write-Host "       searched for vcvars64.bat:" -ForegroundColor Yellow
      Write-Host "         - vswhere ${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -ForegroundColor Yellow
      Write-Host "         - ${env:ProgramFiles(x86)}\Microsoft Visual Studio\**\vcvars64.bat" -ForegroundColor Yellow
      Write-Host "         - $env:ProgramFiles\Microsoft Visual Studio\**\vcvars64.bat" -ForegroundColor Yellow
      return $false
   }

   Inf "sourcing $vcvars"

   # Run vcvars64 in cmd, dump its env, import into this process.
   $envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
   foreach ($line in $envDump) {
      if ($line -match '^([^=]+)=(.*)$') {
         Set-Item -Path "env:$($matches[1])" -Value $matches[2]
      }
   }

   return [bool] (Get-Command cl.exe -ErrorAction SilentlyContinue)
}

# ---- Prereqs ---------------------------------------------------------------

Write-Host "==> Environment" -ForegroundColor Cyan

foreach ($cmd in @("cmake", "ninja")) {
   if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
      Fail "$cmd not on PATH. Install via winget / choco."
   }
   Ok "$cmd"
}

if (-not $env:VCPKG_ROOT) { Fail "VCPKG_ROOT not set" }
if (-not (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) { Fail "VCPKG_ROOT=$env:VCPKG_ROOT but no vcpkg.exe" }
Ok "VCPKG_ROOT = $env:VCPKG_ROOT"

if (-not (Initialize-DevEnv)) {
   Fail @"
cl.exe not found and could not auto-source vcvars64.bat.
Either:
   - launch 'Developer PowerShell for VS 2022' from Start Menu, then re-run; or
   - install VS 2022 Build Tools (run tools\dev-setup.ps1).
"@
}
Ok "MSVC environment (cl.exe sourced)"

# ---- Configure -------------------------------------------------------------

# Redirect vcpkg's installed dir to a short path. In manifest mode vcpkg puts
# build trees under ${VCPKG_INSTALLED_DIR}/vcpkg/blds/<port>/..., so this
# also moves the deepest paths (qtbase's autogen tree is ~260 chars from the
# repo root). MSVC's cl.exe still trips on MAX_PATH even with LongPathsEnabled,
# so we shorten at the source.
$VCPKG_INSTALLED = "C:\v\i"
New-Item -ItemType Directory -Force -Path $VCPKG_INSTALLED | Out-Null
Inf "vcpkg installed/blds -> $VCPKG_INSTALLED"

$cmakeCache = Join-Path $out "CMakeCache.txt"
$ninjaFile  = Join-Path $out "build.ninja"

# A broken cache = CMakeCache.txt present but build.ninja missing means the
# previous configure failed midway. Treat the build dir as poisoned and
# wipe + reconfigure from scratch.
$cacheBroken = (Test-Path $cmakeCache) -and (-not (Test-Path $ninjaFile))

# Detect env change: if the cache was configured with a different
# GRIMVAULT_ENV value, force reconfigure so the env.h regenerates.
$envChanged = $false
if (Test-Path $cmakeCache) {
   $match = Select-String -Path $cmakeCache -Pattern '^GRIMVAULT_ENV:STRING=(.+)$' -ErrorAction SilentlyContinue
   if ($match) {
      $cachedEnv = $match.Matches[0].Groups[1].Value.Trim()
      if ($cachedEnv -and $cachedEnv -ne $Env) {
         Inf "cached GRIMVAULT_ENV=$cachedEnv differs from requested $Env - forcing reconfigure"
         $envChanged = $true
      }
   }
}

$needConfigure = $Build -or (-not (Test-Path $cmakeCache)) -or $cacheBroken -or $envChanged

if ($needConfigure) {
   Write-Host "`n==> Configure ($Preset)" -ForegroundColor Cyan
   if ($cacheBroken) {
      Inf "previous configure left a broken cache (no build.ninja) - wiping $out"
   }
   if (($Build -or $cacheBroken) -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }

   & cmake -S $root --preset $Preset -B $out `
      "-DVCPKG_INSTALLED_DIR=$VCPKG_INSTALLED" `
      "-DGRIMVAULT_ENV=$Env"
   if ($LASTEXITCODE -ne 0) { Fail "cmake configure exit $LASTEXITCODE" }
   Ok "configure (env: $Env)"
} else {
   Inf "skipping configure (use -Build to force)"
}

# ---- Build -----------------------------------------------------------------

Write-Host "`n==> Build" -ForegroundColor Cyan
& cmake --build $out
if ($LASTEXITCODE -ne 0) { Fail "build exit $LASTEXITCODE" }
Ok "build"

# ---- Install CLI shim ------------------------------------------------------

$exe = Join-Path $out "grimvault.exe"
if (-not (Test-Path $exe)) { Fail "binary not found: $exe" }

# Refresh the C:\Users\<you>\.bin\grimvault.cmd shim to point at this build.
# Runs after build regardless of -NoRun so the shim always tracks the latest
# binary. -NoPrompt keeps dev-run non-interactive.
Write-Host "`n==> Install CLI shim" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "install-cli.ps1") -ExePath $exe -NoPrompt
if ($LASTEXITCODE -ne 0) { Fail "install-cli.ps1 exit $LASTEXITCODE" }

# ---- Run -------------------------------------------------------------------

if ($NoRun) {
   Inf "skipping run (-NoRun)"
   exit 0
}

$runArgs = @()
if (-not $NoDebug) { $runArgs += '--debug' }
if ($DetectOnly)   { $runArgs += '--detect-only' }
if ($AppArgs)      { $runArgs += $AppArgs }

Write-Host "`n==> Run (dev mode$(($runArgs | ForEach-Object { ", $_" }) -join ''))" -ForegroundColor Cyan
Inf "GRIMVAULT_DEV_RESOURCES = $root           (models/i18n/assets resolve from repo)"
Inf "GRIMVAULT_DISABLE_UPDATES = 1             (WinSparkle off)"
Inf "userData                = %LOCALAPPDATA%\GrimVault   (db, logs)"
Inf "Ctrl+C stops the app"

$env:GRIMVAULT_DEV_RESOURCES   = $root
$env:GRIMVAULT_DISABLE_UPDATES = "1"

# grimvault.exe is a GUI-subsystem binary; PowerShell doesn't wait for those
# unless output is piped. The pipe also hands the app real stdout/stderr
# handles, so logs stream here instead of vanishing. Color them here rather
# than forcing spdlog's Windows console sink to write colors into a pipe.
& $exe @runArgs 2>&1 | ForEach-Object {
   $line = $_.ToString()
   $color = if ($line -match '\[(error|critical)\]') {
      'Red'
   } elseif ($line -match '\[(warning|warn)\]') {
      'Yellow'
   } elseif ($line -match 'OCR result') {
      'Cyan'
   } elseif ($line -match '\[ocr\].*title_ready') {
      'Green'
   } elseif ($line -match '\[ocr\]') {
      'DarkCyan'
   } elseif ($line -match '\[debug\]') {
      'DarkGray'
   } else {
      'Gray'
   }
   Write-Host $line -ForegroundColor $color
}
$rc = $LASTEXITCODE

Write-Host "`n==> Exited with code $rc" -ForegroundColor Cyan
exit $rc
