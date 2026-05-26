# Release engineering

## Pipeline overview

Releases are cut by publishing a GitHub Release (tag `vX.Y.Z`) on this
repo. `.github/workflows/release.yml` fires on the `release: published`
event and delegates to the reusable workflow in `katforge/hearth`:

```
.github/workflows/release.yml
   → katforge/hearth/.github/workflows/release-template-desktop-windows.yml
        → build (CMake release-windows preset, vcpkg, ninja, MSVC)
        → unit tests (ctest --preset unit)
        → e2e tests   (matrix: 3 resolutions × 5 modes, fixtures from S3)
        → sign        (SSL.com eSigner CodeSignTool, cloud HSM)
        → upload      (artifacts/* → s3://katforge-releases/grimvault/<tag>/)
        → publish     (prepend <item> to appcast.xml, ed25519-sign enclosure)
        → attach      (gh release upload <tag> artifacts/*)
```

Source-of-truth for what Hearth actually does:
`/home/ethan/Projects/katforge/hearth/.github/workflows/release-template-desktop-windows.yml`.

## Auto-update

Client uses **WinSparkle 0.8+ with ed25519 signature verification**.

- Public key: baked into the binary from `cmake/AppcastUrl.cmake`
  → `GRIMVAULT_APPCAST_PUBKEY` (base64-encoded 32-byte ed25519).
- Private key: lives in GitHub Secrets as `APPCAST_ED25519_PRIV`, used
  by Hearth's `publish-appcast` action to sign installer enclosures.
- Appcast URL is hardcoded:
  `https://katforge-releases.s3.us-west-2.amazonaws.com/grimvault/appcast.xml`.
- The client refuses to enable auto-updates if the pubkey constant is
  empty (`gv::update::UpdateService::start` fail-closed).

## Channels

Only `stable` is shipped at v1. Hearth's template supports a second
appcast (`appcast-beta.xml`) keyed off the GitHub Release prerelease
flag. The client only knows about one URL, so beta opt-in is not
user-facing yet. To enable it later: add a runtime channel toggle that
swaps the base URL between `appcast.xml` and `appcast-beta.xml` (single
binary, single S3 layout, no rebuild).

## Code signing

SSL.com eSigner OV cert via CodeSignTool (cloud HSM, no local key
material). Wired in `cmake/Signing.cmake` for local dry-runs; real CI
signing happens in Hearth's `release-template-shared/sign-windows`
action. Required secrets in the consuming repo:

- `SSL_COM_USERNAME`
- `SSL_COM_PASSWORD`
- `SSL_COM_CREDENTIAL_ID`
- `SSL_COM_TOTP_SECRET`

### Known issue: SmartScreen "Windows protected your PC"

OV certs do **not** carry instant SmartScreen reputation. For the first
~hundreds of downloads, Windows 10/11 will show "Windows protected your
PC" on first launch of the installer. Users must click "More info" →
"Run anyway".

This is expected for v1. Mitigations we explicitly chose **not** to do:

- EV cert upgrade — deferred indefinitely; we accept the early friction
  in exchange for cloud HSM simplicity.
- Skipping code-signing — would make it strictly worse (Defender
  SmartScreen treats unsigned binaries as malicious by default).

The reputation builds automatically with download volume; expect the
prompt to disappear within a few weeks of consistent releases.

## Install layout

Per-user install (no admin required, no UAC prompt). Matches VS Code,
Discord, GitHub Desktop convention.

- Install root: `%LOCALAPPDATA%\Programs\GrimVault\`
- Per-user data: `%APPDATA%\GrimVault\` (database, logs, settings)
- Start Menu shortcut + desktop link

The app manifest declares `asInvoker` (not `requireAdministrator`).
WGC capture works fine unelevated; nothing in GrimVault needs admin
at runtime.

## Symbols / crash reports

`gv::core::CrashHandler` writes minidumps to `%APPDATA%\GrimVault\crashes\`.
PDBs are produced by the Release-with-IPO build but are **not currently
uploaded to a symbol server**. Hearth's template does not handle this
either. Symbolicating a user-supplied minidump today requires the PDB
from the matching tagged build — pull it from the GitHub Release attach
step if needed (currently only the installer is attached; PDB upload is
a Phase 3 task).
