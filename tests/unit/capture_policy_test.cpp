#include <gv/ocr/capture_policy.h>

#include <gtest/gtest.h>

using gv::ocr::capture_active;
using gv::ocr::capture_targeted;
using gv::ocr::capture_tracking;

TEST (CapturePolicy, ManualRestsWithoutRequest)
{
   EXPECT_FALSE (capture_active (true, false, false, false));
}

TEST (CapturePolicy, ManualStartsForcedScan)
{
   EXPECT_TRUE (capture_active (true, false, true, false));
   EXPECT_TRUE (capture_targeted (false, true, false));
}

TEST (CapturePolicy, ManualTracksUntilDismissed)
{
   const bool anchored = capture_tracking (false, true, false);
   const bool dismissed = capture_tracking (false, false, true);

   EXPECT_TRUE (capture_active (true, false, false, anchored));
   EXPECT_TRUE (capture_targeted (false, false, anchored));
   EXPECT_FALSE (capture_active (true, false, false, dismissed));
}

TEST (CapturePolicy, AutomaticContinuesReacquisition)
{
   EXPECT_TRUE (capture_tracking (true, false, true));
}

TEST (CapturePolicy, DisabledAlwaysRests)
{
   EXPECT_FALSE (capture_active (false, true, true, true));
}
