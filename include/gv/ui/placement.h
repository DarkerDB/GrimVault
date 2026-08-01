#pragma once

#include <gv/ui/layout.h>

#include <QPoint>
#include <QRect>
#include <QSize>

#include <algorithm>

namespace gv::ui::placement {

// Where the Augment card goes, as pure geometry.
//
// Extracted from AugmentView because every bug this has had was a geometry
// bug — the card sliding over the tooltip it annotates, or off the edge of
// the screen — and none of them were reachable from a test while the maths
// lived inside a WebView2-backed presenter. Physical pixels throughout.

// Fit factor for a card that would overflow the viewport. 1.0 when it
// already fits. `chrome` is the transparent animation headroom, which is on
// every edge and scales with the card, so it counts against both budgets.
//
// Floored: past roughly half size the card is unreadable, and a little
// overflow beats rendering something nobody can read.
inline double fit (const QRect& viewport, const QSize& card_css,
                   int chrome_css, double scale, double floor = 0.5)
{
   if (card_css.width () <= 0 || card_css.height () <= 0) return 1.0;
   if (viewport.width () <= 0 || viewport.height () <= 0) return 1.0;

   const double w = (card_css.width ()  + chrome_css) * scale;
   const double h = (card_css.height () + chrome_css) * scale;
   if (w <= 0.0 || h <= 0.0) return 1.0;

   const double factor = std::min ({ 1.0, viewport.width () / w, viewport.height () / h });
   return factor >= 1.0 ? 1.0 : std::max (factor, floor);
}

// Dock the card beside the in-game tooltip, top edges aligned.
//
// Left is the house style. A tooltip near the left edge leaves no room
// there, and clamping into the viewport would slide the card straight over
// the tooltip — so flip to the right instead. Only when neither side fits
// does it fall back to the roomier edge, because at that point overlapping
// something is unavoidable and the wider gap hides less.
inline QPoint attached (const QRect& viewport, const QRect& anchor,
                        const QSize& card, int gap)
{
   const int left  = anchor.x () - gap - card.width ();
   const int right = anchor.x () + anchor.width () + gap;

   int x = 0;
   if (left >= viewport.x ()) {
      x = left;
   } else if (right + card.width () <= viewport.x () + viewport.width ()) {
      x = right;
   } else {
      const int room_left  = anchor.x () - viewport.x ();
      const int room_right = viewport.x () + viewport.width () - (anchor.x () + anchor.width ());
      x = room_left >= room_right ? viewport.x ()
                                  : viewport.x () + viewport.width () - card.width ();
   }

   return { x, anchor.y () };
}

// Pin to a corner of the viewport, inset by the configured offset.
inline QPoint corner (const QRect& viewport, const QSize& card,
                      Layout::Align align, int offset_x, int offset_y)
{
   const int rightmost  = viewport.x () + viewport.width ()  - card.width ();
   const int bottommost = viewport.y () + viewport.height () - card.height ();

   switch (align) {
      case Layout::Align::TopRight:    return { rightmost - offset_x, viewport.y () + offset_y };
      case Layout::Align::BottomLeft:  return { viewport.x () + offset_x, bottommost - offset_y };
      case Layout::Align::BottomRight: return { rightmost - offset_x, bottommost - offset_y };
      case Layout::Align::TopLeft:
      case Layout::Align::Attached:    break;
   }
   return { viewport.x () + offset_x, viewport.y () + offset_y };
}

// Keep a rect of `size` inside the viewport. Prefers the requested origin,
// gives up on the top-left edge last so a card larger than the viewport
// still shows its head rather than its tail.
inline QPoint clamp (const QRect& viewport, const QPoint& origin, const QSize& size)
{
   return {
      std::max (viewport.x (), std::min (origin.x (), viewport.x () + viewport.width ()  - size.width ())),
      std::max (viewport.y (), std::min (origin.y (), viewport.y () + viewport.height () - size.height ())),
   };
}

} // namespace gv::ui::placement
