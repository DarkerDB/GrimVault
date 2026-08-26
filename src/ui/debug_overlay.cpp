#include <gv/ui/debug_overlay.h>
#include <gv/core/logger.h>
#include <gv/ui/screen.h>

#include <QExposeEvent>
#include <QPainter>
#include <QPen>
#include <QSurfaceFormat>
#include <QTimer>

#ifdef _WIN32
   #include <Windows.h>
#endif

#include <algorithm>

namespace gv::ui {

namespace {

   // DDB blood red, same accent the tray menu uses.
   const QColor k_border { 0xEF, 0x1F, 0x1F };
   const QColor k_uploaded { 0x38, 0xD4, 0x6A };

   constexpr int k_border_px = 2;

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

} // namespace

struct DebugOverlay::Impl
{
   bool   enabled = false;
   bool   highlight_game = false;
   bool   highlight_objects = false;
   bool   game_visible = false;
   QRect  bounds;                       // physical screen px
   QRect  applied;                      // last bounds actually laid out
   qreal  scale = 1.0;

   bool   anchored = false;
   bool   uploaded = false;
   std::uint64_t generation = 0;
   QPoint offset;                       // tooltip top-left minus cursor
   QSize  size;
   bool   pinned_x = false;
   bool   pinned_y = false;
   QPoint pin;
   QRect  box;                          // frame-relative physical px, drawn

   QTimer present;                      // 120 Hz: box = clamp (cursor + offset)

   // clamp (cursor + offset) within the game bounds, frame-relative.
   QRect place () const
   {
      const QPoint c = cursor_physical () - bounds.topLeft ();

      const int max_x = std::max (0, bounds.width ()  - size.width ());
      const int max_y = std::max (0, bounds.height () - size.height ());

      return {
         pinned_x ? pin.x () : std::clamp (c.x () + offset.x (), 0, max_x),
         pinned_y ? pin.y () : std::clamp (c.y () + offset.y (), 0, max_y),
         size.width (),
         size.height (),
      };
   }
};

DebugOverlay::DebugOverlay () : impl_ (std::make_unique<Impl> ())
{
   setFlags (
      Qt::FramelessWindowHint
    | Qt::WindowStaysOnTopHint
    | Qt::Tool
    | Qt::WindowTransparentForInput
   );

   QSurfaceFormat fmt = format ();
   fmt.setAlphaBufferSize (8);
   setFormat (fmt);

   impl_->present.setInterval (8);
   QObject::connect (&impl_->present, &QTimer::timeout, [this] {
      if (!impl_->anchored) return;

      const QRect box = impl_->place ();
      if (box == impl_->box) return;

      impl_->box = box;
      if (isVisible ()) update ();
   });

   hide ();
}

DebugOverlay::~DebugOverlay () = default;

void DebugOverlay::set_region (const QRect& physical_bounds, bool game_visible)
{
   impl_->bounds       = physical_bounds;
   impl_->game_visible = game_visible;
   refresh ();
}

void DebugOverlay::set_anchor (std::uint64_t generation, const QPoint& offset, const QSize& size,
                               bool pinned_x, bool pinned_y, const QPoint& pin)
{
   const bool fresh = !impl_->anchored;
   if (generation != impl_->generation) impl_->uploaded = false;

   impl_->anchored = true;
   impl_->generation = generation;
   impl_->offset   = offset;
   impl_->size     = size;
   impl_->pinned_x = pinned_x;
   impl_->pinned_y = pinned_y;
   impl_->pin      = pin;
   impl_->box      = impl_->place ();

   if (fresh) {
      core::Logger::debug ("debug_overlay: region identified at screen {},{} {}x{}",
         impl_->bounds.x () + impl_->box.x (), impl_->bounds.y () + impl_->box.y (),
         size.width (), size.height ());
   }

   if (!impl_->present.isActive ()) impl_->present.start ();
   if (isVisible ()) update ();
}

void DebugOverlay::mark_uploaded (std::uint64_t generation)
{
   if (!impl_->anchored || generation != impl_->generation || impl_->uploaded) return;
   impl_->uploaded = true;
   if (isVisible ()) update ();
}

void DebugOverlay::clear_anchor ()
{
   if (!impl_->anchored) return;

   impl_->anchored = false;
   impl_->present.stop ();
   if (isVisible ()) update ();
}

void DebugOverlay::set_enabled (bool on)
{
   impl_->enabled = on;
   refresh ();
}

void DebugOverlay::set_highlights (bool game, bool objects)
{
   impl_->highlight_game    = game;
   impl_->highlight_objects = objects;
   refresh ();
}

void DebugOverlay::refresh ()
{
   const bool want = impl_->enabled
                  && (impl_->highlight_game || impl_->highlight_objects)
                  && impl_->game_visible
                  && !impl_->bounds.isEmpty ();

   if (!want) {
      impl_->present.stop ();
      impl_->anchored = false;
      if (isVisible ()) hide ();
      return;
   }

   // Idempotent: window events repeat the same bounds; re-running
   // resize/move/show for identical geometry churns expose events across
   // the other always-on-top windows for nothing.
   if (impl_->applied == impl_->bounds && isVisible ()) return;
   impl_->applied = impl_->bounds;

   // Physical position via SetWindowPos; logical size for Qt's raster
   // surface. Scale comes from the monitor under the game window.
   impl_->scale = screen::scale_at (impl_->bounds.center ());

   resize (
      qRound (impl_->bounds.width ()  / impl_->scale),
      qRound (impl_->bounds.height () / impl_->scale)
   );
   screen::move (this, impl_->bounds.topLeft ());

   if (!isVisible ()) show ();
   update ();
}

void DebugOverlay::exposeEvent (QExposeEvent* event)
{
   QRasterWindow::exposeEvent (event);
   if (isExposed ()) screen::make_passthrough (this);
}

void DebugOverlay::paintEvent (QPaintEvent*)
{
   QPainter p { this };

   p.setCompositionMode (QPainter::CompositionMode_Source);
   p.fillRect (QRect { 0, 0, width (), height () }, Qt::transparent);
   p.setCompositionMode (QPainter::CompositionMode_SourceOver);

   QPen pen { impl_->uploaded ? k_uploaded : k_border };
   pen.setWidth (k_border_px);
   p.setPen (pen);

   if (impl_->highlight_game) {
      pen.setColor (k_border);
      p.setPen (pen);
      // Capture region: border just inside the window edge.
      const int inset = k_border_px / 2;
      p.drawRect (QRect {
         inset, inset,
         width ()  - k_border_px,
         height () - k_border_px,
      });
   }

   if (!impl_->highlight_objects || !impl_->anchored) return;

   pen.setColor (impl_->uploaded ? k_uploaded : k_border);
   p.setPen (pen);

   // The anchored tooltip, frame-relative physical -> logical.
   p.drawRect (QRect {
      qRound (impl_->box.x () / impl_->scale),
      qRound (impl_->box.y () / impl_->scale),
      qRound (impl_->box.width ()  / impl_->scale),
      qRound (impl_->box.height () / impl_->scale),
   });
}

} // namespace gv::ui
