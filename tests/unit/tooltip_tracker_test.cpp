#include <gv/vision/tooltip_tracker.h>

#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>

using gv::capture::Rect;
using gv::vision::Anchor;
using gv::vision::TooltipPresence;
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
   const auto refined = TooltipTracker::select (img, coarse);

   ASSERT_TRUE (refined.refined);
   EXPECT_NEAR (refined.rect.x, k_truth.x, 4);
   EXPECT_NEAR (refined.rect.y, k_truth.y, 4);
   EXPECT_NEAR (refined.rect.w, k_truth.w, 8);
   EXPECT_NEAR (refined.rect.h, k_truth.h, 8);
}

TEST (TooltipTracker, RefineFailsOnEmptyScene)
{
   cv::Mat img { 600, 800, CV_8UC4, cv::Scalar { 20, 20, 20, 255 } };
   cv::randu (img, cv::Scalar { 8, 8, 8, 255 }, cv::Scalar { 32, 32, 32, 255 });

   EXPECT_FALSE (TooltipTracker::refine (img, k_truth).has_value ());
}

TEST (TooltipTracker, SelectionFallsBackToDetectorBox)
{
   cv::Mat img { 600, 800, CV_8UC4, cv::Scalar { 20, 20, 20, 255 } };
   const auto selected = TooltipTracker::select (img, k_truth);

   EXPECT_EQ (selected.rect.x, k_truth.x);
   EXPECT_EQ (selected.rect.y, k_truth.y);
   EXPECT_EQ (selected.rect.w, k_truth.w);
   EXPECT_EQ (selected.rect.h, k_truth.h);
   EXPECT_FALSE (selected.refined);
}

TEST (TooltipTracker, SelectionUsesRefinedBoxWhenAvailable)
{
   const cv::Mat img = scene_with_tooltip (k_truth);
   const Rect coarse { k_truth.x + 6, k_truth.y - 5, k_truth.w - 9, k_truth.h + 8 };
   const auto selected = TooltipTracker::select (img, coarse);

   EXPECT_TRUE (selected.refined);
   EXPECT_NEAR (selected.rect.x, k_truth.x, 4);
   EXPECT_NEAR (selected.rect.y, k_truth.y, 4);
   EXPECT_NEAR (selected.rect.w, k_truth.w, 8);
   EXPECT_NEAR (selected.rect.h, k_truth.h, 8);
}

TEST (TooltipTracker, SelectionRejectsInteriorFrameRidges)
{
   cv::Mat img = scene_with_tooltip (k_truth);
   cv::line (img,
      { k_truth.x, k_truth.y + 18 },
      { k_truth.x + k_truth.w, k_truth.y + 18 },
      cv::Scalar { 255, 255, 255, 255 }, 7);

   const auto selected = TooltipTracker::select (img, k_truth);

   EXPECT_TRUE (selected.refined);
   EXPECT_NEAR (selected.rect.x, k_truth.x, 4);
   EXPECT_NEAR (selected.rect.y, k_truth.y, 4);
   EXPECT_NEAR (selected.rect.w, k_truth.w, 8);
   EXPECT_NEAR (selected.rect.h, k_truth.h, 8);
}

