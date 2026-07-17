#include <gv/cli/cli.h>

#include <gv/api/darkerdb_client.h>
#include <gv/auth/http.h>
#include <gv/auth/oauth_client.h>
#include <gv/auth/session.h>
#include <gv/auth/token_store.h>
#include <gv/core/env.h>
#include <gv/core/env_resolver.h>
#include <gv/core/logger.h>
#include <gv/core/version.h>
#include <gv/db/database.h>
#include <gv/db/repos/user_settings_repo.h>

#include <QCoreApplication>
#include <QStandardPaths>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gv::cli {

namespace {

   constexpr const char* k_check = "\xE2\x9C\x93";   // ✓
   constexpr const char* k_cross = "\xE2\x9C\x97";   // ✗

   std::filesystem::path app_data_dir ()
   {
      auto qpath = QStandardPaths::writableLocation (QStandardPaths::GenericDataLocation)
         + QStringLiteral ("/GrimVault");
      return std::filesystem::path { qpath.toStdWString () };
   }

   std::filesystem::path cli_log_dir ()
   {
      const auto p = app_data_dir ();
      std::filesystem::create_directories (p / "logs");
      return p / "logs";
   }

   std::shared_ptr<auth::OauthClient> make_oauth ()
   {
      const auto& e = core::active_env ();
      auth::OauthClient::Config cfg;
      cfg.client_id     = std::string { e.client_id };
      cfg.api_base_url  = std::string { e.api_base_url };
      cfg.auth_base_url = std::string { e.auth_base_url };
      cfg.spa_base_url  = std::string { e.spa_base_url };
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
         std::cout << "Opening your browser to "
                   << core::active_env ().auth_base_url << " ...\n";
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
      const auto& e = core::active_env ();
      auto oauth = make_oauth ();
      auth::Session session { oauth };

      std::cout << "env:           " << e.name          << "\n";
      std::cout << "api_base_url:  " << e.api_base_url  << "\n";
      std::cout << "auth_base_url: " << e.auth_base_url << "\n";
      std::cout << "client_id:     " << e.client_id     << "\n";

      const auto snap = session.snapshot ();
      if (!snap.has_value () || snap->refresh_token.empty ()) {
         std::cout << "signed_in:     no\n";
         return k_error_auth;
      }
      std::cout << "signed_in:     yes\n";
      std::cout << "access_expiry: " << iso_local (snap->expires_at) << "\n";
      std::cout << "scope:         " << snap->scope << "\n";

      // Try a live ping to confirm the token actually works.
      gv::api::DDBClient::Config api_cfg;
      api_cfg.base_url  = std::string { e.api_base_url };
      api_cfg.client_id = std::string { e.client_id };
      gv::api::DDBClient api { api_cfg, &session };
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
      const auto& e = core::active_env ();
      auto oauth = make_oauth ();
      auth::Session session { oauth };
      if (!session.signed_in ()) {
         std::cerr << "ping: not signed in. Run `grimvault login` first.\n";
         return k_error_auth;
      }

      gv::api::DDBClient::Config api_cfg;
      api_cfg.base_url  = std::string { e.api_base_url };
      api_cfg.client_id = std::string { e.client_id };
      gv::api::DDBClient api { api_cfg, &session };

      auto p = api.ping ();
      if (!p.has_value ()) {
         std::cerr << "ping: " << p.error ().message << "\n";
         return k_error_service;
      }
      std::cout << p->raw.dump (3) << "\n";
      return k_ok;
   }

   void print_kv_table (const std::map<std::string, std::string>& sorted)
   {
      // Group by colon-namespace prefix (overlay:opacity → "overlay"), print
      // each group with a header so a 17-row dump reads as five sections,
      // not a wall of keys.
      std::cout << "key" << std::string (38, ' ') << "value\n";
      std::cout << std::string (70, '-') << "\n";

      std::string current_group;
      for (const auto& [k, v] : sorted) {
         const auto colon = k.find (':');
         const std::string group = colon == std::string::npos
            ? std::string { "(ungrouped)" }
            : k.substr (0, colon);

         if (group != current_group) {
            if (!current_group.empty ()) std::cout << "\n";
            current_group = group;
         }

         std::string key = k;
         if (key.size () < 40) key.append (40 - key.size (), ' ');
         std::cout << key << v << "\n";
      }
   }

   // `grimvault settings` — print every key/value the running addon has
   // synced into its local UserSettingsRepo. Source of truth for what the
   // overlay actually reads at runtime (the server's view is one poll
   // away; this is the post-poll state).
   //
   // The DB lives in %APPDATA%\GrimVault\grimvault.db. If it doesn't
   // exist, the GUI has never run on this machine and there's nothing
   // to print — say so rather than silently creating an empty DB.
   int cmd_settings_local (const std::vector<std::string>& /*args*/)
   {
      const auto db_path = app_data_dir () / "grimvault.db";

      std::error_code ec;
      if (!std::filesystem::exists (db_path, ec)) {
         std::cerr << "settings: no local database at " << db_path.string () << "\n";
         std::cerr << "          run GrimVault once (sign in) so SettingsSync populates it.\n";
         return k_error_generic;
      }

      auto db = gv::db::Database::open (db_path);
      if (!db.has_value ()) {
         std::cerr << "settings: open failed: " << db.error ().message << "\n";
         return k_error_generic;
      }

      gv::db::UserSettingsRepo repo { **db };
      auto all = repo.all ();
      if (!all.has_value ()) {
         std::cerr << "settings: read failed: " << all.error ().message << "\n";
         return k_error_generic;
      }

      if (all->empty ()) {
         std::cout << "No settings stored locally yet. Sign in and let SettingsSync run.\n";
         return k_ok;
      }

      // Sort for deterministic, grouped output.
      const std::map<std::string, std::string> sorted (all->begin (), all->end ());
      print_kv_table (sorted);
      std::cout << "\n(" << sorted.size () << " keys, from " << db_path.string () << ")\n";
      return k_ok;
   }

