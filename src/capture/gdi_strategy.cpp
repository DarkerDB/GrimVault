#include "gdi_strategy.h"

#include <gv/core/logger.h>

#include <Windows.h>

#include <algorithm>
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

   bool visible (const std::uint8_t* pixels, int width, int height, int stride)
   {
      const int step_x = std::max (1, width / 64);
      const int step_y = std::max (1, height / 64);
      for (int y = 0; y < height; y += step_y) {
         const auto* row = pixels + static_cast<std::size_t> (y) * stride;
         for (int x = 0; x < width; x += step_x) {
            const auto* pixel = row + static_cast<std::size_t> (x) * 4;
            if (pixel [0] > 4 || pixel [1] > 4 || pixel [2] > 4) return true;
         }
      }
      return false;
   }

   core::Result<Frame> capture_dc (HDC src_dc, int source_x, int source_y,
                                   int width, int height,
                                   int origin_x, int origin_y,
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

      if (!::BitBlt (mem_dc, 0, 0, width, height, src_dc, source_x, source_y, SRCCOPY)) {
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

      if (!visible (pixels.get (), width, height, stride)) {
         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "gdi: captured frame is blank"));
      }

      return Frame {
         .data       = std::move (pixels),
         .width      = width,
         .height     = height,
         .stride     = stride,
         .origin_x   = origin_x,
         .origin_y   = origin_y,
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

   POINT origin {};
   if (!::ClientToScreen (hwnd, &origin)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "gdi: ClientToScreen failed"));
   }

   const auto monitor = ::MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
   MONITORINFOEXW mi { };
   mi.cbSize = sizeof (mi);
   if (!monitor || !::GetMonitorInfoW (monitor, &mi)) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture, "gdi: GetMonitorInfo failed"));
   }

   RECT wanted {
      origin.x,
      origin.y,
      origin.x + r.right - r.left,
      origin.y + r.bottom - r.top,
   };
   RECT clipped {
      std::max (wanted.left, mi.rcMonitor.left),
      std::max (wanted.top, mi.rcMonitor.top),
      std::min (wanted.right, mi.rcMonitor.right),
      std::min (wanted.bottom, mi.rcMonitor.bottom),
   };
   if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture, "gdi: game window is outside the selected monitor"));
   }

   HDC dc = ::CreateDCW (L"DISPLAY", mi.szDevice, nullptr, nullptr);
   if (!dc) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "gdi: CreateDC failed"));
   }

   auto frame = capture_dc (
      dc,
      clipped.left - mi.rcMonitor.left,
      clipped.top - mi.rcMonitor.top,
      clipped.right - clipped.left,
      clipped.bottom - clipped.top,
      clipped.left,
      clipped.top,
      reinterpret_cast<std::uintptr_t> (monitor),
                            reinterpret_cast<std::uintptr_t> (hwnd));
   ::DeleteDC (dc);
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

   auto frame = capture_dc (dc, 0, 0,
                            mi.rcMonitor.right - mi.rcMonitor.left,
                            mi.rcMonitor.bottom - mi.rcMonitor.top,
                            mi.rcMonitor.left, mi.rcMonitor.top,
                            reinterpret_cast<std::uintptr_t> (hmon), 0);
   ::DeleteDC (dc);
   return frame;
}

std::string_view GdiBitBltStrategy::name   () const noexcept { return "gdi.bitblt"; }
std::string_view GdiBitBltStrategy::reason () const noexcept { return "manual compatibility capture; visible monitor pixels only"; }

} // namespace gv::capture
