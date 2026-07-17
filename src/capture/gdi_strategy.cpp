#include "gdi_strategy.h"

#include <gv/core/logger.h>

#include <Windows.h>

#include <chrono>
#include <cstring>

namespace gv::capture {

struct GdiBitBltStrategy::Impl
{
   bool initialized = false;
};

GdiBitBltStrategy::GdiBitBltStrategy  () : impl_ (std::make_unique<Impl> ()) {}
GdiBitBltStrategy::~GdiBitBltStrategy ()                                     = default;

core::Result<void> GdiBitBltStrategy::initialize ()
{
   impl_->initialized = true;
   core::Logger::info ("gdi: initialized");
   return {};
}

void GdiBitBltStrategy::shutdown () noexcept
{
   if (impl_) impl_->initialized = false;
}

namespace {

   core::Result<Frame> capture_dc (HDC src_dc, int width, int height,
                                   std::uint64_t monitor_id, std::uint64_t window_id)
   {
      if (width <= 0 || height <= 0) {
         return core::fail (core::Error::make (core::ErrorKind::Capture,
            "gdi: zero-size capture ({} x {})", width, height));
      }

      HDC mem_dc = ::CreateCompatibleDC (src_dc);
      if (!mem_dc) {
         return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: CreateCompatibleDC failed"));
      }

      BITMAPINFO bi { };
      bi.bmiHeader.biSize        = sizeof (BITMAPINFOHEADER);
      bi.bmiHeader.biWidth       = width;
      bi.bmiHeader.biHeight      = -height;            // top-down
      bi.bmiHeader.biPlanes      = 1;
      bi.bmiHeader.biBitCount    = 32;
      bi.bmiHeader.biCompression = BI_RGB;

      void* bits = nullptr;
      HBITMAP dib = ::CreateDIBSection (src_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);

      if (!dib || !bits) {
         ::DeleteDC (mem_dc);
         return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: CreateDIBSection failed"));
      }

      HGDIOBJ old = ::SelectObject (mem_dc, dib);

      if (!::BitBlt (mem_dc, 0, 0, width, height, src_dc, 0, 0, SRCCOPY)) {
         ::SelectObject (mem_dc, old);
         ::DeleteObject (dib);
         ::DeleteDC     (mem_dc);
         return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: BitBlt failed"));
      }

      const int stride = width * 4;
      auto pixels = std::shared_ptr<std::uint8_t []> (
         new std::uint8_t [ static_cast<std::size_t> (stride) * height ]
      );

      std::memcpy (pixels.get (), bits, static_cast<std::size_t> (stride) * height);

      ::SelectObject (mem_dc, old);
      ::DeleteObject (dib);
      ::DeleteDC     (mem_dc);

      return Frame {
         .data       = std::move (pixels),
         .width      = width,
         .height     = height,
         .stride     = stride,
         .dpi_scale  = 1.0,
         .monitor_id = monitor_id,
         .window_id  = window_id,
         .timestamp  = std::chrono::steady_clock::now (),
         .cursor     = cursor_now (),
      };
   }

} // namespace

core::Result<Frame> GdiBitBltStrategy::capture_window (void* window)
{
   auto hwnd = static_cast<HWND> (window);
   if (!::IsWindow (hwnd)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: invalid HWND"));
   }

   RECT r;
   if (!::GetClientRect (hwnd, &r)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: GetClientRect failed"));
   }

   HDC dc = ::GetDC (hwnd);
   if (!dc) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: GetDC failed"));
   }

   auto frame = capture_dc (dc, r.right - r.left, r.bottom - r.top, 0,
                            reinterpret_cast<std::uintptr_t> (hwnd));
   ::ReleaseDC (hwnd, dc);
   return frame;
}

core::Result<Frame> GdiBitBltStrategy::capture_monitor (void* monitor)
{
   auto hmon = static_cast<HMONITOR> (monitor);

   MONITORINFOEXW mi { };
   mi.cbSize = sizeof (mi);
   if (!::GetMonitorInfoW (hmon, &mi)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: GetMonitorInfo failed"));
   }

   HDC dc = ::CreateDCW (L"DISPLAY", mi.szDevice, nullptr, nullptr);
   if (!dc) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: CreateDC failed"));
   }

   auto frame = capture_dc (dc, mi.rcMonitor.right - mi.rcMonitor.left,
                                mi.rcMonitor.bottom - mi.rcMonitor.top,
                                reinterpret_cast<std::uintptr_t> (hmon), 0);
   ::DeleteDC (dc);
   return frame;
}

std::string_view GdiBitBltStrategy::name   () const noexcept { return "gdi.bitblt"; }
std::string_view GdiBitBltStrategy::reason () const noexcept { return "Vista+; GDI fallback (no DXGI fullscreen capture)"; }

} // namespace gv::capture
