# Running GrimVault locally (dev mode)

One-shot from a **Developer PowerShell for VS 2022**:

```powershell
pwsh tools/dev-run.ps1
```

That's it. The script configures with `windows-msvc-debug`, builds, installs
the `grimvault` CLI shim on your user PATH, and runs the binary with two env
vars set so it stays local:

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
- Tray icon appears. If you're not already signed in, a tray toast says
  "Opening your browser to sign in…" and your browser pops up on
  https://dev.darkerdb.com/oauth/authorize. After you grant consent the
  callback fires, tokens land in Windows Credential Manager, and a
  "Signed in" toast confirms.
- Settings sync starts: every 60s GrimVault polls /v2/grimvault/settings
  and mirrors the response into the local UserSettingsRepo. Any change
  is logged ("settings updated: key = value").
- Main window opens (if you click the tray). Six pages in the left rail.
- Dashboard / Items / Pricing / Settings / Diagnostics / Logs.
- Ctrl+Shift+P opens the command palette.
- Pricing lookups hit https://api.darkerdb.com (network needed).
- Capture probe picks WGC/DXGI/GDI depending on Windows build; logged on Diagnostics.
- WGC will fail if no foreground game window — that's expected without
  Dark and Darker running. The pipeline reports the failure and stays idle.
- Tray icon (SSL.com 'K' for now until we get a proper icon) — single-click
  toggles the window.
```

Pass `--no-auto-login` to skip the on-launch OAuth prompt (the tray's
"Sign In" menu item still works on demand):

```powershell
grimvault --no-auto-login
```

## CLI on PATH

`tools/dev-run.ps1` writes `C:\Users\<you>\.bin\grimvault.cmd` pointing at
the just-built `build\windows-msvc-debug\grimvault.exe`. From then on:

```powershell
grimvault --help
grimvault status        # env, signed-in user, expiry, /v2/grimvault/ping result
grimvault whoami        # alias of status
grimvault login         # interactive OAuth (browser opens)
grimvault settings      # dump locally synced settings (the addon's view)
grimvault settings get  # one-shot read of /v2/grimvault/settings (the server's view)
grimvault doctor        # end-to-end diagnostics (tokens, JWKS, ping)

grimvault               # GUI foreground: logs stream to this terminal
grimvault --detached    # GUI in the background, returns immediately
grimvault --debug       # GUI foreground + verbose logs, OCR stage dumps
                        # (%TEMP%\grimvault-ocr), debug overlay on
```

### Switching env at runtime

Each binary bakes a *default* env at compile time (set via
`-DGRIMVAULT_ENV=dev|qa|prod`; `dev-run.ps1` defaults to `dev`). Override at
runtime either way:

```powershell
grimvault --env qa status
$env:GRIMVAULT_ENV = "qa"; grimvault status
```

Host map:

| env | API | Auth | SPA |
|---|---|---|---|
| dev | api.dev.darkerdb.com | auth.dev.darkerdb.com | dev.darkerdb.com |
| qa  | api.qa.darkerdb.com  | auth.qa.darkerdb.com  | qa.darkerdb.com  |
| prod| api.darkerdb.com     | auth.darkerdb.com     | darkerdb.com     |

First-time setup — if `C:\Users\<you>\.bin` isn't on your user PATH yet,
run the installer interactively once and let it add the dir:

```powershell
pwsh tools\install-cli.ps1 -AddToPath
# then open a fresh shell so the new PATH is visible
```

The shim is a 2-line `.cmd` that forwards `%*` to the real exe — the exe
stays put so Qt's adjacent-DLL discovery keeps working. Each `dev-run.ps1`
rewrites the shim, so you're always invoking the latest build.

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
web/                <repo>/web/           ← augment.html + vendored ddb-tooltips
                                            (refresh: tools/build/sync-tooltips.sh)
db/migrations/      <repo>/db/migrations/ ← embedded at compile time, also on disk

grimvault.db        %APPDATA%\GrimVault\grimvault.db
logs/               %APPDATA%\GrimVault\logs\
settings.ini        %APPDATA%\GrimVault\settings.ini  (auto-migrated to DB, renamed .migrated)
webview2/           %APPDATA%\GrimVault\webview2      (WebView2 user data)
```

The Augment (overlay card) renders through WebView2 + the vendored
ddb-tooltips library. Machines without the Evergreen runtime, or a dead
browser process, fall back to the legacy QML card automatically; force it
with the `overlay:renderer` setting (`webview` | `qml`).

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
