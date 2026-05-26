#include <gv/cli/cli.h>

#include <gv/api/darkerdb_client.h>
#include <gv/auth/http.h>
#include <gv/auth/oauth_client.h>
#include <gv/auth/session.h>
#include <gv/auth/token_store.h>
#include <gv/core/env.h>
#include <gv/core/logger.h>
#include <gv/core/version.h>

#include <QCoreApplication>
#include <QStandardPaths>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gv::cli {

namespace {

   constexpr const char* k_check = "\xE2\x9C\x93";   // ✓
   constexpr const char* k_cross = "\xE2\x9C\x97";   // ✗

   std::filesystem::path cli_log_dir ()
   {
      auto qpath = QStandardPaths::writableLocation (QStandardPaths::AppDataLocation);
      const std::filesystem::path p { qpath.toStdWString () };
      std::filesystem::create_directories (p / "logs");
      return p / "logs";
   }

   std::shared_ptr<auth::OauthClient> make_oauth ()
   {
      auth::OauthClient::Config cfg;
      cfg.client_id    = core::client_id;
      cfg.api_base_url = core::api_base_url;
      cfg.spa_base_url = core::spa_base_url;
      return std::make_shared<auth::OauthClient> (std::move (cfg));
   }

   std::string iso_local (std::chrono::system_clock::time_point tp)
   {
      const auto tt = std::chrono::system_clock::to_time_t (tp);
      std::tm tm {};
#ifdef _WIN32
      localtime_s (&tm, &tt);
#else
      localtime_r (&tt, &tm);
#endif
      char buf [32];
      std::strftime (buf, sizeof (buf), "%Y-%m-%d %H:%M:%S", &tm);
      return buf;
   }

   bool has_flag (const std::vector<std::string>& args, std::string_view name)
   {
      return std::find (args.begin (), args.end (), std::string { name }) != args.end ();
   }

   int cmd_login (const std::vector<std::string>& args)
   {
      const bool no_browser = has_flag (args, "--no-browser");

      auto oauth = make_oauth ();
      if (no_browser) {
         oauth->set_browser_hook ([] (const std::string& url) {
            std::cout << "Open this URL in your browser to sign in:\n  "
                      << url << "\n";
            return false;
         });
      } else {
         std::cout << "Opening your browser to " << core::spa_base_url
                   << " ...\n";
      }

      auto resp = oauth->authorize ();
      if (!resp.has_value ()) {
         std::cerr << "login failed: " << resp.error ().message << "\n";
         return k_error_auth;
      }

      auth::Session session { oauth };
      auto inst = session.install (resp->tokens);
      if (!inst.has_value ()) {
         std::cerr << "login: token store failed: " << inst.error ().message << "\n";
         return k_error_generic;
      }

      std::cout << "Signed in. Tokens stored in Windows Credential Manager.\n";
      return k_ok;
   }

   int cmd_logout (const std::vector<std::string>& args)
   {
      const bool local_only = has_flag (args, "--local-only");

      auto oauth = make_oauth ();
      auth::Session session { oauth };

      if (!session.signed_in ()) {
         std::cout << "Not signed in. Nothing to do.\n";
         return k_ok;
      }

      auto r = session.sign_out (local_only);
      if (!r.has_value ()) {
         std::cerr << "logout: " << r.error ().message << "\n";
         return k_error_generic;
      }
      std::cout << (local_only
         ? "Local tokens cleared (server-side revocation skipped).\n"
         : "Signed out. Tokens cleared and refresh token revoked.\n");
      return k_ok;
   }

