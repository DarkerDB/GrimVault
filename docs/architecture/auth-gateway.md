# Cross-realm auth gateway architecture

Status: contract. Authoritative for KAT-2 ("Launch auth.katforge.com
cross-realm auth gateway") and every child ticket. Decisions captured
here were ratified in the KAT-2 design conversation; do not re-litigate.

Companion contract: `docs/architecture/grimvault-mvp.md` defines the
desktop OAuth surface. That contract is unchanged. Journey 4 in §6 of
this doc points back to it.

---

## 1. Glossary

| Term | Meaning |
|---|---|
| Realm | A KATforge-powered site family rooted at one parent domain (`katforge.com`, `darkerdb.com`, `stumper.gg`, future siblings). Each realm has its own brand, palette, and `auth.{parent_domain}` host. NOTE: `parent_domain` is not always `.com`; `stumper.gg` proves it. Code that infers anything from "ends with `.com`" is wrong. |
| Shadow realm | Internal alias for non-parent realms (DarkerDB today). User-invisible. KATforge is the canonical realm; everything else is shadow. |
| Auth gateway | The Nuxt app at `auth.katforge.com` (and its per-realm aliases `auth.{parent_domain}`, e.g. `auth.darkerdb.com`, `auth.stumper.gg`). Single codebase, single deploy, served on every realm's parent domain via traefik. |
| Brand bundle | The per-realm data record (`{slug}.ts` under `@katforge/spark/brands`) that drives the auth gateway's appearance and provider list for that realm. |
| Spark | `@katforge/spark`. After KAT-4 it ships brand bundles + generic UI primitives only. No auth UI. |
| Sibling realm | Two realms whose parent domains differ (e.g. `katforge.com` and `darkerdb.com`). Cookies MUST NOT cross sibling boundaries. |

---

## 2. Topology

```
   +--------------------------------------------------------------------+
   |     auth.katforge.com (Nuxt SSR, one deploy, listens on :3000)     |
   |                                                                    |
   |   Served at (host aliases via traefik):                            |
   |     auth.katforge.com         (prod, parent realm)        [MVP]    |
   |     auth.darkerdb.com         (prod, shadow)              [MVP]    |
   |     auth.stumper.gg           (prod, shadow)              [later]  |
   |     auth.lextris.com          (prod, shadow)              [later]  |
   |     auth.geargoblins.com      (prod, shadow)              [later]  |
   |     auth.wc3stats.com         (prod, shadow)              [later]  |
   |     auth.dev.{parent_domain}  (dev, all six)                       |
   |     auth.qa.{parent_domain}   (qa, all six)                        |
   |                                                                    |
   |   Branding chosen at request time by Host header:                  |
   |     host  →  realm slug  →  brand bundle  →  logo/palette/         |
   |                                              providers/copy        |
   |                                                                    |
   |   Routes (all realm-themed by host):                               |
   |     /login                  email + password + provider buttons    |
   |     /register               account creation                       |
   |     /forgot                 password-reset request                 |
   |     /reset                  password-reset execute                 |
   |     /oauth/authorize        consent screen (PKCE clients)          |
   |     /oauth/callback         provider-callback bounce               |
   |     /verify-email           email verification landing             |
   +----------+---------------------------------------------------------+
              |                                  ^
              | calls                            | redirects user back to
              v                                  | the realm SPA
   +---------------------------------+    +--------------------------------+
   |  api.{realm}.com  (Symfony)     |    |   {realm}.com  (Vue/Vite SPA)  |
   |  /v1/gateway/login              |    |   Reads access_token from URL  |
   |  /v1/gateway/refresh            |    |   fragment OR XHRs /refresh    |
   |  /v1/oauth/*                    |    |   on first paint.              |
   |  /v1/oauth/userinfo             |    +--------------------------------+
   +---------------------------------+
```

Single deploy, multiple host aliases. Traefik routes every
`auth.[env.]{parent_domain}` for every registered realm to the same
Nuxt service. The gateway reads `Host` on every request, resolves the
realm via `getBrandByHost`, loads the brand bundle, and renders
accordingly. No per-realm build artefacts.

### 2.1 Host map

| Realm | Status | Parent domain | dev | qa | prod |
|---|---|---|---|---|---|
| katforge | MVP-mandatory | `katforge.com` | `auth.dev.katforge.com` | `auth.qa.katforge.com` | `auth.katforge.com` |
| darkerdb | MVP-mandatory | `darkerdb.com` | `auth.dev.darkerdb.com` | `auth.qa.darkerdb.com` | `auth.darkerdb.com` |
| stumper | aspirational | `stumper.gg` | `auth.dev.stumper.gg` | `auth.qa.stumper.gg` | `auth.stumper.gg` |
| lextris | aspirational | `lextris.com` | `auth.dev.lextris.com` | `auth.qa.lextris.com` | `auth.lextris.com` |
| geargoblins | aspirational | `geargoblins.com` | `auth.dev.geargoblins.com` | `auth.qa.geargoblins.com` | `auth.geargoblins.com` |
| wc3stats | aspirational | `wc3stats.com` | `auth.dev.wc3stats.com` | `auth.qa.wc3stats.com` | `auth.wc3stats.com` |

**MVP-mandatory vs aspirational.** KAT-2 must ship katforge + darkerdb
end-to-end (DNS, cert, brand bundle, E2E test). The other four are
pre-provisioned in the brand registry + REALM_PARENT_DOMAINS map and
the gateway resolves them by Host header, but they are not actively
tested until each realm's SPA lands and the realm-onboarding ticket
fires. Code MUST handle all six (host resolution, brand lookup); deploy
infra MUST be ready to flip them on without a code change.

DNS for `auth.darkerdb.com` lives in the same `darkerdb.com` zone that
already hosts the SPA and API (see `grimvault-mvp.md` §10.1). Terraform
ownership matches that doc: KATforge AWS account, `infra/terraform/dns/`.
Cert sourcing for non-Route53 zones (`lextris.com`, `geargoblins.com`)
is an open question (§11.6).

### 2.2 What moved where (vs the pre-KAT-2 layout)

| Surface | Before | After |
|---|---|---|
| Email/password login UI | `SparkLogin.vue` modal embedded in each realm SPA | `{auth_host}/login` (per brand bundle) |
| OAuth consent screen | `SparkAuthAuthorizePage.vue` mounted on each realm SPA | `{auth_host}/oauth/authorize` |
| OAuth standalone login page | `SparkAuthLoginPage.vue` | `{auth_host}/login` (replaces) |
| Per-realm palette + logo + providers | `realms.ts` single file in Spark | `brands/{slug}.ts` per realm in Spark, exported via `@katforge/spark/brands` subpath |
| Provider list | Hardcoded in `SparkLogin.vue` props | `brand.providers` field in the brand bundle |
| Cookie scope | Single host (`Domain` unset → request host only) | Parent domain (`Domain=.{parent_domain}`) |

The desktop GrimVault PKCE flow is untouched (`grimvault-mvp.md` §3).
The realm SPAs lose their inline auth UI and redirect to the gateway
instead.

### 2.3 Service contract (port, listen, health)

The Nuxt gateway runs as a single long-lived process. Compose, k8s, and
traefik all wire to:

| Property | Value |
|---|---|
| Listen port | `3000` (Nuxt SSR default; do not override) |
| Bind address | `0.0.0.0` inside the container |
| Health check | `GET /healthz` returns `200 { ok: true }`. Cheap, no DB / API call. |
| Process model | Single Node process per container; horizontal scale via replicas. |

KAT-8 (the Nuxt app ticket) may amend this if it has a justified reason
to diverge. Until then, port `3000` is the contract every downstream
infra ticket reads.

---

## 3. Spark scope after KAT-4

Spark ships two things and only two things:

1. **Brand bundles** (`@katforge/spark/brands/{slug}`). Data-only,
   tree-shakable, no Vue runtime needed to import.
2. **Generic UI primitives**: `SparkLogo`, `SparkFavicon`, `SparkSpinner`,
   `SparkButton`, `SparkAvatar`, `SparkInput`, `SparkModal`, etc. No
   business logic, no auth UI, no opinions about your routing.

### 3.1 What gets deleted

| File | Reason |
|---|---|
| `src/components/SparkLogin.vue` | Replaced by the gateway's `/login` route. |
| `src/components/SparkAuthLoginPage.vue` | Same. |
| `src/components/SparkAuthAuthorizePage.vue` | Replaced by the gateway's `/oauth/authorize`. |
| `src/realms.ts` | Split into `src/brands/{slug}.ts`. |

Spark's package version bumps to a new minor on this change (breaking
import paths). Downstream consumers (`katforge.com`, `darkerdb.com`,
internal admin) drop the auth-UI imports and switch their realm-data
imports to `@katforge/spark/brands`.

