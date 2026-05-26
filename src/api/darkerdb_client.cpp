#include <gv/api/darkerdb_client.h>

#include <gv/auth/session.h>
#include <gv/core/logger.h>
#include <gv/core/version.h>
#include <gv/db/database.h>

#include <SQLiteCpp/SQLiteCpp.h>
#include <curl/curl.h>

#ifdef _WIN32
   #include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace gv::api {

namespace {

   constexpr std::array<int, 3> k_retry_delays_ms { 200, 500, 1500 };

   std::size_t write_cb (char* ptr, std::size_t size, std::size_t nmemb, void* user)
   {
      const std::size_t n = size * nmemb;
      auto* out = static_cast<std::string*> (user);
      out->append (ptr, n);
      return n;
   }

   void apply_tls (CURL* curl, const std::string& ca_bundle)
   {
      curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST, 2L);
      if (!ca_bundle.empty ()) {
         curl_easy_setopt (curl, CURLOPT_CAINFO, ca_bundle.c_str ());
      }
   }

   bool retryable_http_status (long s)
   {
      return s == 502 || s == 503 || s == 504;
   }

   void parse_pricing (const nlohmann::json& j, Pricing& p)
   {
      p.currency    = j.value ("currency",    std::string { "gold" });
      p.low         = j.value ("low",         static_cast<std::int64_t> (0));
      p.median      = j.value ("median",      static_cast<std::int64_t> (0));
      p.high        = j.value ("high",        static_cast<std::int64_t> (0));
      p.sample_size = j.value ("sample_size", static_cast<std::int64_t> (0));
      p.ttl_seconds = j.value ("ttl_seconds", 0);
      p.as_of       = j.value ("as_of",       std::string {});
      p.market      = p.median;
      p.raw         = j;
   }

   void parse_attrs (const nlohmann::json& arr, std::vector<TooltipAttribute>& out)
   {
      if (!arr.is_array ()) return;
      out.reserve (arr.size ());
      for (const auto& a : arr) {
         if (!a.is_object ()) continue;
         out.push_back (TooltipAttribute {
            .label = a.value ("label", ""),
            .value = a.value ("value", ""),
         });
      }
   }

   // Symfony envelope wraps the contract body under "body"; tolerate either
   // shape so unwrapped fixtures still parse.
   const nlohmann::json& body_of (const nlohmann::json& j)
   {
      if (j.is_object ()) {
         if (auto it = j.find ("body"); it != j.end ()) return *it;
      }
      return j;
   }

   TooltipLookup parse_lookup (nlohmann::json j)
   {
      TooltipLookup out;
      const auto& body = body_of (j);

      if (body.is_object ()) {
         if (auto it = body.find ("item"); it != body.end () && it->is_object ()) {
            out.canonical_name = it->value ("canonical_name", "");
            out.rarity         = it->value ("rarity",         "");
            parse_attrs ((*it) ["primary"],   out.primary);
            parse_attrs ((*it) ["secondary"], out.secondary);
            parse_attrs ((*it) ["details"],   out.details);
         }
         if (auto it = body.find ("pricing"); it != body.end ()) {
            parse_pricing (*it, out.pricing);
         }
         out.request_id = body.value ("request_id", "");
      }

      out.raw = std::move (j);
      return out;
   }

   PingResult parse_ping (nlohmann::json j)
   {
      PingResult out;
      const auto& body = body_of (j);
      if (body.is_object ()) {
         out.ok          = body.value ("ok",          false);
         out.player_id   = body.value ("player_id",   "");
         out.user_id     = body.value ("user_id",     "");
         out.env         = body.value ("env",         "");
         out.server_time = body.value ("server_time", "");
         out.request_id  = body.value ("request_id",  "");
      }
      out.raw = std::move (j);
      return out;
   }

} // namespace

void DarkerDbClient::global_init    () { curl_global_init    (CURL_GLOBAL_DEFAULT); }
void DarkerDbClient::global_cleanup () { curl_global_cleanup (); }

struct DarkerDbClient::Impl
{
   Config             cfg;
   gv::auth::Session* session  = nullptr;
   gv::db::Database*  cache_db = nullptr;

   struct Req {
      std::string_view  method;       // "GET" or "POST"
      std::string       url;
      std::string       body;
      bool              retryable     = true;
      bool              authenticated = true;
   };

   struct Res {
      long                       status   = 0;
      std::string                body;
      std::chrono::milliseconds  elapsed { 0 };
   };

