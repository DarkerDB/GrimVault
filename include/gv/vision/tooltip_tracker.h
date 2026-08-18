#pragma once

#include <gv/capture/frame.h>

#include <opencv2/core.hpp>

#include <array>
#include <optional>
#include <cstdint>

namespace gv::vision {

enum class AxisPin : std::uint8_t { Free, Low, High };

// Anchoring's vision primitives. The
// detector finds a tooltip coarsely; these run at full resolution:
//
//    refine       snap a coarse box to the tooltip's frame art, ~1 px
//    fingerprint  grab a small border patch for later verification
//    verify       is the fingerprint still at the predicted position?
//
// Pure OpenCV, no Qt, no state: unit-testable against synthetic frames.

struct Anchor {
   int      offset_x = 0;        // tooltip top-left minus cursor, physical px
   int      offset_y = 0;
   bool     locked_x = false;    // measured while not pinned at a clamp edge
   bool     locked_y = false;
   AxisPin  axis_x   = AxisPin::Free;
   AxisPin  axis_y   = AxisPin::Free;
   int      pin_x    = 0;        // fixed frame-relative coordinate while pinned
   int      pin_y    = 0;
   int      w        = 0;
   int      h        = 0;
   cv::Mat  fingerprint;         // gray patch
   int      fp_dx    = 0;        // fingerprint offset inside the box
   int      fp_dy    = 0;
   std::array<cv::Mat, 4> content_fingerprints;
   std::array<int, 4> content_dx {};
   std::array<int, 4> content_dy {};
   int      release_x = 0;
   int      release_y = 0;

   void acquire (const capture::Rect& box, const capture::CursorPos& cursor,
                 int frame_width, int frame_height,
                 int near_edge_px = 48, int right_edge_px = 32);
   void update (const capture::Rect& box, const capture::CursorPos& cursor,
                int frame_width, int frame_height,
                int near_edge_px = 48, int right_edge_px = 32);
};

enum class TooltipPresence : std::uint8_t { Present, Changed, Absent, Uncertain };

struct TooltipTracking {
   TooltipPresence presence = TooltipPresence::Uncertain;
   capture::Rect box;
   double frame_confidence = 0.0;
   double content_confidence = 0.0;
};

struct TooltipSelection {
   capture::Rect rect;
   bool refined = false;
};

class TooltipTracker
{
public:
   static TooltipSelection select (const cv::Mat& bgra,
                                   const capture::Rect& coarse);
   static void remember (const cv::Mat& bgra, const capture::Rect& box, Anchor& anchor);
   static TooltipTracking track (const cv::Mat& bgra, const Anchor& anchor,
                                 int pred_x, int pred_y, int search_px = 24);

   // Snap each edge of `coarse` to the strongest gradient ridge within
   // the search margin. Returns nullopt when no convincing ridge exists
   // (mid fade-in, occlusion, detector ghost).
   static std::optional<capture::Rect> refine (const cv::Mat& bgra,
                                               const capture::Rect& coarse);

   // Border patch from the box's top frame; fills fp_dx / fp_dy.
   static cv::Mat fingerprint (const cv::Mat& bgra, const capture::Rect& box,
                               int& fp_dx, int& fp_dy);

   // NCC-match the anchor's fingerprint around the predicted top-left
   // (±search_px). True while the tooltip is still there.
   static bool verify (const cv::Mat& bgra, const Anchor& anchor,
                       int pred_x, int pred_y, int search_px = 4);

   // Locate the existing fingerprint near a prediction and return the
   // observed tooltip box. Intended for identity sampling only; callers need
   // not use this geometry for presentation.
   static std::optional<capture::Rect> locate (const cv::Mat& bgra,
                                               const Anchor& anchor,
                                               int pred_x, int pred_y,
                                               int search_px = 16);
   static std::optional<capture::Rect> locate (const cv::Mat& bgra,
                                               const Anchor& anchor,
                                               int pred_x, int pred_y,
                                               int search_x, int search_y);

};

} // namespace gv::vision
