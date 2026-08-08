#include <gv/api/darkerdb_client.h>

#include <gv/auth/session.h>
#include <gv/core/env_resolver.h>
#include <gv/core/http.h>
#include <gv/core/logger.h>
#include <gv/core/version.h>
#include <curl/curl.h>

#ifdef _WIN32
   #include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
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

   struct WriteState {
      std::string* body = nullptr;
      std::size_t  limit = 0;
      bool         exceeded = false;
   };

   std::size_t write_cb (char* ptr, std::size_t size, std::size_t nmemb, void* user)
   {
      if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max () / size) return 0;
      const std::size_t n = size * nmemb;
      auto* state = static_cast<WriteState*> (user);
      if (!state || !state->body || state->body->size () > state->limit
          || n > state->limit - state->body->size ()) {
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
         const auto ch = static_cast<unsigned char> (line [i]);
         if (static_cast<char> (std::tolower (ch)) != name [i]) return false;
      }
      return true;
   }

   std::string header_value (std::string_view line, std::size_t prefix)
   {
      line.remove_prefix (prefix);
      while (!line.empty () && (line.front () == ' ' || line.front () == '\t')) {
         line.remove_prefix (1);
      }
      while (!line.empty () && (line.back () == '\r' || line.back () == '\n'
                                || line.back () == ' ' || line.back () == '\t')) {
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
      return code == CURLE_COULDNT_CONNECT
         || code == CURLE_COULDNT_RESOLVE_HOST
         || code == CURLE_OPERATION_TIMEDOUT
         || code == CURLE_RECV_ERROR
         || code == CURLE_SEND_ERROR;
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
      out.sockets         = static_cast<int> (integer_or_zero (j, "sockets"));
      if (out.sockets <= 0) out.sockets = std::max (1, static_cast<int> (out.changes.size ()));
      out.projected_value = integer_or_zero (j, "projected_value");
      out.value_uplift    = integer_or_zero (j, "value_uplift");
      out.socket_fee      = integer_or_zero (j, "socket_fee");
      out.net_uplift      = integer_or_zero (j, "net_uplift");
      out.confidence      = j.value ("confidence", "");
      out.sample_size     = integer_or_zero (j, "sample_size");
      return out;
   }

   // Every /v2/analyze section is optional and independently shaped, so each
   // gets its own fold below and parse_analysis stays a manifest of the
   // contract. A section that is absent, or present with the wrong JSON
   // type, leaves its fields at their struct defaults.
   template <typename Fn>
   void with_object (const nlohmann::json& body, const char* key, Fn&& fn)
   {
      if (auto it = body.find (key); it != body.end () && it->is_object ()) fn (*it);
   }

   template <typename Fn>
   void with_array (const nlohmann::json& body, const char* key, Fn&& fn)
   {
      if (auto it = body.find (key); it != body.end () && it->is_array ()) fn (*it);
   }

   void parse_match (const nlohmann::json& j, TooltipLookup& out)
   {
      out.item_id          = j.value ("item_id", "");
      out.canonical_name   = j.value ("canonical_name", "");
      out.display_name     = j.value ("display_name", out.canonical_name);
      out.rarity           = j.value ("rarity", "");
      out.artifact_type    = j.value ("artifact_type", "");
      out.match_confidence = optional_number<double> (j, "confidence").value_or (0.0);
   }

   void parse_rolls (const nlohmann::json& arr, std::vector<AnalysisRoll>& out)
   {
      out.reserve (arr.size ());

      for (const auto& roll : arr) {
         if (!roll.is_object ()) continue;
         out.push_back (AnalysisRoll {
            .attribute_id    = roll.value ("attribute_id", ""),
            .label           = roll.value ("label", ""),
            .slot            = roll.value ("slot", ""),
            .value           = optional_number<double> (roll, "value").value_or (0.0),
            .formatted_value = roll.value ("formatted_value", ""),
            .gem             = roll.value ("gem", ""),
            .gem_icon_url    = roll.value ("gem_icon_url", ""),
            .minimum         = optional_number<double> (roll, "minimum"),
            .maximum         = optional_number<double> (roll, "maximum"),
            .roll_percentile = optional_number<int> (roll, "roll_percentile"),
            .grade           = roll.value ("grade", ""),
         });
      }
   }

   void parse_instance (const nlohmann::json& j, TooltipLookup& out)
   {
      out.quantity = integer_or_zero (j, "quantity");
      if (out.quantity <= 0) out.quantity = 1;
      out.tradeable = j.value ("tradeable", true);

      with_array (j, "rolls", [&] (const nlohmann::json& rolls) {
         parse_rolls (rolls, out.rolls);
      });
   }

   void parse_valuation (const nlohmann::json& j, Pricing& out)
   {
      out.currency        = j.value ("currency", "gold");
      out.low             = integer_or_zero (j, "low");
      out.median          = integer_or_zero (j, "fair_value");
      out.high            = integer_or_zero (j, "high");
      out.market          = out.median;
      out.quick_list      = integer_or_zero (j, "quick_list");
      out.lowest_ask      = integer_or_zero (j, "lowest_ask");
      out.total_value     = integer_or_zero (j, "total_value");
      out.sample_size     = integer_or_zero (j, "sample_size");
      out.ttl_seconds     = static_cast<std::int32_t> (integer_or_zero (j, "ttl_seconds"));
      out.as_of           = j.value ("as_of", "");
      out.confidence      = j.value ("confidence", "");
      out.mean_similarity = optional_number<double> (j, "mean_similarity").value_or (0.0);
      out.raw             = j;
   }

   void parse_quality (const nlohmann::json& j, TooltipLookup& out)
   {
      out.roll_score          = optional_number<int> (j, "roll_score");
      out.weighted_roll_score = optional_number<int> (j, "weighted_roll_score");
      out.relative_percentile = optional_number<int> (j, "relative_percentile");

      with_object (j, "value_driver", [&] (const nlohmann::json& driver) {
         out.value_driver = ValueDriver {
            .attribute_id      = driver.value ("attribute_id", ""),
            .label             = driver.value ("label", ""),
            .gold_contribution = integer_or_zero (driver, "gold_contribution"),
            .basis             = driver.value ("basis", ""),
         };
      });
   }

   void parse_market (const nlohmann::json& j, MarketAnalysis& out)
   {
      out.active_listings     = integer_or_zero (j, "active_listings");
      out.sales_30d           = integer_or_zero (j, "sales_30d");
      out.average_sale_price  = optional_number<std::int64_t> (j, "average_sale_price");
      out.median_sale_price   = optional_number<std::int64_t> (j, "median_sale_price");
      out.trend_percent       = optional_number<double> (j, "trend_percent");
      out.median_sale_seconds = optional_number<std::int64_t> (j, "median_sale_seconds");
      out.days_supply         = optional_number<double> (j, "days_supply");
      out.price_stability     = j.value ("price_stability", "");
      out.liquidity           = j.value ("liquidity", "");
   }

   void parse_utility (const nlohmann::json& j, UtilityAnalysis& out)
   {
      out.vendor_value     = integer_or_zero (j, "vendor_value");
      out.vendor_total     = integer_or_zero (j, "vendor_total");
      out.adventure_points = integer_or_zero (j, "adventure_points");
      out.gear_score       = integer_or_zero (j, "gear_score");
      out.max_stack_size   = integer_or_zero (j, "max_stack_size");
      out.value_per_slot   = optional_number<std::int64_t> (j, "value_per_slot");
   }

   void parse_quests (const nlohmann::json& arr, std::vector<QuestUse>& out)
   {
      out.reserve (arr.size ());

      for (const auto& quest : arr) {
         if (!quest.is_object ()) continue;
         out.push_back (QuestUse {
            .merchant_id       = quest.value ("merchant_id", ""),
            .merchant_name     = quest.value ("merchant_name", ""),
            .merchant_icon_url = quest.value ("merchant_icon_url", ""),
            .quest_name        = quest.value ("quest_name", ""),
            .quest_index       = optional_number<std::int64_t> (quest, "quest_index"),
            .quest_count       = optional_number<std::int64_t> (quest, "quest_count"),
            .quantity          = optional_number<std::int64_t> (quest, "quantity"),
         });
      }
   }

   RecipeItem parse_recipe_item (const nlohmann::json& j)
   {
      return RecipeItem {
         .item_id  = j.value ("item_id", ""),
         .name     = j.value ("name", ""),
         .rarity   = j.value ("rarity", ""),
         .icon_url = j.value ("icon_url", ""),
         .quantity = j.value ("quantity", static_cast<std::int64_t> (1)),
         .is_this  = j.value ("is_this", false),
      };
   }

   void parse_recipes (const nlohmann::json& arr, std::vector<RecipeUse>& out)
   {
      out.reserve (arr.size ());

      for (const auto& recipe : arr) {
         if (!recipe.is_object ()) continue;

         RecipeUse use {
            .merchant_id       = recipe.value ("merchant_id", ""),
            .merchant_name     = recipe.value ("merchant_name", ""),
            .merchant_icon_url = recipe.value ("merchant_icon_url", ""),
         };

         with_object (recipe, "output", [&] (const nlohmann::json& output) {
            use.output = parse_recipe_item (output);
         });

         with_array (recipe, "materials", [&] (const nlohmann::json& materials) {
            use.materials.reserve (materials.size ());
            for (const auto& material : materials) {
               if (material.is_object ()) use.materials.push_back (parse_recipe_item (material));
            }
         });

         out.push_back (std::move (use));
      }
   }

   void parse_source (const nlohmann::json& j, std::optional<SourceAnalysis>& out)
   {
      out = SourceAnalysis {
         .kind           = j.value ("kind", ""),
         .heading        = j.value ("heading", ""),
         .id             = j.value ("id", ""),
         .icon_url       = j.value ("icon_url", ""),
         .name           = j.value ("name", ""),
         .context        = j.value ("context", ""),
         .drop_rate      = optional_number<double> (j, "drop_rate"),
         .luck_drop_rate = optional_number<double> (j, "luck_drop_rate"),
         .luck           = optional_number<int> (j, "luck"),
      };

      with_array (j, "alternates", [&] (const nlohmann::json& alternatives) {
         out->alternates.reserve (alternatives.size ());
         for (const auto& alternative : alternatives) {
            if (!alternative.is_object ()) continue;
            out->alternates.push_back (SourceAlternative {
               .id        = alternative.value ("id", ""),
               .icon_url  = alternative.value ("icon_url", ""),
               .name      = alternative.value ("name", ""),
               .drop_rate = optional_number<double> (alternative, "drop_rate"),
            });
         }
      });
   }

   TradeChatMessage parse_trade_chat_message (const nlohmann::json& j)
   {
      TradeChatMessage out {
         .message     = j.value ("message", ""),
         .observed_at = j.value ("observed_at", ""),
         .age_seconds = integer_or_zero (j, "age_seconds"),
      };

      with_array (j, "items", [&] (const nlohmann::json& items) {
         out.items.reserve (items.size ());
         for (const auto& item : items) {
            if (!item.is_object ()) continue;
            out.items.push_back (TradeChatItem {
               .name   = item.value ("name", ""),
               .rarity = item.value ("rarity", ""),
            });
         }
      });

      return out;
   }

   void parse_trade_chat (const nlohmann::json& j, TradeChatAnalysis& out)
   {
      out.mentions_14d = integer_or_zero (j, "mentions_14d");

      with_array (j, "messages", [&] (const nlohmann::json& messages) {
         out.messages.reserve (messages.size ());
         for (const auto& message : messages) {
            if (!message.is_object ()) continue;
            out.messages.push_back (parse_trade_chat_message (message));
         }
      });
   }

   void parse_similar_sales (const nlohmann::json& arr, std::vector<SimilarSale>& out)
   {
      out.reserve (arr.size ());
      for (const auto& sale : arr) {
         if (!sale.is_object ()) continue;
         SimilarSale parsed {
            .price        = integer_or_zero (sale, "price"),
            .similarity   = static_cast<std::int32_t> (integer_or_zero (sale, "similarity")),
            .sold_at      = sale.value ("sold_at", ""),
            .age_seconds  = integer_or_zero (sale, "age_seconds"),
            .sale_seconds = optional_number<std::int64_t> (sale, "sale_seconds"),
            .highlight_label = sale.value ("highlight_label", ""),
            .highlight_value = sale.value ("highlight_value", ""),
         };
         with_array (sale, "rolls", [&] (const nlohmann::json& rolls) {
            parsed.rolls.reserve (rolls.size ());
            for (const auto& roll : rolls) {
               if (!roll.is_object ()) continue;
               const auto label = roll.value ("label", "");
               if (label.empty ()) continue;
               parsed.rolls.push_back (SimilarSaleRoll {
                  .attribute_id = roll.value ("attribute_id", ""),
                  .label = label,
                  .formatted_value = roll.value ("formatted_value", ""),
               });
            }
         });
         out.push_back (std::move (parsed));
      }
   }

   // Collects the string members of a JSON array, skipping anything else.
   void collect_strings (const nlohmann::json& arr, std::vector<std::string>& out)
   {
      out.reserve (arr.size ());
      for (const auto& v : arr) {
         if (v.is_string ()) out.push_back (v.get<std::string> ());
      }
   }

   void parse_entitlement (const nlohmann::json& j, Entitlement& out)
   {
      out.plan       = j.value ("plan", "");
      out.slot_limit = integer_or_zero (j, "slots");

      with_array (j, "granted", [&] (const nlohmann::json& granted) {
         collect_strings (granted, out.granted);
      });

      with_array (j, "ladder", [&] (const nlohmann::json& ladder) {
         collect_strings (ladder, out.ladder);
      });

      with_object (j, "tiers", [&] (const nlohmann::json& tiers) {
         out.tiers.reserve (tiers.size ());
         for (const auto& [widget, plan] : tiers.items ()) {
            if (plan.is_string ()) out.tiers.emplace_back (widget, plan.get<std::string> ());
         }
      });

      with_array (j, "locked", [&] (const nlohmann::json& locked) {
         out.locked.reserve (locked.size ());
         for (const auto& row : locked) {
            if (!row.is_object ()) continue;
            out.locked.push_back (LockedWidget {
               .widget             = row.value ("widget", ""),
               .required_plan      = row.value ("required_plan", ""),
               .required_plan_name = row.value ("required_plan_name",
                                                row.value ("required_plan", "")),
            });
         }
      });
   }

   void parse_gems (const nlohmann::json& j, GemOptimization& out)
   {
      out.assumption = j.value ("assumption", "");
      out.reason     = j.value ("reason", "");
      out.note       = j.value ("note", "");

      with_array (j, "plans", [&] (const nlohmann::json& plans) {
         out.plans.reserve (plans.size ());
         for (const auto& value : plans) {
            auto plan = parse_gem_plan (value);
            if (!plan) continue;
            if (plan->sockets == 1) out.one_socket = *plan;
            if (plan->sockets == 2) out.two_socket = *plan;
            out.plans.push_back (std::move (*plan));
         }
      });

      // Old servers expose only these aliases. New servers retain them for
      // old clients, but `plans` is authoritative and must not be duplicated.
      if (!out.plans.empty ()) return;
      if (auto one = j.find ("one_socket"); one != j.end ()) {
         out.one_socket = parse_gem_plan (*one);
         if (out.one_socket) {
            out.one_socket->sockets = 1;
            out.plans.push_back (*out.one_socket);
         }
      }
      if (auto two = j.find ("two_socket"); two != j.end ()) {
         out.two_socket = parse_gem_plan (*two);
         if (out.two_socket) {
            out.two_socket->sockets = 2;
            out.plans.push_back (*out.two_socket);
         }
      }
   }

   TooltipLookup parse_analysis (nlohmann::json j)
   {
      TooltipLookup out;
      const auto&   body = body_of (j);
      out.request_id = j.value ("request_id", "");

      if (!body.is_object ()) {
         out.raw = std::move (j);
         return out;
      }

      with_object (body, "match",     [&] (const nlohmann::json& s) { parse_match       (s, out); });
      with_object (body, "instance",  [&] (const nlohmann::json& s) { parse_instance    (s, out); });
      with_object (body, "valuation", [&] (const nlohmann::json& s) { parse_valuation   (s, out.pricing); });
      with_object (body, "quality",   [&] (const nlohmann::json& s) { parse_quality     (s, out); });
      with_object (body, "market",    [&] (const nlohmann::json& s) { parse_market      (s, out.market_analysis); });
      with_array  (body, "similar_sales", [&] (const nlohmann::json& s) { parse_similar_sales (s, out.similar_sales); });
      with_object (body, "utility",   [&] (const nlohmann::json& s) { parse_utility     (s, out.utility); });
      with_array  (body, "quests",    [&] (const nlohmann::json& s) { parse_quests      (s, out.quests); });
      with_array  (body, "recipes",   [&] (const nlohmann::json& s) { parse_recipes     (s, out.recipes); });
      with_object (body, "source",    [&] (const nlohmann::json& s) { parse_source      (s, out.source_analysis); });
      with_object (body, "trade_chat",[&] (const nlohmann::json& s) { parse_trade_chat  (s, out.trade_chat); });
      with_object (body, "entitlement", [&] (const nlohmann::json& s) { parse_entitlement (s, out.entitlement); });
      with_object (body, "gem_optimization", [&] (const nlohmann::json& s) { parse_gems (s, out.gem_optimization); });

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
      b.values.clear ();
      auto put = [&] (std::string k, std::string v) {
         b.values.emplace (std::move (k), std::move (v));
      };

      put ("overlay:mode",      b.overlay.mode);
      put ("overlay:alignment", b.overlay.alignment);
      put ("overlay:columns",   b.overlay.columns);
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

      for (const auto& [widget, visible] : b.tooltip.analysis) {
         put ("tooltip:analysis:" + widget, visible ? "true" : "false");
      }
      nlohmann::json analysis_order = nlohmann::json::array ();
      for (const auto& [widget, visible] : b.tooltip.analysis) {
         (void) visible;
         analysis_order.push_back (widget);
      }
      put ("tooltip:analysis_order", analysis_order.dump ());

      put ("pricing:currency_display", b.pricing.currency_display);

      put ("behavior:is_auto_update_enabled",
         b.behavior.is_auto_update_enabled ? "true" : "false");
      put ("behavior:is_launch_on_startup_enabled",
         b.behavior.is_launch_on_startup_enabled ? "true" : "false");

      put ("hotkeys:toggle_overlay",  b.hotkeys.toggle_overlay);
      put ("hotkeys:force_refresh",   b.hotkeys.force_refresh);
      put ("hotkeys:open_in_browser", b.hotkeys.open_in_browser);
   }

   // The folds below populate the typed SettingsBundle fields from the
   // nested JSON response, one per settings group, with parse_settings at
   // the end tying them together. Unknown groups / unknown keys are ignored
   // (older client staying compatible with a newer server). Missing fields
   // keep their struct-default values, so a partial server response still
   // yields a fully-populated bundle.
   void parse_overlay (const nlohmann::json& j, SettingsBundle::Overlay& out)
   {
      out.mode      = j.value ("mode",      out.mode);
      out.alignment = j.value ("alignment", out.alignment);
      out.columns   = j.value ("columns",   out.columns);
      out.opacity   = j.value ("opacity",   out.opacity);
      out.scale     = j.value ("scale",     out.scale);
      out.offset_x  = j.value ("offset_x",  out.offset_x);
      out.offset_y  = j.value ("offset_y",  out.offset_y);
   }

   void parse_sections (const nlohmann::json& j, SettingsBundle::TooltipSections& out)
   {
      out.header    = j.value ("header",    out.header);
      out.primary   = j.value ("primary",   out.primary);
      out.secondary = j.value ("secondary", out.secondary);
      out.details   = j.value ("details",   out.details);
      out.quests    = j.value ("quests",    out.quests);
      out.pricing   = j.value ("pricing",   out.pricing);
   }

   // Copied in wire order, not looked up against a client-side list: the
   // server owns the widget vocabulary, so a slug this build has never heard
   // of still reaches the augment's visible_sections. `order` runs first so
   // the server's render order wins; anything it didn't mention is appended
   // in JSON order behind it.
   void parse_widget_toggles (const nlohmann::json& j,
                              const std::vector<std::string>& order,
                              std::vector<std::pair<std::string, bool>>& out)
   {
      out.reserve (j.size ());

      for (const auto& widget : order) {
         if (auto visible = j.find (widget); visible != j.end () && visible->is_boolean ()) {
            out.emplace_back (widget, visible->get<bool> ());
         }
      }

      for (const auto& [widget, visible] : j.items ()) {
         const bool already_added = std::any_of (
            out.begin (), out.end (),
            [&widget] (const auto& row) { return row.first == widget; });

         if (!already_added && visible.is_boolean ()) {
            out.emplace_back (widget, visible.get<bool> ());
         }
      }
   }

   void parse_tooltip (const nlohmann::json& j, SettingsBundle::Tooltip& out,
                       const std::vector<std::string>& analysis_order)
   {
      with_object (j, "sections", [&] (const nlohmann::json& s) {
         parse_sections (s, out.sections);
      });

      out.is_price_history_sparkline_visible = j.value (
         "is_price_history_sparkline_visible",
         out.is_price_history_sparkline_visible);

      with_object (j, "analysis", [&] (const nlohmann::json& a) {
         parse_widget_toggles (a, analysis_order, out.analysis);
      });
   }

   void parse_behavior (const nlohmann::json& j, SettingsBundle::Behavior& out)
   {
      out.is_auto_update_enabled = j.value (
         "is_auto_update_enabled", out.is_auto_update_enabled);
      out.is_launch_on_startup_enabled = j.value (
         "is_launch_on_startup_enabled", out.is_launch_on_startup_enabled);
   }

   void parse_hotkeys (const nlohmann::json& j, SettingsBundle::Hotkeys& out)
   {
      out.toggle_overlay  = j.value ("toggle_overlay",  out.toggle_overlay);
      out.force_refresh   = j.value ("force_refresh",   out.force_refresh);
      out.open_in_browser = j.value ("open_in_browser", out.open_in_browser);
   }

   void parse_settings (const nlohmann::json& body, SettingsBundle& out,
                        const std::vector<std::string>& analysis_order)
   {
      if (!body.is_object ()) {
         flatten_to_values (out);
         return;
      }

      out.updated_at = body.value ("updated_at", "");

      with_object (body, "overlay",  [&] (const nlohmann::json& s) { parse_overlay  (s, out.overlay); });
      with_object (body, "behavior", [&] (const nlohmann::json& s) { parse_behavior (s, out.behavior); });
      with_object (body, "hotkeys",  [&] (const nlohmann::json& s) { parse_hotkeys  (s, out.hotkeys); });
      with_object (body, "tooltip",  [&] (const nlohmann::json& s) {
         parse_tooltip (s, out.tooltip, analysis_order);
      });
      with_object (body, "pricing",  [&] (const nlohmann::json& s) {
         out.pricing.currency_display = s.value ("currency_display", out.pricing.currency_display);
      });

      flatten_to_values (out);
   }

} // namespace

