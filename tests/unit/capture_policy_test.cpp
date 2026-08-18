#include <gv/ocr/capture_policy.h>

#include <gtest/gtest.h>

using gv::ocr::capture_active;
using gv::ocr::capture_targeted;
using gv::ocr::detector_fps;
using gv::ocr::frame_fps;

TEST (CapturePolicy, AutomaticStreamsContinuously)
{
   EXPECT_TRUE (capture_active (true, true, false, false));
}

TEST (CapturePolicy, ManualAcquiresOnRequestAndTracksTooltip)
{
   EXPECT_FALSE (capture_active (true, false, false, false));
   EXPECT_TRUE (capture_active (true, false, false, true));
   EXPECT_TRUE (capture_active (true, false, true, false));
}

TEST (CapturePolicy, DisabledAlwaysRests)
{
   EXPECT_FALSE (capture_active (false, true, true, true));
}

TEST (CapturePolicy, RequestSupportsDesktopCapture)
{
   EXPECT_FALSE (capture_targeted (false, false));
   EXPECT_TRUE (capture_targeted (false, true));
}

TEST (CapturePolicy, PerformanceCapsDetectorRate)
{
   EXPECT_DOUBLE_EQ (detector_fps (15.0, 3.0, false), 15.0);
   EXPECT_DOUBLE_EQ (detector_fps (15.0, 3.0, true), 3.0);
   EXPECT_DOUBLE_EQ (detector_fps (2.0, 3.0, true), 2.0);
}

TEST (CapturePolicy, TrackingUsesItsOwnRate)
{
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, false, false), 15.0);
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, true, false), 3.0);
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, false, true), 60.0);
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, true, true), 30.0);
}
