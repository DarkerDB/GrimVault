#include <gv/ui/augment_payload.h>

#include <gtest/gtest.h>

#include <algorithm>

using gv::api::TooltipAttribute;
using gv::api::TooltipLookup;
using nlohmann::json;

namespace {

   TooltipLookup sample ()
   {
      TooltipLookup l;
      l.canonical_name = "Ruby Silver Ring";
      l.rarity         = "rare";
      l.primary        = { { "Magical Healing", "12" } };
      l.secondary      = { { "Will", "+3" }, { "Knowledge", "+2" } };
      l.details        = { { "Demand", "7 / 10" } };
      l.pricing.median = 412;
      l.pricing.low    = 180;
      return l;
   }

} // namespace

TEST (AugmentPayload, TitleIsFixedIdentity)
{
   const json e = gv::ui::augment::entity (sample ());

   EXPECT_EQ (e ["name"], "GrimVault");
   EXPECT_EQ (e ["rarity"], "rare");
}

TEST (AugmentPayload, SectionsInOrder)
{
   const json e = gv::ui::augment::entity (sample ());
   const auto& s = e ["sections"];

   ASSERT_EQ (s.size (), 5u);
   EXPECT_EQ (s [0]["kind"], "text");
   EXPECT_EQ (s [0]["title"], "Item");
   EXPECT_EQ (s [0]["body"], "Ruby Silver Ring");
   EXPECT_EQ (s [1]["kind"], "stats");
   EXPECT_EQ (s [1]["variant"], "primary");
   EXPECT_EQ (s [2]["kind"], "stats");
   EXPECT_EQ (s [2]["variant"], "secondary");
   EXPECT_EQ (s [2]["entries"].size (), 2u);
   EXPECT_EQ (s [3]["kind"], "rows");
   EXPECT_EQ (s [3]["rows"][0]["label"], "Demand");
   EXPECT_EQ (s [3]["rows"][0]["value"], "7 / 10");
   EXPECT_EQ (s [4]["kind"], "pricing");
   EXPECT_EQ (s [4]["market"], 412);
   EXPECT_EQ (s [4]["vendor"], 180);
}

