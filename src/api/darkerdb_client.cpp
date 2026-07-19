#include <gv/api/darkerdb_client.h>

#include <gv/auth/session.h>
#include <gv/core/env_resolver.h>
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
      // Dev hits hosts that may serve self-signed / locally-issued certs
      // (auth.dev.darkerdb.com, api.dev.darkerdb.com). Skip verification
      // for env=dev only. qa/prod stay strict — cert problems there are
      // real problems and should fail loudly.
      const bool strict_tls = gv::core::active_env ().name != "dev";
      curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, strict_tls ? 1L : 0L);
      curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST, strict_tls ? 2L : 0L);
      // Schannel revocation check fails when the OCSP/CRL endpoint isn't
      // reachable. No-op on OpenSSL builds.
#ifdef _WIN32
      curl_easy_setopt (curl, CURLOPT_SSL_OPTIONS,    CURLSSLOPT_NO_REVOKE);
#endif
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
      out.request_id = j.value ("request_id", "");

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
         if (out.request_id.empty ()) out.request_id = body.value ("request_id", "");
      }

      out.raw = std::move (j);
      return out;
   }

   template <typename T>
   std::optional<T> optional_number (const nlohmann::json& j, std::string_view key)
   {
      auto it = j.find (key);
      if (it == j.end () || it->is_null () || !it->is_number ()) return std::nullopt;
      return it->get<T> ();
   }

   std::int64_t integer_or_zero (const nlohmann::json& j, std::string_view key)
   {
      auto value = optional_number<std::int64_t> (j, key);
      return value.value_or (0);
   }

   GemChange parse_gem_change (const nlohmann::json& j)
   {
      return GemChange {
         .replace_attribute_id = j.value ("replace_attribute_id", ""),
         .replace_label        = j.value ("replace_label", ""),
         .replace_value        = j.value ("replace_value", ""),
         .gem_family           = j.value ("gem_family", ""),
         .gem_item_id          = j.value ("gem_item_id", ""),
         .gem_icon_url         = j.value ("gem_icon_url", ""),
         .new_attribute_id     = j.value ("new_attribute_id", ""),
         .new_label            = j.value ("new_label", ""),
         .new_value            = j.value ("new_value", ""),
      };
   }

   std::optional<GemPlan> parse_gem_plan (const nlohmann::json& j)
   {
      if (!j.is_object ()) return std::nullopt;
      GemPlan out;
      if (auto changes = j.find ("changes"); changes != j.end () && changes->is_array ()) {
         for (const auto& change : *changes) {
            if (change.is_object ()) out.changes.push_back (parse_gem_change (change));
         }
      }
      out.projected_value = integer_or_zero (j, "projected_value");
      out.value_uplift    = integer_or_zero (j, "value_uplift");
      out.socket_fee      = integer_or_zero (j, "socket_fee");
      out.net_uplift      = integer_or_zero (j, "net_uplift");
      out.confidence      = j.value ("confidence", "");
      out.sample_size     = integer_or_zero (j, "sample_size");
      return out;
   }

   TooltipLookup parse_analysis (nlohmann::json j)
   {
      TooltipLookup out;
      const auto& body = body_of (j);
      out.request_id = j.value ("request_id", "");
      if (!body.is_object ()) {
         out.raw = std::move (j);
         return out;
      }

      if (auto match = body.find ("match"); match != body.end () && match->is_object ()) {
         out.item_id          = match->value ("item_id", "");
         out.canonical_name   = match->value ("canonical_name", "");
         out.display_name     = match->value ("display_name", out.canonical_name);
         out.rarity          = match->value ("rarity", "");
         out.match_confidence = optional_number<double> (*match, "confidence").value_or (0.0);
      }

      if (auto instance = body.find ("instance");
          instance != body.end () && instance->is_object ()) {
         out.quantity  = integer_or_zero (*instance, "quantity");
         if (out.quantity <= 0) out.quantity = 1;
         out.tradeable = instance->value ("tradeable", true);

         if (auto rolls = instance->find ("rolls"); rolls != instance->end () && rolls->is_array ()) {
            out.rolls.reserve (rolls->size ());
            for (const auto& roll : *rolls) {
               if (!roll.is_object ()) continue;
               out.rolls.push_back (AnalysisRoll {
                  .attribute_id    = roll.value ("attribute_id", ""),
                  .label           = roll.value ("label", ""),
                  .slot            = roll.value ("slot", ""),
                  .value           = optional_number<double> (roll, "value").value_or (0.0),
                  .formatted_value = roll.value ("formatted_value", ""),
                  .minimum         = optional_number<double> (roll, "minimum"),
                  .maximum         = optional_number<double> (roll, "maximum"),
                  .roll_percentile = optional_number<int> (roll, "roll_percentile"),
                  .grade           = roll.value ("grade", ""),
               });
            }
         }
      }

      if (auto valuation = body.find ("valuation");
          valuation != body.end () && valuation->is_object ()) {
         out.pricing.currency        = valuation->value ("currency", "gold");
         out.pricing.low             = integer_or_zero (*valuation, "low");
         out.pricing.median          = integer_or_zero (*valuation, "fair_value");
         out.pricing.high            = integer_or_zero (*valuation, "high");
         out.pricing.market          = out.pricing.median;
         out.pricing.quick_list      = integer_or_zero (*valuation, "quick_list");
         out.pricing.lowest_ask      = integer_or_zero (*valuation, "lowest_ask");
         out.pricing.total_value     = integer_or_zero (*valuation, "total_value");
         out.pricing.sample_size     = integer_or_zero (*valuation, "sample_size");
         out.pricing.ttl_seconds     = static_cast<std::int32_t> (
            integer_or_zero (*valuation, "ttl_seconds"));
         out.pricing.as_of           = valuation->value ("as_of", "");
         out.pricing.confidence      = valuation->value ("confidence", "");
         out.pricing.mean_similarity = optional_number<double> (
            *valuation, "mean_similarity").value_or (0.0);
         out.pricing.raw = *valuation;
      }

      if (auto quality = body.find ("quality"); quality != body.end () && quality->is_object ()) {
         out.roll_score          = optional_number<int> (*quality, "roll_score");
         out.weighted_roll_score = optional_number<int> (*quality, "weighted_roll_score");
         out.relative_percentile = optional_number<int> (*quality, "relative_percentile");
         if (auto driver = quality->find ("value_driver");
             driver != quality->end () && driver->is_object ()) {
            out.value_driver = ValueDriver {
               .attribute_id     = driver->value ("attribute_id", ""),
               .label            = driver->value ("label", ""),
               .gold_contribution= integer_or_zero (*driver, "gold_contribution"),
               .basis            = driver->value ("basis", ""),
            };
         }
      }

      if (auto market = body.find ("market"); market != body.end () && market->is_object ()) {
         out.market_analysis.active_listings = integer_or_zero (*market, "active_listings");
         out.market_analysis.sales_30d       = integer_or_zero (*market, "sales_30d");
         out.market_analysis.trend_percent   = optional_number<double> (*market, "trend_percent");
         out.market_analysis.median_sale_seconds = optional_number<std::int64_t> (
            *market, "median_sale_seconds");
         out.market_analysis.days_supply     = optional_number<double> (*market, "days_supply");
         out.market_analysis.price_stability = market->value ("price_stability", "");
         out.market_analysis.liquidity       = market->value ("liquidity", "");
      }

      if (auto utility = body.find ("utility"); utility != body.end () && utility->is_object ()) {
         out.utility.vendor_value       = integer_or_zero (*utility, "vendor_value");
         out.utility.vendor_total       = integer_or_zero (*utility, "vendor_total");
         out.utility.adventure_points   = integer_or_zero (*utility, "adventure_points");
         out.utility.gear_score         = integer_or_zero (*utility, "gear_score");
         out.utility.max_stack_size     = integer_or_zero (*utility, "max_stack_size");
         out.utility.required_by_quests = integer_or_zero (*utility, "required_by_quests");
         out.utility.used_in_recipes    = integer_or_zero (*utility, "used_in_recipes");
         out.utility.value_per_slot     = optional_number<std::int64_t> (*utility, "value_per_slot");
         if (auto quests = utility->find ("quest_merchants");
             quests != utility->end () && quests->is_array ()) {
            out.utility.quest_merchants.reserve (quests->size ());
            for (const auto& quest : *quests) {
               if (!quest.is_object ()) continue;
               out.utility.quest_merchants.push_back (QuestMerchant {
                  .merchant_id   = quest.value ("merchant_id", ""),
                  .merchant_name = quest.value ("merchant_name", ""),
                  .quest_index   = integer_or_zero (quest, "quest_index"),
                  .quest_count   = integer_or_zero (quest, "quest_count"),
               });
            }
         }
      }

      if (auto source = body.find ("source"); source != body.end () && source->is_object ()) {
         out.source_analysis = SourceAnalysis {
            .kind           = source->value ("kind", ""),
            .heading        = source->value ("heading", ""),
            .name           = source->value ("name", ""),
            .context        = source->value ("context", ""),
            .drop_rate      = optional_number<double> (*source, "drop_rate"),
            .luck_drop_rate = optional_number<double> (*source, "luck_drop_rate"),
            .luck           = optional_number<int> (*source, "luck"),
         };
      }

      if (auto trade = body.find ("trade_chat"); trade != body.end () && trade->is_object ()) {
         out.trade_chat.mentions_14d = integer_or_zero (*trade, "mentions_14d");
         if (auto messages = trade->find ("messages");
             messages != trade->end () && messages->is_array ()) {
            out.trade_chat.messages.reserve (messages->size ());
            for (const auto& message : *messages) {
               if (!message.is_object ()) continue;
               TradeChatMessage parsed {
                  .message     = message.value ("message", ""),
                  .observed_at = message.value ("observed_at", ""),
                  .age_seconds = integer_or_zero (message, "age_seconds"),
               };
               if (auto items = message.find ("items");
                   items != message.end () && items->is_array ()) {
                  parsed.items.reserve (items->size ());
                  for (const auto& item : *items) {
                     if (!item.is_object ()) continue;
                     parsed.items.push_back (TradeChatItem {
                        .name   = item.value ("name", ""),
                        .rarity = item.value ("rarity", ""),
                     });
                  }
               }
               out.trade_chat.messages.push_back (std::move (parsed));
            }
         }
      }

      if (auto gems = body.find ("gem_optimization"); gems != body.end () && gems->is_object ()) {
         out.gem_optimization.assumption = gems->value ("assumption", "");
         out.gem_optimization.reason     = gems->value ("reason", "");
         out.gem_optimization.note       = gems->value ("note", "");
         if (auto one = gems->find ("one_socket"); one != gems->end ()) {
            out.gem_optimization.one_socket = parse_gem_plan (*one);
         }
         if (auto two = gems->find ("two_socket"); two != gems->end ()) {
            out.gem_optimization.two_socket = parse_gem_plan (*two);
         }
      }

      if (out.request_id.empty ()) out.request_id = body.value ("request_id", "");
      out.raw = std::move (j);
      return out;
   }

   PingResult parse_ping (nlohmann::json j)
   {
      PingResult out;
      const auto& body = body_of (j);
      out.request_id = j.value ("request_id", "");
      if (body.is_object ()) {
         out.ok          = body.value ("ok",          false);
         out.player_id   = body.value ("player_id",   "");
         out.user_id     = body.value ("user_id",     "");
         out.env         = body.value ("env",         "");
         out.server_time = body.value ("server_time", "");
         if (out.request_id.empty ()) out.request_id = body.value ("request_id", "");
      }
      out.raw = std::move (j);
      return out;
   }

   // Translate scalar JSON nodes into the string form that
   // UserSettingsRepo stores. Bools → "true"/"false", numbers via dump
   // (preserves int vs float), strings unwrap. Used by flatten ().
   std::string scalar_to_string (const nlohmann::json& v)
   {
      if (v.is_string ()) return v.get<std::string> ();
      if (v.is_boolean ()) return v.get<bool> () ? "true" : "false";
      return v.dump ();
   }

   // Walk the parsed typed bundle and rebuild the flat colon-namespaced
   // map. Keeps the wire schema (nested) and the storage schema (flat)
   // in sync from a single source of truth.
   void flatten_to_values (SettingsBundle& b)
   {
      auto put = [&] (std::string k, std::string v) {
         b.values.emplace (std::move (k), std::move (v));
      };

      put ("overlay:mode",      b.overlay.mode);
      put ("overlay:alignment", b.overlay.alignment);
      put ("overlay:opacity",   std::to_string (b.overlay.opacity));
      put ("overlay:scale",     std::to_string (b.overlay.scale));
      put ("overlay:offset_x",  std::to_string (b.overlay.offset_x));
      put ("overlay:offset_y",  std::to_string (b.overlay.offset_y));

      put ("tooltip:sections:header",    b.tooltip.sections.header    ? "true" : "false");
      put ("tooltip:sections:primary",   b.tooltip.sections.primary   ? "true" : "false");
      put ("tooltip:sections:secondary", b.tooltip.sections.secondary ? "true" : "false");
      put ("tooltip:sections:details",   b.tooltip.sections.details   ? "true" : "false");
      put ("tooltip:sections:quests",    b.tooltip.sections.quests    ? "true" : "false");
      put ("tooltip:sections:pricing",   b.tooltip.sections.pricing   ? "true" : "false");
      put ("tooltip:is_price_history_sparkline_visible",
         b.tooltip.is_price_history_sparkline_visible ? "true" : "false");

      put ("pricing:currency_display", b.pricing.currency_display);
      put ("pricing:source",           b.pricing.source);
      put ("pricing:window_days",      std::to_string (b.pricing.window_days));

      put ("behavior:is_telemetry_enabled",
         b.behavior.is_telemetry_enabled ? "true" : "false");
      put ("behavior:is_auto_update_enabled",
         b.behavior.is_auto_update_enabled ? "true" : "false");
      put ("behavior:is_launch_on_startup_enabled",
         b.behavior.is_launch_on_startup_enabled ? "true" : "false");

      put ("hotkeys:toggle_overlay", b.hotkeys.toggle_overlay);
      put ("hotkeys:force_refresh",  b.hotkeys.force_refresh);
   }

   // Populate the typed SettingsBundle fields from the nested JSON
   // response. Unknown groups / unknown keys are ignored (older client
   // staying compatible with a newer server). Missing fields keep
   // their struct-default values, so a partial server response still
   // yields a fully-populated bundle.
   void parse_settings (const nlohmann::json& body, SettingsBundle& out)
   {
      if (!body.is_object ()) {
         flatten_to_values (out);
         return;
      }

      out.updated_at = body.value ("updated_at", "");

      if (auto o = body.find ("overlay"); o != body.end () && o->is_object ()) {
         out.overlay.mode      = o->value ("mode",      out.overlay.mode);
         out.overlay.alignment = o->value ("alignment", out.overlay.alignment);
         out.overlay.opacity   = o->value ("opacity",   out.overlay.opacity);
         out.overlay.scale     = o->value ("scale",     out.overlay.scale);
         out.overlay.offset_x  = o->value ("offset_x",  out.overlay.offset_x);
         out.overlay.offset_y  = o->value ("offset_y",  out.overlay.offset_y);
      }

      if (auto t = body.find ("tooltip"); t != body.end () && t->is_object ()) {
         if (auto s = t->find ("sections"); s != t->end () && s->is_object ()) {
            out.tooltip.sections.header    = s->value ("header",    out.tooltip.sections.header);
            out.tooltip.sections.primary   = s->value ("primary",   out.tooltip.sections.primary);
            out.tooltip.sections.secondary = s->value ("secondary", out.tooltip.sections.secondary);
            out.tooltip.sections.details   = s->value ("details",   out.tooltip.sections.details);
            out.tooltip.sections.quests    = s->value ("quests",    out.tooltip.sections.quests);
            out.tooltip.sections.pricing   = s->value ("pricing",   out.tooltip.sections.pricing);
         }
         out.tooltip.is_price_history_sparkline_visible = t->value (
            "is_price_history_sparkline_visible",
            out.tooltip.is_price_history_sparkline_visible);
      }

      if (auto p = body.find ("pricing"); p != body.end () && p->is_object ()) {
         out.pricing.currency_display = p->value ("currency_display", out.pricing.currency_display);
         out.pricing.source           = p->value ("source",           out.pricing.source);
         out.pricing.window_days      = p->value ("window_days",      out.pricing.window_days);
      }

      if (auto b = body.find ("behavior"); b != body.end () && b->is_object ()) {
         out.behavior.is_telemetry_enabled = b->value (
            "is_telemetry_enabled", out.behavior.is_telemetry_enabled);
         out.behavior.is_auto_update_enabled = b->value (
            "is_auto_update_enabled", out.behavior.is_auto_update_enabled);
         out.behavior.is_launch_on_startup_enabled = b->value (
            "is_launch_on_startup_enabled", out.behavior.is_launch_on_startup_enabled);
      }

      if (auto h = body.find ("hotkeys"); h != body.end () && h->is_object ()) {
         out.hotkeys.toggle_overlay = h->value ("toggle_overlay", out.hotkeys.toggle_overlay);
         out.hotkeys.force_refresh  = h->value ("force_refresh",  out.hotkeys.force_refresh);
      }

      flatten_to_values (out);
   }

} // namespace

