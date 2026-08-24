#include <gv/api/darkerdb_client.h>

#include <gtest/gtest.h>

TEST (AnalysisDiagnostic, IncludesResolvedIdentityRollsAndPricing)
{
   gv::api::TooltipLookup lookup;
   lookup.item_id = "id.item.cloth_pants_4001";
   lookup.canonical_name = "Cloth Pants";
   lookup.display_name = "布パンツ";
   lookup.language = "ja";
   lookup.rarity = "rare";
   lookup.match_confidence = 1.0;
   lookup.rolls.push_back ({
      .attribute_id = "dexterity",
      .label = "手腕",
      .value = 5,
      .formatted_value = "+5",
      .minimum = 1,
      .maximum = 3,
      .roll_percentile = 100,
   });
   lookup.pricing.currency = "gold";
   lookup.pricing.low = 200;
   lookup.pricing.median = 210;
   lookup.pricing.high = 268;
   lookup.pricing.sample_size = 149;
   lookup.market_analysis.median_sale_price = 500;
   lookup.utility.gear_score = 20;

   const auto data = gv::api::diagnostic (lookup);

   EXPECT_EQ (data ["item"]["rarity"], "rare");
   EXPECT_EQ (data ["item"]["canonical_name"], "Cloth Pants");
   EXPECT_EQ (data ["item"]["display_name"], "布パンツ");
   EXPECT_EQ (data ["rolls"][0]["attribute"], "dexterity");
   EXPECT_EQ (data ["rolls"][0]["minimum"], 1);
   EXPECT_EQ (data ["rolls"][0]["maximum"], 3);
   EXPECT_EQ (data ["valuation"]["fair_value"], 210);
   EXPECT_EQ (data ["market"]["median_sale_price"], 500);
   EXPECT_EQ (data ["utility"]["gear_score"], 20);
}
