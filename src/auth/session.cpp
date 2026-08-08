#include <gv/auth/session.h>

#include <gv/core/env_resolver.h>
#include <gv/core/logger.h>

#include <nlohmann/json.hpp>

#ifdef _WIN32
   #include <Windows.h>
#endif

namespace gv::auth {

namespace {

std::optional<std::string> jwt_subject (std::string_view token)
{
   const auto first = token.find ('.');
   if (first == std::string_view::npos) return std::nullopt;
   const auto second = token.find ('.', first + 1);
   if (second == std::string_view::npos) return std::nullopt;

   std::string_view encoded = token.substr (first + 1, second - first - 1);
   std::string decoded;
   decoded.reserve ((encoded.size () * 3) / 4 + 3);
   unsigned int buffer = 0;
   int bits = 0;
   for (const char ch : encoded) {
      int value = -1;
      if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
      else if (ch >= 'a' && ch <= 'z') value = 26 + ch - 'a';
      else if (ch >= '0' && ch <= '9') value = 52 + ch - '0';
      else if (ch == '-' || ch == '+') value = 62;
      else if (ch == '_' || ch == '/') value = 63;
      else if (ch == '=') break;
      else return std::nullopt;

      buffer = (buffer << 6) | static_cast<unsigned int> (value);
      bits += 6;
      if (bits < 8) continue;
      bits -= 8;
      decoded.push_back (static_cast<char> ((buffer >> bits) & 0xff));
      buffer &= bits == 0 ? 0u : (1u << bits) - 1u;
   }

   auto json = nlohmann::json::parse (decoded, nullptr, false);
   if (json.is_discarded () || !json.is_object ()) return std::nullopt;
   auto subject = json.find ("sub");
   if (subject == json.end ()) return std::nullopt;
   if (subject->is_string ()) return subject->get<std::string> ();
   if (subject->is_number_integer ()) return std::to_string (subject->get<std::int64_t> ());
   return std::nullopt;
}

#ifdef _WIN32
class RefreshGuard
{
public:
   RefreshGuard ()
   {
      const auto env = gv::core::active_env ().name;
      const std::wstring name = L"Local\\DarkerDB.GrimVault.TokenRefresh."
         + std::wstring { env.begin (), env.end () };
      handle_ = ::CreateMutexW (nullptr, FALSE, name.c_str ());
      if (!handle_) return;
      const DWORD wait = ::WaitForSingleObject (handle_, 20'000);
      owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
   }

   ~RefreshGuard ()
   {
      if (owned_) ::ReleaseMutex (handle_);
      if (handle_) ::CloseHandle (handle_);
   }

   bool acquired () const noexcept { return owned_; }

private:
   HANDLE handle_ = nullptr;
   bool   owned_ = false;
};
#else
class RefreshGuard
{
public:
   bool acquired () const noexcept { return true; }
};
#endif

} // namespace

Session::Session (std::shared_ptr<OauthClient> oauth)
   : oauth_ (std::move (oauth))
{}

bool Session::signed_in () const
{
   std::lock_guard lk { lock_ };
   if (!cache_loaded_) load_locked ();

   // A signed-out cache can be stale: the CLI is a separate process that
   // writes the shared token store without notifying us. Re-read before
   // reporting signed-out so an external `grimvault login` is picked up.
   if (!cache_.has_value () || cache_->refresh_token.empty ()) load_locked ();

   return cache_.has_value () && !cache_->refresh_token.empty ();
}

std::optional<std::string> Session::principal () const
{
   std::lock_guard lk { lock_ };
   if (!cache_loaded_) load_locked ();
   if (!cache_.has_value () || cache_->access_token.empty ()) return std::nullopt;
   return jwt_subject (cache_->access_token);
}

void Session::reload ()
{
   std::lock_guard lk { lock_ };
   load_locked ();
}

void Session::load_locked () const
{
   cache_loaded_ = true;
   cache_.reset ();

   auto r = TokenStore::load ();
   if (!r.has_value ()) {
      core::log::api.warn ("session: token load failed: {}", r.error ().message);
      return;
   }
   cache_ = *r;
}

core::Result<void> Session::install (const TokenSet& tokens)
{
   auto r = TokenStore::save (tokens);
   if (!r.has_value ()) return r;

   std::lock_guard lk { lock_ };
   cache_        = tokens;
   cache_loaded_ = true;
   force_refresh_ = false;
   return {};
}

core::Result<void> Session::refresh_locked ()
{
   if (!cache_.has_value () || cache_->refresh_token.empty ()) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
         "session: no refresh token"));
   }

   const auto attempted = cache_->refresh_token;

   RefreshGuard refresh_guard;
   if (!refresh_guard.acquired ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "session: timed out waiting for another client to refresh"));
   }

   // Another GrimVault process may have refreshed while this process waited.
   load_locked ();
   if (!cache_.has_value () || cache_->refresh_token.empty ()) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
         "session: tokens were cleared by another client"));
   }
   if (cache_->refresh_token != attempted) {
      force_refresh_ = false;
      return {};
   }

   auto r = oauth_->refresh (attempted);

   if (!r.has_value () && r.error ().kind == core::ErrorKind::Permission) {
      // The CLI runs in a separate process against the same token store and
      // rotates the refresh token on every refresh — our in-memory copy may
      // simply be the superseded one. Re-read the store and retry once with
      // whatever is current before treating this as a real revocation.
      load_locked ();

      if (cache_.has_value () && !cache_->refresh_token.empty ()
            && cache_->refresh_token != attempted) {
         core::log::api.info ("session: refresh token rotated externally, retrying with stored token");
         r = oauth_->refresh (cache_->refresh_token);
      }
   }

   if (!r.has_value ()) {
      // invalid_grant — drop to signed-out.
      if (r.error ().kind == core::ErrorKind::Permission) {
         core::log::api.warn ("session: refresh rejected, clearing tokens");
         (void) TokenStore::clear ();
         cache_.reset ();
         force_refresh_ = false;
      }
      return core::fail (r.error ());
   }

   auto save = TokenStore::save (r->tokens);
   if (!save.has_value ()) return save;

   cache_         = r->tokens;
   force_refresh_ = false;
   return {};
}

