# Prepare a signed local installer. This script never publishes artifacts or feeds.
# Example: -Version 2.1.2 -Stage prod produces GrimVault-Setup-prod-2.1.2.exe.
param (
   [Parameter (Mandatory)]
   [ValidatePattern ('^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$')]
   [string] $Version,
   [ValidateSet ('qa', 'prod')]
   [string] $Stage = 'prod',
   [ValidateSet ('stable', 'beta')]
   [string] $Channel = 'stable',
   [Parameter (Mandatory)]
   [ValidatePattern ('^[a-f0-9]{40}$')]
   [string] $SourceRevision,
   [string] $BuildDirectory,
   [string] $VcpkgInstalledDirectory,
   [switch] $UseInstalledDependencies
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $BuildDirectory) {
   $BuildDirectory = Join-Path $env:LOCALAPPDATA "GrimVault\build\release-$Stage-$Version"
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$artifacts = Join-Path $BuildDirectory 'artifacts'
$payload = Join-Path $BuildDirectory 'payload'
New-Item -ItemType Directory -Force $BuildDirectory, $artifacts | Out-Null
$env:RUNNER_TEMP = Join-Path $BuildDirectory 'temp'
New-Item -ItemType Directory -Force $env:RUNNER_TEMP | Out-Null

. (Join-Path $source 'tools\build\msvc-env.ps1')
if (-not (Initialize-MsvcEnv { param ($message) Write-Host $message })) {
   throw 'Visual Studio C++ build tools are unavailable.'
}

foreach ($name in @('SSL_COM_USERNAME', 'SSL_COM_PASSWORD', 'SSL_COM_CREDENTIAL_ID', 'SSL_COM_TOTP_SECRET')) {
   if (-not [Environment]::GetEnvironmentVariable($name)) {
      throw "Missing signing credential: $name"
   }
}

$nsis = Join-Path ${env:ProgramFiles(x86)} 'NSIS'
if (Test-Path (Join-Path $nsis 'makensis.exe')) {
   $env:PATH = "$nsis;$env:PATH"
}
foreach ($command in @('cmake', 'cpack', 'ctest', 'makensis', 'signtool')) {
   Get-Command $command -ErrorAction Stop | Out-Null
}

& (Join-Path $PSScriptRoot 'fetch-webview2.ps1')
$config = @(
   '-S', $source,
   '--preset', "release-windows-$Stage",
   '-B', $BuildDirectory,
   "-DGRIMVAULT_VERSION=$Version",
   "-DGRIMVAULT_CHANNEL=$Channel"
)
if ($VcpkgInstalledDirectory) {
   $config += "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDirectory"
}
if ($UseInstalledDependencies) {
   if (-not $VcpkgInstalledDirectory) {
      throw 'UseInstalledDependencies requires an explicit VcpkgInstalledDirectory.'
   }
   $config += '-DVCPKG_MANIFEST_INSTALL=OFF'
}

& cmake @config
if ($LASTEXITCODE -ne 0) {
   throw 'Release configuration failed.'
}
& cmake --build $BuildDirectory
if ($LASTEXITCODE -ne 0) {
   throw 'Release build failed.'
}
& ctest --test-dir $BuildDirectory --output-on-failure -L 'unit|integration|e2e'
if ($LASTEXITCODE -ne 0) {
   throw 'Release tests failed.'
}

$exe = Join-Path $BuildDirectory 'grimvault.exe'
& (Join-Path $PSScriptRoot 'sign-windows.ps1') -Path $exe
& cpack --config (Join-Path $BuildDirectory 'CPackConfig.cmake') -B $artifacts -G NSIS
if ($LASTEXITCODE -ne 0) {
   throw 'Installer packaging failed.'
}
$installer = Join-Path $artifacts "GrimVault-Setup-$Stage-$Version.exe"
& (Join-Path $PSScriptRoot 'sign-windows.ps1') -Path $installer

# Stage and check dependencies without uninstalling the operator's running app.
& cmake --install $BuildDirectory --prefix $payload
if ($LASTEXITCODE -ne 0) {
   throw 'Payload staging failed.'
}
& (Join-Path $PSScriptRoot 'validate-package.ps1') -Installer $installer -PayloadDirectory $payload

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash.ToLowerInvariant()
$filename = [IO.Path]::GetFileName($installer)
"$hash  $filename" | Set-Content -Encoding ascii -LiteralPath "$installer.sha256"
[ordered]@{
   version = $Version
   stage = $Stage
   channel = $Channel
   source_revision = $SourceRevision
   installer = $filename
   sha256 = $hash
   installer_signature = (Get-AuthenticodeSignature $installer).Status.ToString()
   executable_signature = (Get-AuthenticodeSignature $exe).Status.ToString()
   validation = 'signed installer and staged payload; installer execution still required on a clean Windows host'
   published = $false
} | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $artifacts 'release.json')

Write-Host "Prepared signed release: $installer"
Write-Host 'No artifacts or updater feeds were published.'
