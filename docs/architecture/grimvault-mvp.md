# GrimVault MVP architecture

> Historical V1 design record. The current client contract is
> [`grimvault-v2.md`](grimvault-v2.md). Where the documents differ, V2 wins.

Status: contract. Every other agent builds against this. Decisions are
settled; resolved rulings are appended in §13.

Companion contract: see `docs/architecture/auth-gateway.md` for the
cross-realm `auth.{realm}.com` gateway, Spark brand-bundle data model,
parent-domain cookie scope, and the four sign-in journeys. The GrimVault
desktop OAuth flow in §3 is unchanged by that work (Journey 4 in the
sibling doc points back here).

---

## 1. Glossary

| Term | Meaning |
|---|---|
| GrimVault | The desktop overlay companion app for "Dark and Darker". One word, capital V. |
| Overlay | The in-game tooltip surface rendered by `qml/Tooltip.qml` over the game window. |
| DarkerDB | The public website and price-data brand at `darkerdb.com`. Every URL the desktop client touches lives under this origin. |
| KATforge | Private multi-tenant identity, auth, API-key, and billing platform. Powers DarkerDB and other "shadow realm" sites. Invisible to the desktop client and end users. |
| Shadow realm | Internal label for the family of sites KATforge powers (DarkerDB is one). Not user-facing. |
| Desktop binary | `grimvault.exe`, a Qt/QML Windows app. Tray-resident infrastructure: capture, OCR, overlay, OAuth client, token daemon. No GUI pages. |
| CLI | `grimvault` on PATH. Thin shim over the same process model for advanced/debug use. Shares tokens with the overlay. |

Relationship.

```
   user ──► darkerdb.com ───────────► KATforge (private)
                  ▲                        ▲
                  │ user-facing            │ server-to-server
                  │                        │
   grimvault.exe ─┘                        │
   (only ever sees darkerdb.com)           │
                                           │
   other shadow-realm sites ───────────────┘
```

Why every URL is `darkerdb.com`: the desktop client is branded to the
data product, not the platform. KATforge is an implementation detail.
Re-platforming (embedding, splitting, swapping) must not require a
client change.

---

## 2. System topology

```
   +-------------------------------------------------------------------+
   |                         User's Windows PC                         |
   |                                                                   |
   |  +-------------------+        +-----------------------+           |
   |  |  grimvault.exe    |        |  grimvault (CLI)      |           |
   |  |  (tray, overlay,  |◄──────►|  (same binary, shared |           |
   |  |   OCR, capture,   |  IPC*  |   keychain + state)   |           |
   |  |   token daemon)   |        +-----------------------+           |
   |  +---------+---------+                                            |
   |            │                                                      |
   |            │ reads/writes                                         |
   |            ▼                                                      |
   |  +-------------------+   +----------------------+                 |
   |  | %APPDATA%\        |   | Windows Credential   |                 |
   |  |  GrimVault\       |   |  Manager (DPAPI)     |                 |
   |  |   settings.toml   |   |   access_token       |                 |
   |  |   prices.sqlite   |   |   refresh_token      |                 |
   |  |   logs\           |   |   pkce_verifier (tx) |                 |
   |  +-------------------+   +----------------------+                 |
   |            │                                                      |
   +────────────┼──────────────────────────────────────────────────────+
                │ HTTPS (TLS 1.2+)
                │
                │   browser  ──────────────► darkerdb.com  (Vue/Vite SPA)
                │                              │  consent screen, dashboard,
                │                              │  connected-apps page
                │                              ▼
                │                            api.darkerdb.com  (XHR from SPA)
                ▼                              │
        api.darkerdb.com  ◄───────────────────┘
        (Symfony API on api.katforge.com codebase, /v2/* mount)
                │
                │   /oauth/token, /oauth/revoke
                │   /v1/oauth/authorize/validate, /v1/oauth/authorize
                │   /v1/oauth/grants, /v1/oauth/grants/:client_id[/revoke]
                │   /v1/gateway/login/url
                │   /v2/grimvault/analyze, /lookup, /ping
                ▼
   +-------------------------------------------------------------------+
   |     api.katforge.com (Symfony) — single codebase, single deploy   |
   |                                                                   |
   |   Serves all of:                                                  |
   |     api.katforge.com         (canonical /v1/*)                    |
   |     api.darkerdb.com         (realm /v2/*, host implies realm)    |
   |     api.{dev,qa}.katforge.com / api.{dev,qa}.darkerdb.com         |
   |                                                                   |
   |   Identity, OAuth issuance + validation (Lexik JWT),              |
   |   refresh-token rotation, jti denylist, JWKS,                     |
   |   per-realm controllers under KAT\Realm\DarkerDB\Controller\*.    |
   +-------------------------------------------------------------------+
```

The "DarkerDB backend" and "KATforge backend" are the same Symfony
process. The boundary is namespace (`KAT\Realm\DarkerDB\Controller`)
and route file (`config/routes/darkerdb.yaml`), not network. See §11
for the implications.

`* IPC` between overlay and CLI is out of MVP. CLI talks to the same
on-disk state and the same keychain entry; if the overlay is running,
the CLI's mutations are picked up on next file/keychain read. See
section 8 for the explicit guarantees.

### 2.1 Host map

| Host (prod) | What it serves |
|---|---|
| `darkerdb.com` | Vue/Vite SPA. All user-facing OAuth pages (`/oauth/authorize` consent), `/dashboard/grimvault`, `/account/connected-apps`. No business logic; calls the API host for everything. |
| `api.darkerdb.com` | Symfony API (same codebase as `api.katforge.com`). All endpoints the desktop client calls and all endpoints the SPA calls. |
| `api.katforge.com` | Same Symfony codebase, alternate host. KATforge-canonical paths. The desktop client never hits this host directly; mentioned here only to make the shared-codebase fact unambiguous. |

Per-env hostnames in §10.

---

## 3. OAuth flow

Standard OAuth 2.0 Authorization Code with PKCE, loopback redirect.
No client secret. Reference: RFC 7636.

### 3.1 Endpoint summary

| Endpoint | Host | Purpose |
|---|---|---|
| `GET  /oauth/authorize` | `darkerdb.com` | SPA consent screen. Renders UI; calls API for validation and code issuance. |
| `GET  /v1/oauth/authorize/validate` | `api.darkerdb.com` | SPA-side: confirms `client_id` + `redirect_uri` + returns scope descriptions for the consent screen. |
| `POST /v1/oauth/authorize` | `api.darkerdb.com` | SPA-side: user-confirmed code issuance. Browser then 302s to `redirect_uri`. |
| `POST /oauth/token` | `api.darkerdb.com` | Code exchange and refresh. Called by desktop. |
| `POST /oauth/revoke` | `api.darkerdb.com` | RFC 7009 token revocation. Called by desktop on logout. See §3.4 and §11 for how this relates to `/v1/oauth/grants/:client_id/revoke`. |

### 3.2 Step-by-step

1. User clicks "Sign in to DarkerDB" in the tray menu.
2. Client generates:
   - `code_verifier`: 64 bytes from a CSPRNG, base64url, no padding (length 86).
   - `code_challenge = base64url(SHA256(code_verifier))`, no padding.
   - `state`: 32 bytes from CSPRNG, base64url, no padding.
3. Client binds a loopback HTTP server on `127.0.0.1:0` (kernel picks free
   port). Records the chosen port. Server only accepts one request, only on
   path `/callback`, only for `state` matching step 2. Lifetime: 120s or until
   first valid callback, whichever first.
4. Client launches the user's default browser to the SPA consent
   route on `darkerdb.com`:

   ```
   https://darkerdb.com/oauth/authorize
      ?response_type=code
      &client_id=grimvault-desktop
      &redirect_uri=http%3A%2F%2F127.0.0.1%3A<port>%2Fcallback
      &scope=grimvault.read%20grimvault.write
      &state=<state>
      &code_challenge=<code_challenge>
      &code_challenge_method=S256
   ```

5. SPA boots, parses the query, and calls the API to validate the
   request and fetch scope metadata for display:

   ```
   GET https://api.darkerdb.com/v1/oauth/authorize/validate
      ?client_id=grimvault-desktop
      &redirect_uri=http%3A%2F%2F127.0.0.1%3A<port>%2Fcallback
      &scope=grimvault.read%20grimvault.write
   ```

   Response (see §4.8 for the scope-registry contract):

   ```json
   {
      "body": {
         "client": {
            "id":             "grimvault-desktop",
            "name":           "GrimVault",
            "is_first_party": true,
            "scopes": [
               { "id": "grimvault.read",  "description": "Look up item prices on your behalf." },
               { "id": "grimvault.write", "description": "Submit price observations on your behalf." }
            ]
         }
      }
   }
   ```

