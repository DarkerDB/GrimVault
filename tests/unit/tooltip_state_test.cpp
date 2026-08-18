#include <gv/ocr/tooltip_state.h>

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

using gv::capture::Rect;
using gv::capture::CursorPos;
using gv::ocr::TooltipIdentity;
using gv::ocr::TooltipObservation;
using gv::ocr::TooltipRelation;
using gv::ocr::TooltipState;
using gv::ocr::TooltipTransition;

namespace {

TooltipObservation observation (
   std::uint64_t value,
   Rect box = { 100, 100, 500, 700 },
   CursorPos cursor = { 100, 100, true })
{
   TooltipObservation result { .box = box, .cursor = cursor };
   result.identity.bits.fill (value);
   return result;
}

}

TEST (TooltipState, RequiresStableAcquisition)
{
   TooltipState state;
   EXPECT_EQ (state.observe (observation (0)).transition, TooltipTransition::Candidate);
   EXPECT_EQ (state.observe (observation (0)).transition, TooltipTransition::Acquired);
   EXPECT_TRUE (state.active ());
}

TEST (TooltipState, StableIdentityNeverRetriggers)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   EXPECT_EQ (state.observe (observation (1)).transition, TooltipTransition::Same);
   EXPECT_EQ (state.observe (observation (3)).transition, TooltipTransition::Same);
}

TEST (TooltipState, ReplacementRequiresTwoFrames)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   EXPECT_EQ (state.observe (observation (~0ull)).transition, TooltipTransition::Candidate);
   EXPECT_EQ (state.observe (observation (~0ull)).transition, TooltipTransition::Replaced);
}

TEST (TooltipState, TransientReplacementIsIgnored)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   EXPECT_EQ (state.observe (observation (~0ull)).transition, TooltipTransition::Candidate);
   EXPECT_EQ (state.observe (observation (0)).transition, TooltipTransition::Same);
}

TEST (TooltipState, RequiresTwoMissesToLose)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   EXPECT_EQ (state.observe (std::nullopt).transition, TooltipTransition::None);
   EXPECT_EQ (state.observe (std::nullopt).transition, TooltipTransition::Lost);
   EXPECT_FALSE (state.active ());
}

TEST (TooltipState, ForceAcceptsImmediately)
{
   TooltipState state;
   EXPECT_EQ (state.observe (observation (0), true).transition, TooltipTransition::Acquired);
   EXPECT_EQ (state.observe (observation (0), true).transition, TooltipTransition::Replaced);
}

TEST (TooltipState, CursorMotionExplainsTooltipMotion)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   const auto update = state.observe (observation (
      0, { 300, 220, 500, 700 }, { 300, 220, true }));
   EXPECT_EQ (update.transition, TooltipTransition::Same);
   EXPECT_FALSE (update.position_unexplained);
}

TEST (TooltipState, PinnedTooltipExplainsCursorMotion)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   const auto update = state.observe (observation (
      0, { 100, 100, 500, 700 }, { 300, 220, true }));
   EXPECT_EQ (update.transition, TooltipTransition::Same);
   EXPECT_FALSE (update.position_unexplained);
}

TEST (TooltipState, UnexplainedPositionRequiresReplacement)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   const auto moved = observation (0, { 300, 220, 500, 700 });
   const auto first = state.observe (moved);
   const auto second = state.observe (moved);
   EXPECT_EQ (first.relation, TooltipRelation::Ambiguous);
   EXPECT_TRUE (first.position_unexplained);
   EXPECT_EQ (first.transition, TooltipTransition::Candidate);
   EXPECT_EQ (second.transition, TooltipTransition::Replaced);
}

TEST (TooltipState, DramaticSizeChangeRequiresReplacement)
{
   TooltipState state;
   state.observe (observation (0));
   state.observe (observation (0));
   const auto resized = observation (0, { 100, 100, 650, 700 });
   const auto first = state.observe (resized);
   const auto second = state.observe (resized);
   EXPECT_EQ (first.relation, TooltipRelation::Different);
   EXPECT_TRUE (first.size_changed);
   EXPECT_EQ (first.transition, TooltipTransition::Candidate);
   EXPECT_EQ (second.transition, TooltipTransition::Replaced);
}

TEST (TooltipObservation, CacheRequiresContentAndSize)
{
   const auto base = observation (0);
   EXPECT_TRUE (base.cacheable (
      observation (0, { 400, 300, 506, 694 }), 16, 8));
   EXPECT_FALSE (base.cacheable (
      observation (0, { 400, 300, 650, 700 }), 16, 8));
   EXPECT_FALSE (base.cacheable (
      observation (~0ull), 16, 8));
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
   EXPECT_TRUE (a->same (*b, 0));
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
