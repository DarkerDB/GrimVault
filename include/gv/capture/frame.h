#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace gv::capture {

enum class CaptureBackend : std::uint8_t { Unknown, Wgc, Dxgi, Gdi };

constexpr std::string_view backend_name (CaptureBackend backend) noexcept
{
   switch (backend) {
      case CaptureBackend::Wgc:  return "wgc";
      case CaptureBackend::Dxgi: return "dxgi";
      case CaptureBackend::Gdi:  return "gdi";
      default:                   return "unknown";
   }
}

struct Rect {
   int x = 0;
   int y = 0;
   int w = 0;
   int h = 0;

   constexpr bool empty () const noexcept { return w <= 0 || h <= 0; }
};

constexpr float intersection_over_union (const Rect& a, const Rect& b) noexcept
{
   const int left   = a.x > b.x ? a.x : b.x;
   const int top    = a.y > b.y ? a.y : b.y;
   const int right  = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
   const int bottom = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
   const int width  = right - left;
   const int height = bottom - top;
   if (width <= 0 || height <= 0) return 0.0f;

   const auto overlap = static_cast<long long> (width) * height;
   const auto total = static_cast<long long> (a.w) * a.h
      + static_cast<long long> (b.w) * b.h - overlap;
   return total > 0 ? static_cast<float> (overlap) / static_cast<float> (total) : 0.0f;
}

// Cursor position in physical screen pixels, sampled the moment a frame is
// produced. Anchoring pairs each frame with where the mouse was THEN, so
// the cursor->tooltip offset survives any pipeline latency.
struct CursorPos {
   int  x     = 0;
   int  y     = 0;
   bool valid = false;
};

CursorPos cursor_now () noexcept;

// A single captured frame. BGRA8 pixel format; ownership is shared so the
// frame can outlive the strategy that produced it (it'll be queued across
// the capture → vision → ocr → parse pipeline).
//
// Strategies allocate the buffer; vision wraps it as a cv::Mat without
// copying:
//    cv::Mat (frame.height, frame.width, CV_8UC4, frame.data.get (), frame.stride);
struct Frame {
   std::shared_ptr<std::uint8_t []>      data;
   int                                   width      = 0;
   int                                   height     = 0;
   int                                   stride     = 0;          // bytes per row
   int                                   origin_x   = 0;          // frame top-left in desktop pixels
   int                                   origin_y   = 0;
   double                                dpi_scale  = 1.0;
   std::uint64_t                         monitor_id = 0;
   std::uint64_t                         window_id  = 0;          // HWND as uint, 0 = monitor capture
   CaptureBackend                        backend    = CaptureBackend::Unknown;
   std::chrono::steady_clock::time_point timestamp;
   CursorPos                             cursor;

   bool empty () const noexcept { return !data || width == 0 || height == 0; }

   CursorPos local_cursor () const noexcept
   {
      if (!cursor.valid) return {};
      return CursorPos { cursor.x - origin_x, cursor.y - origin_y, true };
   }
};

} // namespace gv::capture