### 3.2 Brand bundle data model

```ts
// @katforge/spark/brands/{slug}.ts

import type { Brand } from '@katforge/spark/brands';

export const brand: Brand = {
   slug:           'darkerdb',
   display_name:   'DarkerDB',
   parent_domain:  'darkerdb.com',
   auth_host:      'auth.darkerdb.com',

   logo: {
      wordmark:      '/brands/darkerdb/wordmark.svg',
      icon:          '/brands/darkerdb/icon.svg',
      avatar:        '/brands/darkerdb/avatar.png',
   },

   favicons: {
      'ico':         '/brands/darkerdb/favicon.ico',
      'svg':         '/brands/darkerdb/favicon.svg',
      'png-192':     '/brands/darkerdb/favicon-192.png',
      'png-512':     '/brands/darkerdb/favicon-512.png',
      'apple-touch': '/brands/darkerdb/apple-touch-icon.png',
   },

   palette: {
      primary:         '#E60505',
      primary_hover:   '#B80404',
      primary_light:   '#F87171',
      surface:         '#000000',
      surface_raised:  '#1A0E0E',
      surface_sunken:  '#0A0505',
      text:            '#FFEAEA',
      text_secondary:  '#C8A0A0',
      text_muted:      '#785858',
      border:          '#2A1010',
      success:         '#22C55E',
      warning:         '#F59E0B',
      error:           '#EF4444',
   },

   typography: {
      font_class:      'font-metamorphous',
      weight:          400,
      tracking:        '0.02em',
      transform:       'none',
   },

   providers:       [ 'discord', 'google', 'katforge' ],

   frontend_url:    'https://darkerdb.com',
   support_email:   'support@darkerdb.com',
   terms_url:       'https://darkerdb.com/terms',
   privacy_url:     'https://darkerdb.com/privacy',
};
```

### 3.3 Brand registry types

```ts
// @katforge/spark/brands/index.ts

export interface BrandLogo {
   wordmark: string;
   icon:     string;
   avatar:   string;
}

export interface BrandFavicons {
   'ico':         string;
   'svg':         string;
   'png-192':     string;
   'png-512':     string;
   'apple-touch': string;
}

export interface BrandPalette {
   primary:        string;
   primary_hover:  string;
   primary_light:  string;
   surface:        string;
   surface_raised: string;
   surface_sunken: string;
   text:           string;
   text_secondary: string;
   text_muted:     string;
   border:         string;
   success:        string;
   warning:        string;
   error:          string;
}

export interface BrandTypography {
   font_class: string;
   weight:     number;
   tracking:   string;
   transform:  'uppercase' | 'none';
}

export type Provider = 'katforge' | 'discord' | 'google' | 'apple' | 'steam';

export interface Brand {
   slug:          string;
   display_name:  string;
   parent_domain: string;
   auth_host:     string;
   logo:          BrandLogo;
   favicons:      BrandFavicons;
   palette:       BrandPalette;
   typography:    BrandTypography;
   providers:     Provider [];
   frontend_url:  string;
   support_email: string;
   terms_url:     string;
   privacy_url:   string;
}

export function brand (slug: string): Brand | undefined;
export function brands (): Brand [];
export function getBrandByHost (host: string): Brand | undefined;
```

Lookup helpers:

| Helper | Key | Return | Used by |
|---|---|---|---|
| `brand(slug)` | brand `slug` (e.g. `'darkerdb'`) | `Brand` or `undefined` | code that already knows the realm (tests, build-time tooling, CLI). |
| `brands()` | none | `Brand[]` (all registered) | iteration (sitemaps, CI lints, docs generation). |
| `getBrandByHost(host)` | raw `Host` header (e.g. `'auth.dev.darkerdb.com'`) | `Brand` or `undefined` | the gateway middleware on every request. Strips the leading `auth.` and any `{env}.` segment, then matches against `parent_domain`. |

All three return `undefined` on miss (matches Vue/TS idiom, not `null`).
Callers MUST handle the miss case explicitly; the auth gateway middleware
hard-fails with `400 Bad Host` rather than guessing.

### 3.4 Subpath export

```json
// packages/spark/package.json (relevant excerpt)
{
   "exports": {
      ".":         "./dist/index.js",
      "./brands":  "./dist/brands/index.js"
   }
}
```

Importing from `@katforge/spark/brands` pulls only the brand registry
(data + type only). It MUST NOT transitively import Vue or any Spark
component. Sites that need the data but not the UI (e.g. SSR meta-tag
generation, build-time favicon emission) consume this subpath.

---

## 4. Layered Tailwind palette

Spark holds cross-cutting brand identity tokens. Site-specific tokens
extend the Spark layer in each site's local Tailwind config. The auth
gateway only knows about the Spark layer.

```
   +-----------------------------------------------------------+
   |  Tailwind config in <site>                                |
   |                                                           |
   |  theme.extend.colors = {                                  |
   |     ...sparkPalette (brand),     <-- 13 tokens from Spark |
   |     rarity_common:    '#FFFFFF', <-- site-specific layer  |
   |     rarity_uncommon:  '#80D600', |                        |
   |     rarity_rare:      '#3B82F6', |                        |
   |     market_buy:       '#22C55E', |                        |
   |     market_sell:      '#EF4444', |                        |
   |     tooltip_sep:      '#785858', |                        |
   |  }                                                        |
   +-----------------------------------------------------------+
```