6. User clicks Allow. SPA POSTs to the API to issue the code:

   ```http
   POST /v1/oauth/authorize HTTP/1.1
   Host: api.darkerdb.com
   Content-Type: application/json
   Cookie: <katforge session cookie>

   {
      "client_id":             "grimvault-desktop",
      "redirect_uri":          "http://127.0.0.1:<port>/callback",
      "response_type":         "code",
      "state":                 "<state>",
      "scope":                 "grimvault.read grimvault.write",
      "code_challenge":        "<code_challenge>",
      "code_challenge_method": "S256"
   }
   ```

   Response body contains the issued code; SPA then triggers a browser
   navigation to:

   ```
   http://127.0.0.1:<port>/callback?code=<code>&state=<state>
   ```

   On Cancel, the SPA navigates to `redirect_uri?error=access_denied&state=<state>` per RFC 6749 §4.1.2.1.

7. Loopback server validates `state`, captures `code`, responds 200 with
   a **brand-styled HTML page** (DarkerDB header, GrimVault wordmark in
   pure CSS, body text "Signed in. You can close this tab."), then
   shuts down. The page is standalone and self-contained: inline CSS,
   no external image refs, no external font refs. Embedded in the
   binary as a Qt resource. Source asset shipped at
   `~/.katforge/realms/darkerdb.com/grimvault/oauth-close.html`.
8. Client POSTs to `/oauth/token`:

   ```http
   POST /oauth/token HTTP/1.1
   Host: api.darkerdb.com
   Content-Type: application/x-www-form-urlencoded

   grant_type=authorization_code
   &code=<code>
   &redirect_uri=http://127.0.0.1:<port>/callback
   &client_id=grimvault-desktop
   &code_verifier=<code_verifier>
   ```

9. Success response:

   ```json
   {
      "access_token":  "<JWT, RS256-signed>",
      "token_type":    "Bearer",
      "expires_in":    900,
      "refresh_token": "<opaque>",
      "scope":         "grimvault.read grimvault.write"
   }
   ```

   Access token is a **JWT (RS256)** issued and signed by KATforge. See
   §3.8 for claim shape and verification. Refresh token is opaque.
   Access-token TTL is 900s (15 min), short by design so revocation
   lag is bounded without a per-request introspection call.

10. Client stores `access_token`, `refresh_token`, `expires_at` (computed
    = now + `expires_in` - 60s skew) into Windows Credential Manager
    under target `GrimVault:tokens` (single blob, JSON, DPAPI-protected
    by the OS). `code_verifier` is discarded. The client treats the
    access token as opaque: it does not parse the JWT or trust its
    claims for any local decision other than `expires_at`.
11. Tray flips to "Signed in as <handle>". Overlay enables network path.

### 3.3 Refresh

Triggered when any outbound request finds `now >= expires_at`, OR
proactively by the token daemon when `expires_at - now < 3 min`
(20% of the 15-min access-token TTL).

```http
POST /oauth/token HTTP/1.1
Host: api.darkerdb.com
Content-Type: application/x-www-form-urlencoded

grant_type=refresh_token
&refresh_token=<refresh_token>
&client_id=grimvault-desktop
```

Refresh-token rotation is **required by this contract**: each refresh
MUST return a new `refresh_token`, and the previous refresh token MUST
be invalidated server-side on use. Client MUST replace the stored
value atomically (write new keychain blob, then discard old) before
issuing the next request. Reuse of a consumed refresh token MUST
return `invalid_grant` and SHOULD trigger family-wide revocation
(RFC 6749 §10.4, OAuth 2.1 §6.1).

If the API engineer discovers that KATforge does not yet support
rotation, that is a hard blocker for MVP. Surface immediately rather
than ship a non-rotating fallback.

### 3.4 Revocation

Two distinct endpoints, two distinct purposes. The desktop client uses
the RFC-7009 token-revocation endpoint; the SPA's connected-apps page
uses the grant-revocation endpoint (see §11.1 for the rationale).

User-initiated from the desktop (CLI `logout` or "Sign out" tray item):

```http
POST /oauth/revoke HTTP/1.1
Host: api.darkerdb.com
Content-Type: application/x-www-form-urlencoded

token=<refresh_token>
&token_type_hint=refresh_token
&client_id=grimvault-desktop
```

Then locally: delete keychain entry, clear in-memory copies, set state
machine to `no_token`. Server-initiated revocation (user clicked
Disconnect on the web) is observed as a `401 invalid_token` on the
next API call once the access-token `jti` lands in the denylist (§3.9).

### 3.5 PKCE math summary

| Step | Operation |
|---|---|
| Generate | `code_verifier = base64url(CSPRNG(64))` |
| Derive | `code_challenge = base64url(SHA256(code_verifier))` |
| Encode | Both base64url, no `=` padding. Verifier length 43..128 chars (we use 86). |
| Send up | `code_challenge` + `code_challenge_method=S256` on `/authorize` |
| Send up | `code_verifier` on `/token` |
| Server | Recomputes `SHA256(code_verifier)`, compares to stored challenge. |

### 3.6 Error handling per step

| Step fails | Client behavior |
|---|---|
| Loopback bind fails | Surface tray error "could not start sign-in (port unavailable)". Retry on next click. |
| Browser launch fails | Show tray notification with copyable URL fallback. |
| `state` mismatch on callback | Reject. Loopback responds 400. Tray notifies "sign-in rejected (state mismatch), try again". |
| Callback timeout (120s, no hit) | Loopback shuts down. Tray returns to `no_token`. No retry storm. |
| `/oauth/token` 4xx | Show error class (`invalid_grant`, `invalid_client`, etc.). State returns to `no_token`. |
| `/oauth/token` 5xx or network | Exponential backoff for up to 3 attempts (1s, 4s, 16s). Then surface as transient error. State stays at `authing`. |
| `/oauth/token` refresh `invalid_grant` | Refresh token is dead. Wipe keychain. State → `no_token`. Tray prompts re-sign-in. |
| `/oauth/token` refresh transient | Backoff as above. Keep state `authed` if access token still valid; else `refreshing`. |

### 3.7 Token state machine

```
                         user clicks "Sign In"
   +----------+   ───────────────────────────────────►   +----------+
   | no_token |                                          | authing  |
   +----------+                                          +-----+----+
        ▲                                                      │
        │ revoke / invalid_grant                               │ token exchange ok
        │                                                      ▼
        │                                                +----------+
        │                            access ok           |  authed  |◄──────┐
        │                       ┌──────────────────────  +-----+----+       │
        │                       │                              │            │
        │                       │                refresh due   │            │ refresh ok
        │                       │                              ▼            │
        │                       │                        +------------+     │
        │                       │                        | refreshing |─────┘
        │                       │                        +-----+------+
        │                       │                              │
        │                       │                              │ invalid_grant
        │                       │                              ▼
        │                       │                        +----------+
        └───────────────────────┴───────────────────────►| no_token |
                                                         +----------+
                                                              │
                                                              │ user clicks "Sign Out"
                                                              ▼
                                                         +----------+
                                                         | revoked  | (transient; immediately → no_token after wipe)
                                                         +----------+
```

States persist only in memory. Source of truth on disk is the keychain
entry's presence and the contained `expires_at`.

### 3.8 Access-token JWT spec

Access tokens are JWTs signed by KATforge with RS256. The API
validates each token **locally** in the same Symfony process that
serves the request, using its in-memory JWKS cache. No per-request
introspection round trip.

KATforge already issues JWTs via Lexik (`lexik/jwt-authentication-bundle`).
The build-out to satisfy this contract adds: a public JWKS endpoint,
the `aud`/`nbf`/`jti` claims, refresh-token rotation with reuse
detection, and the `jti` denylist. Existing gateway-issued tokens that
predate the `aud` claim keep working on non-grimvault routes; the
`aud` gate is opt-in via per-controller attributes (e.g.
`#[RequireAudience('darkerdb:grimvault')]`, `#[RequireScope('grimvault.read')]`).

JWKS endpoint (public, GET, heavily cacheable). Per OIDC discovery
convention, `iss` equals the JWKS host. Both are published on the
DarkerDB host group to hide the KATforge relationship from third-party
inspection:

| Env | JWKS URL | JWT `iss` |
|---|---|---|
| `dev` | `https://api.dev.darkerdb.com/.well-known/jwks.json` | `https://api.dev.darkerdb.com` |
| `qa` | `https://api.qa.darkerdb.com/.well-known/jwks.json` | `https://api.qa.darkerdb.com` |
| `prod` | `https://api.darkerdb.com/.well-known/jwks.json` | `https://api.darkerdb.com` |

