#include <curl/curl.h>
#include <gv/api/collection_upload.h>
#include <gv/api/darkerdb_client.h>
#include <gv/auth/session.h>
#include <gv/core/api_contract.h>
#include <gv/core/diagnostics.h>
#include <gv/core/env_resolver.h>
#include <gv/core/http.h>
#include <gv/core/logger.h>
#include <gv/core/version.h>
#include <openssl/sha.h>

#include "response_parser.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace gv::api {

namespace {

constexpr std::array<int, 3> k_retry_delays_ms { 200, 500, 1500 };
constexpr std::size_t k_max_response_bytes = 2 * 1024 * 1024;

std::string sha256 (std::string_view value)
{
   std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
   SHA256 (reinterpret_cast<const unsigned char*> (value.data ()), value.size (), digest.data ());
   constexpr char digits[] = "0123456789abcdef";
   std::string result (digest.size () * 2, '0');
   for (std::size_t i = 0; i < digest.size (); ++i) {
      result[i * 2] = digits[digest[i] >> 4];
      result[i * 2 + 1] = digits[digest[i] & 0x0f];
   }
   return result;
}

std::string normalized_cache_text (std::string_view text)
{
   std::string out;
   out.reserve (text.size ());
   bool spacing = true;
   for (const unsigned char ch : text) {
      if (std::isspace (ch)) {
         spacing = !out.empty ();
         continue;
      }
      if (spacing && !out.empty ()) out.push_back (' ');
      spacing = false;
      out.push_back (static_cast<char> (std::tolower (ch)));
   }
   return out;
}

long server_total_us (std::string_view timing)
{
   constexpr std::string_view marker = "total;dur=";
   const auto start = timing.find (marker);
   if (start == std::string_view::npos) return 0;
   const std::string value { timing.substr (start + marker.size ()) };
   char* end = nullptr;
   const double milliseconds = std::strtod (value.c_str (), &end);
   if (end == value.c_str () || milliseconds <= 0.0) return 0;
   return static_cast<long> (std::lround (milliseconds * 1000.0));
}

struct WriteState {
   std::string* body = nullptr;
   std::size_t limit = 0;
   bool exceeded = false;
};

std::size_t write_cb (char* ptr, std::size_t size, std::size_t nmemb, void* user)
{
   if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max () / size) return 0;
   const std::size_t n = size * nmemb;
   auto* state = static_cast<WriteState*> (user);
   if (!state || !state->body || state->body->size () > state->limit ||
       n > state->limit - state->body->size ()) {
      if (state) state->exceeded = true;
      return 0;
   }
   state->body->append (ptr, n);
   return n;
}

struct HeaderState {
   long retry_after_seconds = 0;
   std::string request_id;
   std::string server_timing;
};

bool header_name (std::string_view line, std::string_view name)
{
   if (line.size () < name.size ()) return false;
   for (std::size_t i = 0; i < name.size (); ++i) {
      const auto ch = static_cast<unsigned char> (line[i]);
      if (static_cast<char> (std::tolower (ch)) != name[i]) return false;
   }
   return true;
}

std::string header_value (std::string_view line, std::size_t prefix)
{
   line.remove_prefix (prefix);
   while (!line.empty () && (line.front () == ' ' || line.front () == '\t')) {
      line.remove_prefix (1);
   }
   while (!line.empty () && (line.back () == '\r' || line.back () == '\n' || line.back () == ' ' ||
                             line.back () == '\t')) {
      line.remove_suffix (1);
   }
   return std::string { line.substr (0, 1024) };
}

std::size_t header_cb (char* ptr, std::size_t size, std::size_t nmemb, void* user)
{
   if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max () / size) return 0;
   const std::size_t n = size * nmemb;
   std::string_view line { ptr, n };
   auto* state = static_cast<HeaderState*> (user);
   constexpr std::string_view retry = "retry-after:";
   constexpr std::string_view request = "x-request-id:";
   constexpr std::string_view timing = "server-timing:";
   constexpr std::string_view grimvault_timing = "x-grimvault-timing:";

   if (header_name (line, request)) {
      state->request_id = header_value (line, request.size ());
      return n;
   }
   if (header_name (line, timing)) {
      state->server_timing = header_value (line, timing.size ());
      return n;
   }
   if (header_name (line, grimvault_timing)) {
      state->server_timing = header_value (line, grimvault_timing.size ());
      return n;
   }
   if (!header_name (line, retry)) return n;

   line.remove_prefix (retry.size ());
   while (!line.empty () && (line.front () == ' ' || line.front () == '\t')) line.remove_prefix (1);
   long seconds = 0;
   const auto [end, ec] = std::from_chars (line.data (), line.data () + line.size (), seconds);
   if (ec == std::errc {} && end != line.data ()) {
      state->retry_after_seconds = std::clamp (seconds, 0L, 5L);
   }
   return n;
}

