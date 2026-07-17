#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace gv::capture {

struct Rect {
   int x = 0;
   int y = 0;
   int w = 0;
   int h = 0;

   constexpr bool empty () const noexcept { return w <= 0 || h <= 0; }
};

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
   double                                dpi_scale  = 1.0;
   std::uint64_t                         monitor_id = 0;
   std::uint64_t                         window_id  = 0;          // HWND as uint, 0 = monitor capture
   std::chrono::steady_clock::time_point timestamp;
   CursorPos                             cursor;

   bool empty () const noexcept { return !data || width == 0 || height == 0; }
};

} // namespace gv::capture
