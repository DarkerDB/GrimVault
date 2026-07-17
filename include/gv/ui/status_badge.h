#pragma once

#include <QPoint>
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

protected:
   // Qt sets its ex-styles on expose, after show(); re-apply the
   // non-activating click-through flags here so they stick.
   void exposeEvent (QExposeEvent* event) override;

private:
   QPoint applied_;
};

} // namespace gv::ui
