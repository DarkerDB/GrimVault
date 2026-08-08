#include <gv/vision/gem_detector.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace gv::vision {

namespace {

struct Candidate {
   std::string family;
   double score = 0.0;
};

Candidate classify (const cv::Mat& hsv, const cv::Mat& mask, bool diamond)
{
   cv::Mat labels, stats, centroids;
   const int count = cv::connectedComponentsWithStats (
      mask, labels, stats, centroids, 8, CV_32S);

   Candidate best;
   for (int label = 1; label < count; ++label) {
      const int area = stats.at<int> (label, cv::CC_STAT_AREA);
      const int w = stats.at<int> (label, cv::CC_STAT_WIDTH);
      const int h = stats.at<int> (label, cv::CC_STAT_HEIGHT);
      if (area < (diamond ? 32 : 18) || area > 1400 || w < 8 || h < 8
          || w > 64 || h > 64) continue;

      const double aspect = static_cast<double> (w) / h;
      const double fill = static_cast<double> (area) / (w * h);
      if (aspect < 0.65 || aspect > 1.50 || fill < (diamond ? 0.30 : 0.20)) continue;

      cv::Mat component = labels == label;
      const cv::Scalar mean = cv::mean (hsv, component);
      std::string family;
      if (diamond) {
         if (mean [1] > 60 || mean [2] < 175) continue;
         family = "diamond";
      } else {
         const double hue = mean [0];
         if (hue <= 12 || hue >= 168) family = "ruby";
         else if (hue >= 35 && hue <= 88) family = "emerald";
         else if (hue >= 90 && hue <= 138) family = "blue_sapphire";
         else continue;
      }

      const double square = 1.0 - std::min (1.0, std::abs (1.0 - aspect));
      const double score = area * (0.5 + fill) * (0.5 + square);
      if (score > best.score) best = Candidate { std::move (family), score };
   }
   return best;
}

Candidate classify_cloud (const cv::Mat& hsv, const cv::Mat& mask, bool diamond)
{
   std::vector<cv::Point> points;
   cv::findNonZero (mask, points);
   if (points.size () < (diamond ? 18u : 8u)) return {};

   const cv::Rect bounds = cv::boundingRect (points);
   if (bounds.width < 8 || bounds.height < 8
       || bounds.width > 48 || bounds.height > 48) return {};

   const double aspect = static_cast<double> (bounds.width) / bounds.height;
   const double fill = static_cast<double> (points.size ()) / bounds.area ();
   if (aspect < 0.60 || aspect > 1.70 || fill < 0.06) return {};

   const cv::Scalar mean = cv::mean (hsv, mask);
   std::string family;
   if (diamond) {
      if (mean [1] > 60 || mean [2] < 175) return {};
      family = "diamond";
   } else {
      const double hue = mean [0];
      if (hue <= 12 || hue >= 168) family = "ruby";
      else if (hue >= 35 && hue <= 88) family = "emerald";
      else if (hue >= 90 && hue <= 138) family = "blue_sapphire";
      else return {};
   }

   const double square = 1.0 - std::min (1.0, std::abs (1.0 - aspect));
   return { std::move (family), points.size () * (0.4 + fill) * (0.5 + square) };
}

Candidate inspect_gutter (const cv::Mat& bgr)
{
   cv::Mat hsv;
   cv::cvtColor (bgr, hsv, cv::COLOR_BGR2HSV);

   cv::Mat colorful, white;
   cv::inRange (hsv, cv::Scalar { 0, 90, 70 }, cv::Scalar { 179, 255, 255 }, colorful);
   cv::inRange (hsv, cv::Scalar { 0, 0, 185 }, cv::Scalar { 179, 55, 255 }, white);

   const auto kernel = cv::getStructuringElement (cv::MORPH_ELLIPSE, { 3, 3 });
   cv::morphologyEx (colorful, colorful, cv::MORPH_CLOSE, kernel);
   cv::morphologyEx (white, white, cv::MORPH_CLOSE, kernel);

   Candidate color = classify (hsv, colorful, false);
   Candidate clear = classify (hsv, white, true);
   const Candidate color_cloud = classify_cloud (hsv, colorful, false);
   const Candidate clear_cloud = classify_cloud (hsv, white, true);
   if (color_cloud.score > color.score) color = color_cloud;
   if (clear_cloud.score > clear.score) clear = clear_cloud;
   return color.score >= clear.score * 0.9 ? color : clear;
}

} // namespace

std::optional<std::string> detect_gem_family (const cv::Mat& input)
{
   if (input.empty () || input.cols < 40 || input.rows < 8) return std::nullopt;

   cv::Mat bgr;
   if (input.channels () == 4) cv::cvtColor (input, bgr, cv::COLOR_BGRA2BGR);
   else if (input.channels () == 3) bgr = input;
   else return std::nullopt;

   // Tooltip lines scale with Windows DPI and game resolution. Normalize the
   // line height before applying pixel-size guards so the same socket glyph
   // remains recognizable from 100% through 200% display scaling.
   constexpr int normalized_height = 40;
   if (bgr.rows != normalized_height) {
      const double scale = static_cast<double> (normalized_height) / bgr.rows;
      cv::resize (bgr, bgr, {}, scale, scale,
         scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
   }

   const int gutter = std::clamp (bgr.cols / 5, 28, 64);
   const std::array<cv::Rect, 2> sides {{
      { 0, 0, gutter, bgr.rows },
      { bgr.cols - gutter, 0, gutter, bgr.rows },
   }};

   Candidate best;
   for (const auto& side : sides) {
      auto candidate = inspect_gutter (bgr (side));
      if (candidate.score > best.score) best = std::move (candidate);
   }
   if (best.family.empty ()) return std::nullopt;
   return best.family;
}

} // namespace gv::vision