**Why JWKS is on the DarkerDB host even though KATforge signs the
tokens.** The signing key lives in the KATforge namespace of the
shared Symfony app (per §11). The JWKS endpoint is published on the
`api.darkerdb.com` host group so an external client running
`curl api.darkerdb.com/.well-known/jwks.json` cannot discover the
KATforge↔DarkerDB relationship from the `iss` claim or the JWKS URL.
In practice this is route configuration: the same Symfony app serves
both host groups, so "mirror" means a route bound to the darkerdb
host group that returns the same `JWKSet`. The desktop client never
fetches JWKS itself (it treats access tokens as opaque); this whole
arrangement is purely to prevent third-party discovery via `curl`.

JWKS is consumed by the Symfony app for self-validation; an external
service that wanted to validate tokens (none in MVP) would use the
darkerdb-host endpoint and would not see KATforge in the URL or
claims.

Required claims:

| Claim | Type | Value / meaning |
|---|---|---|
| `iss` | string | The JWKS host (per table above). Verified against an env-pinned value. |
| `sub` | string | **`player_id`**: KATforge's universal identity unit. Stable, opaque, non-PII. See note below. |
| `user_id` | string (optional) | KATforge `user_id`, populated only when the player has a registered user (vs a guest account). Absent for guest-only players. |
| `aud` | string | `darkerdb:grimvault` for grimvault-issued tokens. The grimvault controllers MUST reject tokens whose `aud` does not contain this value. Other realms (and the legacy gateway flow) use their own `aud` values or no `aud` at all; the per-controller `#[RequireAudience]` attribute gates this. |
| `exp` | int (epoch seconds) | Hard expiry. ≤900s after `iat` for grimvault tokens. |
| `iat` | int (epoch seconds) | Issued-at. |
| `nbf` | int (epoch seconds) | Not-before. Equal to `iat` in practice. |
| `scope` | string | Space-separated, e.g. `"grimvault.read grimvault.write"`. |
| `client_id` | string | Echoes the OAuth `client_id` (e.g. `grimvault-desktop`). |
| `jti` | string (ULID) | Unique token id. Required for the denylist check below. |

**Why `sub` is `player_id` and not `user_id`.** KATforge models
identity as `player` (universal, includes guest accounts for some
games) and `user` (registered account, optional). A guest player who
authorizes GrimVault has a `player_id` but no `user_id`. Putting
`user_id` in `sub` would either exclude guests entirely or force a
later refactor when guest support arrives in a flow that needs it.
`player_id` is the durable choice. Per-product code that genuinely
needs the registered-user record reads the optional `user_id` claim
and handles its absence explicitly. Do not "fix" this back to
`user_id` without breaking guest flows.

Validation steps the API performs on every authenticated request
gated by `#[RequireAudience('darkerdb:grimvault')]`:

1. Parse JWT, verify RS256 signature using the JWKS key whose `kid` matches the token header. Reject if `kid` unknown.
2. Verify `iss` is the env-pinned issuer.
3. Verify `aud` contains `darkerdb:grimvault`.
4. Verify `now` is in `[nbf, exp)`, with a ±60s skew tolerance.
5. Verify the requested scope (per `#[RequireScope]` on the route) is present in the token's `scope` claim.
6. Check `jti` against the revocation denylist (see §3.9). If listed, reject with `invalid_token`.

Denylist storage: `revoked_jtis (jti PK, expires_at)` table in
Postgres, APCu-cached on read so the hot path is in-process. Entries
expire naturally at `expires_at = original token exp`; the table
never grows unbounded.

### 3.9 Refresh-token rotation and revocation

#### Refresh-token rotation

Refresh tokens are persisted server-side with rotation metadata:

```
refresh_tokens (
   id           TEXT PRIMARY KEY,    -- opaque token id (server keeps the secret hashed)
   family_id    TEXT NOT NULL,       -- groups all tokens descended from one auth-code grant
   parent_id    TEXT NULL,           -- the refresh token this one replaced (NULL for the first in a family)
   client_id    TEXT NOT NULL,
   player_id    TEXT NOT NULL,
   scope        TEXT NOT NULL,
   issued_at    TIMESTAMPTZ NOT NULL,
   expires_at   TIMESTAMPTZ NOT NULL,
   consumed_at  TIMESTAMPTZ NULL,    -- set when refresh is used
   replaced_by  TEXT NULL,           -- the new token id issued on refresh
   revoked_at   TIMESTAMPTZ NULL
)
```

On each `grant_type=refresh_token`:

1. Look up token by id. Reject if not found, `revoked_at IS NOT NULL`, or `now >= expires_at`.
2. If `consumed_at IS NOT NULL`: this is a reuse of a token we already burned. Mark `revoked_at = now` on **every** row where `family_id` matches (family-wide revocation per RFC 6749 §10.4 / OAuth 2.1 §6.1). Push every outstanding access-token `jti` in the family onto the denylist. Return `invalid_grant`.
3. Otherwise: issue a new refresh token (same `family_id`, `parent_id` = current id), set `consumed_at = now` and `replaced_by = new_id` on the current row.

#### Revocation events

| Event | Effect |
|---|---|
| User clicks Sign Out (desktop, `POST /oauth/revoke`) | Mark refresh token `revoked_at`. Access token continues to work until its ≤15-min `exp`. Client immediately wipes keychain so the access token is unreachable from this machine. |
| User clicks Disconnect (web, `POST /v1/oauth/grants/:client_id/revoke`) | Revoke **every** refresh token for `(player_id, client_id)`. Push outstanding access-token `jti`s for those tokens onto the denylist with `expires_at = original token exp`. |
| Refresh-token reuse detected | Family-wide revocation per the rotation flow above. |
| Token compromise (admin) | Manual revoke of all `(player_id, client_id)` grants. Same effect as Disconnect. |

#### Lag

