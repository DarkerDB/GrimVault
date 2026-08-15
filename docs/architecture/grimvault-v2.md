# GrimVault V2 architecture

## Runtime boundary

GrimVault is a native x64 Windows client. Game capture, Credential Manager,
WebView2, and WinSparkle are Windows facilities. Portable C++ is used below
those adapters.

Supported runtime: 64-bit Windows 10 version 1809 or newer and Windows 11.

## Data flow

```text
game client
  -> WGC, DXGI, or GDI capture
  -> tooltip detector
  -> language OCR and gem-glyph recognition
  -> authenticated POST /v2/grimvault/analyze
  -> WebView2 renderer or QML fallback
```

The client never persists API analysis responses or raw market data.
History stores only the item id, minimal roll projection, account id, and
session id. History is bounded by age and row count.

## Compatibility rules

- Capture coordinates are local to the captured frame, including monitors with negative origins.
- WGC recreates resources after resolution or window-size changes.
- DXGI selects the target monitor and adapter and recovers after duplication access loss.
- Startup probes WGC, DXGI, then GDI and selects the first backend that initializes.
- WGC continuous timeouts degrade to per-call WGC before counting backend failures.
- Three consecutive per-call failures advance WGC to DXGI, then DXGI to GDI. A successful frame resets the counter.
- A new capture target resets failures and reprobes preferred backends, so transient failure does not pin the process to GDI.
- Unavailable runtime candidates are skipped. GDI remains the last resort when GPU capture fails.
- WebView2 absence does not drop results. The installer bootstraps it, and QML can render the same last result.
- Automatic mode scans stable tooltips. Manual mode scans only on request. Disabled mode performs no capture or API work.
- Queue pressure drops obsolete frames rather than blocking capture or shutdown.

## Identity isolation

OAuth tokens are environment-scoped in Windows Credential Manager.
Production also mirrors the legacy target for old-client compatibility.

Local settings and history are account-scoped. An account switch clears
server-managed settings and pending analysis before the new session starts.
Dev and QA use separate data directories.

Refresh is protected by an environment-scoped named mutex so the GUI and CLI
cannot rotate the same refresh token concurrently.

## Onboarding and recovery

The tray header is the source of truth for connection state: signed out,
syncing, ready, or degraded. A fresh install opens OAuth, stores tokens in
Credential Manager, downloads account settings, then becomes ready.

Settings failures retain safe local defaults and retry in the background.
Expired or revoked credentials clear managed settings, return to signed out,
and reopen OAuth unless automatic login was explicitly disabled.

## Transport

- TLS verification is mandatory except for explicit local dev opt-out.
- Redirects are disabled for bearer requests.
- Certificate revocation checks are enabled where Schannel supports them.
- Responses have strict size caps and shape validation.
- `429`, `502`, `503`, and `504` use bounded retries.
- Sign-out and shutdown cancel active OAuth and API requests.
- Identical analyses are cached in memory for at most 30 seconds, partitioned
  by account and capped at 64 entries.
- Structured client metrics include DNS, connect, TLS, first-byte, total, API
  request id, and server phase timings.

## Server contract

The primary endpoint is authenticated `POST /v2/grimvault/analyze` on
`api.darkerdb.com`. `POST /v2/grimvault/lookup` remains an older-client
compatibility contract.

The analyzer receives OCR text, confidence, language, capture backend, and
optional line-to-gem-family observations. Its market estimate and comparable-
sales widget share one bounded nearest-roll set. Existing detected gems are
immutable when upgrade paths are generated. Entitlement projection occurs on
the server before serialization.

Analysis layout accepts one, two, or three columns. Automatic mode escalates
only when the current card exceeds the available height and the wider card
fits the available width.

## Release trust

Every release builds and tests on two supported GitHub Windows images.
SSL.com eSigner signs both `grimvault.exe` and the NSIS installer. CI then
installs the package silently and verifies the installed executable's
Authenticode signature and GrimVault metadata.

WinSparkle appcasts use a separate Ed25519 enclosure signature and publish
through `https://releases.darkerdb.com`. Stable production releases also
refresh a no-cache `grimvault/latest/GrimVault-Setup.exe` alias for the website.
