param (
   [Parameter (Mandatory)]
   [string] $Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$required = @(
   'SSL_COM_USERNAME',
   'SSL_COM_PASSWORD',
   'SSL_COM_CREDENTIAL_ID',
   'SSL_COM_TOTP_SECRET'
)

foreach ($name in $required) {
   if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
      throw "Missing signing credential: $name"
   }
}

$file = (Resolve-Path $Path).Path
$root = Join-Path $env:LOCALAPPDATA 'sslcom\CodeSignTool'
$bat = Get-ChildItem $root -Filter CodeSignTool.bat -Recurse -ErrorAction SilentlyContinue |
   Select-Object -First 1

if (-not $bat) {
   New-Item -ItemType Directory -Force $root | Out-Null
   $zip = Join-Path $env:RUNNER_TEMP 'codesigntool.zip'
   Invoke-WebRequest -UseBasicParsing `
      -Uri 'https://www.ssl.com/download/codesigntool-for-windows/' `
      -OutFile $zip
   Expand-Archive -Path $zip -DestinationPath $root -Force
   $bat = Get-ChildItem $root -Filter CodeSignTool.bat -Recurse | Select-Object -First 1
}

if (-not $bat) {
   throw 'CodeSignTool.bat was not present in the SSL.com package.'
}

Push-Location $bat.DirectoryName
try {
   & $bat.FullName sign `
      "-username=$env:SSL_COM_USERNAME" `
      "-password=$env:SSL_COM_PASSWORD" `
      "-credential_id=$env:SSL_COM_CREDENTIAL_ID" `
      "-totp_secret=$env:SSL_COM_TOTP_SECRET" `
      "-input_file_path=$file" `
      -override
   if ($LASTEXITCODE -ne 0) {
      throw "SSL.com CodeSignTool failed with exit code $LASTEXITCODE."
   }
} finally {
   Pop-Location
}

$signature = Get-AuthenticodeSignature $file
if ($signature.Status -ne 'Valid') {
   throw "Authenticode validation failed: $($signature.StatusMessage)"
}

& signtool verify /pa /all /v $file
if ($LASTEXITCODE -ne 0) {
   throw 'signtool rejected the Authenticode signature.'
}

Write-Host "Signed and verified: $file"
