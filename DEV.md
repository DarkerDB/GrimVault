# Running GrimVault locally (dev mode)

Source of truth lives in WSL at `~/.katforge/realms/grimvault`. Windows
tooling reaches it through a **drive mapping** (one-time setup):

```
net use W: \\wsl.localhost\Ubuntu /persistent:yes
```

The raw `\\wsl.localhost` UNC path is not enough — Qt's build tooling
(cmd-wrapped qmlimportscanner) needs a drive-lettered source directory.
The **build tree stays on a local Windows drive** at
`%LOCALAPPDATA%\GrimVault\build\<preset>`: MSVC and Qt tooling spawn
cmd.exe (no UNC/network cwd for link steps), and object/PDB writes over
the 9P bridge are slow. CI is unaffected (its checkout is on a drive;
presets keep the default in-tree `build/` layout).

One-shot from any Windows PowerShell:

```powershell
wsl.exe bash -lc 'cd ~/.katforge/realms/grimvault && pwsh.exe -File "$(wslpath -w tools/run-dev.ps1)"'
```

or from a WSL shell:

```bash
cmd.exe /c "$(wslpath -w tools/build/wsl-build.bat)"   # configure + build
cmd.exe /c "$(wslpath -w tools/build/wsl-test.bat)"    # + unit tests
```

That's it. The script configures with `windows-msvc-test`, builds, installs
the `grimvault` CLI shim on your user PATH, and runs the binary with two env
vars set so it stays local:

```
GRIMVAULT_DEV_RESOURCES = <repo root>   # models/, i18n/, assets/ resolve from sources
GRIMVAULT_DISABLE_UPDATES = 1           # WinSparkle never inits, no network calls
```

Production data stays in `%LOCALAPPDATA%\GrimVault\`. Dev and QA use
`%LOCALAPPDATA%\GrimVault\<env>\` so clients cannot share state.

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
  callback fires and tokens land in Windows Credential Manager.
- Settings sync starts: GrimVault polls /v2/grimvault/settings (every 5s
  on dev, 30s elsewhere), mirrors the response into the local
  UserSettingsRepo, and applies each changed key live — no restart. Each
  one logs "settings applied: key = value".
- The tray header reports the whole onboarding state: **Signed out**,
  **Syncing settings**, **Ready**, or **Using local defaults; retrying**.
  First launch is ready only after settings arrive. A network failure keeps
  safe defaults active and retries. An expired or revoked token signs out and
  starts OAuth again unless `--no-auto-login` was supplied.
- Main window opens (if you click the tray). Six pages in the left rail.
- Dashboard / Items / Pricing / Settings / Diagnostics / Logs.
- Ctrl+Shift+P opens the command palette.
- Pricing lookups hit https://api.darkerdb.com (network needed).
- Capture probes WGC/DXGI/GDI at startup, then advances through the same ladder after three consecutive runtime failures.
- WGC will fail if no foreground game window — that's expected without
  Dark and Darker running. The pipeline reports the failure and stays idle.
- The GrimVault tray icon single-click toggles the window.
```

### Settings, live

Everything on https://dev.darkerdb.com/dashboard/grimvault applies to the
running app within one poll. Move a slider, watch the next hover.

```
overlay mode           automatic / manual / disabled
overlay alignment      attached, or a fixed game-window corner
overlay opacity        multiplied into the card's fade-in; repaints at once
overlay scale          on top of the monitor's DPI scale
overlay offset x/y     corner alignments only (attached is fixed by the anchor)
augment sections       the tooltip:analysis:* widget toggles
currency display       absolute / compact
launch on startup      rewrites the HKCU\...\Run entry
auto-update            starts / stops the checker (GRIMVAULT_DISABLE_UPDATES wins)
hotkeys                toggle_overlay + force_refresh + open_in_browser rebind live
```

Dashboard hotkeys are the only global bindings. `force_refresh` drives
`scan_now`, `toggle_overlay` shows or hides the card and `open_in_browser`
opens the last analysed item. A widget the plan doesn't grant is forced off
in the payload regardless of the toggle, because the API already stripped
its data.

