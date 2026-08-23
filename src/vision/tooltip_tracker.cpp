#include <gv/vision/tooltip_tracker.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace gv::vision {

namespace {

   constexpr int k_margin      = 20;    // search reach around each coarse edge
   constexpr int k_min_side    = 40;    // anything smaller is not a tooltip
   constexpr int k_fp_h        = 24;    // fingerprint corner-block size
   constexpr int k_fp_w        = 48;
   constexpr int k_content_h   = 40;
   constexpr int k_content_w   = 192;
   constexpr int k_content_search = 8;
   constexpr double k_ridge    = 3.0;   // peak must be this x mean to count
   constexpr double k_verify   = 0.85;  // NCC acceptance
   constexpr double k_absent   = 0.45;
   constexpr double k_content_present = 0.72;
   constexpr double k_content_changed = 0.55;
   constexpr double k_rebase_content_present = 0.80;
   constexpr double k_rebase_size_ratio = 1.2;

   struct Match {
      capture::Rect box;
      double confidence = 0.0;
   };

   cv::Mat gray_of (const cv::Mat& bgra, const cv::Rect& roi)
   {
      cv::Mat gray;
      cv::cvtColor (bgra (roi), gray,
         bgra.channels () == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
      return gray;
   }

   // Strongest projection peak inside [lo, hi); -1 when nothing rises far
   // enough above the mean to be a frame edge.
   int ridge_peak (const cv::Mat& projection, int lo, int hi)
   {
      lo = std::max (lo, 0);
      hi = std::min (hi, projection.rows * projection.cols);
      if (hi - lo < 3) return -1;

      double mean = 0.0;
      for (int i = 0; i < projection.rows * projection.cols; ++i) {
         mean += projection.at<float> (i);
      }
      mean /= projection.rows * projection.cols;

      int    best   = -1;
      double best_v = 0.0;
      for (int i = lo; i < hi; ++i) {
         const double v = projection.at<float> (i);
         if (v > best_v) { best_v = v; best = i; }
      }

      return (best >= 0 && best_v > mean * k_ridge) ? best : -1;
   }

   Match match (
      const cv::Mat& bgra,
      const cv::Mat& fingerprint,
      int fp_dx,
      int fp_dy,
      int width,
      int height,
      int pred_x,
      int pred_y,
      int search_x,
      int search_y)
   {
      if (fingerprint.empty ()) return {};

      cv::Rect window {
         pred_x + fp_dx - search_x,
         pred_y + fp_dy - search_y,
         fingerprint.cols + 2 * search_x,
         fingerprint.rows + 2 * search_y };
      window &= cv::Rect { 0, 0, bgra.cols, bgra.rows };
      if (window.width < fingerprint.cols || window.height < fingerprint.rows) return {};

      cv::Mat scores;
      cv::matchTemplate (gray_of (bgra, window), fingerprint, scores, cv::TM_CCOEFF_NORMED);

      Match result;
      cv::Point at;
      cv::minMaxLoc (scores, nullptr, &result.confidence, nullptr, &at);
      result.box = {
         window.x + at.x - fp_dx,
         window.y + at.y - fp_dy,
         width,
         height,
      };
      return result;
   }

   cv::Mat content_fingerprint (
      const cv::Mat& bgra,
      const capture::Rect& box,
      std::size_t index,
      int& dx,
      int& dy)
   {
      const int width = std::min (k_content_w, box.w - 16);
      const int height = std::min (k_content_h, box.h - 16);
      if (width < 32 || height < 16) return {};
      dx = (box.w - width) / 2;
      dy = index == 3 ? box.h - height - 1
         : 8 + static_cast<int> (index) * (box.h - height - 16) / 3;
      cv::Rect patch { box.x + dx, box.y + dy, width, height };
      patch &= cv::Rect { 0, 0, bgra.cols, bgra.rows };
      return gray_of (bgra, patch).clone ();
   }

   bool size_changed (int first, int second)
   {
      const auto smaller = std::min (first, second);
      const auto larger = std::max (first, second);
      return smaller <= 0 || static_cast<double> (larger) / smaller >= k_rebase_size_ratio;
   }

   AxisPin pin (int position, int size, int extent, int low, int high)
   {
      if (position <= low) return AxisPin::Low;
      if (extent - position - size <= high) return AxisPin::High;
      return AxisPin::Free;
   }

   void update_axis (
      AxisPin& axis,
      int& fixed,
      int& offset,
      bool& locked,
      int& releases,
      int position,
      int size,
      int extent,
      int cursor,
      bool cursor_valid,
      int low,
      int high)
   {
      const bool held = axis == AxisPin::Low
         ? position <= low + 24
         : axis == AxisPin::High && extent - position - size <= high + 24;
      if (axis != AxisPin::Free) {
         if (held) {
            fixed = position;
            releases = 0;
            return;
         }
         ++releases;
         if (releases < 2) return;
         axis = AxisPin::Free;
      }

      fixed = position;
      axis = pin (position, size, extent, low, high);
      if (axis != AxisPin::Free) {
         releases = 0;
      } else if (cursor_valid) {
         offset = position - cursor;
         locked = true;
      }
   }

} // namespace

void Anchor::acquire (
   const capture::Rect& box,
   const capture::CursorPos& cursor,
   int frame_width,
   int frame_height,
   int near_edge_px,
   int right_edge_px)
{
   *this = {};
   w = box.w;
   h = box.h;
   pin_x = box.x;
   pin_y = box.y;
   axis_x = pin (box.x, box.w, frame_width, near_edge_px, right_edge_px);
   axis_y = pin (box.y, box.h, frame_height, near_edge_px, near_edge_px);
   release_x = 0;
   release_y = 0;
   if (cursor.valid && axis_x == AxisPin::Free) {
      offset_x = box.x - cursor.x;
      locked_x = true;
   }
   if (cursor.valid && axis_y == AxisPin::Free) {
      offset_y = box.y - cursor.y;
      locked_y = true;
   }
}

void Anchor::update (
   const capture::Rect& box,
   const capture::CursorPos& cursor,
   int frame_width,
   int frame_height,
   int near_edge_px,
   int right_edge_px)
{
   w = box.w;
   h = box.h;
   update_axis (
      axis_x, pin_x, offset_x, locked_x, release_x,
      box.x, box.w, frame_width, cursor.x, cursor.valid,
      near_edge_px, right_edge_px);
   update_axis (
      axis_y, pin_y, offset_y, locked_y, release_y,
      box.y, box.h, frame_height, cursor.y, cursor.valid,
      near_edge_px, near_edge_px);
}

TooltipSelection TooltipTracker::select (
   const cv::Mat& bgra, const capture::Rect& coarse)
{
   const auto refined = refine (bgra, coarse);
   return {
      .rect = refined.value_or (coarse),
      .refined = refined.has_value (),
   };
}

void TooltipTracker::remember (
   const cv::Mat& bgra, const capture::Rect& box, Anchor& anchor)
{
   anchor.w = box.w;
   anchor.h = box.h;
   anchor.fingerprint = fingerprint (bgra, box, anchor.fp_dx, anchor.fp_dy);
   for (std::size_t index = 0; index < anchor.content_fingerprints.size (); ++index) {
      anchor.content_fingerprints [index] = content_fingerprint (
         bgra, box, index, anchor.content_dx [index], anchor.content_dy [index]);
   }
}

TooltipTracking TooltipTracker::track (
   const cv::Mat& bgra,
   const Anchor& anchor,
   int pred_x,
   int pred_y,
   int search_px)
{
   const auto frame = match (
      bgra, anchor.fingerprint, anchor.fp_dx, anchor.fp_dy,
      anchor.w, anchor.h, pred_x, pred_y, search_px, search_px);

   TooltipTracking result {
      .box = frame.box,
      .frame_confidence = frame.confidence,
   };
   if (frame.confidence < k_absent) {
      result.presence = TooltipPresence::Absent;
      return result;
   }
   if (frame.confidence < k_verify) return result;

   double total = 0.0;
   double weakest = 1.0;
   int count = 0;
   std::array<int, 4> content_x {};
   std::array<int, 4> content_y {};
   int located = 0;
   for (std::size_t index = 0; index < anchor.content_fingerprints.size (); ++index) {
      if (anchor.content_fingerprints [index].empty ()) continue;
      const auto content = match (
         bgra,
         anchor.content_fingerprints [index],
         anchor.content_dx [index],
         anchor.content_dy [index],
         anchor.w,
         anchor.h,
         pred_x,
         pred_y,
         std::max (k_content_search, search_px),
         std::max (k_content_search, search_px));
      total += content.confidence;
      weakest = std::min (weakest, content.confidence);
      if (content.confidence >= k_content_present) {
         content_x [located] = content.box.x;
         content_y [located] = content.box.y;
         ++located;
      }
      ++count;
   }
   if (count == 0) return result;

   result.content_confidence = total / count;
   if (result.content_confidence >= k_content_present && weakest >= k_content_present) {
      std::sort (content_x.begin (), content_x.begin () + located);
      std::sort (content_y.begin (), content_y.begin () + located);
      result.box.x = content_x [located / 2];
      result.box.y = content_y [located / 2];
      result.presence = TooltipPresence::Present;
      return result;
   }
   if (result.content_confidence < k_content_changed || weakest < k_absent) {
      result.presence = TooltipPresence::Changed;
   }
   return result;
}

TooltipTracking TooltipTracker::rebase (
   const cv::Mat& bgra,
   const Anchor& anchor,
   const capture::Rect& box,
   int search_px)
{
   TooltipTracking result { .box = box };
   if (size_changed (anchor.w, box.w) || size_changed (anchor.h, box.h)) {
      result.presence = TooltipPresence::Changed;
      return result;
   }

   const auto frame = match (
      bgra, anchor.fingerprint, anchor.fp_dx, anchor.fp_dy,
      box.w, box.h, box.x, box.y, search_px, search_px);
   result.frame_confidence = frame.confidence;

   double total = 0.0;
   double weakest = 1.0;
   int count = 0;
   for (std::size_t index = 0; index < anchor.content_fingerprints.size (); ++index) {
      const auto& fingerprint = anchor.content_fingerprints [index];
      if (fingerprint.empty ()) continue;
      const int dx = (box.w - fingerprint.cols) / 2;
      const int dy = index == 3 ? box.h - fingerprint.rows - 1
         : 8 + static_cast<int> (index) * (box.h - fingerprint.rows - 16) / 3;
      const auto content = match (
         bgra, fingerprint, dx, dy, box.w, box.h,
         box.x, box.y, search_px, search_px);
      total += content.confidence;
      weakest = std::min (weakest, content.confidence);
      ++count;
   }
   if (count == 0) return result;

   result.content_confidence = total / count;
   if (result.content_confidence >= k_rebase_content_present && weakest >= k_absent)
      result.presence = TooltipPresence::Present;
   else if (result.content_confidence < k_content_changed || weakest < k_absent)
      result.presence = TooltipPresence::Changed;
   return result;
}

std::optional<capture::Rect> TooltipTracker::refine (const cv::Mat& bgra,
                                                     const capture::Rect& coarse)
{
   const cv::Rect frame_rect { 0, 0, bgra.cols, bgra.rows };
   cv::Rect roi {
      coarse.x - k_margin, coarse.y - k_margin,
      coarse.w + 2 * k_margin, coarse.h + 2 * k_margin };
   roi &= frame_rect;

   if (roi.width < k_min_side || roi.height < k_min_side) return std::nullopt;

   const cv::Mat gray = gray_of (bgra, roi);

   cv::Mat gx, gy;
   cv::Sobel (gray, gx, CV_32F, 1, 0, 3);
   cv::Sobel (gray, gy, CV_32F, 0, 1, 3);
   gx = cv::abs (gx);
   gy = cv::abs (gy);

   // Vertical frame edges light up column sums of |d/dx|; horizontal edges
   // light up row sums of |d/dy|.
   cv::Mat colsum, rowsum;
   cv::reduce (gx, colsum, 0, cv::REDUCE_AVG, CV_32F);   // 1 x W
   cv::reduce (gy, rowsum, 1, cv::REDUCE_AVG, CV_32F);   // H x 1
   colsum = colsum.reshape (1, colsum.cols);

   const int ex_l = coarse.x - roi.x;                    // expected edges, roi space
   const int ex_r = ex_l + coarse.w;
   const int ex_t = coarse.y - roi.y;
   const int ex_b = ex_t + coarse.h;

   const int left   = ridge_peak (colsum, ex_l - k_margin, ex_l + k_margin);
   const int right  = ridge_peak (colsum, ex_r - k_margin, ex_r + k_margin);
   const int top    = ridge_peak (rowsum, ex_t - k_margin, ex_t + k_margin);
   const int bottom = ridge_peak (rowsum, ex_b - k_margin, ex_b + k_margin);

   if (left < 0 || right < 0 || top < 0 || bottom < 0) return std::nullopt;
   if (right - left < k_min_side || bottom - top < k_min_side) return std::nullopt;

   return capture::Rect {
      roi.x + left,
      roi.y + top,
      right - left,
      bottom - top,
   };
}

cv::Mat TooltipTracker::fingerprint (const cv::Mat& bgra, const capture::Rect& box,
                                     int& fp_dx, int& fp_dy)
{
   // Top-left corner block: the frame's L-junction (and, in-game, the
   // corner ornament) is 2D structure that exists nowhere else. A border
   // strip alone is just a bright bar and false-matches any other bar.
   const int w = std::min (k_fp_w, box.w - 4);
   const int h = std::min (k_fp_h, box.h - 4);
   fp_dx = 1;
   fp_dy = 1;

   cv::Rect patch { box.x + fp_dx, box.y + fp_dy, w, h };
   patch &= cv::Rect { 0, 0, bgra.cols, bgra.rows };

   return gray_of (bgra, patch).clone ();
}

bool TooltipTracker::verify (const cv::Mat& bgra, const Anchor& anchor,
                             int pred_x, int pred_y, int search_px)
{
   return locate (bgra, anchor, pred_x, pred_y, search_px).has_value ();
}

std::optional<capture::Rect> TooltipTracker::locate (
   const cv::Mat& bgra, const Anchor& anchor,
   int pred_x, int pred_y, int search_px)
{
   return locate (bgra, anchor, pred_x, pred_y, search_px, search_px);
}

std::optional<capture::Rect> TooltipTracker::locate (
   const cv::Mat& bgra, const Anchor& anchor,
   int pred_x, int pred_y, int search_x, int search_y)
{
   const auto result = match (
      bgra, anchor.fingerprint, anchor.fp_dx, anchor.fp_dy,
      anchor.w, anchor.h, pred_x, pred_y, search_x, search_y);
   return result.confidence >= k_verify
      ? std::optional<capture::Rect> { result.box }
      : std::nullopt;
}

} // namespace gv::vision
