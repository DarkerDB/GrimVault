#pragma once

#include <gv/auth/token_store.h>
#include <gv/core/result.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace gv::auth {

// Outcome of a non-interactive token call. Returned by exchange_code +
// refresh; on success the caller writes `tokens` to the TokenStore.
struct TokenResponse {
   TokenSet tokens;
};

// Authorization Code + PKCE + loopback OAuth flow per contract §3.
//
// The client orchestrates:
//    1. PKCE + state generation
//    2. Loopback HTTP listener bind on 127.0.0.1:0
//    3. Browser launch to ${spa_base_url}/oauth/authorize
//    4. /callback validation + branded close page
//    5. Code exchange against ${api_base_url}/oauth/token
//
// All long-running work is synchronous; callers run it on a worker thread.
class OauthClient
{
public:
   struct Config {
      std::string client_id;     // single OAuth client across all envs: "grimvault"
      std::string api_base_url;  // e.g. "https://api.dev.darkerdb.com"
      std::string spa_base_url;  // e.g. "https://dev.darkerdb.com"
      std::string scope        = "grimvault.read grimvault.write";
      std::chrono::seconds callback_timeout { 120 };
   };

   explicit OauthClient (Config cfg);
   ~OauthClient ();

   // Hook fired with the fully-formed authorize URL just before the browser
   // launch. Set if the caller wants to print it (CLI --no-browser) or
   // suppress the default launcher (return false).
   using BrowserHook = std::function<bool (const std::string& url)>;
   void set_browser_hook (BrowserHook hook);

   // Run the full interactive flow. Blocks until the user hits the callback,
   // the user cancels, or `callback_timeout` elapses.
   core::Result<TokenResponse> authorize ();

   // Refresh an existing refresh token. Per contract §3.3, the response MUST
   // include a new refresh token (rotation is mandatory); callers persist the
   // returned blob atomically.
   core::Result<TokenResponse> refresh (std::string_view refresh_token);

   // RFC 7009 token revocation. Used by `logout`.
   core::Result<void> revoke (std::string_view refresh_token);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::auth