TEST (AugmentPayload, CompleteAnalysisRendersPremiumSections)
{
   TooltipLookup lookup;
   lookup.item_id       = "id.item.ruby_silver_ring_5001";
   lookup.canonical_name= "Ruby Silver Ring";
   lookup.display_name  = "Ruby Silver Ring";
   lookup.rarity        = "epic";
   lookup.artifact_type = "minor";
   lookup.pricing.median      = 412;
   lookup.pricing.low         = 360;
   lookup.pricing.high        = 500;
   lookup.pricing.quick_list  = 389;
   lookup.pricing.lowest_ask  = 405;
   lookup.pricing.sample_size = 28;
   lookup.pricing.confidence  = "high";
   lookup.utility.vendor_value = 22;
   lookup.roll_score = 82;
   lookup.weighted_roll_score = 88;
   lookup.relative_percentile = 76;
   lookup.rolls.push_back ({
      .attribute_id = "armor_penetration",
      .label = "Armor Penetration",
      .slot = "secondary",
      .value = 4.1,
      .formatted_value = "+4.1%",
      .gem = "blue_sapphire",
      .gem_icon_url = "https://cdn.darkerdb.com/codex/sapphire",
      .minimum = 3.0,
      .maximum = 5.0,
      .roll_percentile = 55,
      .grade = "C",
   });
   lookup.market_analysis.sales = { .count = 28, .capped = false, .window_hours = 48 };
   lookup.market_analysis.active_listings = { .count = 17, .capped = false };
   lookup.market_analysis.trend_percent = 8.1;
   lookup.market_analysis.median_sale_seconds = 2520;
   lookup.market_analysis.days_supply = 18.2;
   lookup.market_analysis.price_stability = "stable";
   lookup.market_analysis.liquidity = "fast";
   lookup.similar_sales.push_back ({
      .price = 425,
      .similarity = 94,
      .sold_at = "2026-08-02T12:00:00Z",
      .age_seconds = 900,
      .sale_seconds = 1800,
      .rolls = {
         { .attribute_id = "magical_damage_bonus", .label = "Magic Damage Bonus", .formatted_value = "+4.8%" },
         { .attribute_id = "knowledge", .label = "Knowledge", .formatted_value = "+3" },
      },
      .highlight_label = "Magic Damage Bonus",
      .highlight_value = "+4.8%",
   });
   lookup.utility.max_stack_size = 5;
   lookup.utility.value_per_slot = 206;
   lookup.quests.push_back ({
      .merchant_id       = "id.merchant.collector",
      .merchant_name     = "Collector",
      .merchant_icon_url = "https://cdn.example/collector",
      .quest_name        = "A Pound of Flesh",
      .quest_index       = 4,
      .quest_count       = 12,
      .quantity          = 3,
   });
   lookup.recipes.push_back ({
      .merchant_id       = "id.merchant.alchemist",
      .merchant_name     = "Alchemist",
      .merchant_icon_url = "https://cdn.example/alchemist",
      .output    = gv::api::RecipeItem { .item_id = "id.item.iron_powder_2001",
                                         .name = "Iron Powder", .quantity = 1 },
      .materials = { gv::api::RecipeItem { .item_id = "id.item.iron_ores_2001",
                                           .name = "Iron Ore", .quantity = 2,
                                           .is_this = true } },
   });
   lookup.value_driver = gv::api::ValueDriver {
      .attribute_id = "armor_penetration",
      .label = "Armor Penetration",
      .gold_contribution = 117,
      .basis = "minimum_roll_counterfactual",
   };
   lookup.source_analysis = gv::api::SourceAnalysis {
      .kind = "drop",
      .heading = "Best Drop Source",
      .name = "Frost Skeleton Footman",
      .context = "Ice Cavern · High Roller",
      .drop_rate = 0.0284,
      .luck_drop_rate = 0.041,
      .luck = 500,
      .alternates = {
         gv::api::SourceAlternative {
            .id = "id.monster.frost_wolf",
            .icon_url = "https://cdn.example/frost-wolf",
            .name = "Frost Wolf",
            .drop_rate = 0.019,
         },
         gv::api::SourceAlternative {
            .id = "id.monster.frost_skeleton",
            .name = "Frost Skeleton",
            .drop_rate = 0.012,
         },
      },
   };
   lookup.trade_chat.mentions_14d = 12;
   lookup.trade_chat.messages.push_back ({
      .message = "WTS test item for 500g",
      .observed_at = "2026-07-17T04:00:00+00:00",
      .age_seconds = 420,
      .items = {{ .name = "Test Item", .rarity = "epic" }},
   });

   gv::api::GemPlan gem;
   gem.projected_value = 620;
   gem.value_uplift = 208;
   gem.socket_fee = 25;
   gem.net_uplift = 183;
   gem.changes.push_back ({
      .replace_label = "Armor Penetration",
      .replace_value = "+4.1%",
      .gem_family = "diamond",
      .gem_icon_url = "https://cdn.darkerdb.com/codex/gem-icon",
      .new_label = "Action Speed",
      .new_value = "+2%",
   });
   lookup.gem_optimization.one_socket = gem;

   const json entity = gv::ui::augment::entity (lookup);
   EXPECT_FALSE (entity.contains ("eyebrow"));
   EXPECT_EQ (entity ["name"], "GrimVault");
   EXPECT_EQ (entity ["realm"], "grimvault");
   EXPECT_EQ (entity ["rarity"], "epic");

   const auto& sections = entity ["sections"];
   ASSERT_EQ (sections.size (), 1u);
   ASSERT_EQ (sections [0]["kind"], "analysis");
   const auto& analysis = sections [0];
   EXPECT_EQ (analysis ["item_name"], "Ruby Silver Ring");
   EXPECT_EQ (analysis ["item_rarity"], "epic");
   EXPECT_EQ (analysis ["item_artifact_type"], "minor");
   EXPECT_EQ (analysis ["pricing"]["median"], 412);
   EXPECT_EQ (analysis ["pricing"]["low"], 360);
   EXPECT_EQ (analysis ["pricing"]["high"], 500);
   EXPECT_EQ (analysis ["roll_score"], 82);
   EXPECT_EQ (analysis ["weighted_roll_score"], 88);
   EXPECT_EQ (analysis ["relative_percentile"], 76);
   EXPECT_EQ (analysis ["trade_chat"]["mentions_14d"], 12);
   EXPECT_EQ (analysis ["trade_chat"]["messages"][0]["age_seconds"], 420);
   EXPECT_EQ (analysis ["trade_chat"]["messages"][0]["items"][0]["name"], "Test Item");
   EXPECT_EQ (analysis ["trade_chat"]["messages"][0]["items"][0]["rarity"], "epic");
   EXPECT_EQ (analysis ["utility"]["max_stack_size"], 5);
   EXPECT_EQ (analysis ["utility"]["value_per_slot"], 206);
   EXPECT_EQ (analysis ["quests"][0]["merchant_name"], "Collector");
   EXPECT_EQ (analysis ["quests"][0]["quest_name"], "A Pound of Flesh");
   EXPECT_EQ (analysis ["quests"][0]["quest_index"], 4);
   EXPECT_EQ (analysis ["quests"][0]["quest_count"], 12);
   EXPECT_EQ (analysis ["quests"][0]["quantity"], 3);
   EXPECT_EQ (analysis ["recipes"][0]["merchant_name"], "Alchemist");
   EXPECT_EQ (analysis ["recipes"][0]["output"]["name"], "Iron Powder");
   EXPECT_EQ (analysis ["recipes"][0]["materials"][0]["name"], "Iron Ore");
   EXPECT_EQ (analysis ["recipes"][0]["materials"][0]["quantity"], 2);
   EXPECT_EQ (analysis ["recipes"][0]["materials"][0]["is_this"], true);
   EXPECT_EQ (analysis ["market"]["median_sale_seconds"], 2520);
   EXPECT_EQ (analysis ["market"]["sales_30d"], 28);
   EXPECT_EQ (analysis ["market"]["sales_capped"], false);
   EXPECT_EQ (analysis ["market"]["sales_window_hours"], 48);
   EXPECT_EQ (analysis ["market"]["active_listings"], 17);
   EXPECT_EQ (analysis ["market"]["days_supply"], 18.2);
   EXPECT_EQ (analysis ["market"]["price_stability"], "stable");
   EXPECT_EQ (analysis ["similar_sales"][0]["price"], 425);
   EXPECT_EQ (analysis ["similar_sales"][0]["similarity"], 94);
   EXPECT_EQ (analysis ["similar_sales"][0]["age_seconds"], 900);
   EXPECT_EQ (analysis ["similar_sales"][0]["sale_seconds"], 1800);
   EXPECT_EQ (analysis ["similar_sales"][0]["highlight_label"], "Magic Damage Bonus");
   EXPECT_EQ (analysis ["similar_sales"][0]["highlight_value"], "+4.8%");
   EXPECT_EQ (analysis ["similar_sales"][0]["rolls"].size (), 2);
   EXPECT_EQ (analysis ["similar_sales"][0]["rolls"][0]["attribute_id"], "magical_damage_bonus");
   EXPECT_EQ (analysis ["similar_sales"][0]["rolls"][1]["formatted_value"], "+3");
   EXPECT_EQ (analysis ["value_driver"]["gold_contribution"], 117);
   EXPECT_EQ (analysis ["source"]["name"], "Frost Skeleton Footman");
   EXPECT_EQ (analysis ["source"]["luck_drop_rate"], 0.041);
   ASSERT_EQ (analysis ["source"]["alternates"].size (), 2u);
   EXPECT_EQ (analysis ["source"]["alternates"][0]["name"], "Frost Wolf");
   EXPECT_EQ (analysis ["source"]["alternates"][0]["drop_rate"], 0.019);
   ASSERT_EQ (analysis ["rolls"].size (), 1u);
   EXPECT_EQ (analysis ["rolls"][0]["formatted_value"], "+4.1%");
   EXPECT_EQ (analysis ["rolls"][0]["minimum"], 3.0);
   EXPECT_EQ (analysis ["rolls"][0]["maximum"], 5.0);
   EXPECT_EQ (analysis ["rolls"][0]["grade"], "C");
   EXPECT_EQ (analysis ["rolls"][0]["gem"], "blue_sapphire");
   EXPECT_EQ (analysis ["rolls"][0]["gem_icon_url"],
      "https://cdn.darkerdb.com/codex/sapphire");
   ASSERT_EQ (analysis ["gem_plans"].size (), 1u);
   EXPECT_EQ (analysis ["gem_plans"][0]["sockets"], 1);
   EXPECT_EQ (analysis ["gem_plans"][0]["projected_value"], 620);
   EXPECT_EQ (analysis ["gem_plans"][0]["socket_fee"], 25);
   EXPECT_EQ (analysis ["gem_plans"][0]["net_uplift"], 183);
   EXPECT_EQ (analysis ["gem_plans"][0]["changes"][0]["new_label"], "Action Speed");
   EXPECT_EQ (analysis ["gem_plans"][0]["changes"][0]["gem_icon_url"],
      "https://cdn.darkerdb.com/codex/gem-icon");
}