struct CancelState {
   const std::atomic<std::uint64_t>* epoch = nullptr;
   std::uint64_t request_epoch = 0;
};

int progress_cb (void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
   const auto* state = static_cast<const CancelState*> (user);
   return state && state->epoch->load (std::memory_order_relaxed) != state->request_epoch;
}

bool retryable_http_status (long s)
{
   return s == 429 || s == 502 || s == 503 || s == 504;
}

bool retryable_curl_code (CURLcode code)
{
   return code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
          code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR;
}

std::string platform_name ()
{
#if defined(_M_ARM64) || defined(__aarch64__)
   return "Windows; ARM64";
#elif defined(_M_X64) || defined(__x86_64__)
   return "Windows; x64";
#elif defined(_M_IX86) || defined(__i386__)
   return "Windows; x86";
#else
   return "Windows; unknown";
#endif
}

}  // namespace

nlohmann::json diagnostic (const TooltipLookup& lookup)
{
   const auto optional = [] (const auto& value) {
      return value ? nlohmann::json (*value) : nlohmann::json (nullptr);
   };
   nlohmann::json rolls = nlohmann::json::array ();
   for (const auto& roll : lookup.rolls) {
      rolls.push_back ({
         { "attribute", roll.attribute_id },
         { "label", roll.label },
         { "value", roll.formatted_value },
         { "minimum", optional (roll.minimum) },
         { "maximum", optional (roll.maximum) },
         { "percentile", optional (roll.roll_percentile) },
         { "gem", roll.gem },
      });
   }

   return {
      { "item",
        {
           { "id", lookup.item_id },
           { "canonical_name", lookup.canonical_name },
           { "display_name", lookup.display_name },
           { "language", lookup.language },
           { "rarity", lookup.rarity },
           { "match_confidence", lookup.match_confidence },
        } },
      { "rolls", std::move (rolls) },
      { "valuation",
        {
           { "currency", lookup.pricing.currency },
           { "low", lookup.pricing.low },
           { "fair_value", lookup.pricing.median },
           { "high", lookup.pricing.high },
           { "quick_list", lookup.pricing.quick_list },
           { "lowest_ask", lookup.pricing.lowest_ask },
           { "highest_reasonable_ask", lookup.pricing.highest_reasonable_ask },
           { "latest_listing", lookup.pricing.latest_listing },
           { "sample_size", lookup.pricing.sample_size },
           { "confidence", lookup.pricing.confidence },
           { "mean_similarity", lookup.pricing.mean_similarity },
        } },
      { "market",
        {
           { "sales", lookup.market_analysis.sales.count },
           { "sales_window_hours", lookup.market_analysis.sales.window_hours },
           { "active_listings", lookup.market_analysis.active_listings.count },
           { "average_sale_price", optional (lookup.market_analysis.average_sale_price) },
           { "median_sale_price", optional (lookup.market_analysis.median_sale_price) },
        } },
      { "utility",
        {
           { "vendor_value", lookup.utility.vendor_value },
           { "adventure_points", lookup.utility.adventure_points },
           { "gear_score", lookup.utility.gear_score },
        } },
   };
}

core::Result<SettingsBundle> parse_settings (std::string_view response)
{
   auto json = nlohmann::json::parse (response, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: settings invalid JSON"));
   }

   const auto& body = response::body (json);
   constexpr std::array<std::string_view, 5> required_groups { "behavior", "hotkeys", "overlay",
                                                               "pricing", "tooltip" };
   if (!body.is_object () || std::any_of (required_groups.begin (), required_groups.end (),
                                          [&body] (std::string_view group) {
                                             auto it = body.find (group);
                                             return it == body.end () || !it->is_object ();
                                          })) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "darkerdb: settings response is incomplete"));
   }

   std::vector<std::string> analysis_order;
   auto ordered = nlohmann::ordered_json::parse (response, nullptr, false);
   if (!ordered.is_discarded () && ordered.is_object ()) {
      const auto* ordered_body = &ordered;
      if (auto wrapped = ordered.find ("body"); wrapped != ordered.end ()) ordered_body = &*wrapped;
      if (ordered_body->is_object ()) {
         auto tooltip = ordered_body->find ("tooltip");
         if (tooltip != ordered_body->end () && tooltip->is_object ()) {
            auto analysis = tooltip->find ("analysis");
            if (analysis != tooltip->end () && analysis->is_object ()) {
               analysis_order.reserve (analysis->size ());
               for (const auto& [widget, visible] : analysis->items ()) {
                  (void)visible;
                  analysis_order.push_back (widget);
               }
            }
         }
      }
   }

   SettingsBundle out;
   out.raw = json;
   try {
      response::settings (body, out, analysis_order);
   } catch (const nlohmann::json::exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "darkerdb: invalid settings values: {}", e.what ()));
   }
   return out;
}

