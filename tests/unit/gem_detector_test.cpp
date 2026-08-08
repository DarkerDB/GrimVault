#include <gv/ocr/preprocessor.h>
#include <gv/vision/gem_detector.h>

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

cv::Mat line_with_gem (const cv::Scalar& bgra)
{
   cv::Mat line { 40, 220, CV_8UC4, cv::Scalar { 12, 12, 12, 255 } };
   const std::vector<cv::Point> gem {
      { 198, 6 }, { 211, 20 }, { 198, 34 }, { 185, 20 }
   };
   cv::fillConvexPoly (line, gem, bgra);
   return line;
}

TEST (GemDetector, RecognizesSupportedFamilies)
{
   EXPECT_EQ (gv::vision::detect_gem_family (
      line_with_gem ({ 40, 40, 230, 255 })), "ruby");
   EXPECT_EQ (gv::vision::detect_gem_family (
      line_with_gem ({ 230, 80, 30, 255 })), "blue_sapphire");
   EXPECT_EQ (gv::vision::detect_gem_family (
      line_with_gem ({ 50, 210, 50, 255 })), "emerald");
   EXPECT_EQ (gv::vision::detect_gem_family (
      line_with_gem ({ 230, 230, 230, 255 })), "diamond");
}

TEST (GemDetector, RejectsEmptyGutters)
{
   cv::Mat line { 40, 220, CV_8UC4, cv::Scalar { 12, 12, 12, 255 } };
   EXPECT_FALSE (gv::vision::detect_gem_family (line).has_value ());
}

TEST (GemDetector, RecognizesSocketedStatsFromCapturedLyre)
{
   const std::string path = std::string { GRIMVAULT_TEST_SOURCE_DIR }
      + "/tests/fixtures/lyre_socketed_stats.png";
   const cv::Mat crop = cv::imread (path, cv::IMREAD_UNCHANGED);
   ASSERT_FALSE (crop.empty ());

   const auto bands = gv::ocr::preprocess::line_bands (crop);
   ASSERT_GE (bands.size (), 7u);
   const auto buff = gv::vision::detect_gem_family (
      crop (bands [3], cv::Range::all ()));
   EXPECT_FALSE (buff.has_value ()) << buff.value_or ("");
   EXPECT_EQ (gv::vision::detect_gem_family (
      crop (bands [4], cv::Range::all ())), "blue_sapphire");
   const auto armor = gv::vision::detect_gem_family (
      crop (bands [5], cv::Range::all ()));
   EXPECT_FALSE (armor.has_value ()) << armor.value_or ("");
   EXPECT_EQ (gv::vision::detect_gem_family (
      crop (bands [6], cv::Range::all ())), "emerald");
}

} // namespace
