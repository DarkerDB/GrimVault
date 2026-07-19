param(
   [string[]]$LogDir = @(
      "$env:LOCALAPPDATA\GrimVault\logs",
      "$env:APPDATA\GrimVault\logs",
      "$env:APPDATA\DDB\GrimVault\logs"
   )
)

$log = Get-ChildItem $LogDir -File -ErrorAction SilentlyContinue |
   Where-Object { $_.Name -like 'grimvault*.txt' -or $_.Extension -eq '.log' } |
   Sort-Object LastWriteTime -Descending |
   Select-Object -First 1

if (-not $log) { throw "No GrimVault log file found in: $($LogDir -join ', ')" }

Write-Host "Log: $($log.FullName)"
Write-Host "`nEvent counts:"
$events = Select-String -Path $log.FullName -Pattern '\[vision\] (anchor_[a-z_]+|replacement_[a-z_]+)' -AllMatches |
   ForEach-Object { $_.Matches } |
   ForEach-Object { $_.Groups[1].Value }
$events | Group-Object | Sort-Object Count -Descending | Format-Table Count, Name -AutoSize

Write-Host "Latest anchoring events:"
Select-String -Path $log.FullName -Pattern '\[vision\] (anchor_|replacement_)' |
   Select-Object -Last 120 |
   ForEach-Object { $_.Line }

Write-Host "`nDiagnostic frames: $($log.DirectoryName)\anchoring"