### 4.1 Rule of thumb

| In Spark `brand.palette` if | In site's local Tailwind config if |
|---|---|
| Token appears on the realm's auth host | Token only appears inside the realm's product surfaces |
| Token defines cross-domain brand identity (primary, surface, text family) | Token is product-specific (item rarities, market direction, tooltip chrome) |
| Changing it would change the realm's perceived brand | Changing it only affects one product surface |

If unsure, ask: would I want this token to appear on the realm's login
page? Yes → Spark. No → local.

### 4.2 Cross-cutting token set (13)

```
   primary           primary_hover     primary_light
   surface           surface_raised    surface_sunken
   text              text_secondary    text_muted
   border
   success           warning           error
```

These are the ONLY tokens the auth gateway is allowed to render against.
Anything beyond this list MUST live in the site's local config and MUST
NOT appear in auth-host markup.

---

## 5. Cookie and token model

### 5.1 Cookie scope: parent domain

The refresh-token cookie is set with `Domain=.{parent_domain}`, scoped
to the realm's entire subtree. The browser auto-attaches it on any
`*.{parent_domain}` request.

```
   Set-Cookie: katforge_refresh=<opaque>;
               Domain=.darkerdb.com;
               Path=/;
               HttpOnly;
               Secure;
               SameSite=Lax;
               Max-Age=2592000
```

| Property | Value | Reason |
|---|---|---|
| `Domain` | `.{parent_domain}` (resolved per request via `Constants::parentDomainForHost`) | Lets the realm's auth host, SPA, API, and future subdomains share the session. |
| `Path` | `/` | Was `/v1/gateway` pre-KAT-2. Widened so the SPA can also refresh from any path. |
| `HttpOnly` | true | JS cannot read the refresh token. |
| `Secure` | true | TLS only. |
| `SameSite` | `Lax` | Allows top-level navigation cross-site (the realm SPA → auth gateway flow needs this). Was `none` pre-KAT-2; tighten to `Lax` because we no longer rely on cross-site XHR carrying the cookie. |
| `Max-Age` | 30 days (registered) / 2 years (guest) | Unchanged. |

**Leading dot.** RFC 6265 §4.1.2.3 formally deprecated the leading `.`
in `Domain` attributes; modern browsers treat `Domain=.darkerdb.com`
and `Domain=darkerdb.com` identically (both match `*.darkerdb.com` and
the apex). We keep the leading dot because:

1. It matches the `Set-Cookie:` convention every existing tool, log,
   and tutorial uses, so debugging is one fewer "wait, where's the dot?"
   step.
2. It documents intent at a glance: this cookie is scoped to the whole
   subtree, not to one host.
3. No behavioral difference, so no risk.

If a future reviewer flags this as "non-spec-compliant," point them at
this paragraph. The deprecation is for the wire-format ambiguity, not
the semantic.

### 5.2 Sibling realm isolation (mandatory invariant)

Two realms with different parent domains MUST NOT share cookies.

```
   auth.darkerdb.com  →  Set-Cookie: Domain=.darkerdb.com   (visible only on *.darkerdb.com)
   auth.katforge.com  →  Set-Cookie: Domain=.katforge.com   (visible only on *.katforge.com)
```

A session minted on `auth.darkerdb.com` does NOT log the user into
`katforge.com` automatically. Cross-realm "Sign in with KATforge" must
go through a deliberate OAuth handshake (§7), not cookie reuse.

### 5.3 JWT + cookie hybrid

| Token | Storage | Lifetime | Transport |
|---|---|---|---|
| Refresh token | HttpOnly cookie (parent-scoped) | 30 days (registered) / 2 years (guest) | Auto-attached by browser. NEVER returned in response body for browser clients. |
| Access token (JWT, RS256) | Client memory only | 15 min | Returned in JSON body of `/v1/gateway/login` and `/v1/gateway/refresh`. Sent as `Authorization: Bearer <jwt>` on every API call. |

Forbidden access-token storage:

- `localStorage` / `sessionStorage` (XSS-reachable)
- URL parameters / fragments persisted to history
- Cookies (defeats the point: we want browser-controlled refresh + JS-controlled access)

The auth gateway hands the access token to the destination SPA via
URL fragment (`#access_token=...&expires_in=900`), which is parsed and
stripped from history before any third-party script runs. The fragment
is chosen over a query string because fragments are not sent to the
server and not logged by reverse proxies.

### 5.4 Desktop client unchanged

GrimVault desktop:

- Uses PKCE + loopback redirect (`grimvault-mvp.md` §3).
- Receives access + refresh in the `/oauth/token` response body.
- Stores both in Windows Credential Manager (DPAPI).
- Sends `Authorization: Bearer <jwt>` on every API call.
- Does NOT use cookies.

Desktop flow is Journey 4 below; it does not interact with the cookie
scope decision above.

---

## 6. Four sign-in journeys

All four journeys terminate with the user signed in on their intended
destination. Diagrams use these conventions:

```
   ====>   user-initiated browser nav (top-level)
   ---->   server-issued redirect (302 / 303)
   ....>   XHR / fetch (same-tab, no nav)
   |--->   cookie set by Set-Cookie header
```

### 6.1 Journey 1: katforge.com, email/password

User is on `katforge.com`, clicks Sign In, authenticates with email +
password, returns to `katforge.com` signed in.

```
   browser                       auth.katforge.com           api.katforge.com
   -------                       -----------------           ----------------

   on katforge.com
   clicks "Sign In"
        ====================>    GET /login
                                  ?redirect=https://katforge.com/dashboard
                                 (renders branded form,
                                  Host = auth.katforge.com,
                                  brand = katforge)

   types email + password
        ====================>    POST /api/login
                                 (Nuxt server route,
                                  proxies to api below)
                                       ....................>  POST /v1/gateway/login
                                                              { identifier, password }
                                       <....................  200 { access_token, refresh_token, player }
                                                              Set-Cookie: katforge_refresh=...;
                                                                Domain=.katforge.com; Path=/
                                 (Nuxt server route MUST forward
                                  the API's Set-Cookie header
                                  verbatim onto its own response;
                                  see note below)
        <====================    302 Location: https://katforge.com/dashboard
                                                #access_token=<jwt>&expires_in=900
                                 Set-Cookie: katforge_refresh=...;
                                   Domain=.katforge.com; Path=/    (forwarded)
        ====================>    GET https://katforge.com/dashboard#access_token=...
                                 (SPA boots, parses fragment,
                                  stores access in memory,
                                  rewrites URL to strip fragment,
                                  cookie is already on the domain)
```

