#pragma once

#include <gv/auth/oauth_client.h>
#include <gv/auth/token_store.h>
#include <gv/core/result.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace gv::auth {

// In-process token lifecycle owner. Wraps the on-disk TokenStore +
// OauthClient so the rest of the binary asks one question:
//
//    auto token = session.access_token ();   // refreshes as needed
//
// Refreshes:
//    - Proactively when expiry within k_refresh_skew of now.
//    - Lazily after a 401 — caller re-tries the request and gets a fresh
//      token on the second access_token () call after invalidate ().
class Session
{
public:
   // 60-second skew, matching contract §3.2 step 10.
   static constexpr std::chrono::seconds k_refresh_skew { 60 };

   explicit Session (std::shared_ptr<OauthClient> oauth);

   // True if a refresh token is stored on disk (caller may still need to
   // refresh; this is just "is the user signed in?").
   bool signed_in () const;

   // Returns a fresh access token, refreshing on the fly if needed. On
   // refresh failure (invalid_grant) the session drops to signed-out and
   // returns nullopt.
   core::Result<std::optional<std::string>> access_token ();

   // Force a refresh on the next access_token () call. Called by API
   // clients after a 401.
   void invalidate ();

   // Drop everything: revoke server-side, wipe keychain, clear in-memory.
   core::Result<void> sign_out (bool local_only = false);

   // Replace the stored token set wholesale (used after a successful
   // interactive `OauthClient::authorize ()` call).
   core::Result<void> install (const TokenSet& tokens);

   // Snapshot of the current on-disk state (for `grimvault status` etc.).
   std::optional<TokenSet> snapshot () const;

private:
   std::shared_ptr<OauthClient> oauth_;
   mutable std::mutex           lock_;
   mutable std::optional<TokenSet> cache_;
   mutable bool                 cache_loaded_ = false;
   bool                         force_refresh_ = false;

   void load_locked () const;
   core::Result<void> refresh_locked ();
};

} // namespace gv::auth
