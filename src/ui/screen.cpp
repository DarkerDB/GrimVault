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

} // namespace gv::ui::screen
