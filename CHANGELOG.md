# Changelog

All notable changes to this project will be documented in this file.

## Unreleased

- Restored localized OCR and dashboard language selection
- Hardened WebView2 startup with clean-profile and software-rendering recovery
- Removed the alternate QML tooltip renderer
- Fixed dashboard hotkeys using F6 through F8
- Added game-font OCR models for every supported language

## [0.0.5] - 2026-08-15

- Fixed make gdi capture an explicit compatibility mode
- Fixed align manual augment dismissal with automatic mode
- Added dated api contract with widget hints and timing observability
- Fixed sync and log capture mode settings
- Added dev command for building and running local source
- Added log augment view conceal events
- Added log dropped frame counts in ocr pipeline
- Added send session id in darkerdb client requests
- Added log periodic process cpu and memory samples
- Added publish session diagnostics header on startup and auth change
- Added emit session header in daily log files
- Added diagnostics module for session and machine info
- Added require multi-frame confirmation for anchor replacement
- Changed vendored tooltip library to 1.5.22
- Changed use releases.darkerdb.com as canonical appcast host
- Changed sync vendored tooltip lib to 1.5.20

## [0.0.4] - 2026-08-10

- Changed release v0.0.4-rc.1
- Changed publish the stable appcast only from dispatched releases
- Fixed close the running app before the installer extracts

## [0.0.4-rc.1] - 2026-08-10

- Changed publish the stable appcast only from dispatched releases
- Fixed close the running app before the installer extracts

## [0.0.3] - 2026-08-10

- Changed release v0.0.3-rc.3
- Changed stamp the 2.0.3 release date
- Changed sync vendored tooltip lib to 1.5.19
- Added capture backend selection and prefer borderless capture
- Changed add changelog for 2.0.3
- Changed describe dashboard-controlled update checks
- Changed cover capture-fps preference parsing
- Fixed cancel superseded tooltip analysis requests
- Added control automatic updates from the dashboard
- Fixed pace gpu readbacks in wgc capture strategy
- Added cloud-synced capture-rate control
- Fixed validate start menu shortcut in per-user or shared programs
- Changed ensure nsis creates canonical grimvault start menu shortcut
- Fixed point nsis shortcuts to root-level grimvault executable
- Changed move runtime dll staging to post-build step for cpack resolution
- Changed stage direct runtime dlls before scanning dependencies
- Fixed include runtime dependencies and validate installed binary
- Fixed remove incompatible NSIS header bitmap
- Fixed remove incompatible NSIS welcome bitmap
- Fixed stage NSIS assets before packaging
- Fixed shorten Windows package staging path
- Fixed bypass batch parsing for Windows signing
- Fixed expose release DLLs to Windows tests
- Fixed unblock accelerated Windows release
- Changed accelerate Windows releases
- Changed map workspace to short drive for vcpkg path-length fix
- Fixed quote cmake args in windows release workflow
- Changed sync darkerdb tooltip bundle to 1.5.18
- Added harden production and minor artifact analysis
- Changed reconcile preserved checkout with canonical trunk
- Added preserve GrimVault desktop overhaul snapshot
- Changed merge canonical grimvault OAuth client naming
- Added merge complete grimvault market analysis
- Changed sync DarkerDB tooltip bundle 1.5.16
- Added complete grimvault overhaul and tooltip market refinements
- Added overhaul grimvault capture, auth, settings, and release flow
- Changed sync darkerdb tooltip bundle 1.5.12 with vendor-price removal
- Changed sync darkerdb tooltip bundle 1.5.11
- Changed resync darkerdb tooltip bundle after vendor-placement fix
- Changed resync vendored ddb-tooltips to 1.5.9
- Added overlay column layout preference
- Fixed remove stray merge conflict markers from augment_view
- Changed sync the vendored library to 1.5.8
- Changed sync
- Changed [DDB-5] docs: rename OAuth client_id grimvault-desktop to grimvault

## [0.0.3-rc.3] - 2026-08-10

