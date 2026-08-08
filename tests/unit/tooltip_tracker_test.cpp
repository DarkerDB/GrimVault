#include <gv/vision/tooltip_tracker.h>

#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>

#include <bit>

using gv::capture::Rect;
using gv::vision::Anchor;
using gv::vision::TooltipTracker;

namespace {

   // A tooltip-like panel on a noisy dark scene: dim interior, bright 3 px
   // frame, some text-ish clutter inside — enough structure for the
   // refiner's gradient ridges and the verifier's fingerprint.
   cv::Mat scene_with_tooltip (const Rect& box)
   {
      cv::Mat img { 600, 800, CV_8UC4, cv::Scalar { 18, 16, 14, 255 } };

      cv::randu (img, cv::Scalar { 8, 8, 8, 255 }, cv::Scalar { 40, 38, 36, 255 });

      const cv::Rect r { box.x, box.y, box.w, box.h };
      cv::rectangle (img, r, cv::Scalar { 30, 26, 22, 255 }, cv::FILLED);
      cv::rectangle (img, r, cv::Scalar { 190, 170, 140, 255 }, 3);

      for (int line = 0; line < 6; ++line) {
         const int y = box.y + 24 + line * 18;
         cv::line (img,
            { box.x + 14, y }, { box.x + box.w - 14 - (line * 11) % 60, y },
            cv::Scalar { 150, 150, 150, 255 }, 2);
      }

      return img;
   }

   const Rect k_truth { 300, 200, 220, 340 };

} // namespace

TEST (TooltipTracker, RefineSnapsToFrame)
{
   const cv::Mat img = scene_with_tooltip (k_truth);

   const Rect coarse { k_truth.x + 6, k_truth.y - 5, k_truth.w - 9, k_truth.h + 8 };
   const auto refined = TooltipTracker::refine (img, coarse);

   ASSERT_TRUE (refined.has_value ());
   EXPECT_NEAR (refined->x, k_truth.x, 2);
   EXPECT_NEAR (refined->y, k_truth.y, 2);
   EXPECT_NEAR (refined->w, k_truth.w, 4);
   EXPECT_NEAR (refined->h, k_truth.h, 4);
}

TEST (TooltipTracker, RefineFailsOnEmptyScene)
{
   cv::Mat img { 600, 800, CV_8UC4, cv::Scalar { 20, 20, 20, 255 } };
   cv::randu (img, cv::Scalar { 8, 8, 8, 255 }, cv::Scalar { 32, 32, 32, 255 });

   EXPECT_FALSE (TooltipTracker::refine (img, k_truth).has_value ());
}

TEST (TooltipTracker, VerifyMatchesAtTruthAndRejectsElsewhere)
{
   const cv::Mat img = scene_with_tooltip (k_truth);

   Anchor a;
   a.w = k_truth.w;
   a.h = k_truth.h;
   a.fingerprint = TooltipTracker::fingerprint (img, k_truth, a.fp_dx, a.fp_dy);
   ASSERT_FALSE (a.fingerprint.empty ());

   EXPECT_TRUE  (TooltipTracker::verify (img, a, k_truth.x, k_truth.y));
   EXPECT_TRUE  (TooltipTracker::verify (img, a, k_truth.x + 3, k_truth.y - 2));
   EXPECT_FALSE (TooltipTracker::verify (img, a, k_truth.x + 60, k_truth.y + 40));

   const cv::Mat gone = scene_with_tooltip ({ 40, 40, 220, 340 });
   EXPECT_FALSE (TooltipTracker::verify (gone, a, k_truth.x, k_truth.y));
}

TEST (TooltipTracker, LocateAbsorbsPresentationErrorWithoutChangingBoxSize)
{
   const cv::Mat img = scene_with_tooltip (k_truth);
   Anchor a;
   a.w = k_truth.w;
   a.h = k_truth.h;
   a.fingerprint = TooltipTracker::fingerprint (img, k_truth, a.fp_dx, a.fp_dy);

   const auto found = TooltipTracker::locate (
      img, a, k_truth.x + 12, k_truth.y - 11, 16, 16);
   ASSERT_TRUE (found.has_value ());
   EXPECT_NEAR (found->x, k_truth.x, 1);
   EXPECT_NEAR (found->y, k_truth.y, 1);
   EXPECT_EQ (found->w, k_truth.w);
   EXPECT_EQ (found->h, k_truth.h);
}

TEST (TooltipTracker, ContentHashIsTranslationInvariantAndContentSensitive)
{
   const Rect shifted { 80, 90, k_truth.w, k_truth.h };
   cv::Mat first  = scene_with_tooltip (k_truth);
   cv::Mat second = scene_with_tooltip (shifted);

   const auto a = TooltipTracker::content_hash (first, k_truth);
   const auto b = TooltipTracker::content_hash (second, shifted);
   EXPECT_EQ (a, b);

   cv::RNG changed_rng { 0xDDB };
   changed_rng.fill (
      second (cv::Rect { shifted.x + 8, shifted.y + 8,
                         shifted.w - 16, shifted.h - 16 }),
      cv::RNG::UNIFORM,
      cv::Scalar { 0, 0, 0, 255 },
      cv::Scalar { 255, 255, 255, 255 });
   const auto changed = TooltipTracker::content_hash (second, shifted);
   EXPECT_GE (std::popcount (b ^ changed), 14);
}

TEST (TooltipTracker, DetailThumbnailDetectsSmallRollChange)
{
   const Rect shifted { 80, 90, k_truth.w, k_truth.h };
   cv::Mat first  = scene_with_tooltip (k_truth);
   cv::Mat second = scene_with_tooltip (shifted);

   const cv::Mat a = TooltipTracker::detail_thumbnail (first, k_truth);
   const cv::Mat b = TooltipTracker::detail_thumbnail (second, shifted);
   ASSERT_FALSE (a.empty ());
   ASSERT_FALSE (b.empty ());
   EXPECT_EQ (cv::countNonZero (a != b), 0);

   cv::putText (second, "+3.7%", { shifted.x + 72, shifted.y + 115 },
                cv::FONT_HERSHEY_SIMPLEX, 0.8,
                cv::Scalar { 240, 190, 80, 255 }, 2);
   const cv::Mat changed = TooltipTracker::detail_thumbnail (second, shifted);
   cv::Mat delta, mask;
   cv::absdiff (b, changed, delta);
   cv::threshold (delta, mask, 8, 255, cv::THRESH_BINARY);
   EXPECT_GE (cv::countNonZero (mask), 4);
}
