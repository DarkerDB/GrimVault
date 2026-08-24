#include <gv/ocr/preprocessor.h>
#include <gv/ocr/paddle_recognizer.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace prep = gv::ocr::preprocess;

TEST (OcrPreprocessor, FindsSeparateTextBands)
{
   cv::Mat crop { 140, 320, CV_8UC4, cv::Scalar { 20, 20, 20, 255 } };
   cv::putText (crop, "Battle Axe", { 35, 42 }, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar { 230, 230, 230, 255 }, 2);
   cv::putText (crop, "+3 Strength", { 28, 96 }, cv::FONT_HERSHEY_SIMPLEX,
                0.7, cv::Scalar { 200, 180, 100, 255 }, 2);

   const auto bands = prep::line_bands (crop);
   ASSERT_EQ (bands.size (), 2u);
   EXPECT_LT (bands [0].end, bands [1].start);
   EXPECT_EQ (prep::first_tooltip_band (crop, bands), 0u);
}

TEST (OcrPreprocessor, SkipsContentAboveTooltipBorder)
{
   cv::Mat crop { 340, 490, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::putText (crop, "outside text", { 2, 8 }, cv::FONT_HERSHEY_SIMPLEX,
                0.45, cv::Scalar { 210, 210, 210, 255 }, 1);
   cv::rectangle (crop, { 2, 36, 486, 8 }, cv::Scalar { 180, 180, 180, 255 }, cv::FILLED);
   cv::putText (crop, "Tooltip Title", { 130, 82 }, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar { 220, 160, 60, 255 }, 2);

   const auto bands = prep::line_bands (crop);
   ASSERT_GE (bands.size (), 3u);
   EXPECT_EQ (prep::first_tooltip_band (crop, bands), 2u);
}

TEST (OcrPreprocessor, FindsTitleAfterLeadingBorder)
{
   cv::Mat crop { 180, 480, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::line (crop, { 2, 28 }, { 477, 28 }, cv::Scalar { 180, 180, 180, 255 }, 1);
   cv::putText (crop, "Frozen Feather", { 125, 84 }, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar { 230, 150, 40, 255 }, 2);
   cv::putText (crop, "Rarity Rare", { 150, 142 }, cv::FONT_HERSHEY_SIMPLEX,
                0.65, cv::Scalar { 180, 180, 180, 255 }, 2);

   const auto bands = prep::line_bands (crop);
   ASSERT_TRUE (prep::title_band (crop, bands).has_value ());
   EXPECT_EQ (*prep::title_band (crop, bands), 1u);
}

TEST (OcrPreprocessor, SkipsTitleOrnaments)
{
   cv::Mat crop { 180, 480, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::rectangle (crop, { 12, 3, 8, 8 }, cv::Scalar { 180, 180, 180, 255 }, 1);
   cv::rectangle (crop, { 460, 3, 8, 8 }, cv::Scalar { 180, 180, 180, 255 }, 1);
   cv::putText (crop, "Frozen Feather", { 125, 62 }, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar { 230, 150, 40, 255 }, 2);
   cv::putText (crop, "Rarity Rare", { 150, 122 }, cv::FONT_HERSHEY_SIMPLEX,
                0.65, cv::Scalar { 180, 180, 180, 255 }, 2);

   const auto bands = prep::line_bands (crop);
   ASSERT_TRUE (prep::title_band (crop, bands).has_value ());
   EXPECT_EQ (*prep::title_band (crop, bands), 1u);
   EXPECT_FALSE (prep::top_is_clipped (crop, bands));
}

TEST (OcrPreprocessor, DetectsClippedCenteredTitle)
{
   cv::Mat crop { 180, 480, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::putText (crop, "FAMINE", { 180, 10 }, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar { 25, 25, 190, 255 }, 2);
   cv::putText (crop, "Rarity Artifact", { 150, 70 }, cv::FONT_HERSHEY_SIMPLEX,
                0.65, cv::Scalar { 180, 180, 180, 255 }, 2);

   const auto bands = prep::line_bands (crop);
   EXPECT_TRUE (prep::top_is_clipped (crop, bands));
}

TEST (OcrPreprocessor, FindsSaturatedRedText)
{
   cv::Mat crop { 60, 240, CV_8UC4, cv::Scalar { 12, 12, 12, 255 } };
   cv::putText (crop, "FAMINE", { 32, 40 }, cv::FONT_HERSHEY_SIMPLEX,
                1.0, cv::Scalar { 25, 25, 190, 255 }, 2);

   const auto bands = prep::line_bands (crop);
   ASSERT_EQ (bands.size (), 1u);
   EXPECT_GT (bands.front ().size (), 10);
}

TEST (OcrPreprocessor, TrimsEmptyHorizontalMargins)
{
   cv::Mat line { 32, 300, CV_8UC4, cv::Scalar { 10, 10, 10, 255 } };
   cv::rectangle (line, { 110, 8, 70, 16 }, cv::Scalar { 240, 240, 240, 255 },
                  cv::FILLED);
   const cv::Mat trimmed = prep::trim_cols (line);
   EXPECT_LT (trimmed.cols, 100);
   EXPECT_EQ (trimmed.rows, line.rows);
}

TEST (OcrPreprocessor, RemovesDetachedStatOrnaments)
{
   cv::Mat line { 32, 440, CV_8UC4, cv::Scalar { 10, 10, 10, 255 } };
   cv::rectangle (line, { 2, 15, 7, 2 }, cv::Scalar { 240, 240, 240, 255 }, cv::FILLED);
   cv::putText (line, "Move Speed -10", { 125, 24 }, cv::FONT_HERSHEY_SIMPLEX,
                0.65, cv::Scalar { 240, 240, 240, 255 }, 2);
   cv::rectangle (line, { 430, 15, 7, 2 }, cv::Scalar { 240, 240, 240, 255 }, cv::FILLED);

   const cv::Mat trimmed = prep::trim_cols (line);
   EXPECT_LT (trimmed.cols, 250);
   EXPECT_GT (trimmed.cols, 100);
}

TEST (OcrPreprocessor, SplitsWideLinesAtWhitespace)
{
   cv::Mat line { 32, 500, CV_8UC4, cv::Scalar { 10, 10, 10, 255 } };
   for (int x = 8; x < line.cols; x += 55)
      cv::rectangle (line, { x, 7, 34, 18 }, cv::Scalar { 235, 235, 235, 255 },
                     cv::FILLED);

   const auto chunks = prep::col_chunks (line);
   ASSERT_GT (chunks.size (), 1u);
   for (const auto& chunk : chunks) {
      EXPECT_GT (chunk.size (), 0);
      EXPECT_LE (chunk.size (), line.rows * 6 + line.rows);
   }
}

TEST (OcrPreprocessor, RemovesSeparatorMergedIntoTitle)
{
   cv::Mat line { 62, 460, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::putText (line, "Gold Coin Bag", { 125, 27 }, cv::FONT_HERSHEY_SIMPLEX,
                0.75, cv::Scalar { 220, 200, 120, 255 }, 2);
   cv::line (line, { 4, 54 }, { 455, 54 }, cv::Scalar { 180, 160, 90, 255 }, 2);
   const cv::Mat title = prep::trim_title_rule (line);
   EXPECT_LT (title.rows, 54);
   EXPECT_GT (title.rows, 20);
}

TEST (OcrPreprocessor, IdentifiesThinHorizontalRule)
{
   cv::Mat rule { 12, 480, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::line (rule, { 2, 6 }, { 477, 6 }, cv::Scalar { 150, 150, 150, 255 }, 1);
   EXPECT_TRUE (prep::is_horizontal_rule (rule));

   cv::Mat title { 36, 300, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::putText (title, "Bandage", { 95, 27 }, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar { 120, 220, 40, 255 }, 2);
   EXPECT_FALSE (prep::is_horizontal_rule (title));
}

TEST (OcrPreprocessor, DoesNotTreatWideProseAsRule)
{
   cv::Mat prose { 34, 500, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::putText (prose, "dream justice love and kindness hoping", { 8, 25 },
                cv::FONT_HERSHEY_SIMPLEX, 0.58,
                cv::Scalar { 210, 160, 105, 255 }, 1);
   EXPECT_FALSE (prep::is_horizontal_rule (prose));
}

TEST (FontOcrModel, LoadsAndRunsInOpenCvDnn)
{
   namespace fs = std::filesystem;
   fs::path base = fs::path { GRIMVAULT_TEST_MODELS_DIR } / "paddle/en";
   if (!fs::exists (base / gv::ocr::model_files::rec_tooltip_body)) {
      base.clear ();
   }
   for (const auto& candidate : {
      fs::current_path () / "models/paddle/en",
      fs::current_path ().parent_path () / "models/paddle/en",
   }) {
      if (!base.empty ()) break;
      if (fs::exists (candidate / gv::ocr::model_files::rec_tooltip_body)) { base = candidate; break; }
   }
   if (base.empty ()) GTEST_SKIP () << "staged font OCR model not present";

   gv::ocr::PaddleRecognizer rec;
   rec.set_family (gv::ocr::LanguageFamily::English);
   const auto initialized = rec.initialize (
      base / gv::ocr::model_files::rec_tooltip_body, base / gv::ocr::model_files::rec_tooltip_dict);
   ASSERT_TRUE (initialized.has_value ()) << initialized.error ().message;
   EXPECT_TRUE (rec.is_wide ());
   EXPECT_EQ (rec.has_title_model (), fs::exists (base / gv::ocr::model_files::rec_tooltip_title));

   cv::Mat line { 32, 180, CV_8UC4, cv::Scalar { 8, 8, 8, 255 } };
   cv::putText (line, "Spear", { 42, 25 }, cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar { 230, 150, 40, 255 }, 2);
   const auto result = rec.read (line);
   ASSERT_TRUE (result.has_value ()) << result.error ().message;
   const auto title_result = rec.read (line, /*title=*/ true);
   ASSERT_TRUE (title_result.has_value ()) << title_result.error ().message;
}

TEST (FontOcrModel, LocalCapturedCorpusMatchesProductionOpenCv)
{
   namespace fs = std::filesystem;
   const fs::path root { GRIMVAULT_TEST_SOURCE_DIR };

   const char* corpus_env = std::getenv ("GRIMVAULT_OCR_CORPUS_DIR");
   if (!corpus_env)
      GTEST_SKIP () << "local labelled OCR corpus not present";
   const fs::path corpus { corpus_env };
   if (!fs::exists (corpus / "real-train.tsv") || !fs::exists (corpus / "real-crops"))
      GTEST_SKIP () << "local labelled OCR corpus not present";

   const auto manifest = corpus / "real-train.tsv";
   const auto crops    = corpus / "real-crops";

   const auto models = root / "models/paddle/en";
   gv::ocr::PaddleRecognizer rec;
   rec.set_family (gv::ocr::LanguageFamily::English);
   const auto initialized = rec.initialize (
      models / gv::ocr::model_files::rec_tooltip_body, models / gv::ocr::model_files::rec_tooltip_dict);
   ASSERT_TRUE (initialized.has_value ()) << initialized.error ().message;

   std::ifstream rows { manifest };
   ASSERT_TRUE (rows.good ());
   std::string row;
   int checked = 0;
   while (std::getline (rows, row)) {
      const auto tab = row.find ('\t');
      if (tab == std::string::npos) continue;
      const auto relative = row.substr (0, tab);
      const auto expected = row.substr (tab + 1);
      const auto path = crops / fs::path { relative };
      if (!fs::exists (path)) continue;
      cv::Mat line = cv::imread (path.string (), cv::IMREAD_UNCHANGED);
      ASSERT_FALSE (line.empty ()) << path.string ();
      // Debug bands are preserved before production's title-specific lower
      // rule removal. Reproduce the actual recognizer input for band zero.
      if (relative.find ("_band0.png") != std::string::npos
          && line.cols >= line.rows * 8
          && prep::is_horizontal_rule (line)) {
         cv::Mat mask;
         cv::cvtColor (line, mask, line.channels () == 4
            ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
         bool near_full_rule = false;
         for (int y = 0; y < mask.rows && !near_full_rule; ++y) {
            cv::Mat bright;
            cv::threshold (mask.row (y), bright, 50, 255, cv::THRESH_BINARY);
            near_full_rule = cv::countNonZero (bright) > mask.cols * 4 / 5;
         }
         if (near_full_rule)
            line = prep::trim_cols (prep::trim_title_rule (line));
      }
      const auto actual = rec.read (line);
      ASSERT_TRUE (actual.has_value ()) << path.string () << ": "
                                        << actual.error ().message;
      EXPECT_EQ (actual->text, expected) << path.string ();
      ++checked;
   }
   EXPECT_GE (checked, 100);

   const auto title_manifest = corpus / "real-live.tsv";
   std::ifstream title_rows { title_manifest };
   while (std::getline (title_rows, row)) {
      const auto tab = row.find ('\t');
      if (tab == std::string::npos) continue;
      const auto relative = row.substr (0, tab);
      const auto expected = row.substr (tab + 1);
      cv::Mat line = cv::imread ((crops / fs::path { relative }).string (),
                                 cv::IMREAD_UNCHANGED);
      if (line.empty ()) continue;
      const auto actual = rec.read (line, /*title=*/ true);
      ASSERT_TRUE (actual.has_value ()) << relative;
      EXPECT_EQ (actual->text, expected) << relative;
   }
}