**Cookie-forwarding contract (Nuxt server route → browser).** Every
Nuxt `/api/*` route that proxies to a Symfony endpoint which sets
cookies MUST copy the upstream `Set-Cookie` header onto its own
outbound response verbatim. The `Domain` attribute the API emitted
already targets the correct parent domain (resolved by Symfony from
the request `Host`, which Nuxt MUST forward); the Nuxt route MUST NOT
rewrite, strip, or re-emit the cookie. This applies to `/api/login`,
`/api/register`, `/api/refresh`, `/api/logout`, and any future
session-issuing proxy. Hard requirement for KAT-8 (Nuxt app
implementer); easy to get wrong with `$fetch` defaults which drop
`Set-Cookie` from the response.

### 6.2 Journey 2: darkerdb.com, Discord OAuth

User is on `darkerdb.com`, clicks Sign In, picks Discord, returns to
`darkerdb.com` signed in.

```
   browser                  auth.darkerdb.com         api.darkerdb.com         discord.com
   -------                  -----------------         ----------------         -----------

   on darkerdb.com
   clicks "Sign In"
        ===============>    GET /login
                             ?redirect=https://darkerdb.com/
                            (brand = darkerdb,
                             providers from bundle)

   clicks Discord
        ===============>    GET /oauth/start/discord
                             ?redirect=...
                                  ....................>   POST /v1/oauth/start
                                                          { provider: discord,
                                                            final_redirect: ...,
                                                            callback: https://auth.darkerdb.com/oauth/callback }
                                  <....................   200 { url: https://discord.com/oauth2/authorize?... }
        <---------------    302 Location: <discord url>
        ==============================================================================>  GET /oauth2/authorize
                                                                                         (user approves)
        <----------------------------------------------------------------------------    302 Location:
                                                                                         https://api.darkerdb.com/v1/oauth/callback/discord?code=...
        ==========================================>    GET /v1/oauth/callback/discord?code=...
                                                       (API exchanges code with Discord,
                                                        resolves to KATforge player,
                                                        mints access + refresh)
                                                       Set-Cookie: katforge_refresh=...;
                                                         Domain=.darkerdb.com; Path=/
        <==========================================    302 Location:
                                                       https://auth.darkerdb.com/oauth/callback
                                                         ?redirect=https://darkerdb.com/
                                                         #access_token=<jwt>&expires_in=900
        ===============>    GET /oauth/callback
                             (one extra hop: gateway has a chance
                              for uniform error handling, then bounces)
        <---------------    302 Location: https://darkerdb.com/
                                          #access_token=<jwt>&expires_in=900
        ===============>    GET darkerdb.com/#access_token=...
                            (SPA boots, parses fragment, etc.)
```

### 6.3 Journey 3: darkerdb.com, "Sign in with KATforge"

User is on `darkerdb.com`, clicks Sign In, picks "Sign in with KATforge",
authenticates on `auth.katforge.com`, returns to `darkerdb.com` signed in.

This is the cross-realm federation case. KATforge is a registered OAuth
provider for the DarkerDB realm.

```
   browser           auth.darkerdb.com       api.darkerdb.com   auth.katforge.com    api.katforge.com
   -------           -----------------       ----------------   -----------------    ----------------

   on darkerdb.com
   clicks "Sign In"
        ==========>  GET /login
                     ?redirect=https://darkerdb.com/

   clicks "Sign in with KATforge"
        ==========>  GET /oauth/start/katforge
                       ?redirect=...
                          ....................>  POST /v1/oauth/start
                                                 { provider: katforge,
                                                   final_redirect: ...,
                                                   callback: https://auth.darkerdb.com/oauth/callback }
                          <....................  200 { url:
                                                   https://auth.katforge.com/oauth/authorize?
                                                     client_id=darkerdb-realm&
                                                     redirect_uri=https://api.darkerdb.com/v1/oauth/callback/katforge&
                                                     state=...&code_challenge=... }
        <----------  302 Location: <kf authorize url>
        ====================================================>  GET /oauth/authorize?...
                                                               (renders KATforge-branded
                                                                consent screen.
                                                                If user is not signed in
                                                                to katforge.com, prompts
                                                                login first.)

       (user signs in or approves)
        <====================================================  302 Location:
                                                               https://api.katforge.com/v1/oauth/authorize/submit?code=...
                                                                  ....................>  POST /v1/oauth/authorize/submit
                                                                                         (issues authorization code,
                                                                                          302 back to redirect_uri)
        <-------------------------------------------------------------------------------- 302 Location:
                                                                                         https://api.darkerdb.com/v1/oauth/callback/katforge?code=...&state=...
        ===================================>  GET /v1/oauth/callback/katforge?...
                                              (DarkerDB API exchanges code
                                               with KATforge API server-side,
                                               receives KATforge access token,
                                               calls /v1/oauth/userinfo,
                                               resolves to KATforge player,
                                               mints DarkerDB session
                                               for that player)
                                              Set-Cookie: katforge_refresh=...;
                                                Domain=.darkerdb.com; Path=/
        <===================================  302 Location:
                                              https://auth.darkerdb.com/oauth/callback
                                                ?redirect=https://darkerdb.com/
                                                #access_token=<jwt>&expires_in=900
        ==========>  GET /oauth/callback (uniform-error bounce)
        <----------  302 Location: https://darkerdb.com/#access_token=...
        ==========>  GET darkerdb.com/#access_token=...
                     (signed in)
```

Note: the user ends up with TWO independent sessions:

- `auth.katforge.com` set a `.katforge.com` cookie when the user signed in there.
- The DarkerDB callback set a `.darkerdb.com` cookie for the new DarkerDB session.

These do not bleed into each other (§5.2). The KATforge session lets
subsequent "Sign in with KATforge" attempts skip the password prompt.
The DarkerDB session is what `*.darkerdb.com` runs on.

### 6.4 Journey 4: grimvault.exe, PKCE + loopback (unchanged)

Pointer only. Full spec in `docs/architecture/grimvault-mvp.md` §3.

```
   grimvault.exe                browser                  auth.darkerdb.com    api.darkerdb.com
   -------------                -------                  -----------------    ----------------

   user clicks Sign In
   (tray menu)
        binds 127.0.0.1:<port>
        launches browser
        ====================>   GET auth.darkerdb.com/oauth/authorize?
                                  client_id=grimvault-desktop&
                                  redirect_uri=http://127.0.0.1:<port>/callback&
                                  state=...&code_challenge=...
                                (consent screen, single Allow button)

       (user clicks Allow)
                                     ....................>  POST /v1/oauth/authorize
                                     <....................  200 { code: ... }
                                <---  302 Location:
                                       http://127.0.0.1:<port>/callback?code=...&state=...
                                ===>  loopback receives code
        <===========================  one-shot 200 (branded close page)

        POST /oauth/token ...........................................>  (PKCE exchange)
        <............................................................   { access_token, refresh_token }

        stores in Windows Credential Manager (DPAPI)
        NO COOKIE EVER SET (desktop is not a browser session)
```

