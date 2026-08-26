#include <gv/capture/frame.h>
#include <gv/ocr/language.h>
#include <gv/ocr/paddle_recognizer.h>
#include <gv/vision/gem_detector.h>

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using gv::ocr::LanguageFamily;

struct Model {
   LanguageFamily family;
   const char*    directory;
   const char*    network;
   const char*    dictionary;
   bool           title;
};

constexpr std::array models {
   Model { LanguageFamily::English, "en",     gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
   Model { LanguageFamily::Latin,   "latin",  gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
   Model { LanguageFamily::French,  "fr",     gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
   Model { LanguageFamily::Eslav,   "eslav",  gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
   Model { LanguageFamily::Korean,  "korean", gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
   Model { LanguageFamily::Japanese, "ja",    gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
   Model { LanguageFamily::Chinese, "ch",     gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
   Model { LanguageFamily::TraditionalChinese, "ch-hant", gv::ocr::model_files::rec_tooltip_body, gv::ocr::model_files::rec_tooltip_dict, true },
};

cv::Mat gem_line (const cv::Scalar& bgra, double scale)
{
   cv::Mat line { 40, 220, CV_8UC4, cv::Scalar { 12, 12, 12, 255 } };
   const std::vector<cv::Point> gem {
      { 198, 6 }, { 211, 20 }, { 198, 34 }, { 185, 20 }
   };
   cv::fillConvexPoly (line, gem, bgra);

   cv::Mat scaled;
   cv::resize (line, scaled, {}, scale, scale, cv::INTER_LINEAR);
   return scaled;
}

} // namespace

TEST (ClientCompatibilityE2E, EveryBundledLanguageModelLoadsAndRuns)
{
   const fs::path root { GRIMVAULT_TEST_MODELS_DIR };

   for (const auto& model : models) {
      SCOPED_TRACE (model.directory);
      const auto base = root / "paddle" / model.directory;

      gv::ocr::PaddleRecognizer recognizer;
      recognizer.set_family (model.family);
      const auto initialized = recognizer.initialize (
         base / model.network, base / model.dictionary);
      ASSERT_TRUE (initialized.has_value ()) << initialized.error ().message;
      EXPECT_TRUE (recognizer.is_wide ());
      EXPECT_EQ (recognizer.has_title_model (), model.title);

      cv::Mat sample { 40, 260, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
      cv::putText (sample, "123 + 45%", { 16, 29 }, cv::FONT_HERSHEY_SIMPLEX,
                   0.85, cv::Scalar { 225, 180, 110, 255 }, 2);
      const auto result = recognizer.read (sample);
      ASSERT_TRUE (result.has_value ()) << result.error ().message;
      if (model.title) {
         const auto title = recognizer.read (sample, true);
         ASSERT_TRUE (title.has_value ()) << title.error ().message;
      }
   }
}

TEST (ClientCompatibilityE2E, CapturedEnglishTooltipRunsAcrossDisplayScales)
{
   const fs::path root { GRIMVAULT_TEST_SOURCE_DIR };
   const auto fixture = root / "tests/fixtures/lyre_socketed_stats.png";
   ASSERT_TRUE (fs::exists (fixture));

   const cv::Mat tooltip = cv::imread (fixture.string (), cv::IMREAD_UNCHANGED);
   ASSERT_FALSE (tooltip.empty ());

   // Real captured lines exercise ordinary, percentage, and gemmed blue
   // rolls without depending on the developer-only OCR training corpus.
   const std::array lines {
      cv::Rect { 145, 88, 230, 31 },
      cv::Rect { 135, 124, 250, 31 },
      cv::Rect { 80, 163, 350, 31 },
      cv::Rect { 62, 271, 390, 35 },
   };

   gv::ocr::PaddleRecognizer recognizer;
   recognizer.set_family (LanguageFamily::English);
   const auto initialized = recognizer.initialize (
      root / "models/paddle/en" / gv::ocr::model_files::rec_tooltip_body,
      root / "models/paddle/en" / gv::ocr::model_files::rec_tooltip_dict);
   ASSERT_TRUE (initialized.has_value ()) << initialized.error ().message;
   EXPECT_TRUE (recognizer.is_wide ());

   for (const auto& bounds : lines) {
      const cv::Mat original = tooltip (bounds).clone ();
      for (const double scale : { 1.0, 1.25, 1.5, 2.0 }) {
         SCOPED_TRACE (fixture.string () + " y=" + std::to_string (bounds.y)
                       + " scale=" + std::to_string (scale));
         cv::Mat sample;
         cv::resize (original, sample, {}, scale, scale, cv::INTER_LINEAR);
         const auto result = recognizer.read (sample);
         ASSERT_TRUE (result.has_value ()) << result.error ().message;
         EXPECT_FALSE (result->text.empty ());
      }
   }
}

TEST (ClientCompatibilityE2E, GemRecognitionSurvivesDisplayScaling)
{
   const std::array gems {
      std::pair { cv::Scalar { 40, 40, 230, 255 }, "ruby" },
      std::pair { cv::Scalar { 230, 80, 30, 255 }, "blue_sapphire" },
      std::pair { cv::Scalar { 50, 210, 50, 255 }, "emerald" },
      std::pair { cv::Scalar { 230, 230, 230, 255 }, "diamond" },
   };

   for (const double scale : { 1.0, 1.25, 1.5, 2.0 }) {
      for (const auto& [color, expected] : gems) {
         SCOPED_TRACE (std::string { expected } + " scale=" + std::to_string (scale));
         EXPECT_EQ (gv::vision::detect_gem_family (gem_line (color, scale)), expected);
      }
   }
}

TEST (ClientCompatibilityE2E, FrameCoordinatesRemainLocalOnEveryMonitorOrigin)
{
   for (const auto [width, height] : {
      std::pair { 1920, 1080 },
      std::pair { 2560, 1440 },
      std::pair { 3840, 2160 },
   }) {
      for (const auto [origin_x, origin_y] : {
         std::pair { 0, 0 },
         std::pair { -width, 0 },
         std::pair { 0, -height },
         std::pair { width, 0 },
      }) {
         gv::capture::Frame frame;
         frame.width = width;
         frame.height = height;
         frame.origin_x = origin_x;
         frame.origin_y = origin_y;
         frame.cursor = {
            .x = origin_x + width / 2,
            .y = origin_y + height / 2,
            .valid = true,
         };

         const auto local = frame.local_cursor ();
         EXPECT_TRUE (local.valid);
         EXPECT_EQ (local.x, width / 2);
         EXPECT_EQ (local.y, height / 2);
      }
   }
}
