#include <gv/ui/augment_view.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>
#include <gv/ui/augment_payload.h>
#include <gv/ui/screen.h>

#include <QTimer>
#include <QImage>
#include <QEasingCurve>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QVariantAnimation>
#include <QWidget>

#include <nlohmann/json.hpp>

#ifdef _WIN32
   #include <Windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace gv::ui {

using nlohmann::json;

namespace {

   constexpr int k_grace_ms = 40;    // reserved for future soft-loss signals
   constexpr int k_visual_align_up_css = 5; // ignore top ornament for alignment

   QPoint cursor_physical ()
   {
#ifdef _WIN32
      POINT p {};
      ::GetCursorPos (&p);
      return { p.x, p.y };
#else
      return {};
#endif
   }

   // A hidden minimal card warms Chromium, the SDK, fonts, and rasterization.
   // It is cleared at its size handshake and never captured for presentation.
   json warmup_message (std::uint64_t seq)
   {
      return {
         { "type", "render" },
         { "seq",  seq },
         { "entity", json {
            { "name",   "GrimVault Analysis" },
            { "rarity", "common" },
            { "sections", json::array () },
         } },
         { "params", { { "kind", "augment" }, { "compact", true } } },
      };
   }

   class SnapshotWindow final : public QWidget
   {
   public:
      SnapshotWindow ()
         : QWidget (nullptr,
              Qt::FramelessWindowHint
            | Qt::WindowStaysOnTopHint
            | Qt::Tool
            | Qt::WindowTransparentForInput)
      {
         setAttribute (Qt::WA_TranslucentBackground);
         setAttribute (Qt::WA_NoSystemBackground);
         setFocusPolicy (Qt::NoFocus);
         (void) winId (); // Create the native HWND while it is still hidden.
         screen::make_passthrough (windowHandle ());

         enter_.setDuration (240);
         QEasingCurve easing { QEasingCurve::BezierSpline };
         easing.addCubicBezierSegment (
            QPointF { .2, .7 }, QPointF { .3, 1.0 }, QPointF { 1.0, 1.0 });
         enter_.setEasingCurve (easing);
         enter_.setStartValue (0.0);
         enter_.setEndValue (1.0);
         QObject::connect (&enter_, &QVariantAnimation::valueChanged,
            this, [this] (const QVariant& value) {
               enter_progress_ = value.toReal ();
               update ();
            });
         hide ();
      }

      void present (QImage image, const QRect& physical, qreal scale, bool animate)
      {
         image.setDevicePixelRatio (scale);
         image_ = std::move (image);

         // Establish the target monitor before assigning logical dimensions;
         // Qt then maps them through that monitor's DPR.
         screen::move (this->windowHandle (), physical.topLeft ());
         resize (qMax (1, qRound (physical.width () / scale)),
                 qMax (1, qRound (physical.height () / scale)));

         enter_.stop ();
         enter_progress_ = animate ? 0.0 : 1.0;
         if (!isVisible ()) show ();
         screen::make_passthrough (this->windowHandle ());
         update ();
         if (animate) enter_.start ();
      }

      void move_physical (const QPoint& position)
      {
         screen::move (windowHandle (), position);
      }

      void clear ()
      {
         // Dismissal intentionally has no animation: the augment disappears
         // at the same instant as the game's tooltip.
         enter_.stop ();
         hide ();
         image_ = {};
         enter_progress_ = 0.0;
      }

   protected:
      void paintEvent (QPaintEvent*) override
      {
         QPainter painter { this };
         painter.setCompositionMode (QPainter::CompositionMode_Source);

         // Match the old CSS entrance on the captured native image. The PNG
         // includes transparent padding, so this transform has room without
         // clipping the card ornamentation.
         const qreal remaining = 1.0 - enter_progress_;
         const qreal scale     = 0.96 + 0.04 * enter_progress_;
         const QPointF origin { 0.0, height () / 2.0 };

         painter.setOpacity (enter_progress_);
         painter.translate (0.0, 8.0 * remaining);
         painter.translate (origin);
         painter.scale (scale, scale);
         painter.translate (-origin);
         painter.drawImage (rect (), image_);
      }

