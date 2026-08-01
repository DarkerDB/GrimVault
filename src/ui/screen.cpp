#include <gv/ui/screen.h>

#include <QWindow>

#ifdef _WIN32
   #include <Windows.h>
   #include <ShellScalingApi.h>
#endif

namespace gv::ui::screen {

qreal scale_at (const QPoint& physical)
{
#ifdef _WIN32
   const POINT pt { physical.x (), physical.y () };
   HMONITOR mon = ::MonitorFromPoint (pt, MONITOR_DEFAULTTONEAREST);

   UINT dpi_x = 96;
   UINT dpi_y = 96;
   if (SUCCEEDED (::GetDpiForMonitor (mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
      return dpi_x / 96.0;
   }
#else
   Q_UNUSED (physical);
#endif
   return 1.0;
}

QRect viewport_at (const QPoint& physical)
{
#ifdef _WIN32
   const POINT pt { physical.x (), physical.y () };
   HMONITOR mon = ::MonitorFromPoint (pt, MONITOR_DEFAULTTONEAREST);

   MONITORINFO info {};
   info.cbSize = sizeof (info);
   if (!::GetMonitorInfoW (mon, &info)) return {};

   // rcMonitor, not rcWork: the game is borderless fullscreen and the card
   // draws over it, so the taskbar strip is ours to use as well.
   const RECT& r = info.rcMonitor;
   return QRect { r.left, r.top, r.right - r.left, r.bottom - r.top };
#else
   Q_UNUSED (physical);
   return {};
#endif
}

void move (QWindow* window, const QPoint& physical)
{
#ifdef _WIN32
   // SetWindowPos takes physical pixels directly; Qt back-maps the resulting
   // geometry to logical coordinates itself on WM_WINDOWPOSCHANGED.
   ::SetWindowPos (
      reinterpret_cast<HWND> (window->winId ()),
      nullptr,
      physical.x (), physical.y (),
      0, 0,
      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
   );
#else
   window->setPosition (physical);
#endif
}

void make_passthrough (QWindow* window)
{
#ifdef _WIN32
   const HWND h = reinterpret_cast<HWND> (window->winId ());
   const LONG_PTR ex = ::GetWindowLongPtrW (h, GWL_EXSTYLE);
   ::SetWindowLongPtrW (h, GWL_EXSTYLE,
      ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);

   // Qt::WindowTransparentForInput and WS_EX_TRANSPARENT are not a formal
   // cross-process hit-test exclusion. A disabled top-level HWND is skipped
   // by Windows point selection, cannot activate, and still keeps its Qt
   // Quick / DirectComposition content visible.
   ::EnableWindow (h, FALSE);
#else
   Q_UNUSED (window);
#endif
}

} // namespace gv::ui::screen