core::Result<TooltipLookup> parse_lookup_response (std::string_view response)
{
   auto json = nlohmann::json::parse (response, nullptr, false);
   if (json.is_discarded () || !response::body (json).is_object ()) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: invalid lookup response"));
   }
   try {
      return response::lookup (std::move (json));
   } catch (const nlohmann::json::exception&) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: invalid lookup values"));
   }
}

core::Result<TooltipLookup> parse_analysis_response (std::string_view response)
{
   auto json = nlohmann::json::parse (response, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: invalid analysis response"));
   }
   const auto& body = response::body (json);
   if (!body.is_object () || !body.contains ("match") || !body["match"].is_object ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "darkerdb: analysis response is missing match data"));
   }
   try {
      return response::analysis (std::move (json));
   } catch (const nlohmann::json::exception&) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: invalid analysis values"));
   }
}

core::Result<PingResult> parse_ping_response (std::string_view response)
{
   auto json = nlohmann::json::parse (response, nullptr, false);
   if (json.is_discarded () || !response::body (json).is_object ()) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: invalid ping response"));
   }
   try {
      return response::ping (std::move (json));
   } catch (const nlohmann::json::exception&) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: invalid ping values"));
   }
}

struct DDBClient::Impl {
   Config cfg;
   gv::auth::Session* session = nullptr;
   std::atomic<std::uint64_t> general_cancel_epoch { 0 };
   std::atomic<std::uint64_t> analysis_cancel_epoch { 0 };

   struct Req {
      std::string_view method;  // "GET" or "POST"
      std::string url;
      std::string body;
      bool retryable = true;
      bool authenticated = true;
      bool latency_critical = false;
   };

   struct Res {
      long status = 0;
      std::string body;
      std::chrono::milliseconds elapsed { 0 };
      long retry_after_seconds = 0;
      long dns_us = 0;
      long connect_us = 0;
      long tls_us = 0;
      long pretransfer_us = 0;
      long ttfb_us = 0;
      long unattributed_ttfb_us = 0;
      std::string request_id;
      std::string server_timing;
   };

   // Keep an easy handle alive per traffic lane. curl_easy_reset preserves
   // its connection cache, DNS cache and TLS sessions, so hover analysis no
   // longer pays a fresh TCP/TLS handshake for every item. Settings polling
   // gets a separate lane and can never hold the latency-critical request
   // behind a slow background response.
   CURL* general_curl = nullptr;
   CURL* analysis_curl = nullptr;
   std::mutex general_curl_lock;
   std::mutex analysis_curl_lock;

   struct CachedAnalysis {
      TooltipLookup value;
      std::chrono::steady_clock::time_point expires_at;
      std::chrono::steady_clock::time_point used_at;
   };
   std::mutex analysis_cache_lock;
   std::unordered_map<std::string, CachedAnalysis> analysis_cache;
   std::unordered_map<std::string, std::chrono::steady_clock::time_point> not_found_cache;

   std::optional<TooltipLookup> cached_analysis (const std::string& key)
   {
      std::lock_guard lock { analysis_cache_lock };
      const auto found = analysis_cache.find (key);
      if (found == analysis_cache.end ()) return std::nullopt;
      if (found->second.expires_at <= std::chrono::steady_clock::now ()) {
         analysis_cache.erase (found);
         return std::nullopt;
      }
      found->second.used_at = std::chrono::steady_clock::now ();
      return found->second.value;
   }

   void cache_analysis (std::string key, const TooltipLookup& value, std::chrono::seconds ttl)
   {
      if (ttl <= std::chrono::seconds::zero ()) return;
      std::lock_guard lock { analysis_cache_lock };
      constexpr std::size_t max_entries = 64;
      if (!analysis_cache.contains (key) && analysis_cache.size () >= max_entries) {
         const auto oldest = std::min_element (analysis_cache.begin (), analysis_cache.end (),
                                               [] (const auto& left, const auto& right) {
                                                  return left.second.used_at < right.second.used_at;
                                               });
         if (oldest != analysis_cache.end ()) analysis_cache.erase (oldest);
      }
      const auto now = std::chrono::steady_clock::now ();
      analysis_cache.insert_or_assign (std::move (key), CachedAnalysis {
                                                           .value = value,
                                                           .expires_at = now + ttl,
                                                           .used_at = now,
                                                        });
   }

