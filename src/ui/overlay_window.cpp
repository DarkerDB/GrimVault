#include <gv/ui/overlay_window.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>
#include <gv/ui/augment_view.h>
#include <gv/ui/screen.h>

#include <QPointer>
#include <QQuickItem>
#include <QQuickView>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <algorithm>

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

   // Legacy renderer: the QML port of the card. Fallback only — deleted
   // once the WebView2 path has shipped a release.
   class QmlTooltip : public QQuickView
   {
   public:
      QmlTooltip ()
      {
         setFlags (
            Qt::FramelessWindowHint
          | Qt::WindowStaysOnTopHint
          | Qt::Tool
          | Qt::WindowTransparentForInput
         );

         setColor (Qt::transparent);
         setResizeMode (QQuickView::SizeViewToRootObject);

         setSource (QUrl (QStringLiteral ("qrc:/qml/Tooltip.qml")));

         hide ();
      }

      void present (const gv::api::TooltipLookup& lookup,
                    const QRect& game, const QRect& anchor)
      {
         auto* root = rootObject ();
         if (!root) {
            core::Logger::warn ("overlay: rootObject null; QML failed to load");
            return;
         }

         // Pricing labels mirror persist_find: market = median, vendor = low.
         root->setProperty ("title",     QString::fromStdString (lookup.canonical_name));
         root->setProperty ("rarity",    QString::fromStdString (lookup.rarity));
         root->setProperty ("primary",   to_lines (lookup.primary));
         root->setProperty ("secondary", to_lines (lookup.secondary));
         root->setProperty ("details",   to_lines (lookup.details));
         root->setProperty ("market",    static_cast<int> (lookup.pricing.median));
         root->setProperty ("vendor",    static_cast<int> (lookup.pricing.low));

         // All math in physical pixels; width()/height() are logical,
         // scaled by the game window's monitor.
         const qreal s   = screen::scale_at (game.center ());
         const int   gap = qRound (12 * s);
         const QSize size {
            qRound (width ()  * s),
            qRound (height () * s),
         };

         int x = anchor.x () + anchor.width () + gap;
         if (x + size.width () > game.x () + game.width ()) {
            x = anchor.x () - gap - size.width ();
         }
         int y = anchor.y ();

         x = std::max (game.x (), std::min (x, game.x () + game.width ()  - size.width ()));
         y = std::max (game.y (), std::min (y, game.y () + game.height () - size.height ()));

         screen::move (this, { x, y });

         if (!visible_) {
            show ();
            visible_ = true;
         }
      }

      void clear ()
      {
         if (visible_) {
            hide ();
            visible_ = false;
         }
      }

   private:
      bool visible_ = false;
   };

} // namespace

struct OverlayWindow::Impl
{
   Config config;

   std::unique_ptr<AugmentView> augment;
   std::unique_ptr<QmlTooltip>  qml;

   QmlTooltip& qml_renderer ()
   {
      if (!qml) qml = std::make_unique<QmlTooltip> ();
      return *qml;
   }
};

OverlayWindow::OverlayWindow (Config config, QObject* parent)
   : QObject (parent), impl_ (std::make_unique<Impl> ())
{
   impl_->config = std::move (config);

   if (impl_->config.renderer == "qml") {
      core::Logger::info ("overlay: QML renderer selected by setting");
      impl_->qml_renderer ();
      return;
   }

   auto augment = AugmentView::create (
      AugmentView::Config {
         .web_dir       = impl_->config.web_dir,
         .user_data_dir = impl_->config.user_data_dir,
      },
      [this] { fall_back_to_qml (); });

   if (!augment.has_value ()) {
      core::Logger::warn ("overlay: WebView2 unavailable ({}); using QML renderer",
         augment.error ().message);
      impl_->qml_renderer ();
      return;
   }

   impl_->augment = std::move (*augment);
}

OverlayWindow::~OverlayWindow () = default;

void OverlayWindow::fall_back_to_qml ()
{
   // May fire from a WebView2 COM callback mid-present; defer the switch so
   // the failed AugmentView isn't destroyed under its own stack frames.
   QMetaObject::invokeMethod (this, [this] {
      if (!impl_->augment) return;

      core::Logger::warn ("overlay: WebView2 renderer failed; falling back to QML");
      impl_->augment.reset ();
      impl_->qml_renderer ();
   }, Qt::QueuedConnection);
}

void OverlayWindow::present (const gv::api::TooltipLookup& lookup,
                             const QRect& game, const QRect& anchor)
{
   if (impl_->augment) {
      impl_->augment->present (lookup, game, anchor);
      return;
   }

   impl_->qml_renderer ().present (lookup, game, anchor);
}

void OverlayWindow::clear ()
{
   if (impl_->augment) {
      impl_->augment->clear ();
      return;
   }

   if (impl_->qml) impl_->qml->clear ();
}

} // namespace gv::ui