- Changed stamp the 2.0.3 release date
- Changed sync vendored tooltip lib to 1.5.19
- Added capture backend selection and prefer borderless capture
- Changed add changelog for 2.0.3
- Changed describe dashboard-controlled update checks
- Changed cover capture-fps preference parsing
- Fixed cancel superseded tooltip analysis requests
- Added control automatic updates from the dashboard
- Fixed pace gpu readbacks in wgc capture strategy
- Added cloud-synced capture-rate control
- Fixed validate start menu shortcut in per-user or shared programs
- Changed ensure nsis creates canonical grimvault start menu shortcut
- Fixed point nsis shortcuts to root-level grimvault executable
- Changed move runtime dll staging to post-build step for cpack resolution
- Changed stage direct runtime dlls before scanning dependencies
- Fixed include runtime dependencies and validate installed binary
- Fixed remove incompatible NSIS header bitmap
- Fixed remove incompatible NSIS welcome bitmap
- Fixed stage NSIS assets before packaging
- Fixed shorten Windows package staging path
- Fixed bypass batch parsing for Windows signing
- Fixed expose release DLLs to Windows tests
- Fixed unblock accelerated Windows release
- Changed accelerate Windows releases
- Changed map workspace to short drive for vcpkg path-length fix
- Fixed quote cmake args in windows release workflow
- Changed sync darkerdb tooltip bundle to 1.5.18
- Added harden production and minor artifact analysis
- Changed reconcile preserved checkout with canonical trunk
- Added preserve GrimVault desktop overhaul snapshot
- Changed merge canonical grimvault OAuth client naming
- Added merge complete grimvault market analysis
- Changed sync DarkerDB tooltip bundle 1.5.16
- Added complete grimvault overhaul and tooltip market refinements
- Added overhaul grimvault capture, auth, settings, and release flow
- Changed sync darkerdb tooltip bundle 1.5.12 with vendor-price removal
- Changed sync darkerdb tooltip bundle 1.5.11
- Changed resync darkerdb tooltip bundle after vendor-placement fix
- Changed resync vendored ddb-tooltips to 1.5.9
- Added overlay column layout preference
- Fixed remove stray merge conflict markers from augment_view
- Changed sync the vendored library to 1.5.8
- Changed sync
- Changed [DDB-5] docs: rename OAuth client_id grimvault-desktop to grimvault

## [2.0.4] - 2026-08-10

### Fixed

- Exit promptly when the updater asks so the installer never races a running GrimVault.
- Make the installer wait for a running GrimVault to close before extracting, instead of failing on locked files.

## [2.0.3] - 2026-08-10

### Added

- Add cloud-synced capture-rate controls to reduce GPU use.
- Add a cloud-synced capture backend setting: automatic, or force WGC, DXGI, or GDI.

### Changed

- Move automatic-update control to the dashboard and default it to enabled.

### Fixed

- Cancel superseded tooltip requests and pace GPU readbacks at the configured rate.
- Avoid Windows' yellow capture border on Windows 10 by preferring borderless capture backends when WGC cannot suppress it.

## [2.0.2] - 2026-08-09

### Fixed

- Point installed Start Menu and desktop shortcuts at the root-level GrimVault executable.
- Validate the installed Start Menu shortcut target before publishing a release.

## [2.0.1] - 2026-08-09

### Fixed

- Include the complete native dependency closure and Visual C++ runtime so a clean installation starts successfully.
- Install models, translations, assets, and schema as one required payload without component selection.
- Launch the installed binary during release validation so incomplete packages cannot publish.

## [2.0.0] - 2026-08-09

GrimVault 2.0 is a native 64-bit Windows companion for Dark and Darker. It
watches the game for item tooltips, prices what you are hovering, and renders
the result as an analysis card over the game window.

**In-game overlay**

- Tray-resident overlay that anchors a GrimVault analysis card to the hovered item tooltip and renders it through WebView2.
- Automatic, manual, and disabled modes. Automatic scans stable tooltips, manual scans only on request, and disabled performs no capture or API work.
- One-, two-, and three-column card layouts. Automatic mode widens only when the current card exceeds the available height and the wider card still fits.
- Per-section visibility toggles for the analysis card, plus overlay alignment, opacity, scale, offsets, and hotkeys for toggling the overlay and forcing a refresh.
- Localized interface in German, English, Spanish, French, Japanese, Korean, Brazilian Portuguese, Russian, Simplified Chinese, and Traditional Chinese.

**Screen capture and OCR**

- Windows Graphics Capture, DXGI Desktop Duplication, and GDI capture backends, probed at startup so the first backend that initializes wins.
- Automatic degradation under failure. Continuous WGC timeouts drop to per-call WGC, three consecutive failures advance WGC to DXGI and DXGI to GDI, a successful frame resets the counter, and a new capture target reprobes the preferred backends.
- Multi-monitor layouts, per-monitor display scaling, negative monitor origins, window resizes, and resolution changes, with capture coordinates local to the captured frame.
- Tooltip detection and localized OCR that send raw text, confidence, language, and capture backend to the analyzer, including line-to-gem-family observations for socketed items.
- Queue pressure drops obsolete frames instead of blocking capture or shutdown.

