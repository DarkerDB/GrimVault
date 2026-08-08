#include <gv/ui/augment_payload.h>

#include <gtest/gtest.h>

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

   EXPECT_EQ (e ["name"], "<GrimVault>");
   EXPECT_EQ (e ["rarity"], "rare");
}

TEST (AugmentPayload, SectionsInOrder)
{
   const json e = gv::ui::augment::entity (sample ());
   const auto& s = e ["sections"];

   ASSERT_EQ (s.size (), 5u);
   EXPECT_EQ (s [0]["kind"], "text");
   EXPECT_EQ (s [0]["title"], "Ruby Silver Ring");
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

TEST (AugmentPayload, RenderMessageEnvelope)
{
   const json m = gv::ui::augment::render_message (sample (), 42);

   EXPECT_EQ (m ["type"], "render");
   EXPECT_EQ (m ["seq"], 42);
   EXPECT_EQ (m ["params"]["kind"], "augment");
   EXPECT_EQ (m ["params"]["compact"], true);
   EXPECT_TRUE (m ["entity"].is_object ());
}