   bool cached_not_found (const std::string& key)
   {
      std::lock_guard lock { analysis_cache_lock };
      const auto found = not_found_cache.find (key);
      if (found == not_found_cache.end ()) return false;
      if (found->second <= std::chrono::steady_clock::now ()) {
         not_found_cache.erase (found);
         return false;
      }
      return true;
   }

   void cache_not_found (const std::string& key)
   {
      if (key.empty ()) return;
      std::lock_guard lock { analysis_cache_lock };
      if (not_found_cache.size () >= 64) not_found_cache.erase (not_found_cache.begin ());
      not_found_cache.insert_or_assign (
         key, std::chrono::steady_clock::now () + std::chrono::seconds { 30 });
   }

   ~Impl ()
   {
      if (general_curl) curl_easy_cleanup (general_curl);
      if (analysis_curl) curl_easy_cleanup (analysis_curl);
   }

   core::Result<Res> http_once (const Req& req, const std::string& bearer, CURLcode& transport_code)
   {
      auto& lane_lock = req.latency_critical ? analysis_curl_lock : general_curl_lock;
      auto& lane_curl = req.latency_critical ? analysis_curl : general_curl;
      auto& cancel_epoch = req.latency_critical ? analysis_cancel_epoch : general_cancel_epoch;
      std::lock_guard lock { lane_lock };

      if (!lane_curl) lane_curl = curl_easy_init ();
      CURL* curl = lane_curl;
      if (!curl) {
         return core::fail (
            core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: curl_easy_init failed"));
      }
      curl_easy_reset (curl);

      curl_slist* headers = nullptr;
      headers =
         curl_slist_append (headers, std::string { gv::core::api_contract::header_line }.c_str ());
      headers = curl_slist_append (headers, ("User-Agent: " + cfg.user_agent).c_str ());
      headers = curl_slist_append (headers, ("X-Client-Id: " + cfg.client_id).c_str ());
      headers = curl_slist_append (
         headers, ("X-Client-Version: " + std::string { gv::core::version::string }).c_str ());
      headers = curl_slist_append (
         headers, ("X-Client-Session: " + gv::core::diagnostics::session_id ()).c_str ());
      if (!bearer.empty ()) {
         headers = curl_slist_append (headers, ("Authorization: Bearer " + bearer).c_str ());
      }
      headers = curl_slist_append (headers, "Accept: application/json");
      if (!req.body.empty ()) {
         headers = curl_slist_append (headers, "Content-Type: application/json");
      }

      Res res;
      WriteState write_state { &res.body, k_max_response_bytes, false };
      HeaderState header_state;
      CancelState cancel_state {
         .epoch = &cancel_epoch,
         .request_epoch = cancel_epoch.load (std::memory_order_relaxed),
      };
      char err_buf[CURL_ERROR_SIZE] { 0 };
      curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, err_buf);
      curl_easy_setopt (curl, CURLOPT_URL, req.url.c_str ());
      curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, &write_cb);
      curl_easy_setopt (curl, CURLOPT_WRITEDATA, &write_state);
      curl_easy_setopt (curl, CURLOPT_HEADERFUNCTION, &header_cb);
      curl_easy_setopt (curl, CURLOPT_HEADERDATA, &header_state);
      curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt (curl, CURLOPT_XFERINFOFUNCTION, &progress_cb);
      curl_easy_setopt (curl, CURLOPT_XFERINFODATA, &cancel_state);
      core::http::apply_transport (curl, {
                                            .ca_bundle = cfg.ca_bundle,
                                            .timeout = cfg.timeout,
                                            .keep_alive = true,
                                         });

      if (req.method == "POST") {
         curl_easy_setopt (curl, CURLOPT_POST, 1L);
         curl_easy_setopt (curl, CURLOPT_POSTFIELDS, req.body.c_str ());
         curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE, static_cast<long> (req.body.size ()));
      }

      const auto t0 = std::chrono::steady_clock::now ();
      const CURLcode rc = curl_easy_perform (curl);
      transport_code = rc;
      curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &res.status);
      res.retry_after_seconds = header_state.retry_after_seconds;
      res.request_id = std::move (header_state.request_id);
      res.server_timing = std::move (header_state.server_timing);
      res.elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
         std::chrono::steady_clock::now () - t0);
