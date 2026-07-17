#pragma once

#include <QPoint>

class QWindow;

namespace gv::ui::screen {

// Positioning helpers for windows placed from Win32 physical pixels
// (WindowTracker bounds, capture-frame rects). QWindow::setPosition expects
// logical (DPI-scaled) coordinates, so physical input must go through the
// native path or the window lands off-screen on scaled monitors.

// Effective DPI scale (1.0 == 96 dpi) of the monitor nearest the point.
qreal scale_at (const QPoint& physical);

// Move a top-level window so its top-left lands on the given physical
// screen pixel, regardless of per-monitor scaling.
void move (QWindow* window, const QPoint& physical);

// Make an overlay window click-through AND non-activating: mouse events
// pass to the game beneath, and it can never take foreground when clicked
// (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE). Qt::WindowTransparentForInput
// only sets the former, so a stray click on a Qt overlay was alt-tabbing
// the game. Re-apply after each show(): Qt can drop native ex-styles when
// it (re)creates the platform window. winId() must be valid.
void make_passthrough (QWindow* window);

} // namespace gv::ui::screen
