<#
.SYNOPSIS
   Compatibility entry point for the GrimVault development runner.

.DESCRIPTION
   Forwards every argument to tools/dev-run.ps1 so both command spellings use
   the same configure, build, environment, and launch workflow.

.EXAMPLE
   pwsh tools/run-dev.ps1
   pwsh tools/run-dev.ps1 -Build -NoRun
#>

$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "dev-run.ps1") @args
exit $LASTEXITCODE
