#include <gv/api/darkerdb_client.h>

#include <gtest/gtest.h>

TEST (Settings, ParsesCaptureMode)
{
   const auto settings = gv::api::parse_settings (R"({
      "body": {
         "behavior": {
            "capture_fps": 15,
            "capture_mode": "dxgi",
            "is_performance_mode_enabled": true,
            "is_auto_update_enabled": true,
            "is_launch_on_startup_enabled": true
         },
         "hotkeys": {},
         "overlay": {},
         "pricing": {},
         "tooltip": {}
      }
   })");

   ASSERT_TRUE (settings.has_value ()) << settings.error ().message;
   EXPECT_EQ (settings->behavior.capture_mode, "dxgi");
   EXPECT_TRUE (settings->behavior.is_performance_mode_enabled);
   EXPECT_EQ (settings->values.at ("behavior:capture_mode"), "dxgi");
   EXPECT_EQ (settings->values.at ("behavior:is_performance_mode_enabled"), "true");
}
