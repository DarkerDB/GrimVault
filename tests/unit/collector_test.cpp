#include <gv/ocr/collector.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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
   cv::Mat tooltip { 80, 120, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::Mat title { 24, 70, CV_8UC4, cv::Scalar { 220, 160, 20, 255 } };
   const std::vector<gv::ocr::EvidenceLine> lines {{
      .image = title,
      .source_band = 0,
      .title = true,
      .prediction = "신탁의로브",
      .confidence = 0.91f,
   }};
   gv::ocr::Collector collector { root };

   EXPECT_TRUE (collector.save (
      24, "ko", 42, { 10, 20, 120, 80 }, tooltip, lines, "신탁의로브", 0.91f));
   EXPECT_FALSE (collector.save (
      25, "ko", 42, { 10, 20, 120, 80 }, tooltip, lines, "신탁의로브", 0.91f));

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
   EXPECT_EQ (metadata ["lines"].size (), 1);
}
