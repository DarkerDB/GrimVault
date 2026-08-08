<#
.SYNOPSIS
   Smoke-test the GrimVault build on Windows.

.DESCRIPTION
   Verifies environment prerequisites, configures CMake, builds, and runs
   unit tests. Filters CMake/MSVC output to surface the first real error
   instead of drowning the user in scroll.

.PARAMETER Preset
   CMake preset to use (default: windows-msvc-test).

.PARAMETER Tests
   Skip configure + build; just run ctest with the given preset's test set.

.EXAMPLE
   pwsh tools/check-build.ps1
   pwsh tools/check-build.ps1 -Preset windows-msvc-release
   pwsh tools/check-build.ps1 -Tests
#>

[CmdletBinding ()]
param (
   [string] $Preset = "windows-msvc-test",
   [switch] $Tests
)

$ErrorActionPreference = "Stop"
$root                  = (Resolve-Path "$PSScriptRoot\..").Path

function Fail ([string] $msg) {
   Write-Host "FAIL:  $msg" -ForegroundColor Red
   exit 1
}

function Ok ([string] $msg) { Write-Host "OK     $msg" -ForegroundColor Green }
function Inf ([string] $msg) { Write-Host "       $msg" -ForegroundColor DarkGray }

# ---- Prerequisites ---------------------------------------------------------

Write-Host "==> Environment" -ForegroundColor Cyan

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
   Fail "cmake not on PATH. Install from cmake.org or via `winget install Kitware.CMake`."
}
Ok "cmake $(cmake --version | Select-Object -First 1)"

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
   Fail "ninja not on PATH. Install via `choco install ninja` or `winget install Ninja-build.Ninja`."
}
Ok "ninja $(ninja --version)"

if (-not $env:VCPKG_ROOT) {
   Fail "VCPKG_ROOT is not set. `git clone https://github.com/microsoft/vcpkg.git $HOME\vcpkg` then `setx VCPKG_ROOT $HOME\vcpkg`."
}
if (-not (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) {
   Fail "VCPKG_ROOT=$env:VCPKG_ROOT but vcpkg.exe is not there. Run bootstrap-vcpkg.bat inside it."
}
Ok "VCPKG_ROOT = $env:VCPKG_ROOT"

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
   Inf "cl.exe not on PATH - make sure you're running from a Developer PowerShell, or vcvars64.bat is sourced."
}

# ---- Configure -------------------------------------------------------------

if (-not $Tests) {
   Write-Host "`n==> Configure ($Preset)" -ForegroundColor Cyan

   $configLog = Join-Path $root "build\$Preset\configure.log"
   $configDir = Split-Path $configLog
   New-Item -ItemType Directory -Force -Path $configDir | Out-Null

   $cfgArgs = @("--preset", $Preset)

   $proc = Start-Process -FilePath "cmake" -ArgumentList $cfgArgs -WorkingDirectory $root `
      -NoNewWindow -PassThru -RedirectStandardOutput $configLog -RedirectStandardError "$configLog.err"
   $proc.WaitForExit()

   if ($proc.ExitCode -ne 0) {
      Write-Host "`n--- configure errors (last 40 lines) ---" -ForegroundColor Red
      Get-Content "$configLog.err" -Tail 40
      Get-Content $configLog | Where-Object { $_ -match "(error|warning|not found|cannot find|missing)" } |
         Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
      Fail "cmake configure failed (exit $($proc.ExitCode)). Full log: $configLog"
   }
   Ok "configure -> $configLog"

   # ---- Build --------------------------------------------------------------

   Write-Host "`n==> Build" -ForegroundColor Cyan

   $buildLog = Join-Path $root "build\$Preset\build.log"
   $proc = Start-Process -FilePath "cmake" -ArgumentList @("--build", "--preset", $Preset) `
      -WorkingDirectory $root -NoNewWindow -PassThru -RedirectStandardOutput $buildLog -RedirectStandardError "$buildLog.err"
   $proc.WaitForExit()

   if ($proc.ExitCode -ne 0) {
      Write-Host "`n--- build errors (last 40 lines) ---" -ForegroundColor Red
      Get-Content $buildLog        | Select-String -Pattern "error", "fatal error" |
         Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
      Fail "build failed (exit $($proc.ExitCode)). Full log: $buildLog"
   }
   Ok "build -> $buildLog"
}

# ---- Tests -----------------------------------------------------------------

Write-Host "`n==> Unit tests" -ForegroundColor Cyan

Push-Location (Join-Path $root "build\$Preset")
try {
   & ctest --output-on-failure --label-regex unit
   if ($LASTEXITCODE -ne 0) {
      Fail "unit tests failed."
   }
} finally {
   Pop-Location
}
Ok "unit tests passed"

Write-Host "`n==> Smoke test complete." -ForegroundColor Cyan
