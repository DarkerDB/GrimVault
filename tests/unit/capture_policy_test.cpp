#include <gv/ocr/capture_policy.h>

#include <gtest/gtest.h>

#include <chrono>

using gv::ocr::capture_active;
using gv::ocr::capture_targeted;
using gv::ocr::continuous_backoff_max;
using gv::ocr::continuous_backoff_min;
using gv::ocr::detector_fps;
using gv::ocr::frame_fps;
using gv::ocr::minimum_capture_fps;
using gv::ocr::next_continuous_backoff;

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
   EXPECT_DOUBLE_EQ (detector_fps (2.0, 3.0, true), minimum_capture_fps);
   EXPECT_DOUBLE_EQ (detector_fps (1.0, 3.0, false), minimum_capture_fps);
}

TEST (CapturePolicy, TrackingUsesItsOwnRate)
{
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, false, false), 15.0);
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, true, false), 3.0);
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, false, true), 60.0);
   EXPECT_DOUBLE_EQ (frame_fps (15.0, 3.0, 60.0, 30.0, true, true), 30.0);
}

TEST (CapturePolicy, ContinuousRetryBacksOffAndClamps)
{
   EXPECT_EQ (next_continuous_backoff (continuous_backoff_min),
              continuous_backoff_min * 2);
   EXPECT_EQ (next_continuous_backoff (continuous_backoff_max),
              continuous_backoff_max);
   EXPECT_EQ (next_continuous_backoff (continuous_backoff_max / 2 + std::chrono::milliseconds { 1 }),
              continuous_backoff_max);
}