TEST (AugmentPayload, AnalysisContentStaysStructuredForSdkEscaping)
{
   TooltipLookup lookup;
   lookup.item_id = "item.test";
   lookup.display_name = "Safe <Ring>";
   lookup.rarity = "rare";
   lookup.pricing.median = 1234;
   lookup.rolls.push_back ({
      .label = "Damage <script>",
      .slot = "secondary",
      .formatted_value = "+4 & 5",
   });

   const auto entity = gv::ui::augment::entity (lookup);
   const auto& analysis = entity ["sections"][0];
   EXPECT_EQ (analysis ["kind"], "analysis");
   EXPECT_EQ (analysis ["pricing"]["median"], 1234);
   EXPECT_EQ (analysis ["rolls"][0]["formatted_value"], "+4 & 5");
   EXPECT_EQ (analysis ["rolls"][0]["label"], "Damage <script>");
   EXPECT_FALSE (analysis.contains ("html"));
}

TEST (AugmentPayload, EmitsEveryServerRankedGemPlan)
{
   TooltipLookup lookup;
   lookup.item_id = "id.item.legendary_test_6001";
   lookup.display_name = "Legendary Test";

   for (int sockets = 1; sockets <= 4; ++sockets) {
      gv::api::GemPlan plan;
      plan.sockets = sockets;
      plan.projected_value = 1000 + sockets * 100;
      for (int index = 0; index < sockets; ++index) {
         plan.changes.push_back ({
            .replace_label = "Old " + std::to_string (index),
            .new_label = "New " + std::to_string (index),
         });
      }
      lookup.gem_optimization.plans.push_back (std::move (plan));
   }

   const auto analysis = gv::ui::augment::entity (lookup) ["sections"][0];
   ASSERT_EQ (analysis ["gem_plans"].size (), 4u);
   EXPECT_EQ (analysis ["gem_plans"][3]["sockets"], 4);
   EXPECT_EQ (analysis ["gem_plans"][3]["changes"].size (), 4u);
}

