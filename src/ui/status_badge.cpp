#include <gv/ui/status_badge.h>
#include <gv/ui/screen.h>

#include <QExposeEvent>
#include <QQuickItem>

namespace gv::ui {

namespace {

   constexpr int k_margin = 16;

} // namespace

StatusBadge::StatusBadge (QWindow* parent)
   : QQuickView (parent)
{
   setFlags (
      Qt::FramelessWindowHint
    | Qt::WindowStaysOnTopHint
    | Qt::Tool
    | Qt::WindowTransparentForInput
   );

   setColor (Qt::transparent);
   setResizeMode (QQuickView::SizeViewToRootObject);

   setSource (QUrl (QStringLiteral ("qrc:/qml/StatusBadge.qml")));

   hide ();
}

void StatusBadge::set_auto (bool is_auto)
{
   if (auto* root = rootObject ()) root->setProperty ("isAuto", is_auto);
}

void StatusBadge::set_locale (const std::string& locale)
{
   if (auto* root = rootObject ()) {
      root->setProperty ("locale", QString::fromStdString (locale));
   }
}

void StatusBadge::set_signed_in (bool signed_in)
{
   if (auto* root = rootObject ()) root->setProperty ("isSignedIn", signed_in);
}

void StatusBadge::set_enabled (bool enabled)
{
   if (enabled == enabled_) return;
   enabled_ = enabled;

   if (!enabled_) {
      hide ();
      return;
   }

   set_game (game_, game_active_);
}

void StatusBadge::set_game (const QRect& bounds, bool active)
{
   // Recorded even while hidden so set_enabled can replay the current game
   // window instead of waiting for the next one.
   game_        = bounds;
   game_active_ = active;

   if (!active || !enabled_) {
      hide ();
      return;
   }

   // bounds arrive as Win32 physical pixels; width()/height() are logical.
   const qreal s = screen::scale_at (bounds.center ());

   const QPoint target {
      bounds.right ()  - qRound ((width ()  + k_margin) * s),
      bounds.bottom () - qRound ((height () + k_margin) * s)
   };

   // Idempotent: window events repeat identical bounds; re-moving and
   // re-showing every time storms expose events across the other topmost
   // windows (debug overlay, augment).
   if (target == applied_ && isVisible ()) return;
   applied_ = target;

   screen::move (this, target);
   if (!isVisible ()) show ();
}

void StatusBadge::exposeEvent (QExposeEvent* event)
{
   QQuickView::exposeEvent (event);
   if (isExposed ()) screen::make_passthrough (this);
}

void StatusBadge::pulse ()
{
   if (auto* root = rootObject ()) {
      QMetaObject::invokeMethod (root, "pulse");
   }
}

} // namespace gv::ui
