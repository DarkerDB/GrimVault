Conflict 1 (`anchored_rect`, tip clamping):
  Ours (HEAD): raw `x`/`y` clamp against `game` using undefined-in-context `w`/`h`, pre-dating the viewport/placement refactor.
  Theirs (incoming): builds `view` (viewport-clamped) and `anchor` rect for use with `placement::attached`/`corner`/`clamp`.
  Resolution: kept theirs. The non-conflicting code right after the block calls `placement::attached (view, anchor, card, gap)` — `view`/`anchor` only exist on theirs' side, so ours wouldn't compile and duplicates logic the rest of the file already supersedes.

Conflict 2 (`place_legacy`, anchor-relative placement):
  Ours (HEAD): manual side-flip `x`/`y` arithmetic against `legacy_anchor`/`legacy_game`, no viewport clamping.
  Theirs (incoming): builds `view` (viewport-clamped `legacy_game`) and `card` QSize, feeding the same `placement::attached`/`clamp` calls used elsewhere.
  Resolution: kept theirs, same reasoning — required by the surrounding code and consistent with the viewport-clamping refactor already in place throughout the file.

---RESOLVED---
#include <gv/ui/augment_view.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>
#include <gv/ui/augment_payload.h>
#include <gv/ui/placement.h>
#include <gv/ui/screen.h>

#include <QTimer>
#include <QImage>
#include <QEasingCurve>
#include <QPainter>
#include <QPaintEvent>
#include <QElapsedTimer>
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

   // Floor on how long the loading skeleton stays up once shown. Purely a
   // perception number: below roughly this, the spinner's whole life is
   // read as a flicker rather than as the overlay working.
   constexpr qint64 k_loading_min_ms = 1000;

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
            { "name",   "GrimVault" },
            { "realm",  "grimvault" },
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

      // Dashboard opacity, multiplied into the enter animation's own fade so
      // a mid-hover change repaints without restarting the animation.
      void set_opacity (qreal opacity)
      {
         opacity_ = std::clamp (opacity, 0.0, 1.0);
         if (isVisible ()) update ();
      }

      // `dpr` is the MONITOR's device pixel ratio — deliberately not the scale
      // the card was rendered at. Those two used to be the same number, and
      // conflating them broke the moment overlay:scale and the fit-to-viewport
      // shrink started feeding a composite scale in: the window was sized
      // physical/render_scale instead of physical/dpr, so a card rendered at
      // 0.96 came out ~4% too wide and too tall. That stretched the capture
      // (pixelated), pushed the right edge over the game's tooltip, and ran
      // the bottom off the screen — one bug, three symptoms.
      void present (QImage image, const QRect& physical, qreal dpr, bool animate)
      {
         const int logical_w = qMax (1, qRound (physical.width ()  / dpr));
         const int logical_h = qMax (1, qRound (physical.height () / dpr));

         // Derive the image's ratio from what was actually captured rather
         // than assuming it matches `physical`. Whatever the capture size,
         // this maps it onto the logical box without Qt rescaling it.
         image.setDevicePixelRatio (
            image.width () > 0 ? qreal (image.width ()) / logical_w : dpr);
         image_ = std::move (image);

         // Establish the target monitor before assigning logical dimensions;
         // Qt then maps them through that monitor's DPR.
         screen::move (this->windowHandle (), physical.topLeft ());
         resize (logical_w, logical_h);

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

         painter.setOpacity (enter_progress_ * opacity_);
         painter.translate (0.0, 8.0 * remaining);
         painter.translate (origin);
         painter.scale (scale, scale);
         painter.translate (-origin);
         painter.drawImage (rect (), image_);
      }

   private:
      QImage image_;
      qreal  opacity_ = 0.9;
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
   // A real analysis has been rendered for this hover, so the loading
   // skeleton must not come back until the next anchor.
   bool   showing_result = false;

   // The loading card is a still frame: the renderer captures one PNG and a
   // CSS animation inside it is frozen at whatever moment the shutter fell,
   // so the spinner sat there motionless. Re-capture on a timer while the
   // skeleton is up and it turns. Cheap because it only runs for the few
   // hundred ms between "a tooltip is anchored" and "the analysis landed",
   // and only for the small skeleton card.
   bool          loading = false;
   QElapsedTimer loading_since;
   bool   sized    = false;    // css size handshake completed
   bool   shown    = false;    // window currently revealed
   QRect  game;                // physical screen px
   QPoint offset;              // tooltip top-left minus cursor
   QSize  tip;                 // anchored tooltip size
   bool   pinned_x = false;
   bool   pinned_y = false;
   QPoint pin;                 // fixed game-relative tooltip coordinate
   std::uint64_t placed_seq = 0;   // last seq actually placed and captured
   int    css_w   = 0;    // bare card size (CSS px)
   int    css_h   = 0;
   int    pad_css = 0;    // transparent animation headroom (CSS px)
   qreal  scale   = 1.0;
   QRect  placed;              // last window rect

   // Live dashboard settings (overlay:* and the render-time preferences).
   Layout           layout;
   augment::Options options;

   // Where the card is actually allowed to live: the monitor under the game,
   // intersected with the game window. A borderless window can report a rect
   // larger than its monitor, and on a multi-monitor desk the game's rect is
   // not the visible area — clamping to the game rect alone put the card off
   // the edge of the screen.
   QRect viewport (const QRect& area) const
   {
      const QRect monitor = screen::viewport_at (area.center ());
      if (monitor.isEmpty ()) return area;

      const QRect visible = area.intersected (monitor);
      return visible.isEmpty () ? area : visible;
   }

   // The card is as tall as its content, and a full analysis on a short
   // window (or at a high overlay:scale) runs off the bottom — the placement
   // clamp can only pin it to an edge, it can't make it fit. Shrink
   // uniformly instead so the whole card stays readable and on screen.
   qreal fitted_scale (const QRect& area) const
   {
      const qreal wanted = screen::scale_at (area.center ()) * layout.scale;
      return wanted * placement::fit (
         viewport (area), QSize { css_w, css_h }, 2 * pad_css, wanted);
   }

   QTimer follow;              // 120 Hz reposition
   QTimer grace;
   QTimer spin;                // re-capture cadence for the loading skeleton

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
      shown   = false;
      loading = false;
      loading_since.invalidate ();
      spin.stop ();
   }
};

