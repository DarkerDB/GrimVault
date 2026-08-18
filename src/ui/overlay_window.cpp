#include <gv/ui/overlay_window.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>
#include <gv/ui/augment_view.h>
#include <gv/ui/placement.h>
#include <gv/ui/screen.h>

#include <QPointer>
#include <QQuickItem>
#include <QQuickView>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <optional>

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

   QStringList analysis_rolls (const std::vector<gv::api::AnalysisRoll>& rolls)
   {
      QStringList out;
      out.reserve (static_cast<int> (rolls.size ()));
      for (const auto& roll : rolls) {
         QString line = QStringLiteral ("%1 %2").arg (
            QString::fromStdString (roll.formatted_value),
            QString::fromStdString (roll.label));
         if (roll.roll_percentile) {
            line += QStringLiteral (" (%1%)").arg (*roll.roll_percentile);
         }
         out << std::move (line);
      }
      return out;
   }

   bool shows (const gv::api::TooltipLookup& lookup,
               const augment::Options& options, std::string_view widget)
   {
      bool wanted = true;
      for (const auto& [slug, visible] : options.widgets) {
         if (slug == widget) {
            wanted = visible;
            break;
         }
      }
      return wanted && lookup.entitlement.grants (widget);
   }

   QStringList analysis_details (const gv::api::TooltipLookup& lookup,
                                 const augment::Options& options)
   {
      QStringList out;
      if (shows (lookup, options, "market_value")
          && lookup.pricing.low > 0 && lookup.pricing.high > 0) {
         out << QStringLiteral ("Expected range: %1 – %2 G")
                   .arg (lookup.pricing.low).arg (lookup.pricing.high);
      }
      if (shows (lookup, options, "market_value") && lookup.pricing.quick_list > 0) {
         out << QStringLiteral ("Quick list: %1 G").arg (lookup.pricing.quick_list);
      }
      if (shows (lookup, options, "roll_quality") && lookup.roll_score) {
         out << QStringLiteral ("Roll quality: %1 / 100").arg (*lookup.roll_score);
      }
      if (shows (lookup, options, "market_activity")
          && lookup.market_analysis.sales.count > 0) {
         const auto count = QStringLiteral ("%1%2")
            .arg (lookup.market_analysis.sales.count)
            .arg (lookup.market_analysis.sales.capped ? QStringLiteral ("+") : QString {});
         out << QStringLiteral ("Sales: %1 / %2h")
                   .arg (count)
                   .arg (lookup.market_analysis.sales.window_hours);
      }

      const auto append_plan = [&out] (const std::optional<gv::api::GemPlan>& plan,
                                       int sockets) {
         if (!plan || plan->changes.empty () || plan->projected_value <= 0) return;
         const auto& change = plan->changes.front ();
         out << QStringLiteral ("Best %1-gem: %2 → %3 %4")
                   .arg (sockets)
                   .arg (QString::fromStdString (change.replace_label))
                   .arg (QString::fromStdString (change.new_value))
                   .arg (QString::fromStdString (change.new_label));
         out << QStringLiteral ("Projected / net: %1 G / %2%3 G")
                   .arg (plan->projected_value)
                   .arg (plan->net_uplift >= 0 ? "+" : "")
                   .arg (plan->net_uplift);
      };
      if (shows (lookup, options, "gem_optimization")) {
         append_plan (lookup.gem_optimization.one_socket, 1);
         append_plan (lookup.gem_optimization.two_socket, 2);
      }
      return out;
   }

   // Native renderer: the QML port of the DDB card. Unlike WebView2, this has
   // no browser-process top-level window competing with the game for cursor
   // hit testing.
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
         if (!layout_.enabled) {
            clear ();
            return;
         }
         auto* root = rootObject ();
         if (!root) {
            core::Logger::warn ("overlay: rootObject null; QML failed to load");
            return;
         }

         // The native renderer is a resilient fallback. It receives the same
         // complete analysis, condensed into the QML card's legacy fields.
         const bool raw_ocr = !lookup.recognized_text.empty ();
         const bool analysis = !lookup.item_id.empty () || !lookup.rolls.empty ();
         const auto title = !lookup.display_name.empty ()
            ? lookup.display_name : lookup.canonical_name;
         root->setProperty ("title",     raw_ocr ? QString {} : QString::fromStdString (title));
         root->setProperty ("body",      QString::fromStdString (lookup.recognized_text));
         root->setProperty ("rarity",    QString::fromStdString (lookup.rarity));
         root->setProperty ("primary",   raw_ocr ? QStringList {} : to_lines (lookup.primary));
         root->setProperty ("secondary", raw_ocr ? QStringList {}
            : analysis && shows (lookup, options_, "roll_quality")
               ? analysis_rolls (lookup.rolls) : to_lines (lookup.secondary));
         root->setProperty ("details",   raw_ocr ? QStringList {}
            : analysis ? analysis_details (lookup, options_) : to_lines (lookup.details));
         root->setProperty ("market", raw_ocr || !shows (lookup, options_, "market_value")
            ? 0 : static_cast<int> (lookup.pricing.median));
         root->setProperty ("vendor",    raw_ocr ? 0 : static_cast<int> (
            analysis && shows (lookup, options_, "utility_details")
               ? lookup.utility.vendor_value : analysis ? 0 : lookup.pricing.low));

         // All math in physical pixels; width()/height() are logical,
         // scaled by the game window's monitor.
         const qreal s   = screen::scale_at (game.center ()) * layout_.scale;
         const int   gap = qRound (12 * s);
         const QSize size {
            qRound (width ()  * s),
            qRound (height () * s),
         };

         const QRect monitor = screen::viewport_at (game.center ());
         const QRect visible = game.intersected (monitor);
         const QRect view = visible.isEmpty () ? game : visible;
         const int nudge_x = static_cast<int> (layout_.offset_x * s);
         const int nudge_y = static_cast<int> (layout_.offset_y * s);
         const QPoint want = layout_.align == Layout::Align::Attached
            ? placement::attached (view, anchor, size, gap)
            : placement::corner (view, size, layout_.align, nudge_x, nudge_y);

         setOpacity (layout_.opacity);
         screen::move (this, placement::clamp (view, want, size));

         if (!visible_) {
            show ();
            screen::make_passthrough (this);
            visible_ = true;
         }
      }

      void set_layout (const Layout& layout)
      {
         layout_ = layout;
         setOpacity (layout.opacity);
         if (!layout.enabled) clear ();
      }
      void set_options (const augment::Options& options) { options_ = options; }

      void clear ()
      {
         if (visible_) {
            hide ();
            visible_ = false;
         }
      }

   private:
      bool visible_ = false;
      Layout layout_;
      augment::Options options_;
   };

} // namespace

