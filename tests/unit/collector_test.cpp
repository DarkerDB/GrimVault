#include <gv/ocr/collector.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

class CollectorTest : public testing::Test
{
protected:
   void SetUp () override
   {
      root = std::filesystem::temp_directory_path () / "grimvault-collector-test";
      std::error_code error;
      std::filesystem::remove_all (root, error);
   }

   void TearDown () override
   {
      std::error_code error;
      std::filesystem::remove_all (root, error);
   }

   std::filesystem::path root;
};

}

TEST_F (CollectorTest, SavesLocalizedTrainingBundleWithoutFullFrame)
{
   cv::Mat tooltip { 180, 320, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::line (tooltip, { 4, 55 }, { 315, 55 }, cv::Scalar { 180, 160, 90, 255 }, 2);
   cv::Mat title { 24, 70, CV_8UC4, cv::Scalar { 220, 160, 20, 255 } };
   cv::Mat body { 24, 90, CV_8UC4, cv::Scalar { 200, 200, 200, 255 } };
   const std::vector<gv::ocr::EvidenceLine> lines {{
      .image = title,
      .source_band = 0,
      .title = true,
      .prediction = "신탁의로브",
      .confidence = 0.91f,
   }, {
      .image = body,
      .source_band = 1,
      .prediction = "방어 등급 10",
      .confidence = 0.91f,
   }, {
      .image = body,
      .source_band = 2,
      .prediction = "이동 속도 -5",
      .confidence = 0.91f,
   }, {
      .image = body,
      .source_band = 3,
      .prediction = "희귀한",
      .confidence = 0.91f,
   }};
   gv::ocr::Collector collector { root };

   EXPECT_TRUE (collector.save (
      24, "ko", 42, { 10, 20, 320, 180 }, tooltip, lines, "신탁의로브", 0.91f));
   EXPECT_FALSE (collector.save (
      25, "ko", 42, { 10, 20, 320, 180 }, tooltip, lines, "신탁의로브", 0.91f));

   std::vector<std::filesystem::path> samples;
   for (const auto& entry : std::filesystem::directory_iterator { root })
      if (entry.is_directory ()) samples.push_back (entry.path ());
   ASSERT_EQ (samples.size (), 1);
   EXPECT_TRUE (std::filesystem::exists (samples.front () / "tooltip.png"));
   EXPECT_TRUE (std::filesystem::exists (samples.front () / "line-00.png"));
   EXPECT_FALSE (std::filesystem::exists (samples.front () / "frame.jpg"));

   std::ifstream input { samples.front () / "metadata.json", std::ios::binary };
   const auto metadata = nlohmann::json::parse (input);
   EXPECT_EQ (metadata ["language"], "ko");
   EXPECT_EQ (metadata ["family"], "korean");
   EXPECT_EQ (metadata ["prediction"], "신탁의로브");
   EXPECT_EQ (metadata ["lines"].size (), 4);
}

TEST_F (CollectorTest, RejectsNonTooltipCapture)
{
   cv::Mat capture { 180, 320, CV_8UC4, cv::Scalar { 24, 24, 24, 255 } };
   cv::line (capture, { 0, 55 }, { 319, 55 }, cv::Scalar { 180, 180, 180, 255 }, 2);
   cv::Mat line { 24, 90, CV_8UC4, cv::Scalar { 200, 200, 200, 255 } };
   std::vector<gv::ocr::EvidenceLine> lines (4);
   for (auto& value : lines) value.image = line;
   gv::ocr::Collector collector { root };

   EXPECT_FALSE (collector.save (
      1, "ko", 42, { 10, 20, 320, 180 }, capture, lines, "", 0.0f));
   EXPECT_FALSE (std::filesystem::exists (root));
}