TEST (AugmentPayload, EmptySectionsOmitted)
{
   TooltipLookup l;
   l.canonical_name = "Torch";

   const json e = gv::ui::augment::entity (l);
   const auto& s = e ["sections"];

   ASSERT_EQ (s.size (), 1u);
   EXPECT_EQ (s [0]["kind"], "text");
   EXPECT_EQ (e ["rarity"], "common");
}

TEST (AugmentPayload, RecognizedTextIsTheCompleteBody)
{
   auto lookup = sample ();
   lookup.recognized_text = "Wizard Shoes\nArmor Rating 13\nRarity: Common";

   const json e = gv::ui::augment::entity (lookup);
   ASSERT_EQ (e ["sections"].size (), 1u);
   EXPECT_EQ (e ["sections"][0]["kind"], "text");
   EXPECT_EQ (e ["sections"][0]["body"], lookup.recognized_text);
   EXPECT_FALSE (e ["sections"][0].contains ("title"));
}

TEST (AugmentPayload, RenderMessageEnvelope)
{
   const json m = gv::ui::augment::render_message (sample (), 42);

   EXPECT_EQ (m ["type"], "render");
   EXPECT_EQ (m ["seq"], 42);
   EXPECT_EQ (m ["params"]["kind"], "augment");
   EXPECT_EQ (m ["params"]["compact"], true);
   EXPECT_TRUE (m ["entity"].is_object ());
}