   // `grimvault settings get` — one-shot read from /v2/grimvault/settings
   // (server's view). Distinct from `settings` (local DB) because the two
   // can diverge between SettingsSync polls, and during debugging it
   // matters which side you're inspecting.
   int cmd_settings_get (const std::vector<std::string>& /*args*/)
   {
      const auto& e = core::active_env ();
      auto oauth = make_oauth ();
      auth::Session session { oauth };
      if (!session.signed_in ()) {
         std::cerr << "settings get: not signed in\n";
         return k_error_auth;
      }

      gv::api::DDBClient::Config api_cfg;
      api_cfg.base_url  = std::string { e.api_base_url };
      api_cfg.client_id = std::string { e.client_id };
      gv::api::DDBClient api { api_cfg, &session };

      auto bundle = api.get_settings ();
      if (!bundle.has_value ()) {
         std::cerr << "settings get: " << bundle.error ().message << "\n";
         return k_error_service;
      }

      if (bundle->values.empty ()) {
         std::cout << bundle->raw.dump (3) << "\n";
         return k_ok;
      }

      const std::map<std::string, std::string> sorted (
         bundle->values.begin (), bundle->values.end ());
      print_kv_table (sorted);
      return k_ok;
   }

   int cmd_logs (const std::vector<std::string>& /*args*/)
   {
      const auto dir = cli_log_dir ();
      std::filesystem::path newest;
      std::filesystem::file_time_type newest_time {};
      std::error_code ec;
      for (std::filesystem::directory_iterator it { dir, ec }, end;
           !ec && it != end; it.increment (ec)) {
         const auto name = it->path ().filename ().string ();
         if (!it->is_regular_file () || !name.starts_with ("grimvault")
             || it->path ().extension () != ".txt") continue;
         const auto time = it->last_write_time (ec);
         if (!ec && (newest.empty () || time > newest_time)) {
            newest = it->path ();
            newest_time = time;
         }
      }
      std::cout << (newest.empty () ? dir : newest).string () << "\n";
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
      const auto& e = core::active_env ();
      std::cout << "GrimVault doctor (" << e.name << ")\n";
      std::cout << "  api:  " << e.api_base_url  << "\n";
      std::cout << "  auth: " << e.auth_base_url << "\n\n";

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
         req.url    = std::string { e.api_base_url } + "/.well-known/jwks.json";
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
         gv::api::DDBClient::Config api_cfg;
         api_cfg.base_url  = std::string { e.api_base_url };
         api_cfg.client_id = std::string { e.client_id };
         gv::api::DDBClient api { api_cfg, &session };

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
         "grimvault " << gv::core::version::string
            << " (env: " << core::active_env ().name << ")\n"
         "\n"
         "Usage: grimvault [--env <dev|qa|prod>] <command> [flags]\n"
         "\n"
         "Commands:\n"
         "  login             Sign in to DDB via the browser.\n"
         "  logout            Revoke the refresh token and clear local credentials.\n"
         "  status            Print env, signed-in user, and access-token expiry.\n"
         "  whoami            Alias of status.\n"
         "  ping              POST /v2/grimvault/ping and dump the response.\n"
         "  settings          Print all locally synced settings (the addon's view).\n"
         "  settings get      GET /v2/grimvault/settings and print the server's view.\n"
         "  logs              Print the path of the log file.\n"
         "  doctor            Run end-to-end diagnostics.\n"
         "\n"
         "Global flags:\n"
         "  --env <name>      Override the active env (dev | qa | prod). Also\n"
         "                    settable via GRIMVAULT_ENV.\n"
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
   if (cmd == "whoami")   return cmd_status   (rest);
   if (cmd == "ping")     return cmd_ping     (rest);
   if (cmd == "logs")     return cmd_logs     (rest);
   if (cmd == "doctor")   return cmd_doctor   (rest);
   if (cmd == "settings") {
      if (rest.empty ()) {
         return cmd_settings_local (rest);
      }
      if (rest [0] == "get") {
         return cmd_settings_get ({ rest.begin () + 1, rest.end () });
      }
      std::cerr << "settings: unknown subcommand `" << rest [0]
                << "` (try `settings` or `settings get`)\n";
      return k_error_usage;
   }

   std::cerr << "unknown command: " << cmd << "\n";
   print_usage ();
   return k_error_usage;
}

} // namespace gv::cli