   private:
      QImage image_;
      QVariantAnimation enter_;
      qreal enter_progress_ = 0.0;
   };

} // namespace

struct AugmentView::Impl
{
   std::unique_ptr<WebviewHost> host;
   std::unique_ptr<SnapshotWindow> snapshot;
   std::function<void ()>       on_failed;
   bool                         pending_animate = true;

   std::uint64_t seq         = 0;
   std::uint64_t pending_seq = 0;
   bool          warm        = false;
   bool          prewarming  = false;

   // Legacy lookup-driven placement (present ()).
   QRect legacy_game;
   QRect legacy_anchor;

   // Anchoring (anchoring.md §7): card follows the anchored tooltip at
   // presenter rate; anchor-lost starts the grace before hiding.
   bool   anchored = false;
   bool   sized    = false;    // css size handshake completed
   bool   shown    = false;    // window currently revealed
   QRect  game;                // physical screen px
   QPoint offset;              // tooltip top-left minus cursor
   QSize  tip;                 // anchored tooltip size
   bool   pinned_x = false;
   bool   pinned_y = false;
   QPoint pin;                 // fixed game-relative tooltip coordinate
   int    css_w   = 0;    // bare card size (CSS px)
   int    css_h   = 0;
   int    pad_css = 0;    // transparent animation headroom (CSS px)
   qreal  scale   = 1.0;
   QRect  placed;              // last window rect

   QTimer follow;              // 120 Hz reposition
   QTimer grace;

   void on_message (std::string_view text);
   void place_legacy (int w, int h);
   void place_anchored ();
   QRect anchored_rect () const;
   void prewarm ();
   void capture_to_snapshot (const QRect& rect);

   // The transparent window stays shown; the CARD is shown/hidden by
   // rendering/clearing it. reveal re-renders, which re-adds .ddb-tooltip
   // to the cleared element and replays the CSS enter animation. The
   // window is transparent-empty between clear and render, so there is no
   // flash and no window-show race for the animation.
   // Instant, matching the game: its tooltip vanishes the moment the hover
   // ends, so the card does too (no exit animation).
   void conceal ()
   {
      host->post_json (json { { "type", "clear" } }.dump ());
      if (snapshot) snapshot->clear ();
      shown = false;
   }
};

AugmentView::AugmentView () : impl_ (std::make_unique<Impl> ())
{
   impl_->follow.setInterval (8);
   QObject::connect (&impl_->follow, &QTimer::timeout, [impl = impl_.get ()] {
      if (!impl->anchored || !impl->sized) return;

      const QRect want = impl->anchored_rect ();
      if (want.topLeft () == impl->placed.topLeft ()) return;

      impl->placed.moveTo (want.topLeft ());
      if (impl->snapshot) impl->snapshot->move_physical (want.topLeft ());
   });

   impl_->grace.setSingleShot (true);
   impl_->grace.setInterval (k_grace_ms);
   QObject::connect (&impl_->grace, &QTimer::timeout, [impl = impl_.get ()] {
      impl->anchored = false;
      impl->pending_seq = 0;
      impl->follow.stop ();
      impl->conceal ();
   });
}

AugmentView::~AugmentView () = default;

core::Result<std::unique_ptr<AugmentView>> AugmentView::create (Config config,
                                                                std::function<void ()> on_failed)
{
   auto view = std::unique_ptr<AugmentView> { new AugmentView () };
   auto* impl = view->impl_.get ();
   impl->on_failed = std::move (on_failed);
   impl->snapshot  = std::make_unique<SnapshotWindow> ();

   auto host = WebviewHost::create (
      WebviewHost::Config {
         .web_dir       = std::move (config.web_dir),
         .user_data_dir = std::move (config.user_data_dir),
      },
      WebviewHost::Callbacks {
         .on_ready          = [impl] { impl->prewarm (); },
         .on_message        = [impl] (std::string_view text) { impl->on_message (text); },
         .on_process_failed = [impl] { if (impl->on_failed) impl->on_failed (); },
      });

   if (!host.has_value ()) {
      return core::fail (host.error ());
   }

   impl->host = std::move (*host);
   return view;
}

