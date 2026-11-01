#include <gv/api/darkerdb_client.h>
#include <gv/api/price_check_url.h>

#include <QUrl>
#include <QUrlQuery>
#include <gtest/gtest.h>

#include <limits>

TEST (PriceCheckUrl, PreservesExactRollsSlotsOrderAndGems)
{
   gv::api::TooltipLookup lookup;
   lookup.item_id = "id.item.rondel_dagger_6001";
   lookup.rolls = {
      { .attribute_id = "weapon_damage", .slot = "primary", .value = 24 },
      { .attribute_id = "magical_damage_bonus",
        .slot = "secondary",
        .value = 2.7,
        .formatted_value = "+2.7%" },
      { .attribute_id = "max_health", .slot = "secondary", .value = 3, .gem = "ruby" },
      { .attribute_id = "move_speed", .slot = "primary", .value = -10 },
      { .attribute_id = "weapon_damage", .slot = "secondary", .value = 1 },
   };

   const QUrl url { QString::fromStdString (gv::api::price_check_path (lookup)) };
   const QUrlQuery query { url };
   EXPECT_EQ (url.path (), "/tools/price-check");
   EXPECT_EQ (query.queryItemValue ("item"), "id.item.rondel_dagger_6001");
   EXPECT_EQ (query.queryItemValue ("primary[weapon_damage]"), "24");
   EXPECT_EQ (query.queryItemValue ("secondary[weapon_damage]"), "1");
   EXPECT_EQ (query.queryItemValue ("secondary[magical_damage_bonus]"), "2.7");
   EXPECT_EQ (query.queryItemValue ("primary[move_speed]"), "-10");
   EXPECT_EQ (query.queryItemValue ("secondary[max_health]"), "3");
   EXPECT_EQ (query.queryItemValue ("order"), "magical_damage_bonus,max_health,weapon_damage");
   EXPECT_EQ (query.queryItemValue ("gems"), "max_health:ruby");
}

TEST (PriceCheckUrl, HandlesItemsWithoutRollsAndMissingItems)
{
   gv::api::TooltipLookup lookup;
   EXPECT_TRUE (gv::api::price_check_path (lookup).empty ());
   lookup.item_id = "id.item.bandage_1001";
   EXPECT_EQ (gv::api::price_check_path (lookup), "/tools/price-check?item=id.item.bandage_1001");
}

TEST (PriceCheckUrl, EncodesQueryValuesAndSkipsUnsupportedRolls)
{
   gv::api::TooltipLookup lookup;
   lookup.item_id = "item&other=value#fragment";
   lookup.rolls = {
      { .attribute_id = "unknown", .slot = "details", .value = 1 },
      { .attribute_id = "", .slot = "secondary", .value = 1 },
      { .attribute_id = "invalid",
        .slot = "primary",
        .value = std::numeric_limits<double>::infinity () },
      { .attribute_id = "zero", .slot = "secondary", .value = 0 },
   };
   const QUrl url { QString::fromStdString (gv::api::price_check_path (lookup)) };
   const QUrlQuery query { url };
   EXPECT_EQ (query.queryItemValue ("item", QUrl::FullyDecoded),
              QString::fromStdString (lookup.item_id));
   EXPECT_TRUE (url.fragment ().isEmpty ());
   EXPECT_FALSE (query.hasQueryItem ("other"));
   EXPECT_FALSE (query.hasQueryItem ("primary[invalid]"));
   EXPECT_FALSE (query.hasQueryItem ("details[unknown]"));
   EXPECT_EQ (query.queryItemValue ("secondary[zero]"), "0");
}
