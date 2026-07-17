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

# Invoked from a WSL shell via Linux pwsh: this is a Windows build, so
# re-exec under Windows PowerShell through the W: mapping (see DEV.md).
if ($IsLinux) {
   $winScript = if ($PSCommandPath -like '/mnt/*') { wslpath -w $PSCommandPath }
                else                               { 'W:' + ($PSCommandPath -replace '/', '\') }

   $fwd = @(foreach ($kv in $PSBoundParameters.GetEnumerator()) {
      if ($kv.Value -is [System.Management.Automation.SwitchParameter]) {
         if ($kv.Value) { "-$($kv.Key)" }
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

$root                  = (Resolve-Path "$PSScriptRoot\..").Path

# Build tree stays on a local Windows drive when the source lives in WSL
# (W: mapping of \\wsl.localhost\Ubuntu; see DEV.md): MSVC and Qt tooling
# spawn cmd.exe (no UNC cwd), and build-output writes over 9P are slow.
$remote = ($root -like '\\*') -or
          ((Get-PSDrive -Name (Split-Path -Qualifier $root).TrimEnd(':')).DisplayRoot -like '\\*')
$out = if ($remote) { "$env:LOCALAPPDATA\GrimVault\build\$Preset" }
       else         { Join-Path $root "build\$Preset" }

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

   $configLog = Join-Path $out "configure.log"
   New-Item -ItemType Directory -Force -Path $out | Out-Null

   $cfgArgs = @("-S", $root, "--preset", $Preset, "-B", $out)

   $proc = Start-Process -FilePath "cmake" -ArgumentList $cfgArgs -WorkingDirectory $env:LOCALAPPDATA `
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

   $buildLog = Join-Path $out "build.log"
   $proc = Start-Process -FilePath "cmake" -ArgumentList @("--build", $out) `
      -WorkingDirectory $env:LOCALAPPDATA -NoNewWindow -PassThru -RedirectStandardOutput $buildLog -RedirectStandardError "$buildLog.err"
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

Push-Location $out
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
