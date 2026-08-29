#include "response_parser.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gv::api::response {

namespace {

void parse_pricing (const nlohmann::json& j, Pricing& p)
{
   p.currency = j.value ("currency", std::string { "gold" });
   p.low = j.value ("low", static_cast<std::int64_t> (0));
   p.median = j.value ("median", static_cast<std::int64_t> (0));
   p.high = j.value ("high", static_cast<std::int64_t> (0));
   p.sample_size = j.value ("sample_size", static_cast<std::int64_t> (0));
   p.ttl_seconds = j.value ("ttl_seconds", 0);
   p.as_of = j.value ("as_of", std::string {});
   p.market = p.median;
   p.raw = j;
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
         out.rarity = it->value ("rarity", "");
         parse_attrs ((*it)["primary"], out.primary);
         parse_attrs ((*it)["secondary"], out.secondary);
         parse_attrs ((*it)["details"], out.details);
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
      .replace_label = j.value ("replace_label", ""),
      .replace_value = j.value ("replace_value", ""),
      .gem_family = j.value ("gem_family", ""),
      .gem_item_id = j.value ("gem_item_id", ""),
      .gem_icon_url = j.value ("gem_icon_url", ""),
      .new_attribute_id = j.value ("new_attribute_id", ""),
      .new_label = j.value ("new_label", ""),
      .new_value = j.value ("new_value", ""),
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
   out.sockets = static_cast<int> (integer_or_zero (j, "sockets"));
   if (out.sockets <= 0) out.sockets = std::max (1, static_cast<int> (out.changes.size ()));
   out.projected_value = integer_or_zero (j, "projected_value");
   out.value_uplift = integer_or_zero (j, "value_uplift");
   out.socket_fee = integer_or_zero (j, "socket_fee");
   out.net_uplift = integer_or_zero (j, "net_uplift");
   out.confidence = j.value ("confidence", "");
   out.sample_size = integer_or_zero (j, "sample_size");
   return out;
}

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
   out.item_id = j.value ("item_id", "");
   out.canonical_name = j.value ("canonical_name", "");
   out.display_name = j.value ("display_name", out.canonical_name);
   out.language = j.value ("source_language", "en");
   out.rarity = j.value ("rarity", "");
   out.artifact_type = j.value ("artifact_type", "");
   out.match_confidence = optional_number<double> (j, "confidence").value_or (0.0);
}

void parse_rolls (const nlohmann::json& arr, std::vector<AnalysisRoll>& out)
{
   out.reserve (arr.size ());

   for (const auto& roll : arr) {
      if (!roll.is_object ()) continue;
      out.push_back (AnalysisRoll {
         .attribute_id = roll.value ("attribute_id", ""),
         .label = roll.value ("label", ""),
         .slot = roll.value ("slot", ""),
         .value = optional_number<double> (roll, "value").value_or (0.0),
         .formatted_value = roll.value ("formatted_value", ""),
         .gem = roll.value ("gem", ""),
         .gem_icon_url = roll.value ("gem_icon_url", ""),
         .minimum = optional_number<double> (roll, "minimum"),
         .maximum = optional_number<double> (roll, "maximum"),
         .roll_percentile = optional_number<int> (roll, "roll_percentile"),
         .grade = roll.value ("grade", ""),
      });
   }
}

void parse_instance (const nlohmann::json& j, TooltipLookup& out)
{
   out.quantity = integer_or_zero (j, "quantity");
   if (out.quantity <= 0) out.quantity = 1;
   out.tradeable = j.value ("tradeable", true);

   with_array (j, "rolls", [&] (const nlohmann::json& rolls) { parse_rolls (rolls, out.rolls); });
}

void parse_valuation (const nlohmann::json& j, Pricing& out)
{
   out.currency = j.value ("currency", "gold");
   out.low = integer_or_zero (j, "low");
   out.median = integer_or_zero (j, "fair_value");
   out.high = integer_or_zero (j, "high");
   out.market = out.median;
   out.quick_list = integer_or_zero (j, "quick_list");
   out.lowest_ask = integer_or_zero (j, "lowest_ask");
   out.highest_reasonable_ask = integer_or_zero (j, "highest_reasonable_ask");
   out.latest_listing = integer_or_zero (j, "latest_listing");
   out.total_value = integer_or_zero (j, "total_value");
   out.sample_size = integer_or_zero (j, "sample_size");
   out.ttl_seconds = static_cast<std::int32_t> (integer_or_zero (j, "ttl_seconds"));
   out.as_of = j.value ("as_of", "");
   out.confidence = j.value ("confidence", "");
   out.mean_similarity = optional_number<double> (j, "mean_similarity").value_or (0.0);
   out.raw = j;
}