Pass `--no-auto-login` to skip the on-launch OAuth prompt (the tray's
"Sign In" menu item still works on demand):

```powershell
grimvault --no-auto-login
```

## CLI on PATH

`tools/dev-run.ps1` writes `C:\Users\<you>\.bin\grimvault.cmd` pointing at
the just-built executable and the source-only development runner. From then on:

```powershell
grimvault --help
grimvault dev           # configure, build, and run local source with --debug
grimvault dev -NoRun    # configure and build without launching
grimvault dev --fcr 5   # build, run, and forward app arguments
grimvault status        # env, signed-in user, expiry, /v2/grimvault/ping result
grimvault whoami        # alias of status
grimvault login         # interactive OAuth (browser opens)
grimvault settings      # dump locally synced settings (the addon's view)
grimvault settings get  # one-shot read of /v2/grimvault/settings (the server's view)
grimvault doctor        # end-to-end diagnostics (tokens, JWKS, ping)

grimvault                 # GUI foreground: logs stream to this terminal, Ctrl+C quits
grimvault --detached      # GUI in the background, returns immediately
grimvault --debug         # + verbose logs and OCR stage dumps (%TEMP%\grimvault-ocr)
grimvault --debug=highlight:objects
                          # + red borders around detected tooltip boxes
grimvault --debug=highlight:game
                          # + red border around the game/capture region
grimvault --debug=highlight:objects,highlight:game
                          # enable both borders
grimvault --detect-only   # stop after detection: hover -> box, no OCR/lookup/augment
grimvault --fcr 10        # active frame capture rate, 1-60 fps (default 15;
                          # idle drops to 3 until something is detected)
```

### Anchoring diagnostics

Anchoring emits structured `[vision]` events at info level, so ordinary runs
record acquisition, confirmed loss, replacement candidates/rejections, frame
age, locator cost, and hash-distance summaries in:

```text
%LOCALAPPDATA%\GrimVault\<env>\logs\
```

From WSL/Codex the same directory is directly readable at
`/mnt/c/Users/Ethan/AppData/Local/GrimVault/<env>/logs`; no log copy/paste is
needed. Production omits the `<env>` directory. Summarize the newest session with:

```bash
tools/anchor-log-summary.sh
```

or from PowerShell:

```powershell
tools\anchor-log-summary.ps1
```

`--debug` additionally saves frames for replacement candidates and settled
replacements under `%LOCALAPPDATA%\GrimVault\<env>\logs\anchoring\`. PNG writes occur on
detached workers after the immediate UI event, and retention is capped at the
newest 40 frames.

API calls log DNS, connect, TLS, first-byte, total, request id, and server
phase timings. Repeated identical hovers use a short account-scoped memory
cache, so the common second hover avoids another network round trip.

Tooltip tracking detects once, refines to ~1px at full resolution, then draws from
clamp(cursor + offset) at 120 Hz with per-frame presence and identity checks
for immediate disappearance and settled replacement.

`run-dev.ps1` (and its original `dev-run.ps1` target) launches the full
pipeline with `--debug` by default, with no red highlight borders (`-NoDebug`
disables debug logging; `-DetectOnly` skips OCR, lookup, and augment) and builds
RelWithDebInfo — the Debug preset's debug OpenCV makes detection ~10x slower.

### Switching env at runtime

Each binary bakes an env at compile time (set via
`-DGRIMVAULT_ENV=dev|qa|prod`; `dev-run.ps1` defaults to `dev`). Development
and QA builds can override it at runtime either way:

```powershell
grimvault --env qa status
$env:GRIMVAULT_ENV = "qa"; grimvault status
```

Production builds ignore `--env`, `GRIMVAULT_ENV`, and `APP_ENV`. Their API,
authentication, SPA, credential, data, and update environments remain `prod`.

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

The shim forwards ordinary commands to the real exe and `grimvault dev` to
the source runner. The exe stays put so Qt's adjacent-DLL discovery keeps
working. Each `dev-run.ps1` refreshes its target file, so it always invokes the
latest build without rewriting an active shim. Installer-distributed binaries
do not include the source-only command.

## Bundled test data

All tooltip and language ONNX models are committed under `models/`. GrimVault
ships the trained artifacts only — the training pipelines, their corpora and
the tooltip fonts live in the `scry` package (`packages/scry/training`), which
is private because those fonts are not cleared for redistribution.

## Useful one-offs

```powershell
# Just run unit tests (hermetic, no models needed)
pwsh tools/check-build.ps1