core::Result<std::optional<std::string>> Session::access_token ()
{
   std::lock_guard lk { lock_ };
   if (!cache_loaded_) load_locked ();
   if (!cache_.has_value () || cache_->refresh_token.empty ()) {
      return std::optional<std::string> {};
   }

   const auto now = std::chrono::system_clock::now ();
   const bool expiring_soon = (cache_->expires_at - k_refresh_skew) <= now;

   if (force_refresh_ || expiring_soon || cache_->access_token.empty ()) {
      auto rr = refresh_locked ();
      if (!rr.has_value ()) {
         if (rr.error ().kind == core::ErrorKind::Permission) {
            return std::optional<std::string> {};
         }
         return core::fail (rr.error ());
      }
   }

   return std::optional<std::string> { cache_->access_token };
}

void Session::invalidate ()
{
   std::lock_guard lk { lock_ };
   force_refresh_ = true;
}

core::Result<void> Session::sign_out (bool local_only)
{
   std::optional<TokenSet> snap;
   {
      std::lock_guard lk { lock_ };
      if (!cache_loaded_) load_locked ();
      snap = cache_;
   }

   if (!local_only && snap.has_value () && !snap->refresh_token.empty ()) {
      auto r = oauth_->revoke (snap->refresh_token);
      if (!r.has_value ()) {
         core::log::api.warn ("session: server revoke failed: {} (continuing local wipe)",
            r.error ().message);
      }
   }

   auto clr = TokenStore::clear ();

   std::lock_guard lk { lock_ };
   cache_.reset ();
   cache_loaded_ = true;
   force_refresh_ = false;
   return clr;
}

std::optional<TokenSet> Session::snapshot () const
{
   std::lock_guard lk { lock_ };
   if (!cache_loaded_) load_locked ();
   return cache_;
}

} // namespace gv::auth