#if LIBCURL_VERSION_NUM >= 0x073D00
      curl_off_t timing_us = 0;
      curl_easy_getinfo (curl, CURLINFO_NAMELOOKUP_TIME_T, &timing_us);
      res.dns_us = static_cast<long> (timing_us);
      curl_easy_getinfo (curl, CURLINFO_CONNECT_TIME_T, &timing_us);
      res.connect_us = static_cast<long> (timing_us);
      curl_easy_getinfo (curl, CURLINFO_APPCONNECT_TIME_T, &timing_us);
      res.tls_us = static_cast<long> (timing_us);
      curl_easy_getinfo (curl, CURLINFO_PRETRANSFER_TIME_T, &timing_us);
      res.pretransfer_us = static_cast<long> (timing_us);
      curl_easy_getinfo (curl, CURLINFO_STARTTRANSFER_TIME_T, &timing_us);
      res.ttfb_us = static_cast<long> (timing_us);
      res.unattributed_ttfb_us =
         std::max (0L, res.ttfb_us - res.pretransfer_us - server_total_us (res.server_timing));
#endif

      curl_slist_free_all (headers);

      if (rc != CURLE_OK) {
         if (rc == CURLE_ABORTED_BY_CALLBACK) {
            return core::fail (
               core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: request cancelled"));
         }
         if (rc == CURLE_WRITE_ERROR && write_state.exceeded) {
            return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                                  "darkerdb: response exceeded {} bytes",
                                                  k_max_response_bytes));
         }
         const std::string detail = err_buf[0] ? err_buf : curl_easy_strerror (rc);
         core::log::api.event ("http.error", {
                                                { "method", std::string { req.method } },
                                                { "url", req.url },
                                                { "curl_err", curl_easy_strerror (rc) },
                                                { "detail", detail },
                                             });
         return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                               "darkerdb: curl failed (code {}): {}",
                                               static_cast<int> (rc), detail));
      }
      core::log::api.event (
         "http.request", {
                            { "method", std::string { req.method } },
                            { "url", req.url },
                            { "status", std::to_string (res.status) },
                            { "ms", std::to_string (res.elapsed.count ()) },
                            { "dns_us", std::to_string (res.dns_us) },
                            { "connect_us", std::to_string (res.connect_us) },
                            { "tls_us", std::to_string (res.tls_us) },
                            { "pretransfer_us", std::to_string (res.pretransfer_us) },
                            { "ttfb_us", std::to_string (res.ttfb_us) },
                            { "unattributed_ttfb_us", std::to_string (res.unattributed_ttfb_us) },
                            { "request_id", res.request_id },
                            { "server_timing", res.server_timing },
                         });
      return res;
   }

   // Acquire a bearer token from the session. Empty string when the request
   // is explicitly unauthenticated.
   core::Result<std::string> bearer_for (const Req& req)
   {
      if (!req.authenticated) return std::string {};
      if (!session) {
         return core::fail (
            core::Error::make (core::ErrorKind::Permission, "darkerdb: no auth session bound"));
      }

      auto tok = session->access_token ();
      if (!tok.has_value ()) return core::fail (tok.error ());
      if (!tok->has_value ()) {
         return core::fail (
            core::Error::make (core::ErrorKind::Permission, "darkerdb: not signed in"));
      }
      return **tok;
   }

   core::Result<Res> http (const Req& req)
   {
      const int attempts = req.retryable ? 1 + static_cast<int> (k_retry_delays_ms.size ()) : 1;

      core::Result<Res> last =
         core::fail (core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: no attempts"));

      bool did_refresh_after_401 = false;
      int delay_ms = 0;

      for (int attempt = 1; attempt <= attempts; ++attempt) {
         if (delay_ms > 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds { delay_ms });
         }

         auto bearer = bearer_for (req);
         if (!bearer.has_value ()) return core::fail (bearer.error ());

         CURLcode transport_code = CURLE_OK;
         auto res = http_once (req, *bearer, transport_code);
         if (!res.has_value ()) {
            last = core::fail (res.error ());
            if (req.retryable && attempt < attempts && retryable_curl_code (transport_code)) {
               delay_ms = k_retry_delays_ms[attempt - 1];
               continue;
            }
            return last;
         }

         if (res->status == 401 && req.authenticated && !did_refresh_after_401 && session) {
            // Per contract §4.4 / §3.7: trigger one refresh, then redo this
            // same attempt with the new token so the retry still runs when the
            // 401 lands on the last (or only, for non-retryable requests)
            // attempt; otherwise the loop falls through to the stale sentinel
            // and the real error is lost. Redoing the attempt rather than
            // extending `attempts` keeps the k_retry_delays_ms index in
            // bounds. If refresh fails the session signs itself out and the
            // next bearer_for () returns "not signed in".
            did_refresh_after_401 = true;
            session->invalidate ();
            --attempt;
            delay_ms = 0;
            continue;
         }

         if (req.retryable && retryable_http_status (res->status) && attempt < attempts) {
            delay_ms = res->status == 429 && res->retry_after_seconds > 0
                          ? static_cast<int> (res->retry_after_seconds * 1000)
                          : k_retry_delays_ms[attempt - 1];
            continue;
         }

         return res;
      }

      return last;
   }
};