struct OverlayWindow::Impl
{
   struct LastPresentation {
      gv::api::TooltipLookup lookup;
      QRect game;
      QRect anchor;
   };

   Config config;

   // Held here as well as pushed down so a renderer created (or recreated)
   // after the first sync still starts with the player's settings, not the
   // struct defaults.
   Layout           layout;
   augment::Options options;

   std::unique_ptr<AugmentView> augment;
   std::unique_ptr<QmlTooltip>  qml;
   std::optional<LastPresentation> last;
   bool active = true;

   QmlTooltip& qml_renderer ()
   {
      if (!qml) {
         qml = std::make_unique<QmlTooltip> ();
         qml->set_layout (layout);
         qml->set_options (options);
      }
      return *qml;
   }
};

OverlayWindow::OverlayWindow (Config config, QObject* parent)
   : QObject (parent), impl_ (std::make_unique<Impl> ())
{
   impl_->config = std::move (config);

   if (impl_->config.renderer == "qml") {
      core::Logger::info ("overlay: native QML renderer selected by setting");
      impl_->qml_renderer ();
      return;
   }

   core::Logger::info ("overlay: hidden WebView2 snapshot renderer selected");

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
   impl_->augment->set_layout  (impl_->layout);
   impl_->augment->set_options (impl_->options);
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
      auto& qml = impl_->qml_renderer ();
      if (impl_->active && impl_->last.has_value ()) {
         qml.present (impl_->last->lookup, impl_->last->game, impl_->last->anchor);
      }
   }, Qt::QueuedConnection);
}

void OverlayWindow::present (const gv::api::TooltipLookup& lookup,
                             const QRect& game, const QRect& anchor, bool animate)
{
   impl_->last = Impl::LastPresentation { lookup, game, anchor };
   if (!impl_->active) return;

   if (impl_->augment) {
      impl_->augment->present (lookup, game, anchor, animate);
      return;
   }

   impl_->qml_renderer ().present (lookup, game, anchor);
}

void OverlayWindow::clear ()
{
   impl_->last.reset ();
   if (impl_->augment) {
      impl_->augment->clear ();
      return;
   }

   if (impl_->qml) impl_->qml->clear ();
}

bool OverlayWindow::set_active (bool active)
{
   if (impl_->active == active) return false;
   impl_->active = active;

   if (!active) {
      if (impl_->augment) impl_->augment->clear ();
      if (impl_->qml) impl_->qml->clear ();
      return false;
   }

   if (!impl_->last.has_value ()) return false;

   const auto& last = *impl_->last;
   if (impl_->augment) {
      impl_->augment->present (last.lookup, last.game, last.anchor, false);
   } else {
      impl_->qml_renderer ().present (last.lookup, last.game, last.anchor);
   }
   return true;
}

void OverlayWindow::set_layout (const Layout& layout)
{
   impl_->layout = layout;
   if (impl_->augment) impl_->augment->set_layout (layout);
   if (impl_->qml) impl_->qml->set_layout (layout);
}

void OverlayWindow::set_options (const augment::Options& options)
{
   impl_->options = options;
   if (impl_->augment) impl_->augment->set_options (options);
   if (impl_->qml) impl_->qml->set_options (options);
}

void OverlayWindow::anchor_shown (const QRect& game, const QPoint& offset,
                                  const QSize& tip, bool pinned_x, bool pinned_y,
                                  const QPoint& pin)
{
   if (impl_->augment)
      impl_->augment->anchor_shown (game, offset, tip, pinned_x, pinned_y, pin);
}

void OverlayWindow::anchor_lost (bool immediate)
{
   if (impl_->augment) {
      impl_->augment->anchor_lost (immediate);
      if (immediate) impl_->last.reset ();
      return;
   }
   impl_->last.reset ();
   if (impl_->qml) impl_->qml->clear ();
}

} // namespace gv::ui
