function Find-VcVars {
   $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
   if (Test-Path $vswhere) {
      $vsPath = & $vswhere -latest -products * -property installationPath 2>$null |
         Select-Object -First 1
      if ($vsPath) {
         $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
         if (Test-Path $candidate) { return $candidate }
      }
   }

   foreach ($root in @(
      "${env:ProgramFiles(x86)}\Microsoft Visual Studio",
      "$env:ProgramFiles\Microsoft Visual Studio"
   )) {
      if (-not (Test-Path $root)) { continue }
      $hit = Get-ChildItem -Path $root -Filter vcvars64.bat -Recurse `
         -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($hit) { return $hit.FullName }
   }

   return $null
}

function Initialize-MsvcEnv ([scriptblock] $Info = {}) {
   if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return $true }

   $vcvars = Find-VcVars
   if (-not $vcvars) { return $false }

   & $Info "sourcing $vcvars"
   $envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
   foreach ($line in $envDump) {
      if ($line -match '^([^=]+)=(.*)$') {
         Set-Item -Path "env:$($matches[1])" -Value $matches[2]
      }
   }

   return [bool] (Get-Command cl.exe -ErrorAction SilentlyContinue)
}