void parse_quality (const nlohmann::json& j, TooltipLookup& out)
{
   out.roll_score = optional_number<int> (j, "roll_score");
   out.weighted_roll_score = optional_number<int> (j, "weighted_roll_score");
   out.relative_percentile = optional_number<int> (j, "relative_percentile");

   with_object (j, "value_driver", [&] (const nlohmann::json& driver) {
      out.value_driver = ValueDriver {
         .attribute_id = driver.value ("attribute_id", ""),
         .label = driver.value ("label", ""),
         .gold_contribution = integer_or_zero (driver, "gold_contribution"),
         .basis = driver.value ("basis", ""),
      };
   });
}

void parse_market (const nlohmann::json& j, MarketAnalysis& out)
{
   with_object (j, "activity", [&] (const nlohmann::json& activity) {
      with_object (activity, "sales", [&] (const nlohmann::json& sales) {
         out.sales.count = integer_or_zero (sales, "count");
         out.sales.capped = sales.value ("capped", false);
         out.sales.window_hours = integer_or_zero (sales, "window_hours");
      });
      with_object (activity, "active_listings", [&] (const nlohmann::json& active) {
         out.active_listings.count = integer_or_zero (active, "count");
         out.active_listings.capped = active.value ("capped", false);
      });
   });
   if (out.sales.window_hours == 0 && j.contains ("sales_30d")) {
      out.sales.count = integer_or_zero (j, "sales_30d");
      out.sales.window_hours = 30 * 24;
   }
   if (out.active_listings.count == 0 && j.contains ("active_listings")) {
      out.active_listings.count = integer_or_zero (j, "active_listings");
   }
   out.average_sale_price = optional_number<std::int64_t> (j, "average_sale_price");
   out.median_sale_price = optional_number<std::int64_t> (j, "median_sale_price");
   out.trend_percent = optional_number<double> (j, "trend_percent");
   out.median_sale_seconds = optional_number<std::int64_t> (j, "median_sale_seconds");
   out.days_supply = optional_number<double> (j, "days_supply");
   out.price_stability = j.value ("price_stability", "");
   out.liquidity = j.value ("liquidity", "");
}

void parse_utility (const nlohmann::json& j, UtilityAnalysis& out)
{
   out.vendor_value = integer_or_zero (j, "vendor_value");
   out.vendor_total = integer_or_zero (j, "vendor_total");
   out.adventure_points = integer_or_zero (j, "adventure_points");
   out.gear_score = integer_or_zero (j, "gear_score");
   out.max_stack_size = integer_or_zero (j, "max_stack_size");
   out.value_per_slot = optional_number<std::int64_t> (j, "value_per_slot");
}

void parse_quests (const nlohmann::json& arr, std::vector<QuestUse>& out)
{
   out.reserve (arr.size ());

   for (const auto& quest : arr) {
      if (!quest.is_object ()) continue;
      out.push_back (QuestUse {
         .merchant_id = quest.value ("merchant_id", ""),
         .merchant_name = quest.value ("merchant_name", ""),
         .merchant_icon_url = quest.value ("merchant_icon_url", ""),
         .quest_name = quest.value ("quest_name", ""),
         .quest_index = optional_number<std::int64_t> (quest, "quest_index"),
         .quest_count = optional_number<std::int64_t> (quest, "quest_count"),
         .quantity = optional_number<std::int64_t> (quest, "quantity"),
      });
   }
}

RecipeItem parse_recipe_item (const nlohmann::json& j)
{
   return RecipeItem {
      .item_id = j.value ("item_id", ""),
      .name = j.value ("name", ""),
      .rarity = j.value ("rarity", ""),
      .icon_url = j.value ("icon_url", ""),
      .quantity = j.value ("quantity", static_cast<std::int64_t> (1)),
      .is_this = j.value ("is_this", false),
   };
}