The desktop flow does NOT touch the parent-domain cookie. Its
refresh-token is opaque, stored in DPAPI, sent in `/oauth/token` request
bodies. Cookie-scope changes do not affect it.

---

## 7. KATforge as OAuth provider

KATforge is a federated OAuth 2.0 provider that every shadow realm
registers as a sign-in option. The user sees "Sign in with KATforge"
on `auth.{shadow-realm}.com/login` alongside Discord, Google, Apple,
Steam.

### 7.1 Provider list resolution

```
   GET https://auth.darkerdb.com/login
           |
           v
   resolve realm by Host header  =>  darkerdb
           |
           v
   load brand bundle from @katforge/spark/brands/darkerdb
           |
           v
   brand.providers = [ 'discord', 'google', 'katforge' ]
           |
           v
   filter:  if requesting realm IS katforge, drop 'katforge' from list
           (KATforge does not show "Sign in with KATforge")
           |
           v
   render provider buttons in that order
```

### 7.2 Self-reference filter (mandatory)

When the gateway resolves the requesting realm to `katforge`, it MUST
filter `'katforge'` out of the providers list before rendering. Showing
"Sign in with KATforge" on `auth.katforge.com` would send the user
through OAuth back to the same email/password form they're already
looking at.

Rule applies symmetrically: any realm `X` MUST filter `'X'` out of its
own provider list. The simplest implementation is one line in the
gateway's render path:

```ts
const providers = brand.providers.filter (p => p !== brand.slug);
```

### 7.3 KATforge OAuth endpoints

These are KATforge's identity-provider surface (called by shadow-realm
APIs, never by users directly):

| Method | Path | Host | Purpose |
|---|---|---|---|
| GET | `/oauth/authorize` | `auth.katforge.com` | Consent screen for shadow realms. User-facing. |
| POST | `/oauth/token` | `api.katforge.com` | Code → access-token exchange. Server-to-server. |
| GET | `/v1/oauth/userinfo` | `api.katforge.com` | Returns the authenticated player's KATforge identity. Server-to-server. |

`/v1/oauth/userinfo` is new in KAT-2. Response shape:

```json
{
   "body": {
      "player_id":     "01J9X3Q7Z4M2B8K0V5N6Y2T7AA",
      "user_id":       "01J9X3Q7Z4M2B8K0V5N6Y2T7CC",
      "username":      "ethan",
      "display_name":  "Ethan Anders",
      "email":         "ethan@example.com",
      "email_verified": true,
      "avatar_url":    "https://cdn.katforge.com/avatars/...",
      "created_at":    "2024-01-15T10:23:01Z"
   }
}
```

Caller authenticates with the KATforge-issued access token (Bearer
header) obtained from `/oauth/token`. Scopes:

| Scope | Releases |
|---|---|
| `openid` | `player_id`, `user_id` (always present) |
| `profile` | `username`, `display_name`, `avatar_url`, `created_at` |
| `email` | `email`, `email_verified` |

Shadow realms request `openid profile email` by default. Per-realm scope
descriptions render on the consent screen via the existing scope
registry (`grimvault-mvp.md` §4.8); KATforge maintains the canonical
copy.

**Two fields above are pending data-model resolution (§11.7, §11.8).**
`email_verified` requires a `users.email_verified_at` column the
`User` entity does not currently have. `avatar_url` requires a
canonical projection of the six `Player.avatar_*` columns into a
single URL string. KAT-5 is currently hardcoding `email_verified=false`
and `avatar_url=null`. Whether to keep these fields in the contract,
populate them properly, or drop them is a CTO call (see §11).

**Audience is derived statically from `client_id`, not stored on the
authorization code.** When `/oauth/token` exchanges a code for an
access token, the resulting JWT's `aud` claim is looked up from a
`Constants::REALM_CLIENT_AUDIENCES` map keyed by `client_id`. The
`OAuthCode` entity itself carries no audience field. This works for
MVP because the mapping is static (one client → one audience). Tech
debt: revisit when a single client legitimately needs to mint tokens
for multiple audiences (e.g. an admin client that targets several
realms); at that point audience moves onto the code row and the
consent screen has to display per-grant audience selection.

### 7.4 Shadow-realm OAuth client registration

Each shadow realm registers itself as a `client_id` in KATforge's OAuth
client table:

| Field | Value (darkerdb prod example) |
|---|---|
| `client_id` | `darkerdb-realm` |
| `client_secret` | (server-side only, in `api.darkerdb.com` env) |
| `redirect_uris` | `https://api.darkerdb.com/v1/oauth/callback/katforge`, plus dev/qa variants |
| `allowed_scopes` | `openid profile email` |
| `is_first_party` | `true` |
| `requires_pkce` | `true` (even though confidential, hardens against code interception) |

Confidential-client variant (with secret) is used because the OAuth
handshake happens server-to-server between `api.darkerdb.com` and
`api.katforge.com`; the secret never reaches the browser. Per-env
client IDs follow the same `<base>-<env>` pattern as the desktop
client (`grimvault-mvp.md` §10.2): `darkerdb-realm`,
`darkerdb-realm-qa`, `darkerdb-realm-dev`.

**Why per-realm clients, not one shared `katforge-oauth-server`
client.** Each shadow realm gets its own KATforge OAuth client
registration. Two reasons, both mandatory:

1. **Per-realm secret rotation.** Rotating a leaked secret affects only
   the compromised realm. A shared client would force every realm into
   a coordinated rotation on any leak, turning a contained incident
   into an org-wide outage.
2. **Per-realm audience boundary.** The `aud` claim on KATforge-issued
   tokens is derived from `client_id` (per the static-derivation rule
   above). Per-realm clients yield per-realm audiences (`aud=darkerdb`,
   `aud=stumper`, etc.), which lets each realm's API reject tokens
   issued for a different realm. A shared client erases this boundary
   and a token minted for DarkerDB would be valid against Stumper's API.

Cost: O(realms) client rows in the KATforge `oauth_clients` table.
Trivially manageable; the registration is a one-line seed per realm
onboarding.

---

## 8. URL inventory

### 8.1 Auth gateway routes (Nuxt, served on every `auth.[env.]{parent_domain}`)

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/login` | none (sets session on success) | Email/password + provider buttons. Branded by Host. |
| POST | `/api/login` | none | Nuxt server-route proxy to `POST api/v1/gateway/login`. Sets parent-domain refresh cookie. Returns access token + redirects to destination. |
| GET | `/register` | none | Account creation. |
| POST | `/api/register` | none | Server-route proxy to `POST api/v1/gateway/register`. |
| GET | `/forgot` | none | Password-reset request form. |
| POST | `/api/forgot` | none | Server-route proxy. |
| GET | `/reset` | none (token in query) | Password-reset execute form. |
| POST | `/api/reset` | none | Server-route proxy. |
| GET | `/oauth/authorize` | refresh cookie | OAuth consent screen for PKCE clients (desktop, third-party). Reads `client_id`, `redirect_uri`, scopes; if not signed in, prompts login first then resumes consent. |
| POST | `/api/oauth/authorize` | refresh cookie | Issues authorization code. |
| GET | `/oauth/start/:provider` | none | Initiates upstream OAuth (Discord, Google, KATforge, etc). Resolves provider URL via API. 302s to provider. |
| GET | `/oauth/callback` | none | Uniform-error bounce. Receives `?redirect=...&#access_token=...` from API callbacks, validates `redirect` against the realm's allow-list, 302s to destination. |
| GET | `/verify-email` | none (token in query) | Email-verification landing. |

