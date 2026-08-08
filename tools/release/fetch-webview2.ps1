$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$output = Join-Path $PSScriptRoot '..\build\MicrosoftEdgeWebView2Setup.exe'
$output = [IO.Path]::GetFullPath($output)
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($output)) | Out-Null

Invoke-WebRequest -UseBasicParsing `
   -Uri 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' `
   -OutFile $output

$signature = Get-AuthenticodeSignature $output
if ($signature.Status -ne 'Valid') {
   throw "WebView2 bootstrapper signature is invalid: $($signature.StatusMessage)"
}
if ($signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
   throw "Unexpected WebView2 publisher: $($signature.SignerCertificate.Subject)"
}

Write-Host "Verified Microsoft WebView2 bootstrapper: $output"
