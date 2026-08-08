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

.EXAMPLE
   pwsh tools/dev-run.ps1
   pwsh tools/dev-run.ps1 -Build
   pwsh tools/dev-run.ps1 -Preset windows-msvc-test -NoRun
#>

[CmdletBinding ()]
param (
   [string] $Preset = "windows-msvc-debug",
   [ValidateSet ("dev", "qa", "prod")]
   [string] $Env    = "dev",
   [switch] $Build,
   [switch] $NoRun
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$out  = Join-Path $root "build\$Preset"

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

# Bail early if the cwd is on a UNC path. cmake/ninja work, but vcpkg's CMD
# scripts trip on UNC and silently degrade.
if ($root -like '\\*') {
   Fail @"
Working directory is on a UNC path:
   $root
Copy the repo to a Windows-native disk first:
   from WSL:  cp -r /home/$env:USERNAME/.katforge/workshop/grimvault \\
                    /mnt/c/Users/$env:USERNAME/src/grimvault
   then:      cd C:\Users\$env:USERNAME\src\grimvault
              pwsh tools\dev-run.ps1
"@
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

   Push-Location $root
   try {
      & cmake --preset $Preset `
         "-DVCPKG_INSTALLED_DIR=$VCPKG_INSTALLED" `
         "-DGRIMVAULT_ENV=$Env"
   } finally { Pop-Location }
   if ($LASTEXITCODE -ne 0) { Fail "cmake configure exit $LASTEXITCODE" }
   Ok "configure (env: $Env)"
} else {
   Inf "skipping configure (use -Build to force)"
}

# ---- Build -----------------------------------------------------------------

Write-Host "`n==> Build" -ForegroundColor Cyan
Push-Location $root
try { & cmake --build --preset $Preset } finally { Pop-Location }
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

Write-Host "`n==> Run (dev mode)" -ForegroundColor Cyan
Inf "GRIMVAULT_DEV_RESOURCES = $root           (models/i18n/assets resolve from repo)"
Inf "GRIMVAULT_DISABLE_UPDATES = 1             (WinSparkle off)"
Inf "userData                = %APPDATA%\GrimVault   (db, logs)"

$env:GRIMVAULT_DEV_RESOURCES   = $root
$env:GRIMVAULT_DISABLE_UPDATES = "1"

& $exe
$rc = $LASTEXITCODE

Write-Host "`n==> Exited with code $rc" -ForegroundColor Cyan