void parse_recipes (const nlohmann::json& arr, std::vector<RecipeUse>& out)
{
   out.reserve (arr.size ());

   for (const auto& recipe : arr) {
      if (!recipe.is_object ()) continue;

      RecipeUse use {
         .merchant_id = recipe.value ("merchant_id", ""),
         .merchant_name = recipe.value ("merchant_name", ""),
         .merchant_icon_url = recipe.value ("merchant_icon_url", ""),
      };

      with_object (recipe, "output",
                   [&] (const nlohmann::json& output) { use.output = parse_recipe_item (output); });

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
      .kind = j.value ("kind", ""),
      .heading = j.value ("heading", ""),
      .id = j.value ("id", ""),
      .icon_url = j.value ("icon_url", ""),
      .name = j.value ("name", ""),
      .context = j.value ("context", ""),
      .mode = j.value ("mode", ""),
      .reward_quests = optional_number<int> (j, "reward_quests"),
      .drop_rate = optional_number<double> (j, "drop_rate"),
      .luck_drop_rate = optional_number<double> (j, "luck_drop_rate"),
      .luck = optional_number<int> (j, "luck"),
   };

   with_array (j, "alternates", [&] (const nlohmann::json& alternatives) {
      out->alternates.reserve (alternatives.size ());
      for (const auto& alternative : alternatives) {
         if (!alternative.is_object ()) continue;
         out->alternates.push_back (SourceAlternative {
            .id = alternative.value ("id", ""),
            .icon_url = alternative.value ("icon_url", ""),
            .name = alternative.value ("name", ""),
            .drop_rate = optional_number<double> (alternative, "drop_rate"),
         });
      }
   });
}

TradeChatMessage parse_trade_chat_message (const nlohmann::json& j)
{
   TradeChatMessage out {
      .message = j.value ("message", ""),
      .observed_at = j.value ("observed_at", ""),
      .age_seconds = integer_or_zero (j, "age_seconds"),
   };

   with_array (j, "items", [&] (const nlohmann::json& items) {
      out.items.reserve (items.size ());
      for (const auto& item : items) {
         if (!item.is_object ()) continue;
         out.items.push_back (TradeChatItem {
            .name = item.value ("name", ""),
            .display_name = item.value ("display_name", item.value ("name", "")),
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
         .price = integer_or_zero (sale, "price"),
         .similarity = static_cast<std::int32_t> (integer_or_zero (sale, "similarity")),
         .sold_at = sale.value ("sold_at", sale.value ("listed_at", "")),
         .age_seconds = integer_or_zero (sale, "age_seconds"),
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

void collect_strings (const nlohmann::json& arr, std::vector<std::string>& out)
{
   out.reserve (arr.size ());
   for (const auto& v : arr) {
      if (v.is_string ()) out.push_back (v.get<std::string> ());
   }
}

void parse_entitlement (const nlohmann::json& j, Entitlement& out)
{
   out.plan = j.value ("plan", "");
   out.slot_limit = integer_or_zero (j, "slots");

   with_array (j, "granted",
               [&] (const nlohmann::json& granted) { collect_strings (granted, out.granted); });

   with_array (j, "ladder",
               [&] (const nlohmann::json& ladder) { collect_strings (ladder, out.ladder); });

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
            .widget = row.value ("widget", ""),
            .required_plan = row.value ("required_plan", ""),
            .required_plan_name = row.value ("required_plan_name", row.value ("required_plan", "")),
         });
      }
   });
}

void parse_gems (const nlohmann::json& j, GemOptimization& out)
{
   out.assumption = j.value ("assumption", "");
   out.reason = j.value ("reason", "");
   out.note = j.value ("note", "");

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
   const auto& body = body_of (j);
   out.request_id = j.value ("request_id", "");

   if (!body.is_object ()) {
      out.raw = std::move (j);
      return out;
   }

   with_object (body, "match", [&] (const nlohmann::json& s) { parse_match (s, out); });
   with_object (body, "instance", [&] (const nlohmann::json& s) { parse_instance (s, out); });
   with_object (body, "valuation",
                [&] (const nlohmann::json& s) { parse_valuation (s, out.pricing); });
   with_object (body, "quality", [&] (const nlohmann::json& s) { parse_quality (s, out); });
   with_object (body, "market",
                [&] (const nlohmann::json& s) { parse_market (s, out.market_analysis); });
   with_array (body, "similar_sales",
               [&] (const nlohmann::json& s) { parse_similar_sales (s, out.similar_sales); });
   with_array (body, "similar_listings",
               [&] (const nlohmann::json& s) { parse_similar_sales (s, out.similar_listings); });
   with_object (body, "utility", [&] (const nlohmann::json& s) { parse_utility (s, out.utility); });
   with_array (body, "quests", [&] (const nlohmann::json& s) { parse_quests (s, out.quests); });
   with_array (body, "recipes", [&] (const nlohmann::json& s) { parse_recipes (s, out.recipes); });
   with_object (body, "source",
                [&] (const nlohmann::json& s) { parse_source (s, out.source_analysis); });
   with_object (body, "trade_chat",
                [&] (const nlohmann::json& s) { parse_trade_chat (s, out.trade_chat); });
   with_object (body, "entitlement",
                [&] (const nlohmann::json& s) { parse_entitlement (s, out.entitlement); });
   with_object (body, "gem_optimization",
                [&] (const nlohmann::json& s) { parse_gems (s, out.gem_optimization); });

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
      out.ok = body.value ("ok", false);
      out.player_id = body.value ("player_id", "");
      out.user_id = body.value ("user_id", "");
      out.env = body.value ("env", "");
      out.server_time = body.value ("server_time", "");
      if (out.request_id.empty ()) out.request_id = body.value ("request_id", "");
   }
   out.raw = std::move (j);
   return out;
}

