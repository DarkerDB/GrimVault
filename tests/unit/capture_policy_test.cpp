#include <gv/ocr/capture_policy.h>

#include <gtest/gtest.h>

using gv::ocr::capture_active;
using gv::ocr::capture_targeted;
using gv::ocr::capture_tracking;

TEST (CapturePolicy, RestsWithoutRequest)
{
   EXPECT_FALSE (capture_active (true, false, false));
}

TEST (CapturePolicy, StartsForcedScan)
{
   EXPECT_TRUE (capture_active (true, true, false));
   EXPECT_TRUE (capture_targeted (false, true, false));
}

TEST (CapturePolicy, TracksUntilDismissed)
{
   const bool anchored = capture_tracking (false, true, false);
   const bool dismissed = capture_tracking (false, false, true);

   EXPECT_TRUE (capture_active (true, false, anchored));
   EXPECT_TRUE (capture_targeted (false, false, anchored));
   EXPECT_FALSE (capture_active (true, false, dismissed));
}

TEST (CapturePolicy, AutomaticContinuesReacquisition)
{
   EXPECT_TRUE (capture_tracking (true, false, true));
}

/* Automatic mode used to stream continuously whether or not anything was
 * being hovered. The cursor settle watcher fires a forced scan instead, so
 * an idle Automatic session is now as quiet as an idle Manual one. */
TEST (CapturePolicy, AutomaticIdlesUntilTheCursorSettles)
{
   EXPECT_FALSE (capture_active (true, false, false));
   EXPECT_TRUE  (capture_active (true, true,  false));
   EXPECT_TRUE  (capture_active (true, false, true));
}

TEST (CapturePolicy, DisabledAlwaysRests)
{
   EXPECT_FALSE (capture_active (false, true, true));
}
