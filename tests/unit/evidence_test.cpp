#include <gv/ocr/evidence.h>

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

class EvidenceTest : public testing::Test
{
protected:
   void SetUp () override
   {
      root = std::filesystem::temp_directory_path () / "grimvault-evidence-test";
      std::error_code ec;
      std::filesystem::remove_all (root, ec);
      std::filesystem::create_directories (root, ec);
   }

   void TearDown () override
   {
      std::error_code ec;
      std::filesystem::remove_all (root, ec);
   }

   std::filesystem::path root;
};

}

TEST_F (EvidenceTest, TrimKeepsNewestBundlesWithinLimit)
{
   for (const auto& name : { "1", "2", "3" }) {
      const auto path = root / name;
      std::filesystem::create_directory (path);
      std::ofstream output { path / "payload.bin", std::ios::binary };
      output << std::string (64, name [0]);
      output.close ();
      std::filesystem::last_write_time (path,
         std::filesystem::file_time_type::clock::now ()
            + std::chrono::seconds { name [0] - '0' });
   }

   gv::ocr::Evidence::trim (root, 128);

   EXPECT_FALSE (std::filesystem::exists (root / "1"));
   EXPECT_TRUE (std::filesystem::exists (root / "2"));
   EXPECT_TRUE (std::filesystem::exists (root / "3"));
}

TEST_F (EvidenceTest, WritesCorrelatedDetectionOcrAndLossEvidence)
{
   cv::Mat image { 120, 180, CV_8UC4, cv::Scalar { 18, 18, 18, 255 } };
   cv::rectangle (image, { 30, 20, 100, 80 }, cv::Scalar { 64, 64, 64, 255 }, cv::FILLED);
   gv::capture::Frame frame;
   frame.width = image.cols;
   frame.height = image.rows;
   frame.stride = static_cast<int> (image.step);
   frame.backend = gv::capture::CaptureBackend::Dxgi;
   frame.cursor = { 40, 30, true };
   const gv::capture::Rect selected { 30, 20, 100, 80 };
   const std::vector<gv::vision::TooltipBox> boxes {{
      .rect = selected,
      .confidence = 0.9f,
      .class_id = 0,
   }};
   gv::ocr::Evidence evidence { root, 1024 * 1024 };
   evidence.begin (
      7,
      frame,
      image,
      boxes,
      selected,
      image ({ 30, 20, 100, 80 }),
      cv::Mat { 32, 32, CV_8UC1, cv::Scalar { 255 } },
      42,
      true);
   evidence.ocr (7, image ({ 30, 20, 100, 80 }), {}, "Low Boots", 0.95f);
   evidence.snapshot (7, "lost", image, {});
   evidence.event (7, "analysis_ready", { { "item_id", "low-boots" } });

   std::vector<std::filesystem::path> bundles;
   for (const auto& entry : std::filesystem::directory_iterator { root })
      if (entry.is_directory ()) bundles.push_back (entry.path ());
   ASSERT_EQ (bundles.size (), 1);
   EXPECT_TRUE (std::filesystem::exists (bundles.front () / "frame.jpg"));
   EXPECT_TRUE (std::filesystem::exists (bundles.front () / "tooltip.png"));
   EXPECT_TRUE (std::filesystem::exists (bundles.front () / "identity.png"));
   EXPECT_TRUE (std::filesystem::exists (bundles.front () / "ocr-tooltip.png"));
   EXPECT_TRUE (std::filesystem::exists (bundles.front () / "lost-frame.jpg"));

   std::ifstream input { bundles.front () / "events.jsonl", std::ios::binary };
   const std::string events {
      std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} };
   EXPECT_NE (events.find ("accepted"), std::string::npos);
   EXPECT_NE (events.find ("Low Boots"), std::string::npos);
   EXPECT_NE (events.find ("lost"), std::string::npos);
   EXPECT_NE (events.find ("analysis_ready"), std::string::npos);
}

TEST_F (EvidenceTest, WritesThrottledUnacceptedObservation)
{
   cv::Mat image { 120, 180, CV_8UC4, cv::Scalar { 18, 18, 18, 255 } };
   gv::capture::Frame frame;
   frame.width = image.cols;
   frame.height = image.rows;
   frame.stride = static_cast<int> (image.step);
   const gv::capture::Rect selected { 30, 20, 100, 80 };
   const std::vector<gv::vision::TooltipBox> boxes {{
      .rect = selected,
      .confidence = 0.9f,
      .class_id = 0,
   }};
   gv::ocr::Evidence evidence { root, 1024 * 1024 };
   evidence.observe (frame, image, boxes, "candidate_coarse");
   evidence.observe (frame, image, boxes, "candidate_coarse");

   std::vector<std::filesystem::path> bundles;
   for (const auto& entry : std::filesystem::directory_iterator { root })
      if (entry.is_directory ()) bundles.push_back (entry.path ());
   ASSERT_EQ (bundles.size (), 1);
   EXPECT_TRUE (std::filesystem::exists (bundles.front () / "frame.jpg"));
   EXPECT_TRUE (std::filesystem::exists (bundles.front () / "detection-00.png"));

   std::ifstream input { bundles.front () / "manifest.json", std::ios::binary };
   const std::string manifest {
      std::istreambuf_iterator<char> { input }, std::istreambuf_iterator<char> {} };
   EXPECT_NE (manifest.find ("candidate_coarse"), std::string::npos);
}
