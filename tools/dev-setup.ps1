<#
.SYNOPSIS
   One-shot Windows-host prereq installer for GrimVault dev builds.

.DESCRIPTION
   Installs MSVC Build Tools 2022 (with the Windows SDK), CMake, Ninja, Git,
   PowerShell 7, then clones + bootstraps vcpkg into %USERPROFILE%\vcpkg and
   sets VCPKG_ROOT.

   Idempotent - re-running is safe. Each step is skipped if the tool is
   already present.

.EXAMPLE
   # From any PowerShell (5.1 or 7+):
   powershell -ExecutionPolicy Bypass -File tools\dev-setup.ps1

   # Then close the shell, open a NEW "Developer PowerShell for VS 2022",
   # and run:  pwsh tools\dev-run.ps1
#>

[CmdletBinding ()]
param (
   [switch] $SkipBuildTools,
   [switch] $SkipVcpkg
)

$ErrorActionPreference = "Continue"

function Ok   ([string] $m) { Write-Host "OK     $m" -ForegroundColor Green }
function Inf  ([string] $m) { Write-Host "       $m" -ForegroundColor DarkGray }
function Warn ([string] $m) { Write-Host "WARN   $m" -ForegroundColor Yellow }
function Fail ([string] $m) { Write-Host "FAIL   $m" -ForegroundColor Red; exit 1 }

function Have ([string] $cmd) { [bool] (Get-Command $cmd -ErrorAction SilentlyContinue) }

# Re-read PATH from registry so installs from this same session become visible.
# winget updates the persistent environment but not the current process's PATH.
function Refresh-Path {
   $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
   $user    = [Environment]::GetEnvironmentVariable("Path", "User")
   $env:PATH = "$machine;$user"
}

Refresh-Path

# ---- winget present? -------------------------------------------------------

if (-not (Have winget)) {
   Fail @"
winget is not on PATH. Options:
   - Windows 11: open Microsoft Store, search "App Installer", click Update.
   - Windows 10: install "App Installer" from the Store.
Re-run this script after winget works.
"@
}
Ok "winget"

# ---- Visual Studio Build Tools 2022 ----------------------------------------

if ($SkipBuildTools) {
   Inf "Skipping VS Build Tools (-SkipBuildTools)"
} elseif (Have cl) {
   Ok "MSVC cl.exe already on PATH"
} else {
   $vsExists = Test-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC"
   if ($vsExists) {
      Ok "VS Build Tools 2022 installed (cl.exe only visible inside Developer PowerShell)"
   } else {
      Write-Host "==> Installing VS 2022 Build Tools + Windows SDK (~5 GB, ~15 min)" -ForegroundColor Cyan
      $args = '--add Microsoft.VisualStudio.Workload.VCTools ' +
              '--add Microsoft.VisualStudio.Component.Windows11SDK.22621 ' +
              '--add Microsoft.VisualStudio.Component.VC.CMake.Project ' +
              '--includeRecommended'
      winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "$args"
      if ($LASTEXITCODE -ne 0) { Fail "VS Build Tools install failed (exit $LASTEXITCODE)" }
      Ok "VS Build Tools 2022 installed"
   }
}

# ---- CMake, Ninja, Git, PowerShell 7 ---------------------------------------

$tools = @(
   @{ id = "Kitware.CMake";        check = "cmake" },
   @{ id = "Ninja-build.Ninja";    check = "ninja" },
   @{ id = "Git.Git";              check = "git"   },
   @{ id = "Microsoft.PowerShell"; check = "pwsh"  }
)

foreach ($t in $tools) {
   if (Have $t.check) {
      Ok "$($t.check) already installed"
      continue
   }
   Write-Host "==> Installing $($t.id)" -ForegroundColor Cyan
   winget install --id $t.id -e --silent --accept-source-agreements --accept-package-agreements
   if ($LASTEXITCODE -ne 0) { Warn "$($t.id) install returned $LASTEXITCODE (might be already installed)" }
   Refresh-Path
}

# Final PATH refresh before vcpkg step (git is the critical one).
Refresh-Path

# Hard fallback: if git still isn't on PATH (e.g. installer didn't broadcast
# WM_SETTINGCHANGE in time), look in the well-known install location.
if (-not (Have git)) {
   foreach ($p in @("C:\Program Files\Git\cmd", "C:\Program Files (x86)\Git\cmd")) {
      if (Test-Path "$p\git.exe") { $env:PATH = "$p;$env:PATH"; break }
   }
}

# ---- vcpkg ----------------------------------------------------------------

if ($SkipVcpkg) {
   Inf "Skipping vcpkg (-SkipVcpkg)"
} else {
   $vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "$env:USERPROFILE\vcpkg" }

   if (-not (Test-Path "$vcpkgRoot\.git")) {
      Write-Host "==> Cloning vcpkg to $vcpkgRoot" -ForegroundColor Cyan
      git clone https://github.com/microsoft/vcpkg $vcpkgRoot
      if ($LASTEXITCODE -ne 0) { Fail "git clone vcpkg failed" }
   } else {
      Ok "vcpkg already cloned at $vcpkgRoot"
   }

   if (-not (Test-Path "$vcpkgRoot\vcpkg.exe")) {
      Write-Host "==> Bootstrapping vcpkg" -ForegroundColor Cyan
      & "$vcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
      if (-not (Test-Path "$vcpkgRoot\vcpkg.exe")) { Fail "vcpkg.exe not built" }
   }
   Ok "vcpkg ready at $vcpkgRoot"

   $current = [Environment]::GetEnvironmentVariable("VCPKG_ROOT", "User")
   if ($current -ne $vcpkgRoot) {
      [Environment]::SetEnvironmentVariable("VCPKG_ROOT", $vcpkgRoot, "User")
      Ok "set VCPKG_ROOT = $vcpkgRoot (user env)"
   } else {
      Ok "VCPKG_ROOT already pinned"
   }
}

# ---- Final guidance --------------------------------------------------------

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Setup complete. Next steps:" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host " 1. CLOSE this shell."
Write-Host ""
Write-Host " 2. From WSL (one-time copy to fast Windows-native disk):"
Write-Host "       mkdir -p /mnt/c/Users/$env:USERNAME/src" -ForegroundColor Yellow
Write-Host "       cp -r /home/$env:USERNAME/.katforge/workshop/grimvault \"
Write-Host "             /mnt/c/Users/$env:USERNAME/src/grimvault" -ForegroundColor Yellow
Write-Host "    (Skip this if you're fine building from \\wsl`$\... - it's just slower.)"
Write-Host ""
Write-Host " 3. Open Start Menu -> 'Developer PowerShell for VS 2022'"
Write-Host ""
Write-Host " 4. Build + run:"
Write-Host "       cd C:\Users\$env:USERNAME\src\grimvault" -ForegroundColor Yellow
Write-Host "       pwsh tools\dev-run.ps1" -ForegroundColor Yellow
Write-Host ""
Write-Host " First build pulls + compiles Qt6/OpenCV/ONNX via vcpkg (~30-60 min)."
Write-Host ""
