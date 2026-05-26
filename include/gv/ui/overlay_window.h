#pragma once

#include <QQuickView>

#include <memory>

namespace gv::api { struct TooltipLookup; }

namespace gv::ui {

// Frameless transparent always-on-top QML overlay that draws the
// Diablo-esque tooltip on top of the game. The tooltip payload comes from
// gv::api::TooltipLookup — parsing happens server-side, the client just
// renders.
class OverlayWindow : public QQuickView
{
   Q_OBJECT

public:
   explicit OverlayWindow (QWindow* parent = nullptr);
   ~OverlayWindow () override;

   // Position the overlay near a screen point and present a new item.
   void present (const gv::api::TooltipLookup& lookup, int screen_x, int screen_y);

   // Hide the overlay (cleared on focus loss, mode disabled, etc.).
   void clear ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ui