// First render pays WebView2's cold start (font load, style, raster). This
// render remains entirely inside the hidden host and is cleared after its
// size handshake; it can never reach the snapshot window.
void AugmentView::Impl::prewarm ()
{
   if (warm) return;

   warm        = true;
   prewarming  = true;
   pending_seq = ++seq;
   host->post_json (warmup_message (pending_seq).dump ());
}

void AugmentView::anchor_shown (const QRect& game, const QPoint& offset,
                                const QSize& tip, bool pinned_x, bool pinned_y,
                                const QPoint& pin)
{
   auto* impl = impl_.get ();

   impl->grace.stop ();
   impl->game   = game;
   impl->offset = offset;
   impl->tip    = tip;
   impl->pinned_x = pinned_x;
   impl->pinned_y = pinned_y;
   impl->pin      = pin;

   impl->anchored = true;
}

void AugmentView::anchor_lost (bool immediate)
{
   if (!impl_->anchored) return;

   if (immediate) {
      // Cursor jumped away: hide now, no grace.
      impl_->grace.stop ();
      impl_->anchored = false;
      impl_->pending_seq = 0;
      impl_->follow.stop ();
      impl_->conceal ();
      return;
   }

   if (!impl_->grace.isActive ()) impl_->grace.start ();
}

void AugmentView::present (const gv::api::TooltipLookup& lookup,
                           const QRect& game, const QRect& anchor, bool animate)
{
   auto* impl = impl_.get ();

   if (!impl->host || !impl->host->ready ()) {
      core::Logger::debug ("augment: present before webview ready dropped");
      return;
   }

   impl->pending_seq   = ++impl->seq;
   impl->prewarming    = false;
   impl->legacy_game   = game;
   impl->legacy_anchor = anchor;
   impl->pending_animate = animate;

   impl->host->post_json (augment::render_message (lookup, impl->pending_seq).dump ());

   core::Logger::info ("augment: render '{}' seq={}", lookup.canonical_name, impl->seq);
}

void AugmentView::clear ()
{
   impl_->anchored    = false;
   impl_->pending_seq = 0;
   impl_->prewarming  = false;
   impl_->legacy_game = {};
   impl_->legacy_anchor = {};
   impl_->follow.stop ();
   impl_->grace.stop ();

   if (impl_->host) {
      impl_->conceal ();
   }
}

void AugmentView::Impl::on_message (std::string_view text)
{
   json msg = json::parse (text, nullptr, /*allow_exceptions=*/ false);
   if (msg.is_discarded ()) return;

   if (msg.value ("type", "") == "hello") {
      const auto ddb = msg.value ("ddb", "?");
      if (ddb == "object") {
         core::Logger::debug ("augment: page channel up, tooltip library loaded");
      } else {
         core::Logger::error ("augment: tooltip library failed to load (typeof DDB = {})", ddb);
      }
      return;
   }

   if (msg.value ("type", "") == "error") {
      core::Logger::error ("augment: page render error: {}", msg.value ("message", ""));
      return;
   }

   if (msg.value ("type", "") != "size") return;

   const auto msg_seq = msg.value ("seq", std::uint64_t { 0 });
   if (msg_seq == 0 || msg_seq != pending_seq) return;

   if (prewarming) {
      prewarming  = false;
      pending_seq = 0;
      host->post_json (json { { "type", "clear" } }.dump ());
      return;
   }

   css_w   = msg.value ("w", 0);       // bare card, no headroom
   css_h   = msg.value ("h", 0);
   pad_css = msg.value ("pad", 0);     // transparent animation headroom
   sized   = css_w > 0 && css_h > 0;

   if (!sized) return;

   if (anchored) {
      place_anchored ();
   } else if (!legacy_game.isEmpty ()) {
      place_legacy (css_w, css_h);
   } else {
      host->post_json (json { { "type", "clear" } }.dump ());
   }
}

