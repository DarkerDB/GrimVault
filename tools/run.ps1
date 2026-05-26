<#
.SYNOPSIS
   Launch the existing debug build of grimvault.exe with dev env vars.
   Does NOT configure or rebuild. Use tools\dev-run.ps1 for that.

.EXAMPLE
   pwsh tools\run.ps1
#>

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$exe  = Join-Path $root "build\windows-msvc-debug\grimvault.exe"
$log  = Join-Path $root "build\windows-msvc-debug\last-run.log"

if (-not (Test-Path $exe)) {
   Write-Host "FAIL: $exe not found" -ForegroundColor Red
   Write-Host "      build it first: pwsh tools\dev-run.ps1 -NoRun" -ForegroundColor Yellow
   exit 1
}

$env:GRIMVAULT_DEV_RESOURCES   = $root
$env:GRIMVAULT_DISABLE_UPDATES = "1"
$env:QT_DEBUG_PLUGINS          = "1"

# Force Qt's qWarning/qCritical to stderr (default is OutputDebugString for
# WIN32 subsystem exes, which our redirect-to-log doesn't capture).
$env:QT_FORCE_STDERR_LOGGING   = "1"

# Make QML import resolution + load failures verbose so we can see WHICH
# component/import is the problem when QML fails to load.
$env:QT_LOGGING_RULES          = "qt.qml.imports=true;qt.qml.diskcache=true"

Write-Host "==> launching grimvault.exe" -ForegroundColor Cyan
Write-Host "    GRIMVAULT_DEV_RESOURCES   = $root"
Write-Host "    GRIMVAULT_DISABLE_UPDATES = 1"
Write-Host "    QT_DEBUG_PLUGINS          = 1   (Qt logs plugin-load attempts to stderr)"
Write-Host "    log -> $log"
Write-Host ""

& $exe 2>&1 | Tee-Object -FilePath $log
$rc = $LASTEXITCODE

Write-Host ""
Write-Host "==> exit $rc" -ForegroundColor Cyan
if ($rc -ne 0) {
   Write-Host "    last 30 lines of stderr/stdout (full log in $log):" -ForegroundColor Yellow
   Get-Content $log -Tail 30 | ForEach-Object { Write-Host "    $_" }
}

exit $rc