| Revocation path | In-flight access-token lag |
|---|---|
| Desktop sign-out (denylist NOT engaged) | bounded by 15-min access-token TTL |
| Web Disconnect / admin revoke / reuse detection (denylist engaged) | bounded by APCu cache window for the denylist (sub-second per node; DevOps's call on cross-node consistency) |

---

## 4. `/v2/grimvault/*` contracts

Three endpoints are retained: `analyze` is the runtime overlay path,
`lookup` is the legacy-shaped compatibility path, and `ping` is the auth
sanity check used by CLI `doctor`.

Public path prefix is `/v2/` on `api.darkerdb.com` (per
`config/routes/darkerdb.yaml`'s `darkerdb_shadow` mount). The same
controllers are also reachable at `/v1/realms/darkerdb/grimvault/*`
on `api.katforge.com` for KATforge-canonical access; the desktop
client only uses the `api.darkerdb.com` host.

### 4.0 `POST /v2/grimvault/analyze`

The desktop sends the complete OCR tooltip using the request in §4.2. The
server resolves localized text against the current item catalog and returns
one atomic analysis containing:

- the canonical item and parsed roll vector;
- similarity- and recency-weighted sold-comparable valuation, quick-list
  guidance, active listings, trend, liquidity, median sale time, days of
  supply, roll-aware price stability, and confidence;
- per-roll quality, a market-relative percentile, and the strongest observed
  value driver measured against that roll's legal minimum;
- vendor, quest, recipe, adventure-point, gear-score, stack-size, and
  market-value-per-inventory-slot context;
- cached best-drop provenance (including zero- and 500-luck rates) for
  stackable items, or merchant acquisition context for non-tradeable items;
- the highest-valued legal one- and two-gem replacements, evaluated at the
  item's maximum enchanted ranges with socket fees included.

The analytical core always returns the complete premium result. Product
entitlements may redact capabilities at an API presentation boundary later;
pricing and gem logic must not contain plan checks.

Live market reads are deliberately bounded: at most the 250 newest sold
comparables from the prior 30 days are loaded, price-sorted once, and reused
for every counterfactual. Sold comparables cache for 30 seconds, active asks
for 15 seconds, and patch-derived catalog/range/gem/source data for five
minutes to one hour. The endpoint's warm-response budget is 300 ms; adding an
unbounded query or a per-comparable database lookup violates this contract.

The client caches a response for at most `valuation.ttl_seconds`, coalesces
pending hover work, and reveals no card until the complete response has been
laid out and captured. Results from a stale OCR/anchor generation are dropped.

`lookup` remains available for backward compatibility, but new desktop code
must use `analyze`.

### 4.1 `POST /v2/grimvault/lookup`

| Field | Value |
|---|---|
| Method | `POST` |
| Path | `/v2/grimvault/lookup` |
| Host | `api.darkerdb.com` (per-env in §10) |
| Auth | `Authorization: Bearer <access_token>`. Scope: `grimvault.read`. `aud` MUST contain `darkerdb:grimvault`. |
| Content-Type (req) | `application/json` |
| Content-Type (res) | `application/json` |
| Idempotent | Yes. Same request body → same response within cache TTL. |
| Cacheable | Yes (client side, in `prices.sqlite`). |
| **Implementation status** | Legacy compatibility endpoint. Runtime uses §4.0. |

#### 4.1.1 MVP stub behavior

The shape below is the **contract**; the MVP implementation behind
that contract is a stub that returns plausible-shaped pricing without
real market data. Real pricing logic is post-MVP. The stub MUST:

- Return a response that conforms exactly to §4.3 (correct field types, plausible values, correct ranges so `low ≤ median ≤ high`).
- Vary deterministically with `(language, normalize(raw_text))` so the client cache behaves correctly.
- Honor the rate limit and error codes specified in §4.4 and §4.6.

The desktop client **must not pattern-match the stub's specific
numbers**. Wire the overlay to consume the actual response schema
(§4.3) so swapping the stub for real pricing later requires zero
client changes. QA's test plan against `/v2/grimvault/lookup` is
shape-based, not value-based.

### 4.2 Request schema

The overlay sends the OCR'd item, not a hash. Parsing and canonicalization
live server-side so the client stays language-agnostic.

```json
{
   "client_id":      "grimvault-desktop",
   "client_version": "1.2.3",
   "captured_at":    "2026-05-23T14:11:09Z",
   "language":       "en",
   "ocr": {
      "raw_text":   "Falchion\nUncommon\nWeapon Damage: 24\nPhysical Power: 5%\n...",
      "confidence": 0.91
   },
   "hints": {
      "rarity_color_hex": "#80d600",
      "capture_backend":  "wgc"
   }
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `client_id` | string | yes | Must equal the OAuth `client_id`. |
| `client_version` | string | yes | SemVer. Used for server-side compat shims. |
| `captured_at` | string (ISO 8601 UTC) | yes | When the OCR frame was grabbed. |
| `language` | string (BCP 47) | yes | Game UI language, e.g. `en`, `ko`, `zh-Hans`. |
| `ocr.raw_text` | string | yes | Newline-separated OCR output. UTF-8. Max 4 KB. |
| `ocr.confidence` | number (0..1) | no | Aggregate OCR confidence if available. |
| `hints.rarity_color_hex` | string | no | Sampled rarity-tier pixel, helps server disambiguate. |
| `hints.capture_backend` | string | no | `wgc`, `dxgi`, or `gdi`. Diagnostic only. Unknown values remain compatible and are recorded as `unknown`. |

### 4.3 Response schema (200)

```json
{
   "item": {
      "canonical_name": "Falchion",
      "rarity":         "uncommon",
      "primary":   [ { "label": "Weapon Damage",  "value": "24"  } ],
      "secondary": [ { "label": "Physical Power", "value": "5%"  } ],
      "details":   [ { "label": "Slot",           "value": "Main Hand" } ]
   },
   "pricing": {
      "currency":    "gold",
      "low":         120,
      "median":      180,
      "high":        260,
      "sample_size": 47,
      "as_of":       "2026-05-23T13:55:02Z",
      "ttl_seconds": 300
   },
   "request_id": "01J9X3Q7Z4M2B8K0V5N6Y2T7AA"
}
```

| Field | Type | Notes |
|---|---|---|
| `item.canonical_name` | string | Server-resolved canonical English name. |
| `item.rarity` | enum | `poor` `common` `uncommon` `rare` `epic` `legendary` `unique` `artifact`. Matches `Palette.rarity()`. |
| `item.primary` / `secondary` / `details` | array of `{label,value}` | Display-ready, pre-localized to request `language`. |
| `pricing.currency` | string | MVP always `gold`. |
| `pricing.low` / `median` / `high` | int64 | Gold pieces. |
| `pricing.sample_size` | int | Number of comps backing this estimate. |
| `pricing.as_of` | ISO 8601 UTC | When the pricing was computed server-side. |
| `pricing.ttl_seconds` | int | Client honors as cache TTL upper bound. |
| `request_id` | string (ULID) | For support and log correlation. Always present, including on errors. |

### 4.4 Error responses

See section 12 for canonical shape. Endpoint-specific codes:

| HTTP | `error.code` | Meaning | Client behavior |
|---|---|---|---|
| 400 | `bad_ocr` | Raw text could not be parsed into any item shape. | Show tooltip "Unrecognized item". Do not retry. |
| 401 | `invalid_token` | Token expired or revoked. | Trigger refresh; on refresh failure, kick to `no_token`. |
| 403 | `insufficient_scope` | Token lacks `grimvault.read`. | Surface as fatal. Tray prompts re-sign-in. |
| 404 | `item_not_found` | Parsed but no canonical match. | Tooltip shows item, omits pricing. Cache negative for 60s. |
| 422 | `unsupported_language` | Language not yet supported. | Tooltip shows item without pricing. Surface in CLI `doctor`. |
| 429 | `rate_limited` | Per-token bucket exhausted. | Honor `Retry-After`. Suppress overlay until window passes. |
| 5xx | `server_error` | Upstream failure. | Serve from `prices.sqlite` if hit; else show item, omit pricing. |

### 4.5 Idempotency and caching

- Server: treat request as a pure function of `(language, normalize(raw_text))`. Same inputs within `ttl_seconds` MUST yield identical `pricing` and `item`.
- Client: hash request body (SHA-256) → cache key in `prices.sqlite.pricing_cache`. On cache hit within TTL, skip network. On 5xx with valid stale entry, serve stale and tag overlay with `(cached)` marker (visual TBD by designer).
- No write side effects. Lookup MUST NOT mutate server state in MVP.

### 4.6 Rate limit expectations

- Default budget: 60 lookups / minute / token, burst 20. (Tunable server-side; client must read `X-RateLimit-Remaining` / `Retry-After` headers, not hardcode.)
- Client deduplicates: identical lookup fired within 2s of an in-flight request piggybacks on the in-flight result.

### 4.7 `POST /v2/grimvault/ping`

Trivial auth-validation endpoint. Used by CLI `doctor` and any client
that wants to verify "is my token good?" without burning quota or
producing history. Not for runtime overlay use.

| Field | Value |
|---|---|
| Method | `POST` |
| Path | `/v2/grimvault/ping` |
| Host | `api.darkerdb.com` (per-env in §10) |
| Auth | `Authorization: Bearer <access_token>`. Scope: `grimvault.read`. `aud` MUST contain `darkerdb:grimvault`. |
| Content-Type (req) | `application/json` |
| Content-Type (res) | `application/json` |
| Side effects | None. Does not consume `/lookup` quota, does not write history. |
| Rate limit | Separate bucket. 60/min/token, burst 10. |

Request body:

```json
{}
```

Response (200):

```json
{
   "ok":          true,
   "player_id":   "01J9X3Q7Z4M2B8K0V5N6Y2T7AA",
   "user_id":     "01J9X3Q7Z4M2B8K0V5N6Y2T7CC",
   "env":         "prod",
   "server_time": "2026-05-23T14:11:09Z",
   "request_id":  "01J9X3Q7Z4M2B8K0V5N6Y2T7BB"
}
```

| Field | Type | Notes |
|---|---|---|
| `ok` | bool | Always `true` on 200. Future-proofing for soft-degraded modes. |
| `player_id` | string | Mirrors the JWT `sub` claim. Always present. |
| `user_id` | string (optional) | Mirrors the JWT `user_id` claim. Absent for guest-only players. |
| `env` | enum | `dev` / `qa` / `prod`. Lets `doctor` confirm the binary is hitting the env it thinks it is. |
| `server_time` | ISO 8601 UTC | Useful for clock-skew diagnostics. |
| `request_id` | string (ULID) | Standard. |

Error responses follow the canonical envelope (§12). The only
non-2xx ping handles in practice are `401 invalid_token` and `5xx
server_error`.

### 4.8 Scope registry

KATforge maintains a single registry of all OAuth scopes with
human-readable descriptions. The registry is the source of truth for
what the consent screen displays and what `#[RequireScope]` accepts.

For MVP, the registry contains two grimvault scopes:

| Scope id | Description (shown on consent) |
|---|---|
| `grimvault.read` | "Look up item prices on your behalf." |
| `grimvault.write` | "Submit price observations on your behalf." |

`grimvault.admin` is reserved (see §10) and not in the consent-visible
registry yet.

The registry is consumed by `GET /v1/oauth/authorize/validate`: for
each scope the client requests, the response includes `{id, description}`
so the SPA can render the list verbatim. The consent screen does not
hardcode descriptions; everything flows from the registry.

Storage of the registry is an implementation detail for the API
engineer (const PHP array, YAML, or a small DB table all work). What
matters for the contract:

- One source of truth, read-only at request time.
- Description strings are short, second-person, end-with-period sentences ("Look up item prices on your behalf.").
- Adding a scope is a code change (or a migration), not a runtime API. No "user-defined scopes" in MVP.

---

## 5. Auth and API surface inventory

Complete MVP URL list, grouped by host. Per-env hostnames in §10.

### 5.1 `darkerdb.com` (Vue/Vite SPA, prod)

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/oauth/authorize` | session (cookie) | SPA consent screen. Reads query, calls API for validation, renders Allow/Cancel. |
| GET | `/dashboard/grimvault` | session | "GrimVault is connected" page. Calls API for grant state. |
| GET | `/account/connected-apps` | session | List of authorized apps with revoke buttons. Calls API for grant list + revoke. |

The SPA contains no business logic for OAuth or grants; all state
lives in the API. The pages above are URL contracts only.

### 5.2 `api.darkerdb.com` (Symfony API, prod)

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/v1/oauth/authorize/validate` | none | Validate `client_id` + `redirect_uri`, return client name + scope descriptions for the consent screen. |
| POST | `/v1/oauth/authorize` | session (cookie) | Issue authorization code on user consent. Body per §3.2 step 6. |
| POST | `/oauth/token` | none (PKCE) | Code exchange and refresh. |
| POST | `/oauth/revoke` | none (token in body) | RFC 7009 token revocation. Called by desktop on logout. |
| GET | `/v1/oauth/grants` | session | List authorized grants for current user (for `/account/connected-apps`). |
| GET | `/v1/oauth/grants/:client_id` | session | Fetch one grant. 404 = not connected. Used by `/dashboard/grimvault`. |
| POST | `/v1/oauth/grants/:client_id/revoke` | session + CSRF | Revoke a grant. Called by SPA Disconnect buttons. |
| GET | `/v1/gateway/login/url` | none | Provider-agnostic sign-in URL. Returns `{ url }`. Replaces the discord-only path. |
| POST | `/v2/grimvault/analyze` | Bearer, `grimvault.read`, `aud=darkerdb:grimvault` | Complete roll-aware valuation and gem optimization (§4.0). |
| POST | `/v2/grimvault/lookup` | Bearer, `grimvault.read`, `aud=darkerdb:grimvault` | Legacy-shaped compatibility lookup (§4.1). |
| POST | `/v2/grimvault/ping` | Bearer, `grimvault.read`, `aud=darkerdb:grimvault` | Auth-validation probe. Used by CLI `doctor`. |
| GET | `/.well-known/jwks.json` | none | JWKS endpoint. Returns the public-key set used to verify access-token signatures. See §3.8 for why it lives on this host. |

`/oauth/revoke` and `/v1/oauth/grants/:client_id/revoke` are both
revocation surfaces but they are not duplicates:

| Endpoint | Identity input | Revokes | Caller |
|---|---|---|---|
| `POST /oauth/revoke` | the token itself (PKCE-style, no session) | one specific token (and rotation family) | desktop client |
| `POST /v1/oauth/grants/:client_id/revoke` | user's web session | every grant for `(player_id, client_id)` | SPA Disconnect button |

The desktop has a refresh token in hand and wants to surrender it.
The web user does not have a token; they identify via session cookie
and revoke by `client_id`. Both end up adding outstanding access-token
`jti`s to the denylist per §3.9.

### 5.3 Response envelope

Endpoints under `api.darkerdb.com` use Symfony's standard envelope
shape (`{ body: ... }` on success, canonical error envelope per §12
on failure). The grimvault `/v2/*` endpoints return `body` containing
the schemas in §4.3, §4.7. The `/v1/oauth/*` endpoints return `body`
containing the shapes referenced in their per-row descriptions above.

### 5.4 Reserved for naming consistency (out of MVP)

| Method | Path | Status |
|---|---|---|
| GET | `/v2/grimvault/history` | reserved |
| GET | `/v2/grimvault/stats` | reserved |

`GET/PATCH/DELETE /v2/grimvault/settings` has shipped ahead of this doc's
original MVP scope (§6 called it "later") — cloud-synced settings, nested
wire contract (`overlay`/`tooltip`/`pricing`/`behavior`/`hotkeys`), flat
colon-namespaced storage internally. `tooltip.analysis.*` covers the
GrimVault Analysis augment's per-section visibility toggles (added
alongside a redesign of that card's layout). The same contract is served on
two auth surfaces: the desktop app uses its Bearer token at
`/v2/grimvault/settings`; the darkerdb.com dashboard uses the signed-in
user's own session at `GET/PATCH/DELETE /v1/grimvault/settings` (identical
body/response, session auth instead of `aud=darkerdb:grimvault`). See the
settings reference pages under
`docs.katforge.com/reference/realms/darkerdb/grimvault-settings*` for the
full contract — this doc no longer tracks it as the source of truth.

---

## 6. Data residency

| Datum | Location | Why |
|---|---|---|
| `access_token`, `refresh_token`, `expires_at` | Windows Credential Manager (DPAPI), single JSON blob, target `GrimVault:tokens` | OS-level protection, per-user, survives reinstall, integrates with Windows account lock. |
| `pkce_verifier`, `state` (in flight) | Process memory only | Discarded after `/token` exchange. Never written to disk. |
| All user preferences (overlay, tooltip section toggles, pricing, behavior, hotkeys) | Cloud only — `/v2/grimvault/settings` | **Superseded MVP decision**: no `settings.toml`, no OS-bound preferences at all. Sign-in is now mandatory to use the app (§13), so there's no unauthenticated state that would need a local fallback — one settings surface, synced across machines. |
| Cached price lookups | `%APPDATA%\GrimVault\prices.sqlite` | Local cache + offline serving. Non-sensitive. |
| Logs, crash dumps | `%APPDATA%\GrimVault\logs\`, `%APPDATA%\GrimVault\crashes\` | Local diagnostics. Already in place (see `docs/release.md`). |
| Account identity, billing, API-key issuance | KATforge | Out of client scope. |
| Price history aggregates | Server-side (`api.darkerdb.com` Postgres, under the realm's tables) | Server-side only. Client never holds raw market data. Stub-implemented for MVP (§4.1.1). |

---

## 7. Local state schema

### 7.1 Paths

| Path | Purpose |
|---|---|
| `%LOCALAPPDATA%\Programs\GrimVault\` | Install root (binaries). Unchanged from `docs/release.md`. |
| ~~`%APPDATA%\GrimVault\settings.toml`~~ | Removed — see §6. All preferences are cloud-synced via `/v2/grimvault/settings`; there is no local settings file. |
| `%LOCALAPPDATA%\GrimVault\grimvault.db` | Cached `/v2/grimvault/analyze` and legacy lookup responses. |
| `%APPDATA%\GrimVault\logs\grimvault.log` | spdlog rotating sink. |
| `%APPDATA%\GrimVault\crashes\*.dmp` | Crash minidumps. |
| Windows Credential Manager: `GrimVault:tokens` | OAuth tokens. |

### 7.2 `settings.toml` — superseded, kept for historical reference

> **Superseded per §13.** Sign-in is now mandatory (no `no_token` tray
> state), so there is no local-only file — the schema below moved to
> the cloud-synced `/v2/grimvault/settings` contract (see
> `docs.katforge.com/reference/realms/darkerdb/grimvault-settings*`).
> The first-run wizard's confirmed values (hotkey, capture region, OCR
> tuning) now need a home in that cloud schema instead of writing this
> file directly — that migration is native-client implementation work
> for a future pass, not yet done. Left below as the reference for what
> that migration needs to cover.

TOML, ASCII, written atomically (write-temp + rename). Keys are flat,
two-segment namespaces. Defaults shipped if file is missing.

```toml
[hotkey]
lookup = "Alt+D"              # accelerator string, Qt format

[capture]
backend         = "wgc"       # "wgc" | "gdi"
region_mode     = "auto"      # "auto" | "manual"
region_manual   = { x = 0, y = 0, w = 0, h = 0 }   # used when region_mode = "manual"
game_detect     = "title"     # "title" | "process" | "off"
game_title_re   = "^Dark and Darker$"
game_process    = "DungeonCrawler.exe"

[ocr]
language     = "auto"         # "auto" | BCP-47 tag
min_conf     = 0.55           # discard frames below this
upscale      = 2              # 1..4

[app]
autostart        = true       # register Run key on install
telemetry        = "diag-only"   # "diag-only" | "off" (no analytics in MVP)
```

| Key | Type | Default | Notes |
|---|---|---|---|
| `hotkey.lookup` | string | `Alt+D` | Single global hotkey for MVP. |
| `capture.backend` | enum | `wgc` | Falls back to `gdi` if WGC unavailable. |
| `capture.region_mode` | enum | `auto` | `auto` uses game window bounds. |
| `capture.region_manual` | table | zeroed | Required when `region_mode = "manual"`. |
| `capture.game_detect` | enum | `title` | How to identify the game window. |
| `capture.game_title_re` | string | regex | Used when `game_detect = "title"`. |
| `capture.game_process` | string | `DungeonCrawler.exe` | Used when `game_detect = "process"`. |
| `ocr.language` | string | `auto` | `auto` = match Windows display language. |
| `ocr.min_conf` | float | `0.55` | Below this, suppress overlay. |
| `ocr.upscale` | int | `2` | Pre-OCR resize factor. |
| `app.autostart` | bool | `true` | `HKCU\...\Run` entry. |
| `app.telemetry` | enum | `diag-only` | Only on-request diagnostics bundle (already implemented). |

**First-run trigger (as originally designed, now superseded).** The
first-run setup wizard (hotkey + capture region) fired on launch iff
`%APPDATA%\GrimVault\settings.toml` did not exist, writing confirmed
values (or shipped defaults on Skip) so subsequent launches went
straight to tray — independent of sign-in, which left the tray in
`no_token` until the user clicked Sign In.

Per §13's superseding ruling, sign-in is no longer optional: the app
requires a valid, authenticated session before it does anything beyond
showing a sign-in prompt, so there is no unauthenticated `no_token`
tray state and no local settings file for the wizard to gate on. The
wizard's confirmed values now need to persist via `/v2/grimvault/settings`
(authenticated) instead — the exact first-run UX (e.g. whether the
wizard runs before or after the sign-in gate) is unspecified pending
that native-client work.

### 7.3 `prices.sqlite`

Two tables. Migration owned by `gv_db`, lives under `db/migrations/`.

```sql
-- 0002_prices.sql

CREATE TABLE pricing_cache (
   cache_key      TEXT    PRIMARY KEY,    -- sha256 of normalized request body
   request_json   TEXT    NOT NULL,
   response_json  TEXT    NOT NULL,
   fetched_at     INTEGER NOT NULL,       -- unix seconds
   ttl_seconds    INTEGER NOT NULL,
   stale_after    INTEGER NOT NULL        -- fetched_at + ttl_seconds, indexed
);

CREATE INDEX idx_pricing_cache_stale ON pricing_cache (stale_after);

CREATE TABLE lookup_events (
   event_id      INTEGER PRIMARY KEY,
   occurred_at   INTEGER NOT NULL,
   cache_key     TEXT    NOT NULL,
   outcome       TEXT    NOT NULL,        -- "hit" | "miss" | "stale" | "error"
   http_status   INTEGER,
   error_code    TEXT
);

CREATE INDEX idx_lookup_events_time ON lookup_events (occurred_at DESC);
```

`lookup_events` is local-only telemetry for the CLI `doctor` and
`logs` commands. Capped at 10 000 rows; oldest pruned on insert.
Note the existing `0001_init.sql` already has a `pricing_cache`
table. Migration 0002 supersedes it with a DROP + CREATE; the legacy
schema has never shipped to users so no data preservation is needed.

---

## 8. CLI surface

`grimvault` is the same binary as `grimvault.exe`, dispatched by
`argv[0]` or a `--cli` flag. Installed on PATH. Exit codes follow
`sysexits.h` style: `0` ok, `1` generic error, `2` usage, `64`
auth-required, `69` service unavailable, `75` transient (retry).

Shared invariants:
- All commands read and write the same `%APPDATA%\GrimVault\` state and the same `GrimVault:tokens` keychain entry as the overlay.
- All commands are safe to run while the overlay is running. The overlay re-reads `settings.toml` on `WM_FILE_CHANGED`-style notification, and re-reads keychain on next outbound 401.
- All commands print human-readable text by default; `--json` flips to a stable JSON shape (one envelope per command).

| Command | Flags | Behavior | Exit |
|---|---|---|---|
| `login` | `--no-browser` | Runs the OAuth flow (section 3). With `--no-browser`, prints the authorize URL instead of launching. | 0 / 64 / 75 |
| `logout` | `--local-only` | Calls `/oauth/revoke` then wipes keychain. `--local-only` skips the server call. | 0 / 75 |
| `status` | `--json` | Prints: signed-in handle, token expiry, overlay running (Y/N), last lookup outcome. | 0 |
| `doctor` | `--json` | Runs diagnostics: WGC availability, OCR model presence, token validity (calls `/v2/grimvault/ping`), env / clock-skew check (from ping response), network reachability. Prints pass/fail per check. | 0 / 1 |
| `logs` | `--tail N` `--follow` | Streams `grimvault.log`. Defaults to last 200 lines. | 0 |
| `settings get` | `<key>` | Prints value from `settings.toml`. | 0 / 1 |
| `settings set` | `<key> <value>` | Validates against schema (section 7.2), writes atomically. | 0 / 2 |
| `settings list` | `--json` | Dumps all keys and effective values. | 0 |
| `region pick` | none | Launches the region-picker overlay (modal, click and drag), writes `capture.region_manual` and flips `capture.region_mode = "manual"`. | 0 / 1 |
| `hotkey bind` | `<action> <accelerator>` | Validates accelerator parseable by Qt, writes `hotkey.<action>`, rebinds in running overlay. | 0 / 2 |

No subcommands beyond this list in MVP. `login` is the only command
that may spawn a browser. `doctor` is the only command that contacts
the network without explicit user gesture.

The CLI never triggers the first-run setup wizard. The wizard is a
GUI-only surface owned by `grimvault.exe` and fires per the rule in
§7.2 (missing `settings.toml`). A user who runs `grimvault settings
set ...` before ever launching the GUI causes `settings.toml` to be
created, which suppresses the wizard on first GUI launch. That's
acceptable: a user who reaches for the CLI first has opted out of
the hand-holding.

---

## 9. Web surfaces

Three SPA pages on `darkerdb.com`. All session-authenticated via the
existing web login. The SPA holds no business logic; it calls
`api.darkerdb.com` for state and mutations (see §5).

### 9.1 `/oauth/authorize` consent screen

Single Allow button. **No per-scope checkboxes.**

```
   Allow GrimVault to access your DarkerDB account?

   GrimVault will be able to:
     - Look up item prices on your behalf.
     - Submit price observations on your behalf.

   [ Allow ]   [ Cancel ]
```

The bulleted descriptions are not hardcoded in the SPA; they come
from the scope registry (§4.8) via `GET /v1/oauth/authorize/validate`.
Adding or renaming a scope is a backend change; the SPA renders
whatever the API returns.

Visual reference: the existing katforge.com OAuth authorization page.
Same layout language, same interaction model, **branded as DarkerDB**
(DarkerDB header, DarkerDB color tokens, GrimVault logo for the
requesting-app slot). The website dev pulls katforge.com's authorize
page up directly as the spec; this doc does not duplicate the layout.

Flow:

| User action | SPA call | Resulting navigation |
|---|---|---|
| Page load | `GET api.darkerdb.com/v1/oauth/authorize/validate?...` | Renders consent panel from response. On 4xx, renders error and disables Allow. |
| Click Allow | `POST api.darkerdb.com/v1/oauth/authorize` with body per §3.2 step 6 | Browser navigates to `redirect_uri?code=<code>&state=<state>`. |
| Click Cancel | none | Browser navigates to `redirect_uri?error=access_denied&state=<state>`. |

### 9.2 `/dashboard/grimvault`

Minimal "is it on" page. No charts, no history.

States and content:

| State | Source | Content shown |
|---|---|---|
| **Connected** | `GET api.darkerdb.com/v1/oauth/grants/grimvault-desktop` returns 200 | "GrimVault is connected" with `created_at` timestamp. Client metadata if available: `last_used_at`, `client_version`. Actions: **Disconnect** (calls `POST /v1/oauth/grants/grimvault-desktop/revoke`); **Manage connected apps** link to `/account/connected-apps`. No Download CTA. |
| **Not connected** | same call returns 404 | "GrimVault is not connected. Install the desktop app to get started." Actions: **Download GrimVault** (links to releases). No Disconnect, no Manage link. |

The `client_version` and `last_used_at` fields are best-effort: the
page renders cleanly without them. The API engineer surfaces them
when available (token usage timestamps are easy; `client_version`
requires the desktop to send it as a header or it's omitted).

### 9.3 `/account/connected-apps`

Generic page. Lists every OAuth grant for the current user, GrimVault
appears as one row when present.

| Element | Source / call |
|---|---|
| Grant list | `GET api.darkerdb.com/v1/oauth/grants` |
| Per row: app name, scopes (rendered from registry descriptions), `created_at`, `last_used_at` | from grant list |
| Revoke button per row | `POST api.darkerdb.com/v1/oauth/grants/:client_id/revoke` |

Revocation cascades per §3.9: refresh token invalidated, outstanding
access-token `jti`s pushed to denylist.

---

## 10. Environment model

Three envs. (Symfony naming. `test` is intentionally omitted: this is a
single-developer-binary project with no CI integration suite that
needs a long-lived shared env. Test runs target `dev` or use local
mocks; `qa` is the pre-prod gate.)

### 10.1 Hostnames

| Env | SPA host (darkerdb) | API host (darkerdb) | API host (katforge) | JWT `iss` and JWKS host |
|---|---|---|---|---|
| `dev` | `dev.darkerdb.com` | `api.dev.darkerdb.com` | `api.dev.katforge.com` | `https://api.dev.darkerdb.com` |
| `qa` | `qa.darkerdb.com` | `api.qa.darkerdb.com` | `api.qa.katforge.com` | `https://api.qa.darkerdb.com` |
| `prod` | `darkerdb.com` | `api.darkerdb.com` | `api.katforge.com` | `https://api.darkerdb.com` |

The two API hosts in each env point at the **same Symfony codebase**
(one deploy). The host determines which routes mount (see §11). The
desktop client only ever talks to the `api.{env.}darkerdb.com` host.
JWKS and `iss` are pinned to the darkerdb host group so external
inspection cannot discover the KATforge relationship; see §3.8.

DNS: the `darkerdb.com` zone is owned by the KATforge AWS account
and managed via Terraform under `infra/terraform/dns/`. DevOps owns
the records; per-env CNAMEs above are the contract.

### 10.2 OAuth client ids

| Env | `client_id` |
|---|---|
| `dev` | `grimvault-desktop-dev` |
| `qa` | `grimvault-desktop-qa` |
| `prod` | `grimvault-desktop` |

Per-env `client_id` naming uses the `<base>-<env>` suffix pattern with
prod as the unsuffixed canonical id. This keeps the prod id short and
makes non-prod ids self-describing in logs. Reserved for future client
families: `grimvault-mobile`, `grimvault-mobile-dev`, etc.

### 10.3 Client config and shared invariants

Per-env config in the desktop binary lives behind a single compile-time
flag, `GRIMVAULT_ENV` (default `prod`). It selects the SPA host, API
host, and `client_id` from a baked-in table. No runtime env var
override in the shipped build; dev builds get a runtime `--env` flag
for engineers.

Loopback redirect URI is the same shape in every env: `http://127.0.0.1:<port>/callback`.
DevOps registers each `client_id` with the appropriate KATforge realm
allowing wildcard loopback ports per RFC 8252 §7.3.

Scopes are identical across envs: `grimvault.read`, `grimvault.write`,
`grimvault.admin` (reserved). No per-env scope drift.

`aud` claim is identical across envs: `darkerdb:grimvault`. The `iss`
claim is what differs per env (see §10.1). The API pins the expected
`iss` from its own deploy env, so a `dev`-issued token cannot be
replayed against `prod`.

### 10.4 Phasing

- **Build-and-test phase (current):** Dev only. `dev.darkerdb.com` +
  `*.dev.darkerdb.com` wildcard (already exists, points to `127.0.0.1`)
  covers all desktop, SPA, and API hosts. No DNS or infra provisioning
  needed from the GrimVault workstream during build-out. Desktop CI
  exercises the `release-windows-dev` arm only.
- **QA standup (deferred):** Add a cross-account `provider "aws"` with
  `alias = "darkerdb"` to the Terraform tree. Use `data "aws_route53_zone"`
  to reference the existing zone (zone id `Z0002055WECIPLZ36PXL`) in the
  `darkerdb` profile (account `486581310941`); **do not `import` the zone**
  — that would claim ownership of records this workstream did not author.
  Add two records under the data source: `qa.darkerdb.com` and
  `api.qa.darkerdb.com`. Stand up the QA Symfony instance, QA Postgres,
  and QA SPA host. Activate the `windows-qa` arm in `release.yml`
  (already structured to take it).
- **Prod (out of scope):** Separate cutover already in flight. No
  Terraform on prod records from this workstream. The `windows-prod`
  arm in `release.yml` activates whenever Anders calls it.

---

## 11. KATforge ↔ DarkerDB boundary

**Reality.** KATforge and DarkerDB are **not separately deployed**.
They share one Symfony codebase at `api.katforge.com`. The boundary
is namespace and route file, not network:

| Boundary mechanism | What it controls |
|---|---|
| `config/routes/darkerdb.yaml` | Mounts realm controllers at `/v2/*` on the `api.{env.}darkerdb.com` host and at `/v1/realms/darkerdb/*` on the `api.{env.}katforge.com` host. |
| `KAT\Realm\DarkerDB\Controller\*` namespace | Houses every controller that is DarkerDB-specific. KATforge-canonical controllers live in `KAT\Controller\*`. |
| Per-controller attributes (`#[RequireAudience]`, `#[RequireScope]`) | Enforce per-route token claims so the same Symfony app can serve multi-tenant traffic without leakage. |

This contract preserves the right to split later. If DarkerDB's
realm controllers grow heavy enough to warrant their own deploy, the
public surface (`api.darkerdb.com` hosts, `/v2/*` paths) stays
identical and only the deploy topology changes. The desktop client
and the SPA do not need to know about the split. For MVP, do not
build any cross-process abstraction layer; treat KATforge and
DarkerDB-realm controllers as siblings in one app.

### 11.1 Concern ownership

| Concern | Namespace |
|---|---|
| Consent screen SPA (`darkerdb.com/oauth/authorize`) | DarkerDB website (Vue/Vite, separate codebase from the API) |
| OAuth endpoints (`/v1/oauth/*`, `/oauth/token`, `/oauth/revoke`) | `KAT\Controller\OAuthServer` and siblings (canonical KATforge) |
| Token issuance, signing (RS256, Lexik), rotation, denylist push | `KAT\Service\OAuthServerService` and the Token / Keychain plumbing |
| JWKS endpoint, JWT validation, `#[RequireAudience]` / `#[RequireScope]` middleware | KATforge core, used by all realms |
| Refresh-token persistence + rotation enforcement | KATforge core (`refresh_tokens` table, OAuthServerService) |
| Grant queries and revocation (`/v1/oauth/grants*`) | KATforge core |
| Provider-agnostic sign-in URL (`/v1/gateway/login/url`) | `KAT\Controller\Gateway` |
| Scope registry (§4.8) | KATforge core data, contributed-to by realms |
| Per-product endpoints (`/v2/grimvault/*`) | `KAT\Realm\DarkerDB\Controller\GrimVault` (or similar) |
| Per-product business logic (price lookup stub, future real pricing) | `KAT\Realm\DarkerDB\Service\*` |
| Per-product rate-limit policy | per-controller `#[Throttle]` attributes, keyed by token `sub` |

### 11.2 Internal call surface

There are **no inter-service network calls** in MVP. JWT validation,
denylist lookup, JWKS access, refresh-token bookkeeping all happen
in-process via Symfony service-container calls. Earlier revisions
specced a `/internal/*` surface; that surface does not exist and
should not be built. If the topology splits later, the split is the
moment to introduce one; until then, prefer service injection.

### 11.3 User-facing vs internal boundary

| Surface | Host | Caller | Reachable from internet |
|---|---|---|---|
| SPA pages | `darkerdb.com` | browser | yes |
| OAuth + grants endpoints | `api.darkerdb.com` | desktop, browser (XHR from SPA) | yes |
| `/v2/grimvault/*` | `api.darkerdb.com` | desktop | yes |
| JWKS | `api.darkerdb.com/.well-known/jwks.json` | any caller wanting to validate tokens (none in MVP outside the app itself); see §3.8 for why it lives here, not on the katforge host | yes |
| Denylist | in-process Postgres + APCu | the same Symfony app | no (not a network surface) |

---

## 12. Error model

Canonical envelope. All `/v1/oauth/*` and `/v2/grimvault/*` endpoints
return it on non-2xx. `/oauth/token` and `/oauth/revoke` follow the
RFC 6749 error shape (different envelope by spec) and are documented
inline in section 3.

```json
{
   "error": {
      "code":    "rate_limited",
      "message": "Per-token rate limit exceeded.",
      "details": { "retry_after_seconds": 12 }
   },
   "request_id": "01J9X3Q7Z4M2B8K0V5N6Y2T7AA"
}
```

| Field | Type | Notes |
|---|---|---|
| `error.code` | string (snake_case) | Stable enum, machine-readable. |
| `error.message` | string | Human-readable, English. Not localized in MVP. |
| `error.details` | object | Code-specific. Optional. |
| `request_id` | string (ULID) | Always present. Echoes `X-Request-Id` if client sent one; otherwise server-generated. |

### 12.1 Distinct codes the desktop must handle

| `error.code` | HTTP | Desktop reaction |
|---|---|---|
| `invalid_token` | 401 | Trigger refresh; on refresh fail, kick to `no_token`. |
| `insufficient_scope` | 403 | Fatal. Re-sign-in prompt. |
| `rate_limited` | 429 | Honor `Retry-After`. Suppress overlay until window. |
| `bad_ocr` | 400 | Show "Unrecognized item". Do not retry. |
| `item_not_found` | 404 | Tooltip without pricing. Negative-cache 60s. |
| `unsupported_language` | 422 | Tooltip without pricing. Surface in `doctor`. |
| `server_error` | 5xx | Serve stale cache if present; else item-only tooltip. |
| `network_unreachable` | n/a (client-synthesized) | Serve stale cache if present; else item-only tooltip. |

### 12.2 Transient vs fatal

| Class | Definition | Overlay behavior |
|---|---|---|
| Transient | 5xx, network failure, 429, refresh-token transient. Retry will plausibly succeed. | Serve from cache if hit; otherwise show item-only tooltip with discreet "offline" marker. Do not bother the user. |
| Fatal | `insufficient_scope`, `invalid_grant` on refresh, user revoked, persistent 4xx. Retry will not help without user action. | Hide overlay for this lookup. Tray notification once per session prompting re-sign-in. |

---

## 13. Resolved decisions log

Every question raised during architecture review has been ruled on.
The doc body above is authoritative; this log records the rulings for
traceability.

### 13.1 Round 1 rulings

| # | Question | Ruling | Where applied |
|---|---|---|---|
| 1 | Opaque tokens or JWTs? | **JWT (RS256)**, validated locally in the API process. ≤15-min access-token TTL plus a `jti` denylist for prematurely revoked tokens. No per-request introspection. | §3.2, §3.8, §3.9, §11.1, §11.2 |
| 2 | Refresh-token rotation supported? | **Required by contract.** Each refresh returns a new refresh token; reuse of a consumed refresh token MUST fail and triggers family-wide revocation. API engineer to verify KATforge / Lexik integration supports it; surface as hard blocker if absent. | §3.3, §3.9 |
| 3 | Consent screen: per-scope or single button? | **Single Allow button.** Branded as DarkerDB but styled after katforge.com's existing OAuth authorize page. | §9.1 |
| 4 | Introspection cache TTL? | **Moot.** Validation is local under JWT (ruling 1). Revocation lag bounded by 15-min access-token TTL, or sub-second when the denylist path is engaged. | §3.9 |
| 5 | Loopback close-tab page: branded or plain? | **Branded, pure-CSS, standalone.** No external image refs. Asset ships at `~/.katforge/realms/darkerdb.com/grimvault/oauth-close.html`; embedded as a Qt resource. | §3.2 step 7 |
| 6 | First-run wizard always vs on-missing-settings? | **On missing `settings.toml`.** File presence IS the flag; no `first_run_done` key. Sign-in is independent of the wizard. | §7.2, §8 |
| 7 | Legacy `X-API-Key` / `Credentials`: keep dual-auth or delete? | **Delete outright.** No transition period. | Doc no longer mentions either. |
| 8 | Legacy credentials-file migration? | **None.** Previous shape never deployed to users. | §7.3 |
| 9 | Env hostnames and count? | **Three envs.** Naming finalized in round 2 ruling 4. | §10 |
| 10 | `doctor` quota cost vs dedicated ping endpoint? | **Dedicated ping endpoint.** Path finalized in round 2 ruling 4 (`/v2/grimvault/ping`). | §4.7, §5, §8 |

### 13.2 Round 2 rulings

| # | Question | Ruling | Where applied |
|---|---|---|---|
| 11 | DNS ownership for `darkerdb.com`? | **KATforge AWS account.** DevOps manages via Terraform under `infra/terraform/dns/`. | §10.1 |
| 12 | JWT issuer build-out specifics? | **Build on existing Lexik foundation.** Adds: public JWKS at `api.{env.}darkerdb.com/.well-known/jwks.json` (see ruling 19), `iss` matching JWKS host per OIDC convention, `aud` / `nbf` / `jti` claims, per-controller `#[RequireAudience]` and `#[RequireScope]` attributes, refresh-token rotation table with family-wide reuse detection, Postgres `revoked_jtis` denylist with APCu cache. | §3.8, §3.9 |
| 13 | JWT `sub` claim: `user_id` or `player_id`? | **`player_id`** (universal identity; works for guest players). Optional `user_id` claim when the player has a registered user. Per-product code reads `user_id` and handles its absence explicitly. | §3.8, §4.7 |
| 14 | Host topology for SPA vs API? | **Split.** `darkerdb.com` = Vue/Vite SPA (consent, dashboard, connected-apps). `api.darkerdb.com` = Symfony API (OAuth endpoints, grants, grimvault). Per-env: `api.{dev,qa}.darkerdb.com`. Grimvault API paths use `/v2/*` prefix on the darkerdb host. | §2, §2.1, §3, §4, §5, §10.1 |
| 15 | Runtime item-analysis endpoint? | **`/v2/grimvault/analyze`.** It returns live roll-aware pricing and premium gem plans atomically. `/lookup` remains compatibility-only. | §4.0 |
| 16 | Dashboard connected-state content? | **Disconnect + Manage link only.** No Download CTA. Download stays in the not-connected state. | §9.2 |
| 17 | Scope description registry? | **First-class architectural element.** KATforge owns one registry mapping `scope_id → description`. Exposed via `/v1/oauth/authorize/validate` response. Consent screen renders read-only from registry. MVP scopes: `grimvault.read` ("Look up item prices on your behalf."), `grimvault.write` ("Submit price observations on your behalf."). | §4.8 |
| 18 | Additional API endpoints needed by SPA? | **Four added.** `GET /v1/oauth/grants`, `GET /v1/oauth/grants/:client_id`, `POST /v1/oauth/grants/:client_id/revoke`, `GET /v1/gateway/login/url`. Grant-revoke is the user-facing surface; `POST /oauth/revoke` remains as the RFC 7009 token-revoke surface used by the desktop. Both routes feed the denylist. | §5.2, §3.4 |
| 19 | JWKS host: katforge or darkerdb? | **Mirror on `api.{env.}darkerdb.com`.** `iss` pinned to the darkerdb host. Hides the KATforge relationship from third-party `curl` inspection of the JWKS URL or the `iss` claim. The signing key still lives in the KATforge namespace of the shared Symfony app; "mirror" is a route bound to the darkerdb host group returning the same `JWKSet`. | §3.8, §5.2, §10.1, §11.3 |

### 13.3 Round 3 rulings

| # | Question | Ruling | Where applied |
|---|---|---|---|
| 20 | Is sign-in required to use GrimVault at all, or only to sync/analyze? | **Sign-in is now mandatory.** Supersedes ruling 6's "sign-in is independent of the wizard" and the `no_token` tray state described in §7.2/§8: without a valid session the app shows a sign-in prompt rather than a functioning (but unauthenticated) tray/overlay shell. Rationale: usage attribution per user, no meaningful "default settings" identity to fall back to, and — since an account is now required regardless — no reason to keep any preference local/unsynced. | §6, §7.2, §8 (native-client behavior change; not yet implemented) |
| 21 | Local settings file (`settings.toml`) or cloud-only? | **Cloud-only.** Ruling 20 removes the rationale for a local fallback. All preferences (overlay, tooltip section toggles including the new GrimVault Analysis augment toggles, pricing, behavior, hotkeys) live in `/v2/grimvault/settings`; `settings.toml` is retired. | §6, §7.1, §7.2 |
| 22 | Settings wire contract shape? | **Nested groups on the wire** (`overlay`/`tooltip`/`pricing`/`behavior`/`hotkeys`), **flat colon-namespaced keys in storage** (`overlay:opacity`, `tooltip:analysis:roll_quality`, ...) — matches this codebase's existing `namespace:key` convention for storage while keeping the already-built dashboard UI's nested shape as the contract. | §5.4, settings reference docs |
