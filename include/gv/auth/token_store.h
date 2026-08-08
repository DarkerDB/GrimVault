#pragma once

#include <gv/core/result.h>

#include <chrono>
#include <optional>
#include <string>

namespace gv::auth {

// One-shot token snapshot. The store is the source of truth on disk
// (Windows Credential Manager, DPAPI-protected). Callers either pull a
// fresh snapshot or write a new one; there is no in-place mutation.
struct TokenSet {
   std::string                           access_token;
   std::string                           refresh_token;
   std::string                           scope;
   std::chrono::system_clock::time_point expires_at { };

   bool empty () const noexcept { return access_token.empty () && refresh_token.empty (); }
};

// Windows Credential Manager-backed token store. Credentials are isolated by
// environment. Production also mirrors the legacy "GrimVault:tokens" target
// so V1 and V2 clients can coexist during rollout.
class TokenStore
{
public:
   // Load the stored token blob. Missing = std::nullopt (not an error).
   static core::Result<std::optional<TokenSet>> load ();

   // Atomically replace the stored token blob.
   static core::Result<void> save (const TokenSet& tokens);

   // Clear the stored blob. Idempotent: returns success if nothing was stored.
   static core::Result<void> clear ();
};

} // namespace gv::auth