void DDBClient::global_init    () { curl_global_init    (CURL_GLOBAL_DEFAULT); }
void DDBClient::global_cleanup () { curl_global_cleanup (); }

struct DDBClient::Impl
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
      bool              latency_critical = false;
   };

   struct Res {
      long                       status   = 0;
      std::string                body;
      std::chrono::milliseconds  elapsed { 0 };
   };

   // Keep an easy handle alive per traffic lane. curl_easy_reset preserves
   // its connection cache, DNS cache and TLS sessions, so hover analysis no
   // longer pays a fresh TCP/TLS handshake for every item. Settings polling
   // gets a separate lane and can never hold the latency-critical request
   // behind a slow background response.
   CURL* general_curl  = nullptr;
   CURL* analysis_curl = nullptr;
   std::mutex general_curl_lock;
   std::mutex analysis_curl_lock;

   ~Impl ()
   {
      if (general_curl)  curl_easy_cleanup (general_curl);
      if (analysis_curl) curl_easy_cleanup (analysis_curl);
   }

   core::Result<Res> http_once (const Req& req, const std::string& bearer)
   {
      auto& lane_lock = req.latency_critical ? analysis_curl_lock : general_curl_lock;
      auto& lane_curl = req.latency_critical ? analysis_curl      : general_curl;
      std::lock_guard lock { lane_lock };

      if (!lane_curl) lane_curl = curl_easy_init ();
      CURL* curl = lane_curl;
      if (!curl) {
         return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
            "darkerdb: curl_easy_init failed"));
      }
      curl_easy_reset (curl);

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
      char err_buf [CURL_ERROR_SIZE] { 0 };
      curl_easy_setopt (curl, CURLOPT_ERRORBUFFER,       err_buf);
      curl_easy_setopt (curl, CURLOPT_URL,               req.url.c_str ());
      curl_easy_setopt (curl, CURLOPT_HTTPHEADER,        headers);
      curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION,     &write_cb);
      curl_easy_setopt (curl, CURLOPT_WRITEDATA,         &res.body);
      curl_easy_setopt (curl, CURLOPT_TIMEOUT_MS,        static_cast<long> (cfg.timeout.count ()));
      curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
      curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION,    1L);
      curl_easy_setopt (curl, CURLOPT_NOSIGNAL,          1L);
      curl_easy_setopt (curl, CURLOPT_TCP_KEEPALIVE,     1L);
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

      if (rc != CURLE_OK) {
         const std::string detail = err_buf [0] ? err_buf : curl_easy_strerror (rc);
         core::log::api.event ("http.error", {
            { "method",   std::string { req.method } },
            { "url",      req.url },
            { "curl_err", curl_easy_strerror (rc) },
            { "detail",   detail },
         });
         return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
            "darkerdb: curl failed (code {}): {}", static_cast<int> (rc), detail));
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