**Pricing and market intelligence**

- Trade Chat is available for minor artifacts as well as uniques and major artifacts.
- One authenticated analyze request returns the whole card atomically: canonical item, parsed roll vector, valuation, and context.
- Similarity- and recency-weighted valuation from sold comparables, with quick-list guidance, active listings, trend, liquidity, median sale time, days of supply, roll-aware price stability, and confidence.
- Per-roll quality, a market-relative percentile, and the strongest observed value driver measured against that roll's legal minimum.
- Comparable sales drawn from the same bounded nearest-roll set the estimate uses, including each sale's complete localized roll vector.
- Highest-valued legal one- and two-gem replacements, evaluated at the item's maximum enchanted ranges with socket fees included and already-detected gems left immutable.
- Vendor value, quest turn-ins, recipes, adventure points, gear score, stack size, and market value per inventory slot.
- Best-drop provenance with zero-luck and 500-luck rates plus alternate sources, or merchant acquisition context for items that cannot be traded.
- Recent trade-chat mentions of the hovered item from the last 14 days.

**Account sync and entitlements**

- Sign-in through DarkerDB using OAuth 2.0 authorization code with PKCE, with tokens held in Windows Credential Manager and scoped per environment.
- Cloud-synced settings. Overlay, tooltip sections, pricing display, behavior, and hotkeys follow the account across machines.
- Tokens, settings, sessions, and history isolated by environment and account. Switching accounts clears server-managed settings and pending analysis before the new session starts.
- Entitlement-aware card. Sections the current plan does not grant name the tier that unlocks them instead of rendering a silent gap.
- Persistent onboarding, settings-sync, degraded-network, and reauthentication states, with the tray header as the source of truth for signed out, syncing, ready, or degraded.
- Token refresh serialized by an environment-scoped named mutex so the app and the CLI cannot rotate the same refresh token at once.

**Auto-update and distribution**

- The GrimVault appraisal-lens mark across the tray, executable, desktop shell, and installer, with multi-resolution icons and branded installer art.
- Per-user installer that needs no elevation, with Start Menu and desktop shortcuts and a silent WebView2 Evergreen bootstrap when the runtime is missing.
- SSL.com-signed `grimvault.exe` and installer. Release CI installs the package silently and verifies Authenticode status, publisher metadata, icon resources, and executable identity.
- WinSparkle auto-update from signed appcasts, with a separate Ed25519 enclosure signature verified against the public key embedded in the client, and separate stable and beta feeds per environment.
- Stable production releases refresh the no-cache `latest` installer and checksum aliases the website links to.
- Every release builds and tests on two supported Windows images, covering the Windows 10 version 1809 or newer and Windows 11 compatibility matrix and display-scale end-to-end tests.

**Diagnostics and reliability**

- Mandatory TLS verification outside an explicit local dev opt-out, redirects disabled for bearer requests, certificate revocation checks where Schannel supports them, and Windows-native certificate authorities trusted from the OpenSSL HTTP backend.
- Signed production binaries ignore runtime environment overrides so API, authentication, credentials, local data, and updates remain pinned to production.
- Strict response size caps and shape validation, bounded retries for 429, 502, 503, and 504, and cancellation of in-flight OAuth and API requests on sign-out and shutdown.
- Repeated-hover analysis cached in memory for at most 30 seconds, partitioned by account and capped at 64 entries.
- Per-request client metrics for DNS, connect, TLS, first byte, total time, API request id, and server phase timings.
- Rotating logs and crash minidumps under the GrimVault data directory, with per-version symbol archives published alongside each release.
- No API analysis response or raw market data is persisted. History keeps only the item id, a minimal roll projection, the account, and the session, bounded by age and row count.

## [0.0.2] - 2026-07-28

### Changed
- Rebuild minified tooltip script

## [0.0.1] - 2026-07-19

### Added
- Add tooltip anchoring and webview2 augment

### Changed
- Sync
- [DDB-12] docs: note Windows+WSL git fileMode pitfall in DEV.md
- [DDB-12] feat: C++/Qt6 rewrite of GrimVault
- [DDB-12] chore: remove Electron stack
- Sync (alias).
- Sync
- Sync (alias).
- Sync (alias).
- Syncing for palette de-couple.
- Fixing COM issues.
- V1.0.13
- V1.0.12
- V1.0.11
- V1.0.9
- V1.0.8
- Initial upload.
