#pragma once

#include <QPoint>
#include <QRasterWindow>
#include <QRect>
#include <QSize>

#include <memory>

namespace gv::ui {

// Debug-mode visualization: a transparent, click-through, topmost window
// covering the capture region (the game window). Draws a border around the
// region itself and around the anchored tooltip.
//
// The tooltip box is the anchoring presenter (docs/architecture/anchoring.md
// §3): given the pipeline's anchor (cursor->tooltip offset + exact size), a
// 120 Hz timer draws the box at clamp (cursor + offset) — pure cursor math,
// pinned at the game's clamp edges exactly like the real tooltip, no
// capture latency in the loop.
//
//    ┌ capture region ──────────────────┐
//    │                                  │
//    │        ┌ tooltip ┐               │
//    │        └─────────┘               │
//    └──────────────────────────────────┘
class DebugOverlay : public QRasterWindow
{
   Q_OBJECT

public:
   DebugOverlay ();
   ~DebugOverlay () override;

   // Game window bounds in physical (Win32) screen pixels; also controls
   // visibility (enabled + non-empty bounds -> shown).
   void set_region (const QRect& physical_bounds, bool game_visible);

   // Anchor established or updated (offset in physical px, tooltip
   // top-left minus cursor; size exact from the refiner).
   void set_anchor (const QPoint& offset, const QSize& size,
                    bool pinned_x, bool pinned_y, const QPoint& pin);

   // Anchor lost: hide the box immediately.
   void clear_anchor ();

   // Select which diagnostic outlines are permitted to render.
   void set_highlights (bool game, bool objects);
   void set_enabled (bool on);

protected:
   void paintEvent  (QPaintEvent* event) override;
   void exposeEvent (QExposeEvent* event) override;

private:
   void refresh ();

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ui