### 8.2 New `api.{realm}.com` endpoints in KAT-2

| Method | Path | Host group | Purpose |
|---|---|---|---|
| GET | `/v1/oauth/userinfo` | `api.katforge.com` only (KATforge is the provider) | Returns the caller's KATforge identity. Server-to-server, Bearer-auth. See §7.3. |
| GET | `/v1/oauth/callback/:provider` | every realm | Server-side OAuth callback handler. Exchanges provider code, mints realm session, sets parent-domain cookie, 302s to the realm's `{auth_host}/oauth/callback?redirect=...&#access_token=...`. |
| POST | `/v1/oauth/start` | every realm | Returns provider authorize URL for the gateway to redirect into. |

Existing endpoints that change behavior (not shape):

| Endpoint | Change |
|---|---|
| `POST /v1/gateway/login` | Refresh cookie now `Domain=.{realm}.com; Path=/`. Host-derived. |
| `POST /v1/gateway/refresh` | Same. Cookie reads from `Path=/` so SPA-initiated refresh works. |
| `POST /v1/gateway/logout` | Clears cookie with matching `Domain` + `Path` so the browser actually drops it. |

### 8.3 What `grimvault-mvp.md` §5 still owns (unchanged)

- `/oauth/token`, `/oauth/revoke` (RFC 7009 desktop surface)
- `/v1/oauth/grants*` (user-facing connected-apps surface)
- `/v2/grimvault/*` (DarkerDB realm controllers)
- `/.well-known/jwks.json` (JWKS mirroring policy)

The auth gateway does NOT call these; the desktop client and the SPAs
do.

---

## 9. `Constants::REALM_PARENT_DOMAINS` (CTO Option A)

The PHP API needs to know each realm's parent domain at session-issue
time so it can set the right `Domain` cookie attribute. Implementation:
a hardcoded map on `KAT\Constants`, with the canonical reference being
the Spark brand bundles.

```php
final class Constants
{
   // ...existing constants...

   /**
    * Parent domain for each realm slug. Used by Http\Cookie::refresh()
    * to scope the refresh-token cookie to the realm subtree (KAT-6).
    *
    * Canonical source of truth: @katforge/spark/brands/{slug}.parent_domain
    * Mirrored here because PHP cannot easily import the TypeScript bundles.
    * Keep these in sync on every realm add/rename. See "Tech debt" below
    * for the migration path once realm count grows.
    */
   public const REALM_PARENT_DOMAINS = [
      'katforge'    => 'katforge.com',
      'darkerdb'    => 'darkerdb.com',
      'lextris'     => 'lextris.com',
      'stumper'     => 'stumper.gg',
      'geargoblins' => 'geargoblins.com',
      'wc3stats'    => 'wc3stats.com',
   ];

   public static function parentDomainForHost (string $host): ?string
   {
      $host = strtolower ($host);

      foreach (self::REALM_PARENT_DOMAINS as $slug => $parent) {
         if ($host === $parent || str_ends_with ($host, '.' . $parent)) {
            return $parent;
         }
      }

      return null;
   }
}
```

`Http\Cookie::refresh()` then takes the request host, resolves it to
the parent domain, and includes a `.{parent}` Domain attribute:

```php
SymfonyCookie::create (Constants::REFRESH_COOKIE_NAME)
   ->withValue ($value ?? '')
   ->withExpires (...)
   ->withDomain ('.' . $parentDomain)   // <-- new
   ->withPath ('/')                     // <-- widened from /v1/gateway
   ->withSecure (true)
   ->withHttpOnly (true)
   ->withSameSite ('lax');              // <-- tightened from 'none'
```

The resolver hooks the request host through the existing `Request`
service; signature additions on `Cookie::refresh()` and `Session::attach()`
are KAT-6 implementation details.

**Slug naming.** Each key in `REALM_PARENT_DOMAINS` MUST match the
`slug` of the corresponding `@katforge/spark/brands/{slug}.ts` bundle
exactly. The Gear Goblins realm is canonically `geargoblins`, not
`goblins` (the colloquial short form). Existing Spark code that uses
`goblins` is legacy and gets renamed under KAT-4. Any future realm
addition adds rows on both sides under the same slug, in one PR, with
the §9.1 lint as the safety net.