AugmentView::AugmentView () : impl_ (std::make_unique<Impl> ())
{
   impl_->follow.setInterval (8);

   // 20 fps. The spinner reads as motion well below film rate, and every
   // tick is a full WebView2 present + PNG round trip, so buying frames
   // past this costs more than it shows.
   impl_->spin.setInterval (50);
   QObject::connect (&impl_->spin, &QTimer::timeout, [impl = impl_.get ()] {
      if (!impl->loading || !impl->shown || impl->placed.isEmpty ()) return;

      // The card already flew in on the first frame; replaying the enter
      // animation every tick would make it pulse instead of spin.
      impl->pending_animate = false;
      impl->capture_to_snapshot (impl->placed);
   });
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

   if (!impl->anchored) impl->showing_result = false;
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

   // A spinner that appears and vanishes inside a hundred milliseconds reads
   // as a glitch rather than as work: the eye registers the arrival and the
   // departure as one flicker. Holding the skeleton for a beat makes a fast
   // answer feel deliberate. The analysis is already in hand — this delays
   // only the paint, and only when the paint would otherwise strobe.
   if (impl->loading && impl->loading_since.isValid ()) {
      const auto held = impl->loading_since.elapsed ();
      if (held < k_loading_min_ms) {
         const auto for_seq = impl->seq;
         auto       held_lookup = lookup;

         QTimer::singleShot (static_cast<int> (k_loading_min_ms - held), &impl->follow,
            [this, for_seq, held_lookup = std::move (held_lookup), game, anchor, animate] {
               // The hover moved on, or a newer render already won the slot.
               if (impl_->seq != for_seq || !impl_->loading) return;

               impl_->loading_since.invalidate ();
               present (held_lookup, game, anchor, animate);
            });
         return;
      }
   }

   // A skeleton already on screen means this render is a content swap, not
   // an arrival: replaying the enter animation would make the card fly in a
   // second time over itself.
   const bool was_loading = impl->loading;

   impl->pending_seq    = ++impl->seq;
   impl->showing_result = true;
   impl->loading        = false;
   impl->loading_since.invalidate ();
   impl->spin.stop ();
   impl->prewarming     = false;
   impl->legacy_game    = game;
   impl->legacy_anchor = anchor;
   impl->pending_animate = animate && !(was_loading && impl->shown);

   impl->host->post_json (
      augment::render_message (lookup, impl->pending_seq, impl->options).dump ());

   core::Logger::info ("augment: render '{}' seq={}", lookup.canonical_name, impl->seq);
}

void AugmentView::present_loading ()
{
   auto* impl = impl_.get ();
   if (!impl->host || !impl->host->ready ()) return;
   if (impl->showing_result || impl->pending_seq != 0) return;

   impl->pending_seq   = ++impl->seq;
   impl->prewarming    = false;
   impl->pending_animate = true;
   impl->loading       = true;
   impl->loading_since.start ();

   impl->host->post_json (augment::loading_message (impl->pending_seq).dump ());
}

