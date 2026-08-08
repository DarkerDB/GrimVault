#pragma once

#include <QQuickView>
#include <QRect>

namespace gv::ui {

// Tiny frameless click-through badge pinned to the bottom-right corner of
// the game window. Shows sign-in state (dot color), scan mode (label) and
// pulses on pipeline activity so the player can tell GrimVault is alive
// without leaving the game.
class StatusBadge : public QQuickView
{
   Q_OBJECT

public:
   explicit StatusBadge (QWindow* parent = nullptr);

public slots:
   void set_auto      (bool is_auto);
   void set_signed_in (bool signed_in);
   void set_game      (const QRect& bounds, bool active);
   void pulse         ();
};

} // namespace gv::ui