DDBClient::DDBClient (Config cfg, gv::auth::Session* session, gv::db::Database* cache_db)
    : impl_ (std::make_unique<Impl> ())
{
   impl_->cfg = std::move (cfg);
   impl_->session = session;
   (void)cache_db;

   if (impl_->cfg.user_agent.empty ()) {
      std::ostringstream ua;
      ua << "GrimVault/" << gv::core::version::string << " (" << platform_name () << ")";
      impl_->cfg.user_agent = ua.str ();
   }
}

DDBClient::~DDBClient () = default;

void DDBClient::cancel_pending () noexcept
{
   impl_->general_cancel_epoch.fetch_add (1, std::memory_order_relaxed);
   impl_->analysis_cancel_epoch.fetch_add (1, std::memory_order_relaxed);
}

void DDBClient::cancel_analysis () noexcept
{
   impl_->analysis_cancel_epoch.fetch_add (1, std::memory_order_relaxed);
}

core::Result<TooltipLookup> DDBClient::lookup_tooltip (std::string_view raw_text,
                                                       std::string_view language,
                                                       std::chrono::seconds cache_ttl)
{
   const std::string lang { language };
   const std::string text { raw_text };
   const auto principal = impl_->session ? impl_->session->principal () : std::nullopt;
   std::string cache_key;
   if (principal && !principal->empty ()) {
      cache_key = "lookup\x1f" + *principal + "\x1f" + lang + "\x1f" + normalized_cache_text (text);
      if (auto cached = impl_->cached_analysis (cache_key)) {
         core::log::api.event ("lookup.cache_hit", {
                                                      { "item_id", cached->item_id },
                                                   });
         return std::move (*cached);
      }
      if (impl_->cached_not_found (cache_key)) {
         return core::fail (
            core::Error::make (core::ErrorKind::NotFound, "darkerdb: item not recognized"));
      }
   }

   // ISO-8601 UTC timestamp for captured_at.
   const auto now = std::chrono::system_clock::now ();
   const auto tt = std::chrono::system_clock::to_time_t (now);
   char ts_buf[32];
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
      { "client_id", impl_->cfg.client_id },
      { "client_version", gv::core::version::string },
      { "captured_at", ts_buf },
      { "language", lang },
      { "ocr",
        {
           { "raw_text", text },
        } },
   };

   Impl::Req req {
      .method = "POST",
      .url = impl_->cfg.base_url + "/v2/grimvault/lookup",
      .body = payload.dump (),
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 404) {
      impl_->cache_not_found (cache_key);
      return core::fail (
         core::Error::make (core::ErrorKind::NotFound, "darkerdb: item not recognized"));
   }
   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
                                            "darkerdb: lookup auth failed HTTP {}", res->status));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: lookup HTTP {}", res->status));
   }

   auto parsed = parse_lookup_response (res->body);
   if (!parsed.has_value ()) return core::fail (parsed.error ());
   if (!cache_key.empty ()) {
      impl_->cache_analysis (std::move (cache_key), *parsed,
                             std::min (cache_ttl, std::chrono::seconds { 300 }));
   }
   return std::move (*parsed);
}