std::string scalar_to_string (const nlohmann::json& v)
{
   if (v.is_string ()) return v.get<std::string> ();
   if (v.is_boolean ()) return v.get<bool> () ? "true" : "false";
   return v.dump ();
}

void flatten_to_values (SettingsBundle& b)
{
   b.values.clear ();
   auto put = [&] (std::string k, std::string v) {
      b.values.emplace (std::move (k), std::move (v));
   };

   put ("overlay:mode", b.overlay.mode);
   put ("overlay:alignment", b.overlay.alignment);
   put ("overlay:columns", b.overlay.columns);
   put ("overlay:opacity", std::to_string (b.overlay.opacity));
   put ("overlay:scale", std::to_string (b.overlay.scale));
   put ("overlay:offset_x", std::to_string (b.overlay.offset_x));
   put ("overlay:offset_y", std::to_string (b.overlay.offset_y));
   put ("overlay:is_indicator_visible", b.overlay.is_indicator_visible ? "true" : "false");

   put ("tooltip:sections:header", b.tooltip.sections.header ? "true" : "false");
   put ("tooltip:sections:primary", b.tooltip.sections.primary ? "true" : "false");
   put ("tooltip:sections:secondary", b.tooltip.sections.secondary ? "true" : "false");
   put ("tooltip:sections:details", b.tooltip.sections.details ? "true" : "false");
   put ("tooltip:sections:quests", b.tooltip.sections.quests ? "true" : "false");
   put ("tooltip:sections:pricing", b.tooltip.sections.pricing ? "true" : "false");
   put ("tooltip:is_price_history_sparkline_visible",
        b.tooltip.is_price_history_sparkline_visible ? "true" : "false");

   for (const auto& [widget, visible] : b.tooltip.analysis) {
      put ("tooltip:analysis:" + widget, visible ? "true" : "false");
   }
   nlohmann::json analysis_order = nlohmann::json::array ();
   for (const auto& [widget, visible] : b.tooltip.analysis) {
      (void)visible;
      analysis_order.push_back (widget);
   }
   put ("tooltip:analysis_order", analysis_order.dump ());

   put ("pricing:currency_display", b.pricing.currency_display);

   put ("behavior:is_auto_update_enabled", b.behavior.is_auto_update_enabled ? "true" : "false");
   put ("behavior:is_launch_on_startup_enabled",
        b.behavior.is_launch_on_startup_enabled ? "true" : "false");
   put ("behavior:is_performance_mode_enabled",
        b.behavior.is_performance_mode_enabled ? "true" : "false");
   put ("behavior:capture_fps", std::to_string (b.behavior.capture_fps));
   put ("behavior:capture_mode", b.behavior.capture_mode);
   put ("behavior:language", b.behavior.language);

   put ("collection:is_improvement_enabled",
        b.collection.is_improvement_enabled ? "true" : "false");

   put ("hotkeys:toggle_overlay", b.hotkeys.toggle_overlay);
   put ("hotkeys:force_refresh", b.hotkeys.force_refresh);
   put ("hotkeys:open_in_browser", b.hotkeys.open_in_browser);
}

