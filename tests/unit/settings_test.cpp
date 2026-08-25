#include <gv/api/darkerdb_client.h>

#include <gtest/gtest.h>

TEST (Settings, ParsesBehavior)
{
   const auto settings = gv::api::parse_settings (R"({
      "body": {
         "behavior": {
            "capture_fps": 15,
            "capture_mode": "dxgi",
            "is_performance_mode_enabled": true,
            "language": "zh-Hant",
            "is_auto_update_enabled": true,
            "is_launch_on_startup_enabled": true
         },
         "hotkeys": {},
         "overlay": { "renderer": "qml" },
         "pricing": {},
         "tooltip": {}
      }
   })");

   ASSERT_TRUE (settings.has_value ()) << settings.error ().message;
   EXPECT_EQ (settings->behavior.capture_mode, "dxgi");
   EXPECT_TRUE (settings->behavior.is_performance_mode_enabled);
   EXPECT_EQ (settings->values.at ("behavior:capture_mode"), "dxgi");
   EXPECT_EQ (settings->values.at ("behavior:is_performance_mode_enabled"), "true");
   EXPECT_EQ (settings->behavior.language, "zh-Hant");
   EXPECT_EQ (settings->values.at ("behavior:language"), "zh-Hant");
   EXPECT_FALSE (settings->values.contains ("overlay:renderer"));
}

TEST (Settings, ParsesIndicatorVisibility)
{
   const auto settings = gv::api::parse_settings (R"({
      "body": {
         "behavior": {},
         "hotkeys":  {},
         "overlay":  { "is_indicator_visible": false },
         "pricing":  {},
         "tooltip":  {}
      }
   })");

   ASSERT_TRUE (settings.has_value ()) << settings.error ().message;
   EXPECT_FALSE (settings->overlay.is_indicator_visible);
   EXPECT_EQ (settings->values.at ("overlay:is_indicator_visible"), "false");
}

// A server that predates the key must not read as "hide it" — the flattened
// map still carries the compiled default so SettingsSync stores a real value.
TEST (Settings, IndicatorDefaultsToVisibleWhenAbsent)
{
   const auto settings = gv::api::parse_settings (R"({
      "body": {
         "behavior": {},
         "hotkeys":  {},
         "overlay":  {},
         "pricing":  {},
         "tooltip":  {}
      }
   })");

   ASSERT_TRUE (settings.has_value ()) << settings.error ().message;
   EXPECT_TRUE (settings->overlay.is_indicator_visible);
   EXPECT_EQ (settings->values.at ("overlay:is_indicator_visible"), "true");
   EXPECT_FALSE (settings->values.contains ("overlay:renderer"));
}

TEST (Settings, ParsesImprovementCollectionConsent)
{
   const auto settings = gv::api::parse_settings (R"({
      "body": {
         "behavior": {},
         "collection": { "is_improvement_enabled": true },
         "hotkeys": {},
         "overlay": {},
         "pricing": {},
         "tooltip": {}
      }
   })");

   ASSERT_TRUE (settings.has_value ()) << settings.error ().message;
   EXPECT_TRUE (settings->collection.is_improvement_enabled);
   EXPECT_EQ (settings->values.at ("collection:is_improvement_enabled"), "true");
}