void AugmentView::set_layout (const Layout& layout)
{
   const bool scale_changed = impl_->layout.scale != layout.scale;

   impl_->layout = layout;
   if (impl_->snapshot) impl_->snapshot->set_opacity (layout.opacity);

   // Placement is recomputed every follow tick, so alignment and offsets
   // need no nudge here. Scale changes the card's pixel size, which only a
   // re-place picks up.
   if (scale_changed && impl_->anchored && impl_->sized) impl_->place_anchored ();
}

void AugmentView::set_options (const augment::Options& options)
{
   impl_->options = options;
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
   impl_->spin.stop ();
   impl_->loading = false;

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

   // A second size for the same render is the page telling us it now looks
   // different than when we photographed it — cold icons finished decoding.
   // Re-capture, but don't replay the enter animation: the card is already
   // on screen and would fly in twice.
   if (placed_seq == msg_seq) pending_animate = false;
   placed_seq = msg_seq;

   if (anchored) {
      place_anchored ();
      if (loading && !spin.isActive ()) spin.start ();
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
   const QPoint c = cursor_physical () - game.topLeft ();

   const int tip_x = game.x () + (pinned_x ? pin.x ()
      : std::clamp (c.x () + offset.x (), 0,
         std::max (0, game.width () - tip.width ())));
   const int tip_y = game.y () + (pinned_y ? pin.y ()
      : std::clamp (c.y () + offset.y (), 0,
         std::max (0, game.height () - tip.height ())));

   const int pad = static_cast<int> (pad_css * scale);
   const int gap = static_cast<int> (12 * scale);
   const QSize card {
      static_cast<int> (css_w * scale),
      static_cast<int> (css_h * scale),
   };

   // Clamp into what is actually visible, not the raw game rect.
   const QRect view   = viewport (game);
   const QRect anchor { tip_x, tip_y, tip.width (), tip.height () };

   // overlay:offset_x / offset_y are authored in CSS px so the same nudge
   // means the same thing on every monitor. They apply to the corner modes
   // only — the dashboard greys the inputs out while alignment is
   // `attached`, because there the anchor already fixes the position.
   const int nudge_x = static_cast<int> (layout.offset_x * scale);
   const int nudge_y = static_cast<int> (layout.offset_y * scale);

   const QPoint want = layout.align == Layout::Align::Attached
      ? placement::attached (view, anchor, card, gap)
      : placement::corner   (view, card, layout.align, nudge_x, nudge_y);

   // The pad is transparent headroom for the enter animation, so the WINDOW
   // is larger than the card on every edge. Clamp the window, not the card,
   // or the animation clips against the screen edge.
   const QSize  window { card.width () + 2 * pad, card.height () + 2 * pad };
   const QPoint at = placement::clamp (view, want - QPoint { pad, pad }, window);

   return { at, window };
}

void AugmentView::Impl::place_anchored ()
{
   // overlay:scale rides on top of the monitor's DPI scale, so a player who
   // wants a bigger card gets one at every DPI — then shrunk if the result
   // wouldn't fit the game area vertically.
   scale = fitted_scale (game);

   placed = anchored_rect ();
   capture_to_snapshot (placed);

   if (!follow.isActive ()) follow.start ();

   core::Logger::debug ("augment: anchored card at {},{} {}x{} physical",
      placed.x (), placed.y (), placed.width (), placed.height ());
}

void AugmentView::Impl::place_legacy (int w_css, int h_css)
{
   const qreal s   = fitted_scale (legacy_game);
   const int   gap = static_cast<int> (12 * s);
   const int   w   = static_cast<int> (w_css * s);
   const int   h   = static_cast<int> (h_css * s);

   const QRect view = viewport (legacy_game);
   const QSize card { w, h };

   // Same rule as the anchored path: dock beside the anchor, flipping sides
   // rather than sliding over the tooltip the card exists to annotate.
   const QPoint want = placement::attached (view, legacy_anchor, card, gap);

   const int pad = static_cast<int> (pad_css * s);
   scale  = s;

   const QSize window { w + 2 * pad, h + 2 * pad };
   placed = QRect { placement::clamp (view, want - QPoint { pad, pad }, window), window };
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

         // The monitor's own ratio, not `scale` — see SnapshotWindow::present.
         snapshot->present (std::move (image), rect,
                            screen::scale_at (rect.center ()), capture_animate);
         shown = true;
      });
   });
}

} // namespace gv::ui