   core::Result<Res> http_once (const Req& req, const std::string& bearer) const
   {
      CURL* curl = curl_easy_init ();
      if (!curl) {
         return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
            "darkerdb: curl_easy_init failed"));
      }

      curl_slist* headers = nullptr;
      headers = curl_slist_append (headers, ("User-Agent: " + cfg.user_agent).c_str ());
      headers = curl_slist_append (headers, ("X-Client-Id: " + cfg.client_id).c_str ());
      headers = curl_slist_append (headers,
         ("X-Client-Version: " + std::string { gv::core::version::string }).c_str ());
      if (!bearer.empty ()) {
         headers = curl_slist_append (headers, ("Authorization: Bearer " + bearer).c_str ());
      }
      headers = curl_slist_append (headers, "Accept: application/json");
      if (!req.body.empty ()) {
         headers = curl_slist_append (headers, "Content-Type: application/json");
      }

      Res res;
      curl_easy_setopt (curl, CURLOPT_URL,               req.url.c_str ());
      curl_easy_setopt (curl, CURLOPT_HTTPHEADER,        headers);
      curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION,     &write_cb);
      curl_easy_setopt (curl, CURLOPT_WRITEDATA,         &res.body);
      curl_easy_setopt (curl, CURLOPT_TIMEOUT_MS,        static_cast<long> (cfg.timeout.count ()));
      curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
      curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION,    1L);
      curl_easy_setopt (curl, CURLOPT_NOSIGNAL,          1L);
      apply_tls (curl, cfg.ca_bundle);

      if (req.method == "POST") {
         curl_easy_setopt (curl, CURLOPT_POST,          1L);
         curl_easy_setopt (curl, CURLOPT_POSTFIELDS,    req.body.c_str ());
         curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE, static_cast<long> (req.body.size ()));
      }

      const auto t0 = std::chrono::steady_clock::now ();
      const CURLcode rc = curl_easy_perform (curl);
      curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &res.status);
      res.elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
         std::chrono::steady_clock::now () - t0);

      curl_slist_free_all (headers);
      curl_easy_cleanup   (curl);

      if (rc != CURLE_OK) {
         core::log::api.event ("http.error", {
            { "method",   std::string { req.method } },
            { "url",      req.url },
            { "curl_err", curl_easy_strerror (rc) },
         });
         return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
            "darkerdb: curl failed: {}", curl_easy_strerror (rc)));
      }
      core::log::api.event ("http.request", {
         { "method", std::string { req.method } },
         { "url",    req.url },
         { "status", std::to_string (res.status) },
         { "ms",     std::to_string (res.elapsed.count ()) },
      });
      return res;
   }

   // Acquire a bearer token from the session. Empty string when the request
   // is explicitly unauthenticated.
   core::Result<std::string> bearer_for (const Req& req)
   {
      if (!req.authenticated) return std::string {};
      if (!session) {
         return core::fail (core::Error::make (core::ErrorKind::Permission,
            "darkerdb: no auth session bound"));
      }

      auto tok = session->access_token ();
      if (!tok.has_value ()) return core::fail (tok.error ());
      if (!tok->has_value ()) {
         return core::fail (core::Error::make (core::ErrorKind::Permission,
            "darkerdb: not signed in"));
      }
      return **tok;
   }

   core::Result<Res> http (const Req& req)
   {
      const int attempts = req.retryable ? 1 + static_cast<int> (k_retry_delays_ms.size ()) : 1;

      core::Result<Res> last = core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: no attempts"));

      bool did_refresh_after_401 = false;

      for (int attempt = 1; attempt <= attempts; ++attempt) {
         if (attempt > 1) {
            std::this_thread::sleep_for (
               std::chrono::milliseconds { k_retry_delays_ms [attempt - 2] });
         }

         auto bearer = bearer_for (req);
         if (!bearer.has_value ()) return core::fail (bearer.error ());

         auto res = http_once (req, *bearer);
         if (!res.has_value ()) {
            last = core::fail (res.error ());
            // libcurl error: retry on connect/timeout, otherwise bail.
            if (req.retryable && attempt < attempts &&
                res.error ().message.find ("Timeout") != std::string::npos) {
               continue;
            }
            return last;
         }

         if (res->status == 401 && req.authenticated && !did_refresh_after_401 && session) {
            // Per contract §4.4 / §3.7: trigger one refresh, retry. If refresh
            // fails the session sign-outs itself and the next bearer_for ()
            // returns "not signed in".
            did_refresh_after_401 = true;
            session->invalidate ();
            continue;
         }

         if (req.retryable && retryable_http_status (res->status) && attempt < attempts) {
            continue;
         }

         return res;
      }

      return last;
   }
};

DarkerDbClient::DarkerDbClient (Config cfg, gv::auth::Session* session, gv::db::Database* cache_db)
   : impl_ (std::make_unique<Impl> ())
{
   impl_->cfg      = std::move (cfg);
   impl_->session  = session;
   impl_->cache_db = cache_db;

   if (impl_->cfg.user_agent.empty ()) {
      std::ostringstream ua;
      ua << "GrimVault v" << gv::core::version::string
         << " (" << machine_id () << ")";
      impl_->cfg.user_agent = ua.str ();
   }
}

DarkerDbClient::~DarkerDbClient () = default;

