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
      .minimum = 3.0,
      .maximum = 5.0,
      .roll_percentile = 55,
      .grade = "C",
   });
   lookup.market_analysis.sales_30d = 28;
   lookup.market_analysis.active_listings = 17;
   lookup.market_analysis.trend_percent = 8.1;
   lookup.market_analysis.median_sale_seconds = 2520;
   lookup.market_analysis.days_supply = 18.2;
   lookup.market_analysis.price_stability = "stable";
   lookup.market_analysis.liquidity = "fast";
   lookup.utility.used_in_recipes = 3;
   lookup.utility.max_stack_size = 5;
   lookup.utility.value_per_slot = 206;
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
   EXPECT_EQ (entity ["name"], "GrimVault Analysis");
   EXPECT_EQ (entity ["rarity"], "epic");

   const auto& sections = entity ["sections"];
   ASSERT_EQ (sections.size (), 1u);
   ASSERT_EQ (sections [0]["kind"], "analysis");
   const auto& analysis = sections [0];
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
   EXPECT_EQ (analysis ["market"]["median_sale_seconds"], 2520);
   EXPECT_EQ (analysis ["market"]["days_supply"], 18.2);
   EXPECT_EQ (analysis ["market"]["price_stability"], "stable");
   EXPECT_EQ (analysis ["value_driver"]["gold_contribution"], 117);
   EXPECT_EQ (analysis ["source"]["name"], "Frost Skeleton Footman");
   EXPECT_EQ (analysis ["source"]["luck_drop_rate"], 0.041);
   ASSERT_EQ (analysis ["rolls"].size (), 1u);
   EXPECT_EQ (analysis ["rolls"][0]["formatted_value"], "+4.1%");
   EXPECT_EQ (analysis ["rolls"][0]["minimum"], 3.0);
   EXPECT_EQ (analysis ["rolls"][0]["maximum"], 5.0);
   EXPECT_EQ (analysis ["rolls"][0]["grade"], "C");
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
