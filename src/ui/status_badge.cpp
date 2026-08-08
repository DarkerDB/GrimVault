#include <gv/ui/status_badge.h>
#include <gv/ui/screen.h>

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

void StatusBadge::set_signed_in (bool signed_in)
{
   if (auto* root = rootObject ()) root->setProperty ("isSignedIn", signed_in);
}

void StatusBadge::set_game (const QRect& bounds, bool active)
{
   if (!active) {
      hide ();
      return;
   }

   // bounds arrive as Win32 physical pixels; width()/height() are logical.
   const qreal s = screen::scale_at (bounds.center ());

   screen::move (this, {
      bounds.right ()  - qRound ((width ()  + k_margin) * s),
      bounds.bottom () - qRound ((height () + k_margin) * s)
   });
   show ();
}

void StatusBadge::pulse ()
{
   if (auto* root = rootObject ()) {
      QMetaObject::invokeMethod (root, "pulse");
   }
}

} // namespace gv::ui
