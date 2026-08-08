#include <gv/auth/token_store.h>

#include <gv/core/env_resolver.h>
#include <gv/core/logger.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

#ifdef _WIN32
   #include <Windows.h>
   #include <wincred.h>
   #pragma comment (lib, "Advapi32.lib")
#endif

namespace gv::auth {

namespace {

   constexpr const wchar_t* k_legacy_target = L"GrimVault:tokens";

   std::wstring target_name ()
   {
      const auto env = gv::core::active_env ().name;
      return L"GrimVault:tokens:" + std::wstring { env.begin (), env.end () };
   }

   nlohmann::json to_json (const TokenSet& t)
   {
      const auto exp = std::chrono::duration_cast<std::chrono::seconds> (
         t.expires_at.time_since_epoch ()).count ();

      return nlohmann::json {
         { "access_token",  t.access_token  },
         { "refresh_token", t.refresh_token },
         { "scope",         t.scope         },
         { "expires_at",    exp             },
      };
   }

   TokenSet from_json (const nlohmann::json& j)
   {
      TokenSet t;
      t.access_token  = j.value ("access_token",  "");
      t.refresh_token = j.value ("refresh_token", "");
      t.scope         = j.value ("scope",         "");
      const auto exp  = j.value ("expires_at",    static_cast<std::int64_t> (0));
      t.expires_at = std::chrono::system_clock::time_point {
         std::chrono::seconds { exp } };
      return t;
   }

} // namespace

#ifdef _WIN32

namespace {

core::Result<std::optional<TokenSet>> read_named (const wchar_t* target)
{
   PCREDENTIALW cred = nullptr;
   if (!::CredReadW (target, CRED_TYPE_GENERIC, 0, &cred)) {
      const auto err = ::GetLastError ();
      if (err == ERROR_NOT_FOUND) return std::optional<TokenSet> {};
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "token_store: CredRead failed: {}", err));
   }

   std::string buf;
   if (cred->CredentialBlob && cred->CredentialBlobSize > 0) {
      buf.assign (reinterpret_cast<const char*> (cred->CredentialBlob),
                  cred->CredentialBlobSize);
   }
   ::CredFree (cred);

   auto json = nlohmann::json::parse (buf, nullptr, false);
   if (json.is_discarded () || !json.is_object ()) {
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "token_store: stored blob is not JSON"));
   }

   return std::optional<TokenSet> { from_json (json) };
}

core::Result<void> write_named (const wchar_t* target, const TokenSet& tokens)
{
   const std::string blob = to_json (tokens).dump ();

   CREDENTIALW cred {};
   cred.Type            = CRED_TYPE_GENERIC;
   cred.TargetName      = const_cast<LPWSTR> (target);
   cred.CredentialBlob  = reinterpret_cast<LPBYTE> (const_cast<char*> (blob.data ()));
   cred.CredentialBlobSize = static_cast<DWORD> (blob.size ());
   cred.Persist         = CRED_PERSIST_LOCAL_MACHINE;

   if (!::CredWriteW (&cred, 0)) {
      const auto err = ::GetLastError ();
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "token_store: CredWrite failed: {}", err));
   }
   return {};
}

core::Result<void> clear_named (const wchar_t* target)
{
   if (!::CredDeleteW (target, CRED_TYPE_GENERIC, 0)) {
      const auto err = ::GetLastError ();
      if (err == ERROR_NOT_FOUND) return {};
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "token_store: CredDelete failed: {}", err));
   }
   return {};
}

} // namespace

core::Result<std::optional<TokenSet>> TokenStore::load ()
{
   const auto target = target_name ();
   auto current = read_named (target.c_str ());
   if (gv::core::active_env ().name != "prod") return current;

   auto legacy = read_named (k_legacy_target);
   if (!legacy.has_value ()) return current.has_value () ? current : legacy;
   if (!legacy->has_value ()) return current;
   if (!current.has_value () || !current->has_value ()
       || (*legacy)->expires_at > (**current).expires_at) {
      auto migrated = write_named (target.c_str (), **legacy);
      if (!migrated.has_value ()) return core::fail (migrated.error ());
      core::log::api.info ("token_store: synchronized legacy production credential");
      return legacy;
   }
   return current;
}

core::Result<void> TokenStore::save (const TokenSet& tokens)
{
   const auto target = target_name ();
   auto saved = write_named (target.c_str (), tokens);
   if (!saved.has_value ()) return saved;

   if (gv::core::active_env ().name == "prod") {
      auto legacy = write_named (k_legacy_target, tokens);
      if (!legacy.has_value ()) return legacy;
   }
   core::log::api.info ("token_store: wrote credential blob");
   return {};
}

core::Result<void> TokenStore::clear ()
{
   const auto target = target_name ();
   auto cleared = clear_named (target.c_str ());
   if (!cleared.has_value ()) return cleared;

   if (gv::core::active_env ().name == "prod") {
      auto legacy = clear_named (k_legacy_target);
      if (!legacy.has_value ()) return legacy;
   }
   core::log::api.info ("token_store: cleared credential blob");
   return {};
}

#else  // !_WIN32 — stub so the source still configures on non-Windows hosts

core::Result<std::optional<TokenSet>> TokenStore::load ()
{
   return std::optional<TokenSet> {};
}

core::Result<void> TokenStore::save (const TokenSet&)
{
   return core::fail (core::Error::make (core::ErrorKind::Io,
      "token_store: only supported on Windows"));
}

core::Result<void> TokenStore::clear ()
{
   return {};
}

#endif

} // namespace gv::auth
