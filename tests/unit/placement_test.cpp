#include <gv/ui/placement.h>

#include <gtest/gtest.h>

using gv::ui::Layout;
namespace place = gv::ui::placement;

namespace {

   // The desk this was reported from: a 3440x1440 ultrawide, game borderless
   // on it, second monitor to the left of the origin.
   constexpr int k_view_w = 3440;
   constexpr int k_view_h = 1440;

   QRect view (int x = 0) { return QRect { x, 0, k_view_w, k_view_h }; }

} // namespace

TEST (Placement, DocksToTheLeftOfTheTooltip)
{
   const QRect anchor { 1800, 400, 520, 580 };
   const QSize card   { 600, 900 };

   const QPoint at = place::attached (view (), anchor, card, 12);

   EXPECT_EQ (at.x () + card.width (), anchor.x () - 12);
   EXPECT_EQ (at.y (), anchor.y ());
}

// The bug: a tooltip near the left edge left no room on the left, and the
// old code clamped into the viewport — sliding the card over the tooltip it
// exists to annotate.
TEST (Placement, FlipsRightWhenThereIsNoRoomOnTheLeft)
{
   const QRect anchor { 40, 400, 520, 580 };
   const QSize card   { 600, 900 };

   const QPoint at = place::attached (view (), anchor, card, 12);

   EXPECT_EQ (at.x (), anchor.x () + anchor.width () + 12);
   EXPECT_GE (at.x (), anchor.x () + anchor.width ())
      << "card must not cover the tooltip";
}

TEST (Placement, NeverOverlapsTheTooltipWhenEitherSideFits)
{
   const QSize card { 600, 900 };

   for (int tip_x = 0; tip_x <= k_view_w - 520; tip_x += 20) {
      const QRect anchor { tip_x, 400, 520, 580 };
      const QPoint at = place::attached (view (), anchor, card, 12);
      const QRect placed { at, card };

      // One side or the other always has room for a 600px card beside a
      // 520px tooltip on a 3440px viewport, so overlap is never forced.
      EXPECT_FALSE (placed.intersects (anchor))
         << "overlap at tip_x=" << tip_x << " placed x=" << at.x ();
   }
}

TEST (Placement, PicksTheRoomierSideWhenNeitherFits)
{
   const QRect anchor { 100, 400, 3200, 580 };   // tooltip nearly fills the width
   const QSize card   { 600, 900 };

   const QPoint at = place::attached (view (), anchor, card, 12);

   // room_left = 100, room_right = 3440 - 3300 = 140 -> right edge wins.
   EXPECT_EQ (at.x (), k_view_w - card.width ());
}

// Placement is in physical desktop coordinates, so a monitor that does not
// start at x=0 must not pull the card back to the origin.
TEST (Placement, RespectsAViewportOffOrigin)
{
   const QRect anchor { 4800, 400, 520, 580 };   // mid-screen on the right monitor
   const QSize card   { 600, 900 };

   const QPoint at = place::attached (view (3440), anchor, card, 12);

   EXPECT_GE (at.x (), 3440);
   EXPECT_EQ (at.x () + card.width (), anchor.x () - 12);
}

// "No room on the left" is measured against the VIEWPORT edge, not x=0 — on
// the right-hand monitor of a two-monitor desk there is plenty of desktop to
// the left, and none of it is visible from that screen.
TEST (Placement, FlipsRightAtTheLeftEdgeOfAnOffOriginViewport)
{
   const QRect anchor { 3800, 400, 520, 580 };   // only 360px of screen to its left
   const QSize card   { 600, 900 };

   const QPoint at = place::attached (view (3440), anchor, card, 12);

   EXPECT_EQ (at.x (), anchor.x () + anchor.width () + 12);
   EXPECT_FALSE (QRect (at, card).intersects (anchor));
}

TEST (Placement, ClampKeepsTheWindowInside)
{
   const QSize card { 600, 900 };

   EXPECT_EQ (place::clamp (view (), { -200, -50 }, card), (QPoint { 0, 0 }));
   EXPECT_EQ (place::clamp (view (), { 9000, 9000 }, card),
              (QPoint { k_view_w - 600, k_view_h - 900 }));
   EXPECT_EQ (place::clamp (view (), { 100, 100 }, card), (QPoint { 100, 100 }));
}

// A card taller than the screen should show its head, not its tail.
TEST (Placement, ClampPrefersTheTopLeftForAnOversizedCard)
{
   const QSize huge { 4000, 2000 };
   EXPECT_EQ (place::clamp (view (), { 300, 300 }, huge), (QPoint { 0, 0 }));
}

TEST (Placement, FitIsOneWhenTheCardAlreadyFits)
{
   EXPECT_DOUBLE_EQ (place::fit (view (), QSize { 600, 900 }, 40, 1.0), 1.0);
}

TEST (Placement, FitShrinksToTheViewportHeight)
{
   // 1400 + 40 chrome = 1440 tall at scale 1.5 -> 2160 physical, viewport 1440.
   const double factor = place::fit (view (), QSize { 1400, 1400 }, 40, 1.5);

   EXPECT_LT (factor, 1.0);
   EXPECT_NEAR ((1400 + 40) * 1.5 * factor, k_view_h, 1.0);
}

TEST (Placement, FitShrinksToTheViewportWidthToo)
{
   const QRect narrow { 0, 0, 500, 4000 };
   const double factor = place::fit (narrow, QSize { 900, 400 }, 0, 1.0);

   EXPECT_NEAR (900 * factor, 500, 1.0);
}

// Past roughly half size the card is unreadable; a little overflow beats
// rendering something nobody can read.
TEST (Placement, FitIsFloored)
{
   const QRect tiny { 0, 0, 100, 100 };
   EXPECT_DOUBLE_EQ (place::fit (tiny, QSize { 4000, 4000 }, 0, 1.0), 0.5);
}

TEST (Placement, FitIgnoresDegenerateInput)
{
   EXPECT_DOUBLE_EQ (place::fit (view (), QSize { 0, 0 }, 0, 1.0), 1.0);
   EXPECT_DOUBLE_EQ (place::fit (QRect {}, QSize { 600, 900 }, 0, 1.0), 1.0);
}

TEST (Placement, CornersInsetFromTheViewportEdges)
{
   const QSize card { 600, 900 };

   EXPECT_EQ (place::corner (view (), card, Layout::Align::TopLeft, 20, 30),
              (QPoint { 20, 30 }));
   EXPECT_EQ (place::corner (view (), card, Layout::Align::TopRight, 20, 30),
              (QPoint { k_view_w - 600 - 20, 30 }));
   EXPECT_EQ (place::corner (view (), card, Layout::Align::BottomLeft, 20, 30),
              (QPoint { 20, k_view_h - 900 - 30 }));
   EXPECT_EQ (place::corner (view (), card, Layout::Align::BottomRight, 20, 30),
              (QPoint { k_view_w - 600 - 20, k_view_h - 900 - 30 }));
}

TEST (Placement, CornersRespectAViewportOffOrigin)
{
   const QSize card { 600, 900 };

   EXPECT_EQ (place::corner (view (3440), card, Layout::Align::TopLeft, 20, 30),
              (QPoint { 3460, 30 }));
}
