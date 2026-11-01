#include <gv/ocr/tooltip_filter.h>

#include <gtest/gtest.h>

using gv::capture::Rect;
using gv::ocr::looks_like_tooltip;
using gv::ocr::OverlayExclusion;
using namespace std::chrono_literals;

namespace {

const auto t0 = OverlayExclusion::Clock::time_point {} + 10s;

}

TEST (OverlayExclusion, NothingExcludedBeforeACardIsShown)
{
   OverlayExclusion exclusion;
   EXPECT_FALSE (exclusion.covers ({ 0, 0, 500, 500 }, 0, 0, t0));
}

// 2026-10-23 22:15:23: card for gen 26 (Bandage) at desktop 1167,891 526x497
// was detected 350 ms later as gen 27 and replaced the real tooltip.
TEST (OverlayExclusion, VisibleCardIsExcluded)
{
   OverlayExclusion exclusion;
   exclusion.update ({ 1167, 891, 526, 497 }, t0);
   EXPECT_TRUE (exclusion.covers ({ 1167, 891, 526, 497 }, 0, 0, t0 + 350ms));
   EXPECT_TRUE (exclusion.covers ({ 1180, 900, 500, 480 }, 0, 0, t0 + 350ms));
}

TEST (OverlayExclusion, FrameOriginIsApplied)
{
   OverlayExclusion exclusion;
   exclusion.update ({ 2560 + 300, 200, 526, 500 }, t0);
   EXPECT_TRUE (exclusion.covers ({ 300, 200, 526, 500 }, 2560, 0, t0));
   EXPECT_FALSE (exclusion.covers ({ 2560 + 300, 200, 526, 500 }, 2560, 0, t0));
}

TEST (OverlayExclusion, AdjacentTooltipIsKept)
{
   // The card sits beside the game's tooltip, so they may touch or overlap
   // a little; the tooltip itself must never be dropped.
   OverlayExclusion exclusion;
   exclusion.update ({ 1700, 600, 526, 600 }, t0);
   EXPECT_FALSE (exclusion.covers ({ 1207, 623, 506, 652 }, 0, 0, t0));
   EXPECT_FALSE (exclusion.covers ({ 1400, 623, 506, 652 }, 0, 0, t0));
}

TEST (OverlayExclusion, HiddenCardStaysExcludedForInFlightFrames)
{
   OverlayExclusion exclusion;
   exclusion.update ({ 100, 100, 526, 500 }, t0);
   exclusion.update ({}, t0 + 1s);
   EXPECT_TRUE (exclusion.covers ({ 100, 100, 526, 500 }, 0, 0, t0 + 1s));
   EXPECT_TRUE (
      exclusion.covers ({ 100, 100, 526, 500 }, 0, 0, t0 + 1s + OverlayExclusion::k_hide_grace));
   EXPECT_FALSE (exclusion.covers ({ 100, 100, 526, 500 }, 0, 0, t0 + 1s + 151ms));
}

TEST (OverlayExclusion, RepeatedHideKeepsFirstHideTime)
{
   OverlayExclusion exclusion;
   exclusion.update ({ 100, 100, 526, 500 }, t0);
   exclusion.update ({}, t0);
   exclusion.update ({}, t0 + 1s);
   EXPECT_FALSE (exclusion.covers ({ 100, 100, 526, 500 }, 0, 0, t0 + 500ms));
}

TEST (OverlayExclusion, MovedCardIsTracked)
{
   OverlayExclusion exclusion;
   exclusion.update ({ 100, 100, 526, 500 }, t0);
   exclusion.update ({ 900, 100, 526, 500 }, t0 + 8ms);
   EXPECT_TRUE (exclusion.covers ({ 900, 100, 526, 500 }, 0, 0, t0 + 8ms));
   EXPECT_FALSE (exclusion.covers ({ 100, 100, 526, 500 }, 0, 0, t0 + 8ms));
}

// OCR text the API rejected on 2026-10-23, all from non-tooltip detections.
TEST (LooksLikeTooltip, RejectsSingleLines)
{
   EXPECT_FALSE (looks_like_tooltip (""));
   EXPECT_FALSE (looks_like_tooltip ("olKlft"));
   EXPECT_FALSE (looks_like_tooltip ("BB-"));
   EXPECT_FALSE (looks_like_tooltip ("s items from all inventory and st"));
   EXPECT_FALSE (looks_like_tooltip ("Frock\n"));
   EXPECT_FALSE (looks_like_tooltip ("\n  \nFrock\n \n"));
}

// Shortest real tooltips in the prod collection: OCR cut them to two lines.
TEST (LooksLikeTooltip, AcceptsCutShortRealTooltips)
{
   EXPECT_TRUE (looks_like_tooltip ("Slim Bangle (Perfect)\nSlot Type: Invalid"));
   EXPECT_TRUE (looks_like_tooltip ("Frock\nArmor Rating 5\nMagic Resistance 26"));
   EXPECT_TRUE (looks_like_tooltip ("钛金巨盾\n护甲值 43\n移動速度 -45"));
}

TEST (RejectedTooltips, RejectsRepeatedButtonButKeepsChangedContent)
{
   gv::ocr::RejectedTooltips rejected;
   gv::ocr::TooltipObservation button;
   button.box = { 226, 866, 341, 84 };
   rejected.reject (button, t0);
   EXPECT_TRUE (rejected.contains (button, 16, 8, t0 + 100ms));

   auto item = button;
   item.identity.bits.fill (0xffffffffffffffffull);
   EXPECT_FALSE (rejected.contains (item, 16, 8, t0 + 100ms));

   auto shoes = button;
   shoes.box = { 1294, 685, 518, 542 };
   EXPECT_FALSE (rejected.contains (shoes, 16, 8, t0 + 100ms));
}

TEST (RejectedTooltips, RetriesTransientIncompleteOcrAfterTwoSeconds)
{
   gv::ocr::RejectedTooltips rejected;
   gv::ocr::TooltipObservation observation;
   observation.box = { 100, 100, 500, 400 };
   rejected.reject (observation, t0);
   EXPECT_TRUE (rejected.contains (observation, 16, 8, t0 + 1999ms));
   EXPECT_FALSE (rejected.contains (observation, 16, 8, t0 + 2s));
}

TEST (RejectedTooltips, BoundedCacheEvictsOldestObservation)
{
   gv::ocr::RejectedTooltips rejected;
   gv::ocr::TooltipObservation observation;
   observation.box = { 100, 100, 500, 400 };

   for (std::size_t index = 0; index <= gv::ocr::RejectedTooltips::k_capacity; ++index) {
      observation.box.w = 500 + static_cast<int> (index) * 20;
      rejected.reject (observation, t0);
   }

   EXPECT_TRUE (rejected.contains (observation, 16, 8, t0));
   observation.box.w = 500;
   EXPECT_FALSE (rejected.contains (observation, 16, 8, t0));
}