void parse_overlay (const nlohmann::json& j, SettingsBundle::Overlay& out)
{
   out.mode = j.value ("mode", out.mode);
   out.alignment = j.value ("alignment", out.alignment);
   out.columns = j.value ("columns", out.columns);
   out.opacity = j.value ("opacity", out.opacity);
   out.scale = j.value ("scale", out.scale);
   out.offset_x = j.value ("offset_x", out.offset_x);
   out.offset_y = j.value ("offset_y", out.offset_y);
   out.is_indicator_visible = j.value ("is_indicator_visible", out.is_indicator_visible);
}

void parse_sections (const nlohmann::json& j, SettingsBundle::TooltipSections& out)
{
   out.header = j.value ("header", out.header);
   out.primary = j.value ("primary", out.primary);
   out.secondary = j.value ("secondary", out.secondary);
   out.details = j.value ("details", out.details);
   out.quests = j.value ("quests", out.quests);
   out.pricing = j.value ("pricing", out.pricing);
}

void parse_widget_toggles (const nlohmann::json& j, const std::vector<std::string>& order,
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
         out.begin (), out.end (), [&widget] (const auto& row) { return row.first == widget; });

      if (!already_added && visible.is_boolean ()) {
         out.emplace_back (widget, visible.get<bool> ());
      }
   }
}

void parse_tooltip (const nlohmann::json& j, SettingsBundle::Tooltip& out,
                    const std::vector<std::string>& analysis_order)
{
   with_object (j, "sections", [&] (const nlohmann::json& s) { parse_sections (s, out.sections); });

   out.is_price_history_sparkline_visible =
      j.value ("is_price_history_sparkline_visible", out.is_price_history_sparkline_visible);

   with_object (j, "analysis", [&] (const nlohmann::json& a) {
      parse_widget_toggles (a, analysis_order, out.analysis);
   });
}

void parse_behavior (const nlohmann::json& j, SettingsBundle::Behavior& out)
{
   out.is_auto_update_enabled = j.value ("is_auto_update_enabled", out.is_auto_update_enabled);
   out.is_launch_on_startup_enabled =
      j.value ("is_launch_on_startup_enabled", out.is_launch_on_startup_enabled);
   out.is_performance_mode_enabled =
      j.value ("is_performance_mode_enabled", out.is_performance_mode_enabled);
   out.capture_fps = j.value ("capture_fps", out.capture_fps);
   out.capture_mode = j.value ("capture_mode", out.capture_mode);
   out.language = j.value ("language", out.language);
}

void parse_hotkeys (const nlohmann::json& j, SettingsBundle::Hotkeys& out)
{
   out.toggle_overlay = j.value ("toggle_overlay", out.toggle_overlay);
   out.force_refresh = j.value ("force_refresh", out.force_refresh);
   out.open_in_browser = j.value ("open_in_browser", out.open_in_browser);
}

void fold_settings (const nlohmann::json& body, SettingsBundle& out,
                    const std::vector<std::string>& analysis_order)
{
   if (!body.is_object ()) {
      flatten_to_values (out);
      return;
   }

   out.updated_at = body.value ("updated_at", "");

   with_object (body, "overlay", [&] (const nlohmann::json& s) { parse_overlay (s, out.overlay); });
   with_object (body, "behavior",
                [&] (const nlohmann::json& s) { parse_behavior (s, out.behavior); });
   with_object (body, "collection", [&] (const nlohmann::json& s) {
      out.collection.is_improvement_enabled =
         s.value ("is_improvement_enabled", out.collection.is_improvement_enabled);
   });
   with_object (body, "hotkeys", [&] (const nlohmann::json& s) { parse_hotkeys (s, out.hotkeys); });
   with_object (body, "tooltip",
                [&] (const nlohmann::json& s) { parse_tooltip (s, out.tooltip, analysis_order); });
   with_object (body, "pricing", [&] (const nlohmann::json& s) {
      out.pricing.currency_display = s.value ("currency_display", out.pricing.currency_display);
   });

   flatten_to_values (out);
}

}

const nlohmann::json& body (const nlohmann::json& json)
{
   return body_of (json);
}

TooltipLookup lookup (nlohmann::json json)
{
   return parse_lookup (std::move (json));
}

TooltipLookup analysis (nlohmann::json json)
{
   return parse_analysis (std::move (json));
}

PingResult ping (nlohmann::json json)
{
   return parse_ping (std::move (json));
}

void settings (const nlohmann::json& body, SettingsBundle& settings,
               const std::vector<std::string>& analysis_order)
{
   fold_settings (body, settings, analysis_order);
}

}
