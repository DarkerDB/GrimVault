#include <gv/ui/overlay_window.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>
#include <gv/ui/augment_view.h>
#include <gv/ui/augment_payload.h>
#include <gv/ui/placement.h>
#include <gv/ui/screen.h>

#include <QJsonDocument>
#include <QQuickItem>
#include <QQuickView>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <optional>

namespace gv::ui {

namespace {

   std::string normalize_renderer (std::string renderer)
   {
      if (renderer == "webview" || renderer == "qml") return renderer;
      return "automatic";
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

         const auto document = QJsonDocument::fromJson (QByteArray::fromStdString (
            augment::entity (lookup, options_).dump ()));
         root->setProperty ("renderScale", 1.0);
         root->setProperty ("entity", document.toVariant ());

         const qreal dpr = screen::scale_at (game.center ());
         const QRect monitor = screen::viewport_at (game.center ());
         const QRect visible = game.intersected (monitor);
         const QRect view = visible.isEmpty () ? game : visible;
         const bool attached = layout_.align == Layout::Align::Attached && !anchor.isEmpty ();
         const QSize available = attached
            ? placement::attached_space (view, anchor, 0) : view.size ();
         const QSize card_css { qRound (root->width ()), qRound (root->height ()) };
         const qreal wanted = dpr * layout_.scale;
         const qreal fit = placement::fit (
            QRect { QPoint {}, available },
            { card_css.width () + (attached ? 12 : 0), card_css.height () },
            0, wanted);
         const qreal render_scale = layout_.scale * fit;
         root->setProperty ("renderScale", render_scale);

         const qreal s = dpr * render_scale;
         const int gap = qRound (12 * s);
         const QSize size {
            qRound (root->width () * dpr),
            qRound (root->height () * dpr),
         };
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
   if (renderer == impl_->config.renderer) return;

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
