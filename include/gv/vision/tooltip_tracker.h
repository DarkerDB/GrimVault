#pragma once

#include <gv/capture/frame.h>

#include <opencv2/core.hpp>

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
   // Bottom edge as `measure_height` reads it at acquisition. Compared
   // against itself on later frames, never against `h`: `refine` snaps to the
   // frame's inner ridge and the probe can settle on an outer one, so the two
   // differ by a constant offset on a perfectly static card. Baselining the
   // probe against its own first reading cancels that bias.
   int      measured_h = 0;
   std::uint64_t content_hash = 0; // coarse interior dHash for change detection
   cv::Mat  detail_thumbnail;      // aligned 32x32 interior detail signature
};

class TooltipTracker
{
public:
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

   // Measure the tooltip's height by snapping only its bottom edge, given a
   // box whose top-left is known (from `locate`) and whose height is the
   // anchor's — i.e. possibly stale. One row projection over a thin band, so
   // it costs a fraction of `refine` and can run on every anchored frame.
   //
   // This is the signal that catches a swap `locate` cannot: the fingerprint
   // is the top-left corner block, which is identical art on every tooltip,
   // so a replacement card under a barely-moved cursor still matches. Item
   // height varies with stat-line count, and no amount of capture noise
   // moves a frame edge, so a height delta is a replacement outright rather
   // than one vote among several.
   //
   // `search_px` has to cover the height difference between two real items,
   // not a jitter tolerance: stat-line count swings tooltip height by well
   // over a hundred pixels, and a band too narrow to reach the new edge just
   // declines and falls back to the interior hash — the case this exists to
   // cover. Cost is one Sobel over `width x 2*search_px`, so reach is cheap.
   //
   // Returns nullopt when no convincing bottom ridge is in reach, which is
   // the same "stay put" answer `refine` gives mid fade-in.
   static std::optional<int> measure_height (const cv::Mat& bgra,
                                             const capture::Rect& box,
                                             int search_px = 160);

   // Fast, deliberately coarse hash of the tooltip interior. Stable under
   // tiny pixel noise but changes when a different object replaces the card.
   static std::uint64_t content_hash (const cv::Mat& bgra,
                                      const capture::Rect& box);
   static cv::Mat detail_thumbnail (const cv::Mat& bgra,
                                    const capture::Rect& box);
};

} // namespace gv::vision