core::Result<TooltipLookup> DDBClient::analyze_tooltip (
   std::string_view raw_text, std::string_view language, float confidence,
   std::string_view capture_backend, const std::unordered_map<std::string, std::string>& gems,
   const std::vector<std::string>& enabled_widgets, std::string_view rarity,
   std::chrono::seconds cache_ttl)
{
   const std::string lang { language };
   const std::string text { raw_text };
   std::vector<std::pair<std::string, std::string>> ordered_gems { gems.begin (), gems.end () };
   std::sort (ordered_gems.begin (), ordered_gems.end ());
   auto ordered_widgets = enabled_widgets;
   std::sort (ordered_widgets.begin (), ordered_widgets.end ());
   ordered_widgets.erase (std::unique (ordered_widgets.begin (), ordered_widgets.end ()),
                          ordered_widgets.end ());
   const auto principal = impl_->session ? impl_->session->principal () : std::nullopt;
   std::string cache_key;
   const auto confidence_bucket = std::lround (std::clamp (confidence, 0.0f, 1.0f) * 20.0f);
   if (principal && !principal->empty ()) {
      cache_key.append ("analyze\x1f")
         .append (*principal)
         .append ("\x1f")
         .append (lang)
         .append ("\x1f")
         .append (normalized_cache_text (text))
         .append ("\x1f")
         .append (std::to_string (confidence_bucket))
         .append ("\x1f")
         .append (capture_backend)
         .append ("\x1f")
         .append (rarity);
      for (const auto& [line, family] : ordered_gems) {
         cache_key.append ("\x1e").append (line).append ("\x1f").append (family);
      }
      for (const auto& widget : ordered_widgets) {
         cache_key.append ("\x1d").append (widget);
      }
   }
   if (!cache_key.empty ()) {
      if (auto cached = impl_->cached_analysis (cache_key)) {
         core::log::api.event ("analysis.cache_hit", {
                                                        { "item_id", cached->item_id },
                                                     });
         return std::move (*cached);
      }
      if (impl_->cached_not_found (cache_key)) {
         return core::fail (
            core::Error::make (core::ErrorKind::NotFound, "ddb: item not recognized"));
      }
   }

   const auto now = std::chrono::system_clock::now ();
   const auto tt = std::chrono::system_clock::to_time_t (now);
   char ts_buf[32];
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
      { "client_id", impl_->cfg.client_id },
      { "client_version", gv::core::version::string },
      { "captured_at", ts_buf },
      { "language", lang },
      { "ocr",
        {
           { "raw_text", text },
           { "confidence", std::clamp (confidence, 0.0f, 1.0f) },
           { "gems", gems },
           { "rarity", rarity },
        } },
      { "hints",
        {
           { "capture_backend", capture_backend.empty () ? "unknown" : capture_backend },
           { "enabled_widgets", ordered_widgets },
        } },
   };

   Impl::Req req {
      .method = "POST",
      .url = impl_->cfg.base_url + "/v2/grimvault/analyze",
      .body = payload.dump (),
      .latency_critical = true,
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 404) {
      impl_->cache_not_found (cache_key);
      return core::fail (core::Error::make (core::ErrorKind::NotFound, "ddb: item not recognized"));
   }
   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
                                            "ddb: analysis auth failed HTTP {}", res->status));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "ddb: analysis HTTP {}", res->status));
   }

   auto parsed = parse_analysis_response (res->body);
   if (!parsed.has_value ()) return core::fail (parsed.error ());

   // Entitlements are resolved server-side with a 60-second TTL. Cap the
   // response cache below that so an upgrade or lapse becomes visible on the
   // next normal hover without allowing cross-account reuse.
   const auto ttl = std::min (cache_ttl, std::chrono::seconds { 30 });
   if (!cache_key.empty ()) {
      impl_->cache_analysis (std::move (cache_key), *parsed, ttl);
   }
   return std::move (*parsed);
}

core::Result<PingResult> DDBClient::ping ()
{
   Impl::Req req {
      .method = "POST",
      .url = impl_->cfg.base_url + "/v2/grimvault/ping",
      .body = "{}",
      .latency_critical = true,
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
                                            "darkerdb: ping auth failed HTTP {}", res->status));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "darkerdb: ping HTTP {}", res->status));
   }

   return parse_ping_response (res->body);
}

core::Result<SettingsBundle> DDBClient::get_settings ()
{
   Impl::Req req {
      .method = "GET",
      .url = impl_->cfg.base_url + "/v2/grimvault/settings",
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
                                            "darkerdb: settings auth failed HTTP {}", res->status));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "darkerdb: settings HTTP {}", res->status));
   }

   return parse_settings (res->body);
}