// ---- Widget visibility -----------------------------------------------------
//
// The card's per-section visibility is the player's tooltip:analysis:*
// toggles intersected with what their plan grants. The tooltip library
// defaults an absent key to shown, so only explicit entries matter.

namespace {

   TooltipLookup analysis_sample ()
   {
      TooltipLookup l;
      l.item_id      = "item.test";
      l.display_name = "Ruby Silver Ring";
      l.rarity       = "epic";
      l.pricing.median = 412;
      return l;
   }

} // namespace

TEST (AugmentPayload, WidgetTogglesReachVisibleSections)
{
   gv::ui::augment::Options options;
   options.widgets = { { "market_value", true }, { "trade_chat", false } };

   const json e = gv::ui::augment::entity (analysis_sample (), options);
   const auto& visible = e ["sections"][0]["visible_sections"];

   EXPECT_EQ (visible ["market_value"], true);
   EXPECT_EQ (visible ["trade_chat"], false);
}

TEST (AugmentPayload, CurrencyDisplayReachesTheCard)
{
   gv::ui::augment::Options options;
   options.currency_display = "compact";

   const json e = gv::ui::augment::entity (analysis_sample (), options);
   EXPECT_EQ (e ["sections"][0]["currency_display"], "compact");
}

TEST (AugmentPayload, ThreeColumnChoiceReachesTheCard)
{
   gv::ui::augment::Options options;
   options.columns = "3";

   const json e = gv::ui::augment::entity (analysis_sample (), options);
   EXPECT_EQ (e ["sections"][0]["columns"], "3");
}

TEST (AugmentPayload, DefaultOptionsShowEverythingAtAbsolutePrices)
{
   const json e = gv::ui::augment::entity (analysis_sample ());

   EXPECT_TRUE (e ["sections"][0]["visible_sections"].empty ());
   EXPECT_EQ (e ["sections"][0]["currency_display"], "absolute");
}

// A widget the plan does not grant had its blocks stripped server-side by
// GrimVaultProjector, so leaving it visible would ask the renderer to draw
// a section with nothing in it.
TEST (AugmentPayload, LockedWidgetsAreHiddenEvenWhenToggledOn)
{
   auto lookup = analysis_sample ();
   lookup.entitlement.plan    = "free";
   lookup.entitlement.slot_limit = 2;
   lookup.entitlement.granted = { "market_value", "roll_quality" };
   lookup.entitlement.locked  = { { .widget = "trade_chat", .required_plan = "warlord" } };

   gv::ui::augment::Options options;
   options.widgets = { { "market_value", true }, { "trade_chat", true } };

   const json e = gv::ui::augment::entity (lookup, options);
   const auto& visible = e ["sections"][0]["visible_sections"];

   EXPECT_EQ (visible ["market_value"], true);
   EXPECT_EQ (visible ["trade_chat"], false);
}

// A locked widget with no toggle of its own still has to be suppressed.
TEST (AugmentPayload, LockedWidgetsWithoutAToggleAreHidden)
{
   auto lookup = analysis_sample ();
   lookup.entitlement.plan    = "free";
   lookup.entitlement.granted = { "market_value" };
   lookup.entitlement.locked  = { { .widget = "upgrade_paths", .required_plan = "champion" } };

   const json e = gv::ui::augment::entity (lookup, {});
   EXPECT_EQ (e ["sections"][0]["visible_sections"]["upgrade_paths"], false);
}

// An absent entitlement block (older server, or a fixture) must not hide
// everything: the projector is the real boundary.
TEST (AugmentPayload, MissingEntitlementGrantsEverything)
{
   gv::ui::augment::Options options;
   options.widgets = { { "trade_chat", true } };

   const json e = gv::ui::augment::entity (analysis_sample (), options);
   EXPECT_EQ (e ["sections"][0]["visible_sections"]["trade_chat"], true);
}