struct DDBClient::Impl
{
   Config             cfg;
   gv::auth::Session* session  = nullptr;
   std::atomic<std::uint64_t> cancel_epoch { 0 };

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
      long                       retry_after_seconds = 0;
      long                       dns_us = 0;
      long                       connect_us = 0;
      long                       tls_us = 0;
      long                       ttfb_us = 0;
      std::string                request_id;
      std::string                server_timing;
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

   struct CachedAnalysis {
      TooltipLookup value;
      std::chrono::steady_clock::time_point expires_at;
      std::chrono::steady_clock::time_point used_at;
   };
   std::mutex analysis_cache_lock;
   std::unordered_map<std::string, CachedAnalysis> analysis_cache;

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

   void cache_analysis (std::string key, const TooltipLookup& value,
                        std::chrono::seconds ttl)
   {
      if (ttl <= std::chrono::seconds::zero ()) return;
      std::lock_guard lock { analysis_cache_lock };
      constexpr std::size_t max_entries = 64;
      if (!analysis_cache.contains (key) && analysis_cache.size () >= max_entries) {
         const auto oldest = std::min_element (
            analysis_cache.begin (), analysis_cache.end (), [] (const auto& left, const auto& right) {
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

   ~Impl ()
   {
      if (general_curl)  curl_easy_cleanup (general_curl);
      if (analysis_curl) curl_easy_cleanup (analysis_curl);
   }

   core::Result<Res> http_once (const Req& req, const std::string& bearer,
                                CURLcode& transport_code)
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
      WriteState write_state { &res.body, k_max_response_bytes, false };
      HeaderState header_state;
      CancelState cancel_state {
         .epoch = &cancel_epoch,
         .request_epoch = cancel_epoch.load (std::memory_order_relaxed),
      };
      char err_buf [CURL_ERROR_SIZE] { 0 };
      curl_easy_setopt (curl, CURLOPT_ERRORBUFFER,       err_buf);
      curl_easy_setopt (curl, CURLOPT_URL,               req.url.c_str ());
      curl_easy_setopt (curl, CURLOPT_HTTPHEADER,        headers);
      curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION,     &write_cb);
      curl_easy_setopt (curl, CURLOPT_WRITEDATA,         &write_state);
      curl_easy_setopt (curl, CURLOPT_HEADERFUNCTION,    &header_cb);
      curl_easy_setopt (curl, CURLOPT_HEADERDATA,        &header_state);
      curl_easy_setopt (curl, CURLOPT_TIMEOUT_MS,        static_cast<long> (cfg.timeout.count ()));
      curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
      curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION,    0L);
      curl_easy_setopt (curl, CURLOPT_NOSIGNAL,          1L);
      curl_easy_setopt (curl, CURLOPT_TCP_KEEPALIVE,     1L);
      curl_easy_setopt (curl, CURLOPT_NOPROGRESS,        0L);
      curl_easy_setopt (curl, CURLOPT_XFERINFOFUNCTION,  &progress_cb);
      curl_easy_setopt (curl, CURLOPT_XFERINFODATA,      &cancel_state);
      core::http::apply_tls (curl, cfg.ca_bundle);

      if (req.method == "POST") {
         curl_easy_setopt (curl, CURLOPT_POST,          1L);
         curl_easy_setopt (curl, CURLOPT_POSTFIELDS,    req.body.c_str ());
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
#ifdef CURLINFO_NAMELOOKUP_TIME_T
      curl_off_t timing_us = 0;
      curl_easy_getinfo (curl, CURLINFO_NAMELOOKUP_TIME_T, &timing_us);
      res.dns_us = static_cast<long> (timing_us);
      curl_easy_getinfo (curl, CURLINFO_CONNECT_TIME_T, &timing_us);
      res.connect_us = static_cast<long> (timing_us);
      curl_easy_getinfo (curl, CURLINFO_APPCONNECT_TIME_T, &timing_us);
      res.tls_us = static_cast<long> (timing_us);
      curl_easy_getinfo (curl, CURLINFO_STARTTRANSFER_TIME_T, &timing_us);
      res.ttfb_us = static_cast<long> (timing_us);
#endif

      curl_slist_free_all (headers);

      if (rc != CURLE_OK) {
         if (rc == CURLE_ABORTED_BY_CALLBACK) {
            return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
               "darkerdb: request cancelled"));
         }
         if (rc == CURLE_WRITE_ERROR && write_state.exceeded) {
            return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
               "darkerdb: response exceeded {} bytes", k_max_response_bytes));
         }
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
         { "dns_us", std::to_string (res.dns_us) },
         { "connect_us", std::to_string (res.connect_us) },
         { "tls_us", std::to_string (res.tls_us) },
         { "ttfb_us", std::to_string (res.ttfb_us) },
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
            if (req.retryable && attempt < attempts
                && retryable_curl_code (transport_code)) {
               delay_ms = k_retry_delays_ms [attempt - 1];
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
            delay_ms = 0;
            continue;
         }

