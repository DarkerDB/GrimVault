#include <gv/capture/frame.h>
#include <gv/vision/tooltip_detector.h>

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <filesystem>

TEST (TooltipDetector, DetectsHeldOutTooltip)
{
   // One held-out capture, vendored here rather than read from the training
   // corpus: that corpus lives in packages/scry now, and this test has to
   // stand on its own in a GrimVault checkout.
   const auto root = std::filesystem::path { GRIMVAULT_TEST_SOURCE_DIR };
   const auto image_path = root / "tests" / "fixtures"
      / "tooltip_0033_20251028_171457_539.jpg";
   const auto model_path = std::filesystem::path { GRIMVAULT_TEST_MODELS_DIR } / gv::vision::model_files::tooltip_full;

   const cv::Mat bgr = cv::imread (image_path.string ());
   ASSERT_FALSE (bgr.empty ());
   cv::Mat bgra;
   cv::cvtColor (bgr, bgra, cv::COLOR_BGR2BGRA);

   auto pixels
      = std::shared_ptr<std::uint8_t[]> { new std::uint8_t[bgra.total () * bgra.elemSize ()] };
   std::copy_n (bgra.data, bgra.total () * bgra.elemSize (), pixels.get ());
   const gv::capture::Frame frame {
      .data = std::move (pixels),
      .width = bgra.cols,
      .height = bgra.rows,
      .stride = static_cast<int> (bgra.step),
   };

   gv::vision::TooltipDetector detector;
   const auto initialized = detector.initialize (model_path);
   ASSERT_TRUE (initialized.has_value ()) << initialized.error ().message;

   const auto boxes = detector.detect (frame);
   ASSERT_TRUE (boxes.has_value ()) << boxes.error ().message;
   ASSERT_FALSE (boxes->empty ());
   EXPECT_GT (boxes->front ().confidence, 0.8f);
   EXPECT_GT (boxes->front ().rect.w, 100);
   EXPECT_GT (boxes->front ().rect.h, 100);
}
