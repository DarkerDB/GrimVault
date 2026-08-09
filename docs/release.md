# Release engineering

## Pipeline

`.github/workflows/release.yml` is standalone. It does not depend on a
cross-organization reusable workflow.

A published GitHub Release runs this gate:

1. Validate `vX.Y.Z` or `vX.Y.Z-rc.N`. A prerelease version fails unless the
   resolved channel is `beta`, so an `-alpha`, `-beta`, or `-rc` build cannot
   reach the stable feed or the `latest` alias.
2. Build and test on `windows-2022` and `windows-2025`.
3. Fetch the Microsoft-signed WebView2 Evergreen bootstrapper.
4. Sign `grimvault.exe` with the SSL.com eSigner certificate.
5. build the branded NSIS installer.
6. Sign the installer with the same certificate.
7. Install the required monolithic payload silently, verify Authenticode, publisher metadata, icon resources, and executable identity, then launch the installed CLI.
8. Publish the installer, SHA-256 file, symbols, and signed WinSparkle appcast.
9. For stable production, atomically refresh the website's no-cache `latest` installer and checksum aliases.

The `windows-2022` arm packages the release. The `windows-2025` arm is an
independent compatibility build and test gate.

Manual runs can target `dev`, `qa`, or `prod`. Publishing is opt-in for a
manual run. Published GitHub Releases target `prod`.

Production binaries are environment-locked. They ignore `--env`,
`GRIMVAULT_ENV`, and `APP_ENV`, so their API, authentication, data, credential,
and update environments cannot drift after signing. Development and QA builds
retain the supported `--env` and `GRIMVAULT_ENV` overrides.

## Required secrets

The workflow fails closed when signing material is absent.

- `SSL_COM_USERNAME`
- `SSL_COM_PASSWORD`
- `SSL_COM_CREDENTIAL_ID`
- `SSL_COM_TOTP_SECRET`
- `APPCAST_ED25519_PRIV`
- `AWS_ACCESS_KEY_ID`
- `AWS_SECRET_ACCESS_KEY`

`tools/release/sign-windows.ps1` downloads SSL.com's official CodeSignTool,
signs in place, and verifies both PowerShell Authenticode status and
`signtool verify /pa`.

## Auto-update

WinSparkle verifies every enclosure with the Ed25519 public key embedded by
`cmake/AppcastUrl.cmake`.

| Environment | Stable feed | Beta feed |
|---|---|---|
| `dev` | disabled | disabled |
| `qa` | `https://releases.katforge.com/grimvault/qa/appcast.xml` | `https://releases.katforge.com/grimvault/qa/appcast-beta.xml` |
| `prod` | `https://releases.katforge.com/grimvault/appcast.xml` | `https://releases.katforge.com/grimvault/appcast-beta.xml` |

Artifacts remain private in `s3://katforge-releases`. CloudFront OAC serves
the public `releases.katforge.com` URLs.

The appcast generator verifies that the configured private key derives the
embedded public key before publishing.

The V2 website cutover should download
`https://releases.katforge.com/grimvault/latest/GrimVault-Setup.exe` only after
the first stable production publish has created it. Only stable production
publishes update that alias. Versioned release artifacts stay immutable.

## Compatibility

The supported runtime is 64-bit Windows 10 version 1809 or newer and
Windows 11.

- WGC is preferred and recreates its frame pool after game resize.
- DXGI selects the correct adapter and monitor, crops to the client area, and recovers from access loss.
- Three consecutive capture failures advance WGC to DXGI, then DXGI to GDI.
- Successful frames reset the failure threshold. A new capture target also reprobes preferred backends.
- Unavailable runtime candidates are skipped. GDI remains the last-resort backend.
- Negative monitor origins and per-monitor DPI are supported.
- WebView2 is bootstrapped when missing. QML remains the renderer fallback.
- The app and installer run per-user without elevation.
- The installer exposes no optional components. Models, translations, assets, and schema are required.
- Runtime dependency discovery and a post-install launch gate prevent incomplete packages from publishing.

## Branding

The executable, installer, Start Menu shortcut, desktop shortcut, welcome
art, uninstaller, and version metadata all use GrimVault branding. Release
validation requires the Start Menu shortcut to resolve to the installed
executable before publication.

Source art is under `assets/images/`. Installer art is under
`assets/installer/`. Regenerate both with `tools/build/png_to_ico.py`.

## Layout

- Install: `%LOCALAPPDATA%\Programs\GrimVault\`
- Production data: `%LOCALAPPDATA%\GrimVault\`
- Dev and QA data: `%LOCALAPPDATA%\GrimVault\<env>\`
- Credential target: `GrimVault:tokens:<env>`

Production reads and writes the legacy `GrimVault:tokens` target during the
V2 transition so older clients remain signed in. Dev and QA never share it.

## Symbols

Release PDBs are archived as `GrimVault-Symbols-<version>.zip` and attached
to the workflow artifact and GitHub Release. Local minidumps are retained
under the GrimVault data directory.
