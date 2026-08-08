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

} // namespace gv::ui::screen
