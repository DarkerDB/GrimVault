param (
   [Parameter (Mandatory)]
   [string] $Installer
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing

function Get-IconHash ([string] $Path) {
   $icon = [Drawing.Icon]::ExtractAssociatedIcon($Path)
   if (-not $icon) {
      throw "No Windows icon resource: $Path"
   }

   $bitmap = $icon.ToBitmap()
   $stream = [IO.MemoryStream]::new()
   $sha = [Security.Cryptography.SHA256]::Create()
   try {
      $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
      return [BitConverter]::ToString($sha.ComputeHash($stream.ToArray())).Replace('-', '')
   } finally {
      $sha.Dispose()
      $stream.Dispose()
      $bitmap.Dispose()
      $icon.Dispose()
   }
}

$installerPath = (Resolve-Path $Installer).Path
$installerSignature = Get-AuthenticodeSignature $installerPath
if ($installerSignature.Status -ne 'Valid') {
   throw "Installer is not signed: $($installerSignature.StatusMessage)"
}

$installerVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($installerPath)
if ($installerVersion.ProductName -ne 'GrimVault' -or
    $installerVersion.CompanyName -ne 'DarkerDB' -or
    $installerVersion.FileDescription -ne 'GrimVault Installer') {
   throw "Incorrect installer branding: $($installerVersion.CompanyName) / $($installerVersion.ProductName)"
}
$installerIconHash = Get-IconHash $installerPath

$destination = Join-Path $env:RUNNER_TEMP 'grimvault-package-check'
New-Item -ItemType Directory -Force $destination | Out-Null

$process = Start-Process -FilePath $installerPath `
   -ArgumentList @('/S', "/D=$destination") `
   -Wait -PassThru
if ($process.ExitCode -ne 0) {
   throw "Silent install failed with exit code $($process.ExitCode)."
}

$exe = Join-Path $destination 'grimvault.exe'
$required = @(
   $exe,
   (Join-Path $destination 'WinSparkle.dll'),
   (Join-Path $destination 'models\tooltip.onnx'),
   (Join-Path $destination 'Qt6\plugins\platforms\qwindows.dll'),
   (Join-Path $destination 'web\augment.html')
)
foreach ($path in $required) {
   if (-not (Test-Path $path)) {
      throw "Installer omitted required file: $path"
   }
}

$exeSignature = Get-AuthenticodeSignature $exe
if ($exeSignature.Status -ne 'Valid') {
   throw "Installed GrimVault executable is not signed: $($exeSignature.StatusMessage)"
}

$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($version.ProductName -ne 'GrimVault' -or $version.CompanyName -ne 'DarkerDB') {
   throw "Incorrect executable branding: $($version.CompanyName) / $($version.ProductName)"
}

$exeIconHash = Get-IconHash $exe
if ($exeIconHash -ne $installerIconHash) {
   throw 'Installer and application icons do not match.'
}

Write-Host "Validated signed GrimVault package: $installerPath"
