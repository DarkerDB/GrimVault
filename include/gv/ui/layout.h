#pragma once

namespace gv::ui {

// Dashboard-controlled placement and appearance of the Augment card.
// Mirrors the overlay:* settings group; the renderer re-reads it on every
// placement, so a change lands on the next hover with no restart.
//
// Deliberately Qt-free and in its own header: gv::app::Preferences folds the
// synced settings into one of these, and that fold is unit-tested without an
// event loop or a WebView2 runtime.
struct Layout
{
   // attached  — dock beside the anchored in-game tooltip (the default).
   // corner    — pin to a fixed corner of the game window, ignoring the
   //             anchor. Useful on ultrawides where the tooltip sits far
   //             from where the player is looking.
   enum class Align { Attached, TopLeft, TopRight, BottomLeft, BottomRight };

   // How many columns the card lays its widgets out in.
   //
   // Auto is the default and the reason this isn't a plain flag: extra
   // columns exist to stop a tall card being shrunk to fit the screen, and
   // the renderer already computes that shrink. So auto can decide exactly
   // rather than guess — and a player running three widgets, who never had
   // the problem, never sees the card change shape.
   enum class Columns { Auto, One, Two, Three };

   Align   align   = Align::Attached;
   Columns columns = Columns::Auto;
   double opacity  = 0.9;   // 0..1, multiplied into the card's own fade-in
   double scale    = 1.0;   // on top of the monitor's DPI scale
   int    offset_x = 20;    // nudge in CSS px, applied after placement
   int    offset_y = 20;

   // overlay:mode == "disabled". The pipeline keeps running (the tray still
   // reports scans); nothing is ever drawn.
   bool   enabled  = true;
};

} // namespace gv::ui