core::Result<CollectionResult> DDBClient::collect (const CollectionSample& sample)
{
   if (sample.channel.empty () || sample.content_type.empty () || sample.body.empty () ||
       !sample.metadata.is_object ()) {
      return core::fail (core::Error::make (core::ErrorKind::InvalidArgument,
                                            "katforge: invalid collection sample"));
   }

   const nlohmann::json authorization {
      { "channel", sample.channel },
      { "content_type", sample.content_type },
      { "content_bytes", sample.body.size () },
      { "content_sha256", sha256 (sample.body) },
      { "metadata", sample.metadata },
   };
   Impl::Req authorize {
      .method = "POST",
      .url = impl_->cfg.collection_base_url + "/v1/collections/authorize",
      .body = authorization.dump (),
      .retryable = false,
   };
   auto response = impl_->http (authorize);
   if (!response.has_value ()) return core::fail (response.error ());
   if (response->status == 401 || response->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
                                            "katforge: collection authorization denied"));
   }
   if (response->status < 200 || response->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: collection authorization HTTP {}",
                                            response->status));
   }

   auto envelope = nlohmann::json::parse (response->body, nullptr, false);
   if (envelope.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: invalid collection authorization"));
   }
   const auto& body = response::body (envelope);
   if (!body.is_object ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: invalid collection authorization"));
   }

   CollectionResult result;
   try {
      result = {
         .accepted = body.value ("accepted", false),
         .retry_after = body.value ("retry_after", 0),
         .reason = body.value ("reason", ""),
         .object_key = body.value ("object_key", ""),
      };
   } catch (const nlohmann::json::exception&) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: invalid collection authorization"));
   }
   if (!result.accepted) return result;

   const auto upload = body.find ("upload");
   const auto sample_id_value = body.find ("sample_id");
   if (upload == body.end () || !upload->is_object () || sample_id_value == body.end () ||
       !sample_id_value->is_string ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: incomplete collection authorization"));
   }
   const auto sample_id = sample_id_value->get<std::string> ();
   if (sample_id.empty ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: incomplete collection authorization"));
   }

   std::vector<core::http::Header> headers;
   if (const auto values = upload->find ("headers");
       values != upload->end () && values->is_object ()) {
      for (const auto& [name, value] : values->items ()) {
         if (!value.is_string ()) {
            return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                                  "katforge: invalid collection authorization"));
         }
         std::string lower = name;
         std::transform (lower.begin (), lower.end (), lower.begin (),
                         [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
         if (lower == "content-type" || lower == "content-length" || lower == "host") continue;
         headers.push_back ({ name, value.get<std::string> () });
      }
   }

   const auto method_value = upload->find ("method");
   const auto url_value = upload->find ("url");
   if (method_value == upload->end () || !method_value->is_string () ||
       url_value == upload->end () || !url_value->is_string ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: incomplete collection authorization"));
   }
   const auto method = method_value->get<std::string> ();
   const auto url = url_value->get<std::string> ();
   auto safe =
      validate_collection_upload (method, url, headers, impl_->cfg.collection_upload_hosts);
   if (!safe.has_value ()) return core::fail (safe.error ());

   auto uploaded = core::http::perform ({
      .method = method,
      .url = url,
      .body = sample.body,
      .content_type = sample.content_type,
      .ca_bundle = impl_->cfg.ca_bundle,
      .headers = std::move (headers),
      .timeout = std::chrono::milliseconds { 30000 },
      .max_response_bytes = 64 * 1024,
      .protocols = "https",
      .cancel_epoch = &impl_->general_cancel_epoch,
      .cancel_epoch_at = impl_->general_cancel_epoch.load (std::memory_order_relaxed),
   });
   if (!uploaded.has_value ()) return core::fail (uploaded.error ());
   if (uploaded->status < 200 || uploaded->status >= 300) {
      return core::fail (core::Error::make (
         core::ErrorKind::ExternalApi, "katforge: collection upload HTTP {}", uploaded->status));
   }

   Impl::Req complete {
      .method = "POST",
      .url = impl_->cfg.collection_base_url + "/v1/collections/complete",
      .body = nlohmann::json ({ { "sample_id", sample_id } }).dump (),
      .retryable = false,
   };
   auto completed = impl_->http (complete);
   if (!completed.has_value ()) return core::fail (completed.error ());
   if (completed->status < 200 || completed->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: collection completion HTTP {}",
                                            completed->status));
   }
   auto completion_envelope = nlohmann::json::parse (completed->body, nullptr, false);
   if (completion_envelope.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: invalid collection completion"));
   }
   const auto& completion = response::body (completion_envelope);
   const auto completion_value =
      completion.is_object () ? completion.find ("completed") : completion.end ();
   if (!completion.is_object () || completion_value == completion.end () ||
       !completion_value->is_boolean () || !completion_value->get<bool> ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
                                            "katforge: collection completion rejected"));
   }

   return result;
}

}  // namespace gv::api