std::string DarkerDbClient::machine_id ()
{
#ifdef _WIN32
   HKEY  key   = nullptr;
   wchar_t buf [128] {};
   DWORD size = sizeof (buf);

   if (::RegOpenKeyExW (HKEY_LOCAL_MACHINE,
         L"SOFTWARE\\Microsoft\\Cryptography",
         0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
      return "unknown";
   }

   const auto rc = ::RegQueryValueExW (key, L"MachineGuid", nullptr, nullptr,
                                       reinterpret_cast<LPBYTE> (buf), &size);
   ::RegCloseKey (key);

   if (rc != ERROR_SUCCESS) return "unknown";

   const int wlen = static_cast<int> (size / sizeof (wchar_t));
   const int n    = ::WideCharToMultiByte (CP_UTF8, 0, buf, wlen, nullptr, 0, nullptr, nullptr);
   std::string out (static_cast<std::size_t> (std::max (0, n - 1)), '\0');
   ::WideCharToMultiByte (CP_UTF8, 0, buf, wlen, out.data (), n, nullptr, nullptr);
   return out;
#else
   return "non-windows";
#endif
}

core::Result<void> DarkerDbClient::send_diagnostics (const nlohmann::json& payload)
{
   Impl::Req req {
      .method        = "POST",
      .url           = impl_->cfg.base_url + "/diagnostics",
      .body          = payload.dump (),
      .retryable     = false,
      .authenticated = false,
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: diagnostics HTTP {}: {}", res->status, res->body.substr (0, 200)));
   }

   core::log::api.info ("diagnostics: submitted ({} bytes)", req.body.size ());
   return {};
}

core::Result<TooltipLookup> DarkerDbClient::lookup_tooltip (
   std::string_view     raw_text,
   std::string_view     language,
   std::chrono::seconds cache_ttl
) {
   const std::string lang { language };
   const std::string text { raw_text };

   const std::string cache_key = "lookup:" + lang + "@" + text;

   if (impl_->cache_db) {
      try {
         SQLite::Statement q { impl_->cache_db->sqlite (), R"sql(
            SELECT response_json
              FROM pricing_cache
             WHERE cache_key = ?
               AND fetched_at + ttl_seconds > unixepoch ()
         )sql" };
         q.bind (1, cache_key);
         if (q.executeStep ()) {
            auto json = nlohmann::json::parse (q.getColumn (0).getString (), nullptr, false);
            if (!json.is_discarded ()) {
               return parse_lookup (std::move (json));
            }
         }
      } catch (const std::exception& e) {
         core::log::api.warn ("lookup cache read failed: {}", e.what ());
      }
   }

   // ISO-8601 UTC timestamp for captured_at.
   const auto now = std::chrono::system_clock::now ();
   const auto tt  = std::chrono::system_clock::to_time_t (now);
   char ts_buf [32];
   {
      std::tm tm {};
#ifdef _WIN32
      gmtime_s (&tm, &tt);
#else
      gmtime_r (&tt, &tm);
#endif
      std::strftime (ts_buf, sizeof (ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
   }

   nlohmann::json payload {
      { "client_id",      impl_->cfg.client_id },
      { "client_version", gv::core::version::string },
      { "captured_at",    ts_buf },
      { "language",       lang },
      { "ocr", {
         { "raw_text",    text },
      }},
   };

   Impl::Req req {
      .method = "POST",
      .url    = impl_->cfg.base_url + "/v2/grimvault/lookup",
      .body   = payload.dump (),
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 404) {
      return core::fail (core::Error::make (core::ErrorKind::NotFound,
         "darkerdb: item not recognized"));
   }
   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
         "darkerdb: lookup auth failed HTTP {}: {}", res->status,
         res->body.substr (0, 200)));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: lookup HTTP {}: {}", res->status, res->body.substr (0, 200)));
   }

   auto json = nlohmann::json::parse (res->body, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: invalid JSON response"));
   }

   if (impl_->cache_db) {
      try {
         SQLite::Statement upsert { impl_->cache_db->sqlite (), R"sql(
            INSERT INTO pricing_cache (cache_key, response_json, fetched_at, ttl_seconds)
                 VALUES               (?, ?, unixepoch (), ?)
            ON CONFLICT (cache_key) DO UPDATE
               SET response_json = excluded.response_json,
                   fetched_at    = excluded.fetched_at,
                   ttl_seconds   = excluded.ttl_seconds
         )sql" };
         upsert.bind (1, cache_key);
         upsert.bind (2, json.dump ());
         upsert.bind (3, static_cast<long long> (cache_ttl.count ()));
         upsert.exec ();
      } catch (const std::exception& e) {
         core::log::api.warn ("lookup cache upsert failed: {}", e.what ());
      }
   }

   return parse_lookup (std::move (json));
}

core::Result<PingResult> DarkerDbClient::ping ()
{
   Impl::Req req {
      .method = "POST",
      .url    = impl_->cfg.base_url + "/v2/grimvault/ping",
      .body   = "{}",
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
         "darkerdb: ping auth failed HTTP {}: {}", res->status,
         res->body.substr (0, 200)));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: ping HTTP {}: {}", res->status, res->body.substr (0, 200)));
   }

   auto json = nlohmann::json::parse (res->body, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: ping invalid JSON"));
   }
   return parse_ping (std::move (json));
}

} // namespace gv::api