**Unknown-host behavior.** If `parentDomainForHost()` returns `null`
(the request `Host` doesn't match any registered realm), the calling
code's response is an open question (see §11.9). KAT-6 currently
defaults to katforge as a safety fallback; whether that becomes the
ratified behavior or flips to a hard 500 is a CTO call.

### 9.1 Tech debt: shared YAML (Option B)

Revisit when `count(REALM_PARENT_DOMAINS) >= 5`. Migration target:

```
   katforge-meta/
      realms.yaml          # single source of truth
                           # generated artefacts:
                           #   - PHP class Constants::REALM_PARENT_DOMAINS
                           #   - TS  @katforge/spark/brands/*
                           #   - traefik labels for auth.* host aliases
```

Decision driver: when the cost of editing two files (PHP + TS) on every
realm change exceeds the cost of standing up codegen. Under five realms,
the duplication is cheaper than the build pipeline. Above five, codegen
wins.

Until then: a CI lint covers the three lists that must stay in sync.
Failing any one of the checks fails the build. Owned by Mekkatorque
(devops); spawn ticket under KAT-2.

| Check | Failure means |
|---|---|
| Every `REALM_PARENT_DOMAINS[slug] === @katforge/spark/brands/{slug}.parent_domain` | PHP map drifted from Spark bundles. Fix the side that's wrong. |
| Every value of `REALM_PARENT_DOMAINS` appears in `Constants::ALLOWED_REDIRECT_DOMAINS` | A realm parent domain is not on the redirect allow-list, so its own OAuth callbacks will be rejected. |
| Every slug used by Spark bundles has a matching `REALM_PARENT_DOMAINS` entry | A bundle was added without wiring the PHP cookie scope, so the auth gateway can serve the realm but the API cannot mint sessions for it. |

The current state (as of KAT-3 R2) has `wc3stats.com` in
`REALM_PARENT_DOMAINS` but NOT in `ALLOWED_REDIRECT_DOMAINS`. The
second check catches exactly this kind of drift. Fix in KAT-6's wake
or in the next API-engineer pass through `Constants.php`, whichever
is sooner.

---

## 10. Security properties (must hold)

| Property | Mechanism |
|---|---|
| XSS cannot steal long-lived sessions | Refresh token is HttpOnly cookie; access token (memory only) expires in 15 min. |
| CSRF cannot mint sessions | Refresh cookie is `SameSite=Lax`, so cross-site POST to `/v1/gateway/refresh` does not carry it. All state-changing endpoints additionally check `Origin` / `Referer`. |
| Sibling realms cannot impersonate each other | Cookies scoped to parent domain per §5.2; no cross-domain cookie set. Cross-realm sign-in requires deliberate OAuth handshake (§7). |
| Reverse proxies / access logs don't capture access tokens | Tokens travel in URL fragment (not query), Authorization header (not URL), or response body (not URL). |
| Self-reference loop on KATforge auth page | §7.2 filter rule. |
| KATforge ↔ realm relationship hidden from third-party `curl` | Inherited from `grimvault-mvp.md` §3.8: JWKS + `iss` pinned to realm host; `/v1/oauth/userinfo` is the only KATforge-host call and it's server-to-server only. |
| Access tokens cannot be replayed across envs | `iss` claim is env-pinned (`grimvault-mvp.md` §10.3). |

---

## 11. Open questions

Ranked by blast radius. Surface for CTO before downstream tickets hit
the affected surfaces; do NOT pre-decide.

### 11.1 Cookie path widening: any compat fallout?

Pre-KAT-2 the refresh cookie was scoped to `Path=/v1/gateway`. KAT-2
widens to `Path=/`. Existing browsers holding the old cookie will keep
sending it under the old path; the new cookie under `/` lands alongside
it. Server sees two cookies with the same name, RFC says order is
unspecified. Risk: `$request->cookies->get('katforge_refresh')` picks
the wrong one and refresh fails until the user re-logs in.

Mitigation options (need CTO call):

- (a) Add a one-time logout-everyone deploy step before KAT-6 ships.
- (b) Server-side: prefer the cookie whose value validates; fall through to the other.
- (c) Bump cookie name (`katforge_refresh_v2`) so old and new coexist cleanly.

Blast radius: every active session at deploy time. Highest-impact open question.

**Recommendation pending ratification:** Thrall (engineer) recommends
option (b), server-side disambiguation. It avoids a forced-logout
event and avoids a permanent cookie-name change for a one-time issue.
Symfony exposes the raw `Cookie:` header, so iterating candidate values
and picking the one that successfully validates as a refresh token is
straightforward and self-cleaning (the wrong-path cookie expires
naturally over 30 days). Awaiting Anders' confirm or override.

### 11.2 SameSite=Lax + top-level navigation cookie set

When the realm SPA does `window.location = brand.auth_host + '/login'`,
the gateway sets a cookie. On the redirect back to the realm's
`frontend_url`, will that cookie be on the inbound request? With
`SameSite=Lax` and a top-level GET navigation: yes per spec. But:
some embedded browser environments (Discord overlay, in-app webviews
on iOS) handle Lax inconsistently. Need a verification pass before
claiming the gateway works in all contexts the realms care about.

Owner: Maiev Shadowsong (security) + Rhonin (research). Not blocking
for desktop browser; possibly blocking for in-app browsers.

### 11.3 Brand bundle hot-reload

The gateway is a single deploy. Adding a realm today means: edit Spark,
publish Spark, redeploy gateway. Is that acceptable? Or should bundles
be fetched at runtime (gateway boots, pulls a manifest) so adding a
realm is a config push, not a deploy?

Recommendation (not ratified): bake at build time for MVP, manifest-based
for v2. Single-deploy-per-week cadence makes this a non-issue at current
realm count. Surface once realm count crosses 4 or release cadence
slows below weekly.

### 11.4 `/oauth/callback` uniform-error scope

Spec'd as "uniform error handling bounce" but the actual error
vocabulary is unspecified. Are these standardised across realms (with
realm-specific copy from the brand bundle), or per-realm? What does the
gateway do with `?error=access_denied` vs `?error=server_error` vs
`?error=invalid_state`?

Owner: Anduin Wrynn (product) for copy + behaviour, Khadgar (this doc)
for the response-shape contract. Trivial to fix post-MVP if we ship
the basic path first.

### 11.5 Email-template per-realm branding

The auth gateway sends transactional emails (verify, password reset)
under the realm's branding. Today `EmailService` uses one
`FROM_ADDRESS` constant and one Twig template path. Do we:

- (a) Add `Brand::from_address`, `Brand::email_template_dir` fields and route per realm?
- (b) Keep one template, render realm name/colors from a context var?

(b) is cheaper for MVP, (a) is correct long-term. Probably (b) now and
spawn a ticket for (a) when realm-specific email design lands. Owner:
Tyrande (designer) + Thrall (engineer).

### 11.6 TLS cert sourcing for non-Route53 realm zones

`auth.lextris.com` and `auth.geargoblins.com` (and any future realm
whose zone is NOT in the KATforge Route53 account, per `aws-profiles.md`)
need TLS certs but cannot use cert-manager's DNS-01 against Route53
out of the box. Three options:

| Option | Tradeoff |
|---|---|
| (a) Move the zones into Route53 under the KATforge account | Cleanest. Reuses the existing cert-manager + DNS-01 pattern. Cost: one-time zone transfer + NS update at the registrar. Future realms are zero-friction. |
| (b) Per-realm `Certificate` resources with DNS-01 against the upstream provider | Keeps zone ownership distributed. Cost: provision provider credentials per realm; cert-manager needs the provider's webhook installed for each. Adds a moving part per realm. |
| (c) HTTP-01 against the k3s ingress | Avoids DNS provider integration entirely. Cost: ingress must be reachable on :80 for the LE challenge; works fine for these external-facing hosts. Simpler than (b), less clean than (a). |

Owner: Mekkatorque (devops). Blast radius: blocks aspirational realm
rollout (not katforge/darkerdb MVP). Recommend (a) if zone-transfer
political cost is low; (c) if it isn't.

### 11.7 `email_verified` field: column or drop?

§7.3's `/v1/oauth/userinfo` response includes `email_verified`. The
`User` entity has no `email_verified_at` column today. Options:

- (a) Add `users.email_verified_at` column + verification flow + populate the field for real.
- (b) Drop `email_verified` from the response shape; OIDC consumers that need it can implement their own verification or assume false.

Owner: Magni Bronzebeard (dba) for the schema, Maiev Shadowsong
(security) on whether shadow realms need to trust this signal. Blast
radius: the shape contract for every shadow-realm consumer of
`/v1/oauth/userinfo`. Cheap to flip either way as long as we decide
before the first non-MVP realm reads the field.

### 11.8 `avatar_url` field: projection or drop?

§7.3's response includes `avatar_url: string`. The `Player` entity has
six avatar columns (`avatar_icon`, `avatar_color`, `avatar_bg`,
`avatar_border`, `avatar_src`, `avatar_upload_id`) but no canonical
URL projection; today they're served via `GET /v1/players/{id}/avatar`
which 302s to whatever underlying image is current. Options:

- (a) Spec how the six columns project into a single URL string for OIDC consumers (e.g. always return the `/v1/players/{id}/avatar` URL, which itself redirects to the live blob).
- (b) Drop `avatar_url` from `/v1/oauth/userinfo`; consumers hit `/v1/players/{id}/avatar` directly using the `player_id` from the response.

Owner: Thrall (engineer). Recommend (b): `/v1/players/{id}/avatar`
already exists and handles the column complexity; duplicating that
logic in the userinfo response is unnecessary indirection. Blast
radius: same scope as §11.7.

### 11.9 `Cookie::refresh` behavior on unknown realm host

If `parentDomainForHost($host)` returns null (e.g. a misconfigured
traefik route sends an unrecognized Host through), `Cookie::refresh()`
has to do something. Options:

- (a) Default to katforge (current KAT-6 behavior). Defensive. Keeps sessions issuable even on misconfig.
- (b) Hard-fail 500 ("unknown realm host"). Loud. Surfaces misconfig before it can silently emit wrong-domain cookies.

**Khadgar's lean (not ratified):** (b). A cookie emitted with
`Domain=.katforge.com` from a request whose Host was
`auth.darkerdb.com` would land on the wrong subtree and silently
break sessions for the affected realm. A 500 is debuggable; a wrong-
domain cookie is not. Awaiting Anders' confirm or override.

Blast radius: traefik misconfig recovery posture. Either decision is
implementable in one line; what matters is which failure mode we
prefer.

---

## 12. What this doc does NOT cover

- Implementation specifics of the Nuxt app structure (routing, modules,
  state). Owned by Thrall (engineer) in implementation tickets.
- Traefik host-alias config for `auth.*` deployment. Owned by Mekkatorque
  (devops) in KAT-2's deploy ticket.
- Tests for the gateway (E2E journeys 1-4, sibling-isolation invariant).
  Owned by Sylvanas Windrunner (qa).
- Documentation pages on `katforge.dev` describing how a new realm joins.
  Owned by Lorewalker Cho (scribe) post-MVP.
- The three-way CI lint (`REALM_PARENT_DOMAINS` ↔ `@katforge/spark/brands`
  ↔ `ALLOWED_REDIRECT_DOMAINS`), spec'd in §9.1. Spawn ticket under
  KAT-2; assign Mekkatorque.

The above are downstream from this contract; this doc constrains them,
it does not specify them.

---

## 13. Resolved decisions log

### 13.1 R1 (initial contract)

The full §1–§10 body. Decisions documented inline; CTO ratifications
captured in the KAT-2 design conversation.

### 13.2 R2 (KAT-3 reopen, fed by KAT-4/5/6/7 wave 1 findings)

| # | Question | Ruling | Where applied |
|---|---|---|---|
| 1 | Palette token names: §3.2 13-token set vs KAT-4 ticket body 13-token set | **Lock §3.2's list verbatim** (`primary/_hover/_light`, `surface/_raised/_sunken`, `text/_secondary/_muted`, `border`, `success/warning/error`). Contract is authoritative; KAT-4 ticket body is the one that gets updated. | §3.2 (unchanged), §4.2 |
| 2 | Spark helper signatures: `brand(slug)` only vs ticket's `getBrandByHost(host)` | **Keep both** for distinct use cases. `brand(slug)` for callers that already know the slug; `getBrandByHost(host)` for the gateway middleware that resolves from raw `Host` header. Use `undefined` (not `null`) on miss to match Vue/TS idiom. | §3.3 |
| 3 | Realm slug for Gear Goblins: `goblins` vs `geargoblins` | **Canonical slug is `geargoblins`**, matching the project's domain (`geargoblins.com`). Legacy `goblins` in Spark gets renamed under KAT-4. | §9 (`REALM_PARENT_DOMAINS`), §9 slug-naming note, §2.1 host map |
| 4 | Parent-domain shape: implicit `.com` vs explicit (stumper.gg) | **Parent domain is data; code MUST NOT infer `.com`.** `stumper.gg` proves the pattern can't assume TLD. References use `{parent_domain}` and `{auth_host}` placeholders. | §1 glossary, §2.1, §2.2, §5.1 |
| 5 | Nuxt server-route cookie-forwarding (Journey 1) | **Hard requirement on KAT-8.** Nuxt `/api/*` proxies MUST copy upstream `Set-Cookie` headers verbatim onto their outbound response. Called out explicitly in §6.1 so KAT-8 implementer can't miss it. | §6.1 |
| 6 | Per-realm OAuth clients vs shared `katforge-oauth-server` | **Per-realm clients** (`darkerdb-realm`, `stumper-realm`, etc.). Two reasons: per-realm secret rotation, per-realm audience boundary. | §7.4 |
| 7 | Audience storage: column on `OAuthCode` vs static derivation from `client_id` | **Static derivation from `client_id`** via `Constants::REALM_CLIENT_AUDIENCES` map. Tech debt note for when a single client legitimately needs multiple audiences. | §7.3 |
| 8 | CI lint scope: Spark↔PHP only vs three-way including `ALLOWED_REDIRECT_DOMAINS` | **Three-way lint.** All three lists must stay in sync; any drift fails the build. | §9.1 |
| 9 | Cookie `Domain=` leading dot (RFC 6265 §4.1.2.3 deprecation) | **Keep leading dot.** Semantically identical to no-dot in modern browsers; matches `Set-Cookie:` convention and documents subtree-scope intent. | §5.1 |
| 10 | Nuxt service port | **Pinned to `3000`** (Nuxt SSR default). KAT-8 may amend with cause; until then, port `3000` is the contract for all downstream infra tickets. | §2.3 |
| 11 | Aspirational realm enumeration | **All six realms enumerated** in §2.1 host map with MVP-mandatory vs aspirational labels. Code/infra must handle all six; only katforge + darkerdb get end-to-end testing in KAT-2. | §2 (topology diagram), §2.1 |

### 13.3 R2 open questions (CTO call required)

Not pre-decided. Listed in §11.1, §11.6, §11.7, §11.8, §11.9. Plus
the §11.2–§11.5 carry-overs from R1.

| § | Question | Recommended action (not ratified) |
|---|---|---|
| 11.1 | Cookie path-widening compat: which mitigation? | Thrall (engineer) recommends (b) server-side disambiguation. |
| 11.6 | TLS cert sourcing for non-Route53 realm zones | Khadgar (this doc) recommends (a) Route53 zone consolidation if registrar transfer is cheap; else (c) HTTP-01. |
| 11.7 | `email_verified` field: column or drop? | No recommendation. Schema vs contract trade-off, CTO call. |
| 11.8 | `avatar_url` field: projection or drop? | Khadgar recommends (b) drop; consumers use existing `/v1/players/{id}/avatar`. |
| 11.9 | `Cookie::refresh` unknown-host fallback: katforge default or hard-fail? | Khadgar recommends (b) hard-fail 500. Wrong-domain cookies are silent breakage; 500s are debuggable. |
