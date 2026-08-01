#pragma once

#include <gv/api/darkerdb_client.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gv::ui::augment {

// Render-time preferences the card honours, mirrored from the player's
// dashboard settings. Kept as a value type so the render path can take a
// snapshot without locking against the settings poller.
//
// `widgets` is the server's tooltip:analysis:* vocabulary in render order,
// not a client-side enum — see SettingsBundle::Tooltip::analysis.
struct Options {
   std::vector<std::pair<std::string, bool>> widgets;
   std::string currency_display = "absolute";

   // Accelerator that opens the item on DarkerDB. The card advertises it
   // wherever it truncates a list, so a cut-off section reads as "there is
   // more, here is how" rather than as missing data.
   std::string browse_hotkey;
};

namespace detail {

inline nlohmann::json stats_section (
   const char* variant,
   const std::vector<gv::api::TooltipAttribute>& attrs)
{
   nlohmann::json entries = nlohmann::json::array ();
   for (const auto& attr : attrs) {
      entries.push_back ({ { "label", attr.label }, { "value", attr.value } });
   }
   return { { "kind", "stats" }, { "variant", variant },
            { "entries", std::move (entries) } };
}

inline bool is_analysis (const gv::api::TooltipLookup& lookup)
{
   return !lookup.item_id.empty () || !lookup.display_name.empty () || !lookup.rolls.empty ();
}

inline nlohmann::json analysis_roll (const gv::api::AnalysisRoll& roll)
{
   nlohmann::json value = {
      { "attribute_id", roll.attribute_id },
      { "label", roll.label },
      { "slot", roll.slot },
      { "value", roll.value },
      { "formatted_value", roll.formatted_value },
   };
   if (roll.minimum) value ["minimum"] = *roll.minimum;
   if (roll.maximum) value ["maximum"] = *roll.maximum;
   if (roll.roll_percentile) value ["roll_percentile"] = *roll.roll_percentile;
   if (!roll.grade.empty ()) value ["grade"] = roll.grade;
   return value;
}

inline nlohmann::json gem_plan (const gv::api::GemPlan& plan, int sockets)
{
   nlohmann::json changes = nlohmann::json::array ();
   for (const auto& change : plan.changes) {
      changes.push_back ({
         { "replace_attribute_id", change.replace_attribute_id },
         { "replace_label", change.replace_label },
         { "replace_value", change.replace_value },
         { "gem_family", change.gem_family },
         { "gem_item_id", change.gem_item_id },
         { "gem_icon_url", change.gem_icon_url },
         { "new_attribute_id", change.new_attribute_id },
         { "new_label", change.new_label },
         { "new_value", change.new_value },
      });
   }

   return {
      { "sockets", sockets },
      { "changes", std::move (changes) },
      { "projected_value", plan.projected_value },
      { "value_uplift", plan.value_uplift },
      { "socket_fee", plan.socket_fee },
      { "net_uplift", plan.net_uplift },
      { "confidence", plan.confidence },
      { "sample_size", plan.sample_size },
   };
}

// The card's per-section visibility: the player's toggle, except that a
// widget the plan does not grant is forced off. Its blocks were already
// stripped server-side by GrimVaultProjector, so leaving it "visible" would
// only ask the renderer to draw a section with nothing in it.
inline nlohmann::json visible_sections (const gv::api::TooltipLookup& lookup,
                                        const Options& options)
{
   nlohmann::json visible = nlohmann::json::object ();

   for (const auto& [widget, wanted] : options.widgets) {
      visible [widget] = wanted && lookup.entitlement.grants (widget);
   }

   // A locked widget the player never had a toggle for still has to be
   // suppressed, so walk the grant as well as the preference.
   for (const auto& locked : lookup.entitlement.locked) {
      visible [locked.widget] = false;
   }

   return visible;
}

inline nlohmann::json analysis_entity (const gv::api::TooltipLookup& lookup,
                                       const Options& options)
{
   nlohmann::json rolls = nlohmann::json::array ();
   for (const auto& roll : lookup.rolls) rolls.push_back (analysis_roll (roll));

   nlohmann::json plans = nlohmann::json::array ();
   if (lookup.gem_optimization.one_socket) {
      plans.push_back (gem_plan (*lookup.gem_optimization.one_socket, 1));
   }

   nlohmann::json trade_messages = nlohmann::json::array ();
   for (const auto& message : lookup.trade_chat.messages) {
      nlohmann::json items = nlohmann::json::array ();
      for (const auto& item : message.items) {
         items.push_back ({ { "name", item.name }, { "rarity", item.rarity } });
      }
      trade_messages.push_back ({
         { "message", message.message },
         { "observed_at", message.observed_at },
         { "age_seconds", message.age_seconds },
         { "items", std::move (items) },
      });
   }
   if (lookup.gem_optimization.two_socket) {
      plans.push_back (gem_plan (*lookup.gem_optimization.two_socket, 2));
   }
   const auto recipe_item = [] (const gv::api::RecipeItem& item) {
      return nlohmann::json {
         { "item_id", item.item_id }, { "name", item.name },
         { "rarity", item.rarity },   { "icon_url", item.icon_url },
         { "quantity", item.quantity }, { "is_this", item.is_this },
      };
   };

   nlohmann::json quests = nlohmann::json::array ();
   for (const auto& quest : lookup.quests) {
      nlohmann::json row {
         { "merchant_id", quest.merchant_id },
         { "merchant_name", quest.merchant_name },
         { "merchant_icon_url", quest.merchant_icon_url },
         { "quest_name", quest.quest_name },
      };
      if (quest.quest_index) row ["quest_index"] = *quest.quest_index;
      if (quest.quest_count) row ["quest_count"] = *quest.quest_count;
      if (quest.quantity)    row ["quantity"]    = *quest.quantity;
      quests.push_back (std::move (row));
   }

   nlohmann::json recipes = nlohmann::json::array ();
   for (const auto& recipe : lookup.recipes) {
      nlohmann::json materials = nlohmann::json::array ();
      for (const auto& material : recipe.materials) materials.push_back (recipe_item (material));

      nlohmann::json row {
         { "merchant_id", recipe.merchant_id },
         { "merchant_name", recipe.merchant_name },
         { "merchant_icon_url", recipe.merchant_icon_url },
         { "materials", std::move (materials) },
      };
      if (recipe.output) row ["output"] = recipe_item (*recipe.output);
      recipes.push_back (std::move (row));
   }

   nlohmann::json analysis = {
      { "kind", "analysis" },
      { "visible_sections", visible_sections (lookup, options) },
      // Lets the card group its sections by tier. Both come straight from
      // the entitlement block; the renderer does the ordering.
      { "section_tiers", [&lookup] {
         nlohmann::json map = nlohmann::json::object ();
         for (const auto& [widget, plan] : lookup.entitlement.tiers) map [widget] = plan;
         return map;
      } () },
      { "plan_ladder", lookup.entitlement.ladder },
      { "currency_display", options.currency_display },
      { "browse_hotkey", options.browse_hotkey },
      { "item_name", lookup.display_name.empty ()
            ? lookup.canonical_name : lookup.display_name },
      { "item_rarity", lookup.rarity },
      { "quantity", lookup.quantity },
      { "tradeable", lookup.tradeable },
      { "pricing", {
         { "low", lookup.pricing.low },
         { "median", lookup.pricing.median },
         { "high", lookup.pricing.high },
         { "sample_size", lookup.pricing.sample_size },
         { "quick_list", lookup.pricing.quick_list },
         { "lowest_ask", lookup.pricing.lowest_ask },
         { "total_value", lookup.pricing.total_value },
         { "confidence", lookup.pricing.confidence },
      } },
      { "market", {
         { "active_listings", lookup.market_analysis.active_listings },
         { "sales_30d", lookup.market_analysis.sales_30d },
         { "liquidity", lookup.market_analysis.liquidity },
      } },
      { "trade_chat", {
         { "mentions_14d", lookup.trade_chat.mentions_14d },
         { "messages", std::move (trade_messages) },
      } },
      { "rolls", std::move (rolls) },
      { "gem_plans", std::move (plans) },
      // Why there are no plans, when there are none. Without it the card can
      // only drop the section, and a section that silently vanishes reads as
      // a bug rather than as "this item can't be gemmed".
      { "gem_reason", lookup.gem_optimization.reason },
      { "utility", {
         { "vendor_value", lookup.utility.vendor_value },
         { "vendor_total", lookup.utility.vendor_total },
         { "adventure_points", lookup.utility.adventure_points },
         { "gear_score", lookup.utility.gear_score },
         { "max_stack_size", lookup.utility.max_stack_size },
      } },
      { "quests", std::move (quests) },
      { "recipes", std::move (recipes) },
   };

   if (lookup.roll_score) analysis ["roll_score"] = *lookup.roll_score;
   if (lookup.weighted_roll_score) {
      analysis ["weighted_roll_score"] = *lookup.weighted_roll_score;
   }
   if (lookup.relative_percentile) {
      analysis ["relative_percentile"] = *lookup.relative_percentile;
   }
   if (lookup.market_analysis.average_sale_price) {
      analysis ["market"]["average_sale_price"] = *lookup.market_analysis.average_sale_price;
   }
   if (lookup.market_analysis.median_sale_price) {
      analysis ["market"]["median_sale_price"] = *lookup.market_analysis.median_sale_price;
   }
   if (lookup.market_analysis.trend_percent) {
      analysis ["market"]["trend_percent"] = *lookup.market_analysis.trend_percent;
   }
   if (lookup.market_analysis.median_sale_seconds) {
      analysis ["market"]["median_sale_seconds"] = *lookup.market_analysis.median_sale_seconds;
   }
   if (lookup.market_analysis.days_supply) {
      analysis ["market"]["days_supply"] = *lookup.market_analysis.days_supply;
   }
   if (!lookup.market_analysis.price_stability.empty ()) {
      analysis ["market"]["price_stability"] = lookup.market_analysis.price_stability;
   }
   if (lookup.value_driver) {
      analysis ["value_driver"] = {
         { "attribute_id", lookup.value_driver->attribute_id },
         { "label", lookup.value_driver->label },
         { "gold_contribution", lookup.value_driver->gold_contribution },
         { "basis", lookup.value_driver->basis },
      };
   }
   if (lookup.source_analysis) {
      analysis ["source"] = {
         { "kind", lookup.source_analysis->kind },
         { "heading", lookup.source_analysis->heading },
         { "id", lookup.source_analysis->id },
         { "icon_url", lookup.source_analysis->icon_url },
         { "name", lookup.source_analysis->name },
         { "context", lookup.source_analysis->context },
      };
      if (lookup.source_analysis->drop_rate) {
         analysis ["source"]["drop_rate"] = *lookup.source_analysis->drop_rate;
      }
      if (lookup.source_analysis->luck_drop_rate) {
         analysis ["source"]["luck_drop_rate"] = *lookup.source_analysis->luck_drop_rate;
      }
      if (lookup.source_analysis->luck) {
         analysis ["source"]["luck"] = *lookup.source_analysis->luck;
      }
   }
   if (lookup.utility.value_per_slot) {
      analysis ["utility"]["value_per_slot"] = *lookup.utility.value_per_slot;
   }

   // The card is titled by the realm wordmark alone. `realm` is what the
   // tooltip library draws the mark from; `name` stays the plain-text spelling
   // of that same title, so a tooltip build older than the field still renders
   // a correct (if unbranded) header rather than an empty one.
   return {
      { "name", "GrimVault" },
      { "realm", "grimvault" },
      { "rarity", lookup.rarity.empty () ? "common" : lookup.rarity },
      { "sections", nlohmann::json::array ({ std::move (analysis) }) },
   };
}

inline nlohmann::json legacy_entity (const gv::api::TooltipLookup& lookup)
{
   nlohmann::json sections = nlohmann::json::array ();

   if (!lookup.recognized_text.empty ()) {
      sections.push_back ({ { "kind", "text" }, { "body", lookup.recognized_text } });
      return {
         { "name", "GrimVault" },
         { "rarity", lookup.rarity.empty () ? "common" : lookup.rarity },
         { "sections", std::move (sections) },
      };
   }

   if (!lookup.canonical_name.empty ()) {
      sections.push_back ({ { "kind", "text" },
                            { "title", "Item" },
                            { "body", lookup.canonical_name } });
   }
   if (!lookup.primary.empty ()) sections.push_back (stats_section ("primary", lookup.primary));
   if (!lookup.secondary.empty ()) sections.push_back (stats_section ("secondary", lookup.secondary));

   if (!lookup.details.empty ()) {
      nlohmann::json rows = nlohmann::json::array ();
      for (const auto& detail : lookup.details) {
         rows.push_back ({ { "label", detail.label }, { "value", detail.value } });
      }
      sections.push_back ({ { "kind", "rows" }, { "rows", std::move (rows) } });
   }

   if (lookup.pricing.median > 0 || lookup.pricing.low > 0) {
      sections.push_back ({ { "kind", "pricing" },
                            { "market", lookup.pricing.median },
                            { "vendor", lookup.pricing.low } });
   }

   return {
      { "name", "GrimVault" },
      { "rarity", lookup.rarity.empty () ? "common" : lookup.rarity },
      { "sections", std::move (sections) },
   };
}

} // namespace detail

inline nlohmann::json entity (const gv::api::TooltipLookup& lookup,
                              const Options& options = {})
{
   return detail::is_analysis (lookup)
      ? detail::analysis_entity (lookup, options)
      : detail::legacy_entity (lookup);
}

// Shown the moment a tooltip region is anchored, before OCR and the network
// round trip have produced anything. Without it the card simply appears some
// hundreds of milliseconds later and the wait reads as lag rather than work.
inline nlohmann::json loading_message (std::uint64_t seq)
{
   return {
      { "type", "render" },
      { "seq",  seq },
      { "entity", {
         { "name",   "GrimVault" },
         { "realm",  "grimvault" },
         { "rarity", "common" },
         { "sections", nlohmann::json::array ({
            nlohmann::json { { "kind", "loading" } }
         }) },
      } },
      { "params", { { "kind", "augment" }, { "compact", true } } },
   };
}

inline nlohmann::json render_message (const gv::api::TooltipLookup& lookup,
                                      std::uint64_t seq,
                                      const Options& options = {})
{
   return {
      { "type", "render" },
      { "seq", seq },
      { "entity", entity (lookup, options) },
      { "params", { { "kind", "augment" }, { "compact", true } } },
   };
}

} // namespace gv::ui::augment