$ErrorActionPreference = "Stop"

[string[]] $devArgs = if ($args.Count -gt 1) {
   $args[1..($args.Count - 1)]
} else {
   @()
}

& pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "dev-run.ps1") @devArgs
exit $LASTEXITCODE
