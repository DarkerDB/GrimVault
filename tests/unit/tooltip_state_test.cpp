#include <gv/ocr/tooltip_state.h>

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

using gv::capture::Rect;
using gv::ocr::TooltipIdentity;
using gv::ocr::TooltipState;
using gv::ocr::TooltipTransition;

namespace {

TooltipIdentity identity (std::uint64_t value, int width = 500, int height = 700)
{
   TooltipIdentity result;
   result.bits.fill (value);
   result.width = width;
   result.height = height;
   return result;
}

}

TEST (TooltipState, RequiresStableAcquisition)
{
   TooltipState state;
   EXPECT_EQ (state.observe (identity (0)), TooltipTransition::Candidate);
   EXPECT_EQ (state.observe (identity (0)), TooltipTransition::Acquired);
   EXPECT_TRUE (state.active ());
}

TEST (TooltipState, StableIdentityNeverRetriggers)
{
   TooltipState state;
   state.observe (identity (0));
   state.observe (identity (0));
   EXPECT_EQ (state.observe (identity (1)), TooltipTransition::Same);
   EXPECT_EQ (state.observe (identity (3)), TooltipTransition::Same);
}

TEST (TooltipState, ReplacementRequiresTwoFrames)
{
   TooltipState state;
   state.observe (identity (0));
   state.observe (identity (0));
   EXPECT_EQ (state.observe (identity (~0ull)), TooltipTransition::Candidate);
   EXPECT_EQ (state.observe (identity (~0ull)), TooltipTransition::Replaced);
}

TEST (TooltipState, TransientReplacementIsIgnored)
{
   TooltipState state;
   state.observe (identity (0));
   state.observe (identity (0));
   EXPECT_EQ (state.observe (identity (~0ull)), TooltipTransition::Candidate);
   EXPECT_EQ (state.observe (identity (0)), TooltipTransition::Same);
}

TEST (TooltipState, RequiresTwoMissesToLose)
{
   TooltipState state;
   state.observe (identity (0));
   state.observe (identity (0));
   EXPECT_EQ (state.observe (std::nullopt), TooltipTransition::None);
   EXPECT_EQ (state.observe (std::nullopt), TooltipTransition::Lost);
   EXPECT_FALSE (state.active ());
}

TEST (TooltipState, ForceAcceptsImmediately)
{
   TooltipState state;
   EXPECT_EQ (state.observe (identity (0), true), TooltipTransition::Acquired);
   EXPECT_EQ (state.observe (identity (0), true), TooltipTransition::Replaced);
}

TEST (TooltipIdentity, TranslationPreservesIdentity)
{
   cv::Mat frame { 500, 700, CV_8UC4, cv::Scalar { 12, 12, 12, 255 } };
   const Rect first { 40, 30, 260, 360 };
   const Rect second { 360, 100, 260, 360 };
   for (const auto& box : { first, second }) {
      cv::rectangle (frame, cv::Rect { box.x, box.y, box.w, box.h },
         cv::Scalar { 30, 30, 30, 255 }, cv::FILLED);
      cv::putText (frame, "Courtly Dress", { box.x + 30, box.y + 60 },
         cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar { 220, 220, 220, 255 }, 2);
   }
   const auto a = TooltipIdentity::read (frame, first);
   const auto b = TooltipIdentity::read (frame, second);
   ASSERT_TRUE (a.has_value ());
   ASSERT_TRUE (b.has_value ());
   EXPECT_TRUE (a->same (*b, 0, 0));
}

TEST (TooltipIdentity, ContentChangesIdentity)
{
   cv::Mat first_frame { 500, 700, CV_8UC4, cv::Scalar { 12, 12, 12, 255 } };
   cv::Mat second_frame = first_frame.clone ();
   const Rect box { 40, 30, 260, 360 };
   for (auto* frame : { &first_frame, &second_frame })
      cv::rectangle (*frame, cv::Rect { box.x, box.y, box.w, box.h },
         cv::Scalar { 30, 30, 30, 255 }, cv::FILLED);
   cv::putText (first_frame, "Low Boots", { box.x + 30, box.y + 60 },
      cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar { 220, 220, 220, 255 }, 2);
   cv::putText (second_frame, "Courtly Dress", { box.x + 30, box.y + 60 },
      cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar { 220, 220, 220, 255 }, 2);
   const auto first = TooltipIdentity::read (first_frame, box);
   const auto second = TooltipIdentity::read (second_frame, box);
   ASSERT_TRUE (first.has_value ());
   ASSERT_TRUE (second.has_value ());
   EXPECT_GT (first->distance (*second), 16);
}