TEST (TooltipTracker, RefineRecoversLargeDetectorError)
{
   const cv::Mat img = scene_with_tooltip (k_truth);
   const Rect coarse { k_truth.x + 38, k_truth.y - 34, k_truth.w - 49, k_truth.h + 51 };
   const auto refined = TooltipTracker::select (img, coarse);

   ASSERT_TRUE (refined.refined);
   EXPECT_NEAR (refined.rect.x, k_truth.x, 4);
   EXPECT_NEAR (refined.rect.y, k_truth.y, 4);
   EXPECT_NEAR (refined.rect.w, k_truth.w, 8);
   EXPECT_NEAR (refined.rect.h, k_truth.h, 8);
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

TEST (TooltipTracker, TrackPreservesTranslatedTooltip)
{
   const cv::Mat first = scene_with_tooltip (k_truth);
   const Rect shifted { 80, 90, k_truth.w, k_truth.h };
   const cv::Mat second = scene_with_tooltip (shifted);
   Anchor anchor;
   TooltipTracker::remember (first, k_truth, anchor);

   const auto tracked = TooltipTracker::track (
      second, anchor, shifted.x + 10, shifted.y - 8);

   EXPECT_EQ (tracked.presence, TooltipPresence::Present);
   EXPECT_NEAR (tracked.box.x, shifted.x, 1);
   EXPECT_NEAR (tracked.box.y, shifted.y, 1);
}

TEST (TooltipTracker, TrackIgnoresAmbiguousFrameFingerprintPosition)
{
   const cv::Mat img = scene_with_tooltip (k_truth);
   Anchor anchor;
   TooltipTracker::remember (img, k_truth, anchor);
   anchor.fingerprint.setTo (0);

   const auto tracked = TooltipTracker::track (
      img, anchor, k_truth.x, k_truth.y);

   EXPECT_EQ (tracked.presence, TooltipPresence::Present);
   EXPECT_NEAR (tracked.box.x, k_truth.x, 1);
   EXPECT_NEAR (tracked.box.y, k_truth.y, 1);
}

TEST (TooltipTracker, TrackRejectsMissingTooltip)
{
   const cv::Mat first = scene_with_tooltip (k_truth);
   const cv::Mat gone = scene_with_tooltip ({ 40, 40, 220, 340 });
   Anchor anchor;
   TooltipTracker::remember (first, k_truth, anchor);

   const auto tracked = TooltipTracker::track (
      gone, anchor, k_truth.x, k_truth.y);

   EXPECT_EQ (tracked.presence, TooltipPresence::Absent);
}

TEST (TooltipTracker, TrackDetectsContentReplacement)
{
   const cv::Mat first = scene_with_tooltip (k_truth);
   cv::Mat changed = first.clone ();
   cv::RNG rng { 0xDDB };
   rng.fill (
      changed (cv::Rect { k_truth.x + 8, k_truth.y + 32,
                          k_truth.w - 16, k_truth.h - 64 }),
      cv::RNG::UNIFORM,
      cv::Scalar { 0, 0, 0, 255 },
      cv::Scalar { 255, 255, 255, 255 });
   Anchor anchor;
   TooltipTracker::remember (first, k_truth, anchor);

   const auto tracked = TooltipTracker::track (
      changed, anchor, k_truth.x, k_truth.y);

   EXPECT_EQ (tracked.presence, TooltipPresence::Changed);
}

TEST (TooltipTracker, TrackEscalatesSmallContentChange)
{
   const cv::Mat first = scene_with_tooltip (k_truth);
   cv::Mat changed = first.clone ();
   cv::putText (
      changed,
      "+3.7%",
      { k_truth.x + 72, k_truth.y + 125 },
      cv::FONT_HERSHEY_SIMPLEX,
      0.8,
      cv::Scalar { 240, 190, 80, 255 },
      2);
   Anchor anchor;
   TooltipTracker::remember (first, k_truth, anchor);

   const auto tracked = TooltipTracker::track (
      changed, anchor, k_truth.x, k_truth.y);

   EXPECT_NE (tracked.presence, TooltipPresence::Present);
}

TEST (TooltipTracker, TrackEscalatesSizeReplacement)
{
   const cv::Mat first = scene_with_tooltip (k_truth);
   const Rect taller { k_truth.x, k_truth.y, k_truth.w, k_truth.h + 50 };
   const cv::Mat changed = scene_with_tooltip (taller);
   Anchor anchor;
   TooltipTracker::remember (first, k_truth, anchor);

   const auto tracked = TooltipTracker::track (
      changed, anchor, k_truth.x, k_truth.y);

   EXPECT_EQ (tracked.presence, TooltipPresence::Changed);
}

TEST (TooltipTracker, RebasePreservesDetectorCropJitter)
{
   const cv::Mat img = scene_with_tooltip (k_truth);
   const Rect first {
      k_truth.x - 8, k_truth.y - 18, k_truth.w + 15, k_truth.h + 22 };
   const Rect second {
      k_truth.x + 3, k_truth.y - 2, k_truth.w - 5, k_truth.h + 4 };
   Anchor anchor;
   TooltipTracker::remember (img, first, anchor);

   const auto tracked = TooltipTracker::rebase (img, anchor, second);

   EXPECT_EQ (tracked.presence, TooltipPresence::Present);
}

TEST (TooltipTracker, RebaseRejectsContentReplacement)
{
   const cv::Mat first = scene_with_tooltip (k_truth);
   cv::Mat changed = first.clone ();
   cv::RNG rng { 0xDDB };
   rng.fill (
      changed (cv::Rect { k_truth.x + 8, k_truth.y + 32,
                          k_truth.w - 16, k_truth.h - 64 }),
      cv::RNG::UNIFORM,
      cv::Scalar { 0, 0, 0, 255 },
      cv::Scalar { 255, 255, 255, 255 });
   Anchor anchor;
   TooltipTracker::remember (first, k_truth, anchor);

   const auto tracked = TooltipTracker::rebase (changed, anchor, k_truth);

   EXPECT_NE (tracked.presence, TooltipPresence::Present);
}

TEST (TooltipTracker, RebaseRejectsSizeReplacement)
{
   const cv::Mat img = scene_with_tooltip (k_truth);
   Anchor anchor;
   TooltipTracker::remember (img, k_truth, anchor);

   const auto tracked = TooltipTracker::rebase (
      img, anchor, { k_truth.x, k_truth.y, k_truth.w, k_truth.h + 80 });

   EXPECT_EQ (tracked.presence, TooltipPresence::Changed);
}

TEST (Anchor, RightPinSurvivesCursorMotion)
{
   Anchor anchor;
   anchor.acquire (
      { 570, 100, 220, 340 }, { 700, 200, true }, 800, 600);

   anchor.update (
      { 570, 100, 220, 340 }, { 400, 200, true }, 800, 600);

   EXPECT_EQ (anchor.axis_x, gv::vision::AxisPin::High);
   EXPECT_EQ (anchor.pin_x, 570);
}

TEST (Anchor, RightPinReleasesAfterTwoObservedMoves)
{
   Anchor anchor;
   anchor.acquire (
      { 570, 100, 220, 340 }, { 700, 200, true }, 800, 600);

   anchor.update (
      { 400, 100, 220, 340 }, { 350, 200, true }, 800, 600);
   EXPECT_EQ (anchor.axis_x, gv::vision::AxisPin::High);
   anchor.update (
      { 400, 100, 220, 340 }, { 350, 200, true }, 800, 600);

   EXPECT_EQ (anchor.axis_x, gv::vision::AxisPin::Free);
   EXPECT_EQ (anchor.offset_x, 50);
}
