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

   std::string normalize_renderer (std::string renderer)
   {
      if (renderer == "webview" || renderer == "qml") return renderer;
      return "automatic";
   }

   QStringList analysis_details (const gv::api::TooltipLookup& lookup,
                                 const augment::Options& options)
   {
      QStringList out;
      if (shows (lookup, options, "item_overview")) {
         if (lookup.utility.vendor_value > 0) {
            out << QStringLiteral ("Vendor: %1 G").arg (lookup.utility.vendor_value);
         }
         if (lookup.utility.gear_score > 0) {
            out << QStringLiteral ("Gear score: %1").arg (lookup.utility.gear_score);
         }
         if (lookup.utility.adventure_points > 0) {
            out << QStringLiteral ("Adv. points: %1").arg (lookup.utility.adventure_points);
         }
         out << QStringLiteral ("Tradeable: %1").arg (
            lookup.tradeable ? QStringLiteral ("Yes") : QStringLiteral ("No"));
      }
      if (shows (lookup, options, "actions")) {
         if (lookup.pricing.quick_list > 0) {
            out << QStringLiteral ("Sell quickly: %1 G").arg (lookup.pricing.quick_list);
         }
         if (lookup.pricing.lowest_ask > 0) {
            out << QStringLiteral ("Lowest ask: %1 G").arg (lookup.pricing.lowest_ask);
         }
      }
      if (shows (lookup, options, "market_activity")) {
         if (lookup.market_analysis.sales.count > 0) {
            out << QStringLiteral ("Sales 30d: %1%2")
                      .arg (lookup.market_analysis.sales.count)
                      .arg (lookup.market_analysis.sales.capped ? QStringLiteral ("+") : QString {});
         }
         if (lookup.market_analysis.active_listings.count > 0) {
            out << QStringLiteral ("Listings: %1%2")
                      .arg (lookup.market_analysis.active_listings.count)
                      .arg (lookup.market_analysis.active_listings.capped
                         ? QStringLiteral ("+") : QString {});
         }
         if (lookup.pricing.sample_size > 0) {
            out << QStringLiteral ("Samples: %1").arg (lookup.pricing.sample_size);
         }
      }

      const auto append_plan = [&out] (const gv::api::GemPlan* plan, int sockets) {
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
      if (shows (lookup, options, "upgrade_paths")) {
         if (!lookup.gem_optimization.plans.empty ()) {
            for (const auto& plan : lookup.gem_optimization.plans) {
               append_plan (&plan, plan.sockets);
            }
         } else {
            append_plan (lookup.gem_optimization.one_socket
               ? &*lookup.gem_optimization.one_socket : nullptr, 1);
            append_plan (lookup.gem_optimization.two_socket
               ? &*lookup.gem_optimization.two_socket : nullptr, 2);
         }
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

         const bool raw_ocr = !lookup.recognized_text.empty ();
         const bool analysis = !lookup.item_id.empty () || !lookup.display_name.empty ()
            || !lookup.rolls.empty ();
         const auto score = lookup.weighted_roll_score
            ? lookup.weighted_roll_score : lookup.roll_score;
         root->setProperty ("renderScale", layout_.scale);
         root->setProperty ("analysis",   analysis);
         root->setProperty ("showMarket", shows (lookup, options_, "market_value"));
         root->setProperty ("body",      QString::fromStdString (lookup.recognized_text));
         root->setProperty ("primary",   raw_ocr ? QStringList {} : to_lines (lookup.primary));
         root->setProperty ("secondary", raw_ocr ? QStringList {}
            : analysis && shows (lookup, options_, "roll_quality")
               ? analysis_rolls (lookup.rolls) : to_lines (lookup.secondary));
         root->setProperty ("details",   raw_ocr ? QStringList {}
            : analysis ? analysis_details (lookup, options_) : to_lines (lookup.details));
         root->setProperty ("market", raw_ocr || !shows (lookup, options_, "market_value")
            ? 0 : static_cast<int> (lookup.pricing.median));
         root->setProperty ("marketLow", raw_ocr || !shows (lookup, options_, "market_value")
            ? 0 : static_cast<int> (lookup.pricing.low));
         root->setProperty ("marketHigh", raw_ocr || !shows (lookup, options_, "market_value")
            ? 0 : static_cast<int> (lookup.pricing.high));
         root->setProperty ("vendor", raw_ocr ? 0 : static_cast<int> (
            analysis ? lookup.utility.vendor_value : lookup.pricing.low));
         root->setProperty ("rollScore", raw_ocr || !score ? -1 : *score);
         root->setProperty ("confidence", raw_ocr
            ? QString {} : QString::fromStdString (lookup.pricing.confidence));

         const qreal dpr = screen::scale_at (game.center ());
         const qreal s = dpr * layout_.scale;
         const int gap = qRound (12 * s);
         const QSize size {
            qRound (root->width () * dpr),
            qRound (root->height () * dpr),
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
   std::uint64_t generation = 0;
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
   auto renderer = std::move (config.renderer);
   impl_->config = std::move (config);
   impl_->config.renderer.clear ();
   set_renderer (std::move (renderer));
}

OverlayWindow::~OverlayWindow () = default;

void OverlayWindow::set_renderer (std::string renderer)
{
   renderer = normalize_renderer (std::move (renderer));
   if (renderer == impl_->config.renderer
       && ((renderer == "qml" && impl_->qml) || (renderer != "qml" && impl_->augment))) {
      return;
   }

   ++impl_->generation;
   const auto generation = impl_->generation;
   if (impl_->augment) impl_->augment->clear ();
   if (impl_->qml) impl_->qml->clear ();
   impl_->augment.reset ();
   impl_->qml.reset ();
   impl_->config.renderer = std::move (renderer);

   if (impl_->config.renderer == "qml") {
      core::Logger::info ("overlay: QML renderer selected");
      auto& qml = impl_->qml_renderer ();
      if (impl_->active && impl_->last) {
         qml.present (impl_->last->lookup, impl_->last->game, impl_->last->anchor);
      }
      return;
   }

   core::Logger::info ("overlay: WebView2 renderer selected mode={}", impl_->config.renderer);
   auto augment = AugmentView::create (
      AugmentView::Config {
         .web_dir       = impl_->config.web_dir,
         .user_data_dir = impl_->config.user_data_dir,
      },
      [this, generation] { fall_back_to_qml (generation); });

   if (!augment.has_value ()) {
      core::Logger::warn ("overlay: WebView2 unavailable ({}) mode={}",
         augment.error ().message, impl_->config.renderer);
      if (impl_->config.renderer == "automatic") {
         auto& qml = impl_->qml_renderer ();
         if (impl_->active && impl_->last) {
            qml.present (impl_->last->lookup, impl_->last->game, impl_->last->anchor);
         }
      }
      return;
   }

   impl_->augment = std::move (*augment);
   impl_->augment->set_layout  (impl_->layout);
   impl_->augment->set_options (impl_->options);
   if (impl_->active && impl_->last) {
      impl_->augment->present (
         impl_->last->lookup, impl_->last->game, impl_->last->anchor, false);
   }
}

const std::string& OverlayWindow::renderer () const noexcept
{
   return impl_->config.renderer;
}

void OverlayWindow::fall_back_to_qml (std::uint64_t generation)
{
   // May fire from a WebView2 COM callback mid-present; defer the switch so
   // the failed AugmentView isn't destroyed under its own stack frames.
   QMetaObject::invokeMethod (this, [this, generation] {
      if (generation != impl_->generation || !impl_->augment) return;

      impl_->augment.reset ();
      if (impl_->config.renderer != "automatic") {
         core::Logger::warn ("overlay: WebView2 renderer failed; fallback disabled");
         return;
      }

      core::Logger::warn ("overlay: WebView2 renderer failed; falling back to QML");
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

   if (impl_->qml) impl_->qml->present (lookup, game, anchor);
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
      return true;
   }
   if (!impl_->qml) return false;
   impl_->qml->present (last.lookup, last.game, last.anchor);
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
