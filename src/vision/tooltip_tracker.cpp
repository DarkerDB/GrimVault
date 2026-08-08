#include <gv/vision/tooltip_tracker.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>

namespace gv::vision {

namespace {

   constexpr int k_margin      = 20;    // search reach around each coarse edge
   constexpr int k_min_side    = 40;    // anything smaller is not a tooltip
   constexpr int k_fp_h        = 24;    // fingerprint corner-block size
   constexpr int k_fp_w        = 48;
   constexpr double k_ridge    = 3.0;   // peak must be this x mean to count
   constexpr double k_verify   = 0.85;  // NCC acceptance

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

} // namespace

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
   if (anchor.fingerprint.empty ()) return std::nullopt;

   cv::Rect window {
      pred_x + anchor.fp_dx - search_x,
      pred_y + anchor.fp_dy - search_y,
      anchor.fingerprint.cols + 2 * search_x,
      anchor.fingerprint.rows + 2 * search_y };
   window &= cv::Rect { 0, 0, bgra.cols, bgra.rows };

   if (window.width  < anchor.fingerprint.cols
    || window.height < anchor.fingerprint.rows) return std::nullopt;

   const cv::Mat gray = gray_of (bgra, window);

   cv::Mat scores;
   cv::matchTemplate (gray, anchor.fingerprint, scores, cv::TM_CCOEFF_NORMED);

   double max_v = 0.0;
   cv::Point max_at;
   cv::minMaxLoc (scores, nullptr, &max_v, nullptr, &max_at);

   if (max_v < k_verify) return std::nullopt;
   return capture::Rect {
      window.x + max_at.x - anchor.fp_dx,
      window.y + max_at.y - anchor.fp_dy,
      anchor.w,
      anchor.h,
   };
}

std::uint64_t TooltipTracker::content_hash (const cv::Mat& bgra,
                                            const capture::Rect& box)
{
   cv::Rect roi { box.x + 8, box.y + 8,
                  std::max (0, box.w - 16), std::max (0, box.h - 16) };
   roi &= cv::Rect { 0, 0, bgra.cols, bgra.rows };
   if (roi.width < 9 || roi.height < 8) return 0;

   // Fixed-cost grid hash: 72 local samples regardless of tooltip area.
   // A 3x3 average makes it insensitive to isolated raster noise.
   const auto sample = [&bgra, &roi] (int gx, int gy) {
      const int cx = roi.x + (gx * (roi.width  - 1)) / 8;
      const int cy = roi.y + (gy * (roi.height - 1)) / 7;
      int sum = 0, n = 0;
      for (int y = std::max (roi.y, cy - 1);
           y <= std::min (roi.y + roi.height - 1, cy + 1); ++y) {
         for (int x = std::max (roi.x, cx - 1);
              x <= std::min (roi.x + roi.width - 1, cx + 1); ++x) {
            const auto& p = bgra.at<cv::Vec4b> (y, x);
            sum += (29 * p [0] + 150 * p [1] + 77 * p [2]) >> 8;
            ++n;
         }
      }
      return n > 0 ? sum / n : 0;
   };
   std::array<int, 64> values {};
   int total = 0;
   for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
         const int value = sample (x, y);
         values [y * 8 + x] = value;
         total += value;
      }
   }
   const int mean = total / static_cast<int> (values.size ());
   std::uint64_t hash = 0;
   for (const int value : values) {
      hash <<= 1;
      hash |= value > mean;
   }
   return hash;
}

cv::Mat TooltipTracker::detail_thumbnail (const cv::Mat& bgra,
                                          const capture::Rect& box)
{
   cv::Rect roi { box.x + 8, box.y + 8,
                  std::max (0, box.w - 16), std::max (0, box.h - 16) };
   roi &= cv::Rect { 0, 0, bgra.cols, bgra.rows };
   if (roi.width < 32 || roi.height < 32) return {};

   cv::Mat detail;
   cv::resize (gray_of (bgra, roi), detail, { 32, 32 }, 0.0, 0.0, cv::INTER_AREA);
   return detail;
}

} // namespace gv::vision