// The card docks a fixed gap to the left of the tooltip with top edges aligned.
// Input is disabled at the WebView2 controller and host-window layers, so the
// geometry does not need to dodge the pointer.
//
// The returned rect is the WINDOW: the card grown by the transparent pad on
// all sides (the card sits inset by pad, the enter transform animates in that
// headroom).
QRect AugmentView::Impl::anchored_rect () const
{
   const QPoint c   = cursor_physical () - game.topLeft ();

   const int tip_x = game.x () + (pinned_x ? pin.x ()
      : std::clamp (c.x () + offset.x (), 0,
         std::max (0, game.width () - tip.width ())));
   const int tip_y = game.y () + (pinned_y ? pin.y ()
      : std::clamp (c.y () + offset.y (), 0,
         std::max (0, game.height () - tip.height ())));

   const int pad = static_cast<int> (pad_css * scale);
   const int gap = static_cast<int> (12 * scale);
   const int w   = static_cast<int> (css_w * scale);
   const int h   = static_cast<int> (css_h * scale);

   const int x = std::max (game.x (),
      std::min (tip_x - gap - w, game.x () + game.width () - w));   // left of region
   const int align_up = static_cast<int> (k_visual_align_up_css * scale);
   const int y = std::max (game.y (),
      std::min (tip_y - align_up, game.y () + game.height () - h));

   return { x - pad, y - pad, w + 2 * pad, h + 2 * pad };
}

void AugmentView::Impl::place_anchored ()
{
   scale = screen::scale_at (game.center ());

   placed = anchored_rect ();
   capture_to_snapshot (placed);

   if (!follow.isActive ()) follow.start ();

   core::Logger::debug ("augment: anchored card at {},{} {}x{} physical",
      placed.x (), placed.y (), placed.width (), placed.height ());
}

void AugmentView::Impl::place_legacy (int w_css, int h_css)
{
   const qreal s   = screen::scale_at (legacy_game.center ());
   const int   gap = static_cast<int> (12 * s);
   const int   w   = static_cast<int> (w_css * s);
   const int   h   = static_cast<int> (h_css * s);

   int x = legacy_anchor.x () + legacy_anchor.width () + gap;
   if (x + w > legacy_game.x () + legacy_game.width ()) {
      x = legacy_anchor.x () - gap - w;
   }
   int y = legacy_anchor.y () - static_cast<int> (k_visual_align_up_css * s);

   x = std::max (legacy_game.x (), std::min (x, legacy_game.x () + legacy_game.width ()  - w));
   y = std::max (legacy_game.y (), std::min (y, legacy_game.y () + legacy_game.height () - h));

   const int pad = static_cast<int> (pad_css * s);
   scale  = s;
   placed = QRect { x - pad, y - pad, w + 2 * pad, h + 2 * pad };
   capture_to_snapshot (placed);
}

void AugmentView::Impl::capture_to_snapshot (const QRect& rect)
{
   if (!host || !snapshot || rect.isEmpty ()) return;

   const std::uint64_t capture_seq = pending_seq;
   const bool capture_animate = pending_animate;
   QPointer<QTimer> guard { &follow };
   host->resize (rect.size (), scale);

   // Let Chromium present the resized visual once before CapturePreview.
   QTimer::singleShot (0, &follow, [this, guard, capture_seq, capture_animate, rect] {
      host->capture_png ([this, guard, capture_seq, capture_animate, rect] (std::vector<std::uint8_t> png) {
         if (!guard) return;
         if (capture_seq == 0 || capture_seq != pending_seq) return;
         if (!anchored && legacy_game.isEmpty ()) return;

         QImage image = QImage::fromData (
            reinterpret_cast<const uchar*> (png.data ()),
            static_cast<int> (png.size ()), "PNG");
         if (image.isNull ()) {
            core::Logger::warn ("augment: WebView2 snapshot capture failed");
            if (on_failed) on_failed ();
            return;
         }

         snapshot->present (std::move (image), rect, scale, capture_animate);
         shown = true;
      });
   });
}

} // namespace gv::ui

One design call worth flagging: I standardized on HEAD's "hidden, never-shown" warmup card over theirs' "visible placeholder with Lorem ipsum" approach, since HEAD's is the more fully-built-out mechanism (prewarming flag, explicit clear-before-capture). If the placeholder-shown UX was actually the intended direction, say so and I'll flip conflicts 2, 3, and 10.