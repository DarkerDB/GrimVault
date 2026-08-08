# Changelog

All notable changes to this project will be documented in this file.

## [2.0.0] - Unreleased

GrimVault 2.0 is a native 64-bit Windows companion for Dark and Darker. It
watches the game for item tooltips, prices what you are hovering, and renders
the result as an analysis card over the game window.

**In-game overlay**

- Tray-resident overlay that anchors a GrimVault analysis card to the hovered item tooltip, rendered in WebView2 with a QML renderer as the fallback when the runtime is absent.
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