         if (req.retryable && retryable_http_status (res->status) && attempt < attempts) {
            delay_ms = res->status == 429 && res->retry_after_seconds > 0
               ? static_cast<int> (res->retry_after_seconds * 1000)
               : k_retry_delays_ms [attempt - 1];
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
   (void) cache_db;

   if (impl_->cfg.user_agent.empty ()) {
      std::ostringstream ua;
      ua << "GrimVault/" << gv::core::version::string
         << " (" << platform_name () << ")";
      impl_->cfg.user_agent = ua.str ();
   }
}

DDBClient::~DDBClient () = default;

void DDBClient::cancel_pending () noexcept
{
   impl_->cancel_epoch.fetch_add (1, std::memory_order_relaxed);
}

core::Result<TooltipLookup> DDBClient::lookup_tooltip (
   std::string_view     raw_text,
   std::string_view     language,
   std::chrono::seconds cache_ttl
) {
   const std::string lang { language };
   const std::string text { raw_text };
   const auto principal = impl_->session
      ? impl_->session->principal ()
      : std::nullopt;
   std::string cache_key;
   if (principal && !principal->empty ()) {
      cache_key = "lookup\x1f" + *principal + "\x1f" + lang + "\x1f" + text;
      if (auto cached = impl_->cached_analysis (cache_key)) {
         core::log::api.event ("lookup.cache_hit", {
            { "item_id", cached->item_id },
         });
         return std::move (*cached);
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
   if (!body_of (json).is_object ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: lookup response has no object body"));
   }

   auto parsed = parse_lookup (std::move (json));
   if (!cache_key.empty ()) {
      impl_->cache_analysis (
         std::move (cache_key), parsed,
         std::min (cache_ttl, std::chrono::seconds { 300 }));
   }
   return parsed;
}

core::Result<TooltipLookup> DDBClient::analyze_tooltip (
   std::string_view     raw_text,
   std::string_view     language,
   float                confidence,
   std::string_view     capture_backend,
   const std::unordered_map<std::string, std::string>& gems,
   std::chrono::seconds cache_ttl
) {
   const std::string lang { language };
   const std::string text { raw_text };
   std::vector<std::pair<std::string, std::string>> ordered_gems {
      gems.begin (), gems.end ()
   };
   std::sort (ordered_gems.begin (), ordered_gems.end ());
   const auto principal = impl_->session
      ? impl_->session->principal ()
      : std::nullopt;
   std::string cache_key;
   const auto confidence_bucket = std::lround (std::clamp (confidence, 0.0f, 1.0f) * 20.0f);
   if (principal && !principal->empty ()) {
      cache_key.append ("analyze\x1f").append (*principal)
         .append ("\x1f").append (lang)
         .append ("\x1f").append (text)
         .append ("\x1f").append (std::to_string (confidence_bucket))
         .append ("\x1f").append (capture_backend);
      for (const auto& [line, family] : ordered_gems) {
         cache_key.append ("\x1e").append (line).append ("\x1f").append (family);
      }
   }
   if (!cache_key.empty ()) {
      if (auto cached = impl_->cached_analysis (cache_key)) {
         core::log::api.event ("analysis.cache_hit", {
            { "item_id", cached->item_id },
         });
         return std::move (*cached);
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
         { "gems",        gems },
      }},
      { "hints", {
         { "capture_backend", capture_backend.empty () ? "unknown" : capture_backend },
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
   const auto& body = body_of (json);
   if (!body.is_object () || !body.contains ("match") || !body ["match"].is_object ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "ddb: analysis response is missing match data"));
   }

   auto parsed = parse_analysis (std::move (json));

   // Entitlements are resolved server-side with a 60-second TTL. Cap the
   // response cache below that so an upgrade or lapse becomes visible on the
   // next normal hover without allowing cross-account reuse.
   const auto ttl = std::min (cache_ttl, std::chrono::seconds { 30 });
   if (!cache_key.empty ()) {
      impl_->cache_analysis (std::move (cache_key), parsed, ttl);
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

   const auto& body = body_of (json);
   constexpr std::array<std::string_view, 5> required_groups {
      "behavior", "hotkeys", "overlay", "pricing", "tooltip"
   };
   if (!body.is_object () || std::any_of (
         required_groups.begin (), required_groups.end (), [&body] (std::string_view group) {
            auto it = body.find (group);
            return it == body.end () || !it->is_object ();
         })) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: settings response is incomplete"));
   }

   std::vector<std::string> analysis_order;
   auto ordered = nlohmann::ordered_json::parse (res->body, nullptr, false);
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
                  (void) visible;
                  analysis_order.push_back (widget);
               }
            }
         }
      }
   }

   SettingsBundle out;
   out.raw = json;
   try {
      parse_settings (body, out, analysis_order);
   } catch (const nlohmann::json::exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "darkerdb: invalid settings values: {}", e.what ()));
   }
   return out;
}

} // namespace gv::api
