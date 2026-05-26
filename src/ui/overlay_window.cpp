#include <gv/ui/overlay_window.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>

#include <QQmlContext>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace gv::ui {

namespace {

   QStringList to_lines (const std::vector<gv::api::TooltipAttribute>& attrs)
   {
      QStringList out;
      out.reserve (static_cast<int> (attrs.size ()));
      for (const auto& a : attrs) {
         out << QStringLiteral ("%1: %2").arg (
            QString::fromStdString (a.label),
            QString::fromStdString (a.value));
      }
      return out;
   }

} // namespace

struct OverlayWindow::Impl
{
   bool visible = false;
};

OverlayWindow::OverlayWindow (QWindow* parent)
   : QQuickView (parent), impl_ (std::make_unique<Impl> ())
{
   setFlags (
      Qt::FramelessWindowHint
    | Qt::WindowStaysOnTopHint
    | Qt::Tool
    | Qt::WindowTransparentForInput
   );

   setColor (Qt::transparent);
   setResizeMode (QQuickView::SizeRootObjectToView);

   setSource (QUrl (QStringLiteral ("qrc:/qml/Tooltip.qml")));

   resize (320, 200);
   hide ();
}

OverlayWindow::~OverlayWindow () = default;

void OverlayWindow::present (const gv::api::TooltipLookup& lookup, int screen_x, int screen_y)
{
   auto* root = rootObject ();
   if (!root) {
      core::Logger::warn ("overlay: rootObject null; QML failed to load");
      return;
   }

   root->setProperty ("title",     QString::fromStdString (lookup.canonical_name));
   root->setProperty ("rarity",    QString::fromStdString (lookup.rarity));
   root->setProperty ("primary",   to_lines (lookup.primary));
   root->setProperty ("secondary", to_lines (lookup.secondary));
   root->setProperty ("details",   to_lines (lookup.details));
   root->setProperty ("low",       static_cast<qlonglong> (lookup.pricing.low));
   root->setProperty ("median",    static_cast<qlonglong> (lookup.pricing.median));
   root->setProperty ("high",      static_cast<qlonglong> (lookup.pricing.high));

   setPosition (screen_x, screen_y);

   if (!impl_->visible) {
      show ();
      impl_->visible = true;
   }
}

void OverlayWindow::clear ()
{
   if (impl_->visible) {
      hide ();
      impl_->visible = false;
   }
}

} // namespace gv::ui