# Re-run without rebuild
$env:GRIMVAULT_DEV_RESOURCES   = (Resolve-Path .).Path
$env:GRIMVAULT_DISABLE_UPDATES = "1"
build\windows-msvc-debug\grimvault.exe

# Wipe the user data (DB + logs + migrated INI)
Remove-Item -Recurse -Force $env:LOCALAPPDATA\GrimVault
```

## Where things live at runtime (with dev env vars set)

```
models/             <repo>/models/        ← bundled
i18n/<lang>/        <repo>/i18n/<lang>/   ← already in repo
assets/             <repo>/assets/        ← already in repo
web/                <repo>/web/           ← augment.html + vendored ddb-tooltips
                                            (refresh: tools/build/sync-tooltips.sh)
db/migrations/      <repo>/db/migrations/ ← embedded at compile time, also on disk

grimvault.db        %LOCALAPPDATA%\GrimVault\<env>\grimvault.db
logs/               %LOCALAPPDATA%\GrimVault\<env>\logs\
settings.ini        %LOCALAPPDATA%\GrimVault\<env>\settings.ini
webview2/           %LOCALAPPDATA%\GrimVault\<env>\webview2
```

The Augment is rendered by the vendored DDB tooltip SDK in a permanently
hidden WebView2, captured as a transparent PNG, and displayed through a
disabled native Qt window. No browser HWND is placed over the game. The QML
port remains available as fallback with `overlay:renderer=qml`.

The card's enter animation is CSS in `web/augment.html`, played on the
`.ddb-tooltip` element the library recreates on each render (a fresh
element with a CSS `animation` plays from frame 0; the transparent window
stays shown, so no native show/move can drop the frames). Dismissal is
instant, matching the game (its tooltip vanishes the moment the hover
ends), including on a substantial cursor jump off the item. Tunables are
the `--aug-enter-*` custom properties at the top of that file (duration,
easing, rise, shift, scale, origin), plus `--aug-pad` (transparent headroom
so the transform never clips the window edge; keep it larger than the
biggest rise/shift and any scale overshoot). Editing `web/augment.html`
alone needs no rebuild: it is staged next to the exe, so copy it into
`build/<preset>/web/` (or rerun dev-run) to pick up changes.

Note: DirectComposition visual opacity/transform do NOT reach WebView2
content, so the animation cannot be done natively; CSS on the recreated
element is the only path.

## Working on Windows + WSL

The repo lives on WSL ext4, so git runs in WSL and POSIX modes are real.
This clone still carries `core.fileMode false` from its NTFS days; that's
harmless here (it only means git ignores exec-bit flips). A clone checked
out on an NTFS path needs it set — the `/mnt/c/` mount reports every file
as mode 755 and pollutes diffs otherwise.

If you see `Game.json:Zone.Identifier` files in `i18n/<lang>/`, those are
NTFS Alternate Data Stream remnants (Mark-of-the-Web tags) from the old
NTFS checkout. They're gitignored; `rm` them freely.

## When something breaks

The script filters CMake/MSVC output for the first useful error and tails
`%LOCALAPPDATA%\GrimVault\build\<preset>\configure.log` or `build.log`.
If it's a vcpkg port issue, check
`%VCPKG_ROOT%\buildtrees\<port>\install-x64-windows-rel-err.log`.
