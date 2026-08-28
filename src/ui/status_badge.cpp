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

   // set_locale rewidths the QML label and SizeViewToRootObject resizes the
   // view after the fact; re-pin so the bottom-right margin survives a wider
   // locale ("(zh-Hans)" vs "(en)").
   connect (this, &QWindow::widthChanged,  this, [this] { apply_placement (); });
   connect (this, &QWindow::heightChanged, this, [this] { apply_placement (); });

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
   apply_placement ();
}

void StatusBadge::apply_placement ()
{
   if (!game_active_ || !enabled_) {
      hide ();
      return;
   }

   // bounds arrive as Win32 physical pixels; width()/height() are logical.
   const qreal s = screen::scale_at (game_.center ());

   const QPoint target {
      game_.right ()  - qRound ((width ()  + k_margin) * s),
      game_.bottom () - qRound ((height () + k_margin) * s)
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
