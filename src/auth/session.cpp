#include <gv/auth/session.h>

#include <gv/core/logger.h>

namespace gv::auth {

Session::Session (std::shared_ptr<OauthClient> oauth)
   : oauth_ (std::move (oauth))
{}

bool Session::signed_in () const
{
   std::lock_guard lk { lock_ };
   if (!cache_loaded_) load_locked ();
   return cache_.has_value () && !cache_->refresh_token.empty ();
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

   auto r = oauth_->refresh (cache_->refresh_token);
   if (!r.has_value ()) {
      // invalid_grant — drop to signed-out.
      if (r.error ().kind == core::ErrorKind::Permission) {
         core::log::api.warn ("session: refresh rejected, clearing tokens");
         TokenStore::clear ();
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