   int cmd_status (const std::vector<std::string>& /*args*/)
   {
      auto oauth = make_oauth ();
      auth::Session session { oauth };

      std::cout << "env:           " << core::env          << "\n";
      std::cout << "api_base_url:  " << core::api_base_url << "\n";
      std::cout << "client_id:     " << core::client_id    << "\n";

      const auto snap = session.snapshot ();
      if (!snap.has_value () || snap->refresh_token.empty ()) {
         std::cout << "signed_in:     no\n";
         return k_error_auth;
      }
      std::cout << "signed_in:     yes\n";
      std::cout << "access_expiry: " << iso_local (snap->expires_at) << "\n";
      std::cout << "scope:         " << snap->scope << "\n";

      // Try a live ping to confirm the token actually works.
      gv::api::DarkerDbClient::Config api_cfg;
      api_cfg.base_url  = core::api_base_url;
      api_cfg.client_id = core::client_id;
      gv::api::DarkerDbClient api { api_cfg, &session };
      auto p = api.ping ();
      if (!p.has_value ()) {
         std::cout << "ping:          FAILED (" << p.error ().message << ")\n";
         return k_error_service;
      }
      std::cout << "player_id:     " << p->player_id << "\n";
      if (!p->user_id.empty ()) {
         std::cout << "user_id:       " << p->user_id << "\n";
      }
      std::cout << "server_time:   " << p->server_time << "\n";
      return k_ok;
   }

   int cmd_ping (const std::vector<std::string>& /*args*/)
   {
      auto oauth = make_oauth ();
      auth::Session session { oauth };
      if (!session.signed_in ()) {
         std::cerr << "ping: not signed in. Run `grimvault login` first.\n";
         return k_error_auth;
      }

      gv::api::DarkerDbClient::Config api_cfg;
      api_cfg.base_url  = core::api_base_url;
      api_cfg.client_id = core::client_id;
      gv::api::DarkerDbClient api { api_cfg, &session };

      auto p = api.ping ();
      if (!p.has_value ()) {
         std::cerr << "ping: " << p.error ().message << "\n";
         return k_error_service;
      }
      std::cout << p->raw.dump (3) << "\n";
      return k_ok;
   }

   int cmd_settings_get (const std::vector<std::string>& /*args*/)
   {
      auto oauth = make_oauth ();
      auth::Session session { oauth };
      if (!session.signed_in ()) {
         std::cerr << "settings: not signed in\n";
         return k_error_auth;
      }

      auto tok = session.access_token ();
      if (!tok.has_value () || !tok->has_value ()) {
         std::cerr << "settings: no usable access token\n";
         return k_error_auth;
      }

      auth::http::Request req;
      req.method  = "GET";
      req.url     = std::string { core::api_base_url } + "/v2/grimvault/settings";
      req.headers = { { "Authorization", "Bearer " + **tok } };

      auto res = auth::http::perform (req);
      if (!res.has_value ()) {
         std::cerr << "settings: " << res.error ().message << "\n";
         return k_error_service;
      }
      if (res->status < 200 || res->status >= 300) {
         std::cerr << "settings: HTTP " << res->status << ": "
                   << res->body.substr (0, 200) << "\n";
         return k_error_service;
      }

      auto json = nlohmann::json::parse (res->body, nullptr, false);
      if (json.is_discarded () || !json.is_object ()) {
         std::cerr << "settings: response is not JSON object\n";
         return k_error_generic;
      }

      const auto& body = json.contains ("body") ? json ["body"] : json;
      if (!body.is_object ()) {
         std::cout << json.dump (3) << "\n";
         return k_ok;
      }

      std::cout << "key" << std::string (28, ' ') << "value\n";
      std::cout << std::string (60, '-') << "\n";
      for (auto it = body.begin (); it != body.end (); ++it) {
         std::string key = it.key ();
         if (key.size () < 30) key.append (30 - key.size (), ' ');
         std::cout << key << it.value ().dump () << "\n";
      }
      return k_ok;
   }

   int cmd_logs (const std::vector<std::string>& /*args*/)
   {
      // MVP: print the log file path. tail/--follow deferred per scope brief.
      const auto path = cli_log_dir () / "grimvault.log";
      std::cout << path.string () << "\n";
      return k_ok;
   }

   void doctor_step (const std::string& label, bool ok, const std::string& detail)
   {
      std::cout << "  " << (ok ? k_check : k_cross) << " " << label;
      if (!detail.empty ()) std::cout << " — " << detail;
      std::cout << "\n";
   }

