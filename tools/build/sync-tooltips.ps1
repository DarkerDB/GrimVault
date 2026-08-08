# Vendor the ddb-tooltips dist into web/tooltips/ for the WebView2 Augment.
#
#    pwsh tools/build/sync-tooltips.ps1 [-Source <ddb-tooltips checkout>]
#
# Windows twin of sync-tooltips.sh; same contract.

param (
   [string] $Source = "$env:USERPROFILE\.katforge\realms\darkerdb.com\tooltips"
)

$ErrorActionPreference = 'Stop'

$repo = Resolve-Path "$PSScriptRoot\..\.."
$dist = Join-Path $Source 'dist'
$dst  = Join-Path $repo 'web\tooltips'

if (-not (Test-Path (Join-Path $dist 'tooltip.min.js'))) {
   throw "no dist at $dist — run 'npm run build' there first"
}

$version = (Get-Content (Join-Path $Source 'package.json') | ConvertFrom-Json).version

if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Path $dst | Out-Null

Copy-Item (Join-Path $dist 'tooltip.min.js') $dst
Copy-Item (Join-Path $dist 'tooltip.css')    $dst
Copy-Item (Join-Path $dist 'assets')         (Join-Path $dst 'assets') -Recurse
Set-Content (Join-Path $dst 'VERSION') $version

Write-Host "vendored ddb-tooltips $version -> $dst"
