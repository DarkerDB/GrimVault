# Running GrimVault locally (dev mode)

One-shot from a **Developer PowerShell for VS 2022**:

```powershell
pwsh tools/dev-run.ps1
```

That's it. The script configures with `windows-msvc-debug`, builds, and runs
the binary with two env vars set so it stays local:

```
GRIMVAULT_DEV_RESOURCES = <repo root>   # models/, i18n/, assets/ resolve from sources
GRIMVAULT_DISABLE_UPDATES = 1           # WinSparkle never inits, no network calls
```

Per-user data (SQLite DB, logs) still goes to `%APPDATA%\GrimVault\`.

## Prereqs (first time only)

```
1. Visual Studio Build Tools 2022 (Desktop development with C++)
2. CMake 3.28+      winget install Kitware.CMake
3. Ninja            winget install Ninja-build.Ninja
4. vcpkg            git clone https://github.com/microsoft/vcpkg %USERPROFILE%\vcpkg
                    %USERPROFILE%\vcpkg\bootstrap-vcpkg.bat
                    setx VCPKG_ROOT %USERPROFILE%\vcpkg
   then RESTART the shell
```

The first `cmake --preset` will compile Qt6 + OpenCV + ONNX Runtime + WinSparkle
from sources via vcpkg. **Plan for 30–60 minutes** and ~12 GB of disk. Subsequent
configures are cached.

## What you'll see

```
- Main window opens. Six pages in the left rail.
- Dashboard / Items / Pricing / Settings / Diagnostics / Logs.
- Ctrl+Shift+P opens the command palette.
- Pricing lookups hit https://api.darkerdb.com (network needed).
- Capture probe picks WGC/DXGI/GDI depending on Windows build; logged on Diagnostics.
- WGC will fail if no foreground game window — that's expected without
  Dark and Darker running. The pipeline reports the failure and stays idle.
- Tray icon (SSL.com 'K' for now until we get a proper icon) — single-click
  toggles the window.
```

## What's missing in dev mode

```
- ONNX models: models/tooltip.onnx and models/paddle/<family>/{rec.onnx,dict.txt}
  These are NOT in the repo. Either copy them from the existing Electron build
  (src/native/.build/models/) or download from DarkerDB releases. Without them
  the tooltip detector + OCR pipeline log init failures but the main UI still
  works.

- E2E fixtures: tests/fixtures/  (run `pip install Pillow && python tools/gen-fixtures/main.py --out tests/fixtures`).
```

## Useful one-offs

```powershell
# Just run unit tests (hermetic, no models needed)
pwsh tools/check-build.ps1

# Re-run without rebuild
$env:GRIMVAULT_DEV_RESOURCES   = (Resolve-Path .).Path
$env:GRIMVAULT_DISABLE_UPDATES = "1"
build\windows-msvc-debug\grimvault.exe

# Wipe the user data (DB + logs + migrated INI)
Remove-Item -Recurse -Force $env:APPDATA\GrimVault
```

## Where things live at runtime (with dev env vars set)

```
models/             <repo>/models/        ← you need to populate this
i18n/<lang>/        <repo>/i18n/<lang>/   ← already in repo
assets/             <repo>/assets/        ← already in repo
db/migrations/      <repo>/db/migrations/ ← embedded at compile time, also on disk

grimvault.db        %APPDATA%\GrimVault\grimvault.db
logs/               %APPDATA%\GrimVault\logs\
settings.ini        %APPDATA%\GrimVault\settings.ini  (auto-migrated to DB, renamed .migrated)
```

## Working on Windows + WSL

If you have this repo checked out on a Windows path (e.g. `C:\Users\…\Projects\grimvault-cpp`) and also touch it from a WSL shell over the `/mnt/c/` mount, run this once per clone:

```bash
git config core.fileMode false
```

The WSL `/mnt/c/` mount reports every file as executable (mode 755), but the underlying NTFS volume has no concept of POSIX exec bits. Without this setting, every tracked asset (fonts, PNGs, settings.ini, etc.) will show up as modified in `git status` with a `mode 100644 → 100755` flip, polluting diffs and PRs. Setting `core.fileMode false` tells git to ignore mode changes for this clone; the setting is per-clone (not committed), so each contributor sets it locally.

If you do an `ls` inside WSL and see Windows `Game.json:Zone.Identifier` files in `i18n/<lang>/`, those are NTFS Alternate Data Stream metadata files (Mark-of-the-Web tags) leaking through to the WSL view. They're gitignored, so they won't get committed, but you can safely `rm` them.

## When something breaks

The script filters CMake/MSVC output for the first useful error and tails
`build/<preset>/configure.log` or `build.log`. If it's a vcpkg port issue,
check `%VCPKG_ROOT%\buildtrees\<port>\install-x64-windows-rel-err.log`.