DDBClient::DDBClient (Config cfg, gv::auth::Session* session, gv::db::Database* cache_db)
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

DDBClient::~DDBClient () = default;

std::string DDBClient::machine_id ()
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

core::Result<void> DDBClient::send_diagnostics (const nlohmann::json& payload)
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

core::Result<TooltipLookup> DDBClient::lookup_tooltip (
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

core::Result<TooltipLookup> DDBClient::analyze_tooltip (
   std::string_view     raw_text,
   std::string_view     language,
   float                confidence,
   std::chrono::seconds cache_ttl
) {
   const std::string lang { language };
   const std::string text { raw_text };
   const std::string cache_key = "analyze:" + lang + "@" + text;

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
            if (!json.is_discarded ()) return parse_analysis (std::move (json));
         }
      } catch (const std::exception& e) {
         core::log::api.warn ("analysis cache read failed: {}", e.what ());
      }
   }

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
         { "confidence",  std::clamp (confidence, 0.0f, 1.0f) },
      }},
      { "hints", {
         { "capture_backend", "wgc" },
      }},
   };

   Impl::Req req {
      .method = "POST",
      .url    = impl_->cfg.base_url + "/v2/grimvault/analyze",
      .body   = payload.dump (),
      .latency_critical = true,
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 404) {
      return core::fail (core::Error::make (core::ErrorKind::NotFound,
         "ddb: item not recognized"));
   }
   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
         "ddb: analysis auth failed HTTP {}: {}", res->status,
         res->body.substr (0, 200)));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "ddb: analysis HTTP {}: {}", res->status, res->body.substr (0, 200)));
   }

   auto json = nlohmann::json::parse (res->body, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "ddb: invalid analysis JSON response"));
   }

   auto parsed = parse_analysis (json);
   const auto server_ttl = parsed.pricing.ttl_seconds > 0
      ? std::chrono::seconds { parsed.pricing.ttl_seconds }
      : cache_ttl;
   const auto effective_ttl = std::min (cache_ttl, server_ttl);

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
         upsert.bind (3, static_cast<long long> (effective_ttl.count ()));
         upsert.exec ();
      } catch (const std::exception& e) {
         core::log::api.warn ("analysis cache upsert failed: {}", e.what ());
      }
   }

   return parsed;
}

core::Result<PingResult> DDBClient::ping ()
{
   Impl::Req req {
      .method = "POST",
      .url    = impl_->cfg.base_url + "/v2/grimvault/ping",
      .body   = "{}",
      .latency_critical = true,
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

core::Result<SettingsBundle> DDBClient::get_settings ()
{
   Impl::Req req {
      .method = "GET",
      .url    = impl_->cfg.base_url + "/v2/grimvault/settings",
   };

   auto res = impl_->http (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 401 || res->status == 403) {
      return core::fail (core::Error::make (core::ErrorKind::Permission,
         "darkerdb: settings auth failed HTTP {}: {}", res->status,
         res->body.substr (0, 200)));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: settings HTTP {}: {}", res->status, res->body.substr (0, 200)));
   }

   auto json = nlohmann::json::parse (res->body, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: settings invalid JSON"));
   }

   SettingsBundle out;
   out.raw = json;
   const auto& body = body_of (json);
   parse_settings (body, out);
   return out;
}

} // namespace gv::api