   int cmd_doctor (const std::vector<std::string>& /*args*/)
   {
      std::cout << "GrimVault doctor (" << core::env << ")\n";
      std::cout << "  api: " << core::api_base_url << "\n\n";

      int failures = 0;

      // 1. Auth credentials present
      auto raw_tokens = auth::TokenStore::load ();
      const bool tokens_loaded = raw_tokens.has_value () && raw_tokens->has_value ();
      doctor_step ("tokens present in Windows Credential Manager", tokens_loaded,
         tokens_loaded ? "" : "run `grimvault login`");
      if (!tokens_loaded) failures++;

      // 2. JWKS endpoint reachable (TLS + HTTP both verified by one request)
      {
         auth::http::Request req;
         req.method = "GET";
         req.url    = std::string { core::api_base_url } + "/.well-known/jwks.json";
         auto res = auth::http::perform (req);
         const bool ok = res.has_value () && res->status == 200;
         std::string detail;
         if (!res.has_value ()) {
            detail = res.error ().message;
            failures++;
         } else if (res->status != 200) {
            detail = "HTTP " + std::to_string (res->status);
            failures++;
         }
         doctor_step ("JWKS endpoint reachable (TLS + 200)", ok, detail);
      }

      // 3. /v2/grimvault/ping (auth probe)
      if (tokens_loaded) {
         auto oauth = make_oauth ();
         auth::Session session { oauth };
         gv::api::DarkerDbClient::Config api_cfg;
         api_cfg.base_url  = core::api_base_url;
         api_cfg.client_id = core::client_id;
         gv::api::DarkerDbClient api { api_cfg, &session };

         auto p = api.ping ();
         const bool ok = p.has_value () && p->ok && !p->player_id.empty ();
         std::string detail;
         if (!p.has_value ()) {
            detail = p.error ().message;
            failures++;
         } else if (!ok) {
            detail = "ping returned ok=false or empty player_id";
            failures++;
         } else {
            detail = "player_id=" + p->player_id +
               (p->user_id.empty () ? "" : ", user_id=" + p->user_id);
         }
         doctor_step ("/v2/grimvault/ping returns 200 with user", ok, detail);
      } else {
         doctor_step ("/v2/grimvault/ping returns 200 with user", false, "skipped (no tokens)");
      }

      std::cout << "\n" << (failures == 0 ? "All checks passed." :
         std::to_string (failures) + " check(s) failed.") << "\n";
      return failures == 0 ? k_ok : k_error_generic;
   }

   void print_usage ()
   {
      std::cout <<
         "grimvault " << gv::core::version::string << " (env: " << core::env << ")\n"
         "\n"
         "Usage: grimvault <command> [flags]\n"
         "\n"
         "Commands:\n"
         "  login             Sign in to DarkerDB via the browser.\n"
         "  logout            Revoke the refresh token and clear local credentials.\n"
         "  status            Print env, signed-in user, and access-token expiry.\n"
         "  ping              POST /v2/grimvault/ping and dump the response.\n"
         "  settings get      GET /v2/grimvault/settings and print as a table.\n"
         "  logs              Print the path of the log file.\n"
         "  doctor            Run end-to-end diagnostics.\n"
         "\n"
         "Run with no args from a window manager / Explorer to start GUI (tray) mode.\n";
   }

} // namespace

int run (const std::vector<std::string>& args)
{
   // Ensure core/version constants are linked. Touch one in this TU.
   (void) gv::core::version::string;

   if (args.empty () || args [0] == "-h" || args [0] == "--help" || args [0] == "help") {
      print_usage ();
      return k_ok;
   }

   const auto& cmd = args [0];
   const std::vector<std::string> rest { args.begin () + 1, args.end () };

   if (cmd == "login")    return cmd_login    (rest);
   if (cmd == "logout")   return cmd_logout   (rest);
   if (cmd == "status")   return cmd_status   (rest);
   if (cmd == "ping")     return cmd_ping     (rest);
   if (cmd == "logs")     return cmd_logs     (rest);
   if (cmd == "doctor")   return cmd_doctor   (rest);
   if (cmd == "settings") {
      if (rest.empty () || rest [0] != "get") {
         std::cerr << "settings: only `get` is implemented in MVP\n";
         return k_error_usage;
      }
      return cmd_settings_get ({ rest.begin () + 1, rest.end () });
   }

   std::cerr << "unknown command: " << cmd << "\n";
   print_usage ();
   return k_error_usage;
}

} // namespace gv::cli
