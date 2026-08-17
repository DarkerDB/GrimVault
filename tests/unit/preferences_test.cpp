#include <gv/app/preferences.h>

#include <gtest/gtest.h>

using gv::app::Mode;
using gv::app::Preferences;
using gv::app::apply;
using gv::capture::CaptureMode;
using Align = gv::ui::Layout::Align;
using Columns = gv::ui::Layout::Columns;

namespace {

   // The flat key/value form SettingsSync writes into UserSettingsRepo:
   // doubles arrive from std::to_string, so "0.900000" not "0.9".
   Preferences folded (std::initializer_list<std::pair<const char*, const char*>> rows)
   {
      Preferences prefs;
      for (const auto& [key, value] : rows) apply (prefs, key, value);
      return prefs;
   }

   bool widget (const Preferences& prefs, std::string_view slug)
   {
      for (const auto& [name, visible] : prefs.options.widgets) {
         if (name == slug) return visible;
      }
      ADD_FAILURE () << "widget '" << slug << "' not folded";
      return false;
   }

} // namespace

TEST (Preferences, DefaultsMatchTheServerSchema)
{
   const Preferences prefs;

   EXPECT_EQ (prefs.overlay_mode, Mode::Auto);
   EXPECT_EQ (prefs.layout.align, Align::Attached);
   EXPECT_DOUBLE_EQ (prefs.layout.opacity, 0.9);
   EXPECT_DOUBLE_EQ (prefs.layout.scale, 1.0);
   EXPECT_EQ (prefs.layout.offset_x, 20);
   EXPECT_EQ (prefs.layout.offset_y, 20);
   EXPECT_TRUE (prefs.layout.enabled);
   EXPECT_TRUE (prefs.indicator_visible);
   EXPECT_EQ (prefs.options.currency_display, "absolute");
   EXPECT_EQ (prefs.capture_fps, 15);
   EXPECT_EQ (prefs.capture_mode, CaptureMode::Automatic);
}

TEST (Preferences, FoldsTheOverlayGroup)
{
   const auto prefs = folded ({
      { "overlay:mode",      "manual" },
      { "overlay:alignment", "bottom_right" },
      { "overlay:opacity",   "0.500000" },
      { "overlay:scale",     "1.250000" },
      { "overlay:offset_x",  "-40" },
      { "overlay:offset_y",  "12" },
      { "overlay:columns",   "2" },
      { "overlay:is_indicator_visible", "false" },
   });

   EXPECT_EQ (prefs.overlay_mode, Mode::Manual);
   EXPECT_EQ (prefs.layout.align, Align::BottomRight);
   EXPECT_DOUBLE_EQ (prefs.layout.opacity, 0.5);
   EXPECT_DOUBLE_EQ (prefs.layout.scale, 1.25);
   EXPECT_EQ (prefs.layout.offset_x, -40);
   EXPECT_EQ (prefs.layout.offset_y, 12);
   EXPECT_EQ (prefs.layout.columns, Columns::Two);
   EXPECT_TRUE (prefs.layout.enabled);
   EXPECT_FALSE (prefs.indicator_visible);
}

/* The card and the corner badge are separate windows and separate decisions:
 * disabling the overlay is how a player stops the analysis card, not how they
 * ask for no on-screen trace of GrimVault at all. */
TEST (Preferences, IndicatorSurvivesADisabledOverlay)
{
   const auto prefs = folded ({ { "overlay:mode", "disabled" } });

   EXPECT_FALSE (prefs.layout.enabled);
   EXPECT_TRUE  (prefs.indicator_visible);
}

/* Auto is the default, and anything the server sends that isn't one of the
 * two manual values has to land there rather than on a column count — a
 * desktop build older than a settings change must degrade to "let the
 * overlay decide", never to a layout the player didn't pick. */
TEST (Preferences, ColumnsDefaultToAutoAndRejectNonsense)
{
   EXPECT_EQ (folded ({}).layout.columns, Columns::Auto);
   EXPECT_EQ (folded ({ { "overlay:columns", "auto" } }).layout.columns, Columns::Auto);
   EXPECT_EQ (folded ({ { "overlay:columns", "1" } }).layout.columns,    Columns::One);
   EXPECT_EQ (folded ({ { "overlay:columns", "3" } }).layout.columns,    Columns::Three);
   EXPECT_EQ (folded ({ { "overlay:columns", "4" } }).layout.columns,    Columns::Auto);
   EXPECT_EQ (folded ({ { "overlay:columns", "" } }).layout.columns,     Columns::Auto);
}

TEST (Preferences, DisabledModeClearsTheEnabledFlag)
{
   EXPECT_FALSE (folded ({ { "overlay:mode", "disabled" } }).layout.enabled);
   EXPECT_TRUE  (folded ({ { "overlay:mode", "automatic" } }).layout.enabled);
}

TEST (Preferences, ClampsNumbersOutOfRange)
{
   const auto prefs = folded ({
      { "overlay:opacity", "4.0" },
      { "overlay:scale",   "0.0" },
   });

   EXPECT_DOUBLE_EQ (prefs.layout.opacity, 1.0);
   EXPECT_DOUBLE_EQ (prefs.layout.scale, 0.5);
}

TEST (Preferences, KeepsDefaultsForUnparseableNumbers)
{
   const auto prefs = folded ({
      { "overlay:opacity",  "not-a-number" },
      { "overlay:offset_x", "" },
      { "overlay:scale",    "nan" },
      { "overlay:offset_y", "12px" },
   });

   EXPECT_DOUBLE_EQ (prefs.layout.opacity, 0.9);
   EXPECT_DOUBLE_EQ (prefs.layout.scale, 1.0);
   EXPECT_EQ (prefs.layout.offset_x, 20);
   EXPECT_EQ (prefs.layout.offset_y, 20);
}

TEST (Preferences, FoldsWidgetTogglesInKeyOrder)
{
   const auto prefs = folded ({
      { "tooltip:analysis:market_value", "true"  },
      { "tooltip:analysis:trade_chat",   "false" },
      { "tooltip:analysis:roll_quality", "true"  },
   });

   ASSERT_EQ (prefs.options.widgets.size (), 3u);
   EXPECT_EQ (prefs.options.widgets [0].first, "market_value");
   EXPECT_EQ (prefs.options.widgets [1].first, "trade_chat");
   EXPECT_EQ (prefs.options.widgets [2].first, "roll_quality");

   EXPECT_TRUE  (widget (prefs, "market_value"));
   EXPECT_FALSE (widget (prefs, "trade_chat"));
}

// The widget vocabulary lives in grimvault-widgets.yaml. A client that
// dropped slugs it wasn't compiled against would need a release per widget.
TEST (Preferences, AcceptsWidgetSlugsItHasNeverSeen)
{
   const auto prefs = folded ({ { "tooltip:analysis:some_future_widget", "false" } });

   ASSERT_EQ (prefs.options.widgets.size (), 1u);
   EXPECT_FALSE (widget (prefs, "some_future_widget"));
}

TEST (Preferences, ReTogglingAWidgetKeepsItsPosition)
{
   Preferences prefs;
   apply (prefs, "tooltip:analysis:market_value", "true");
   apply (prefs, "tooltip:analysis:trade_chat",   "true");
   apply (prefs, "tooltip:analysis:market_value", "false");

   ASSERT_EQ (prefs.options.widgets.size (), 2u);
   EXPECT_EQ (prefs.options.widgets [0].first, "market_value");
   EXPECT_FALSE (widget (prefs, "market_value"));
}

TEST (Preferences, AppliesExplicitServerWidgetOrder)
{
   Preferences prefs;
   apply (prefs, "tooltip:analysis:market_value", "true");
   apply (prefs, "tooltip:analysis:trade_chat", "false");
   apply (prefs, "tooltip:analysis:roll_quality", "true");
   apply (prefs, "tooltip:analysis_order",
      R"(["roll_quality","market_value","trade_chat"])");

   ASSERT_EQ (prefs.options.widgets.size (), 3u);
   EXPECT_EQ (prefs.options.widgets [0].first, "roll_quality");
   EXPECT_EQ (prefs.options.widgets [1].first, "market_value");
   EXPECT_EQ (prefs.options.widgets [2].first, "trade_chat");
}

TEST (Preferences, FoldsCurrencyDisplay)
{
   EXPECT_EQ (folded ({ { "pricing:currency_display", "compact" } })
      .options.currency_display, "compact");
   // Anything outside the allowlist falls back rather than reaching the card.
   EXPECT_EQ (folded ({ { "pricing:currency_display", "gold" } })
      .options.currency_display, "absolute");
}

TEST (Preferences, FoldsBehaviorAndHotkeys)
{
   const auto prefs = folded ({
      { "behavior:is_auto_update_enabled",       "false" },
      { "behavior:is_launch_on_startup_enabled", "true"  },
      { "behavior:is_performance_mode_enabled",  "true"  },
      { "behavior:capture_fps",                  "10"    },
      { "hotkeys:force_refresh",                 "F9"    },
      { "hotkeys:toggle_overlay",                "Ctrl+Alt+G" },
   });

   EXPECT_FALSE (prefs.auto_updates);
   EXPECT_TRUE  (prefs.launch_on_startup);
   EXPECT_TRUE  (prefs.performance_mode);
   EXPECT_EQ (prefs.capture_fps, 10);
   EXPECT_EQ (prefs.hotkey_scan_now, "F9");
   EXPECT_EQ (prefs.hotkey_toggle_overlay, "Ctrl+Alt+G");
}

// SettingsSync emits an empty value when the server drops a key, which has
// to mean "back to the compiled default", not "false" or "empty string".
TEST (Preferences, ErasedKeysRevertToDefaults)
{
   Preferences prefs;
   apply (prefs, "overlay:mode",                       "disabled");
   apply (prefs, "overlay:alignment",                  "top_left");
   apply (prefs, "behavior:is_launch_on_startup_enabled", "false");
   apply (prefs, "behavior:is_performance_mode_enabled",  "true");
   apply (prefs, "overlay:is_indicator_visible",       "false");
   apply (prefs, "behavior:capture_fps",                    "5");
   apply (prefs, "hotkeys:force_refresh",              "F9");

   apply (prefs, "overlay:mode",                       "");
   apply (prefs, "overlay:alignment",                  "");
   apply (prefs, "behavior:is_launch_on_startup_enabled", "");
   apply (prefs, "behavior:is_performance_mode_enabled",  "");
   apply (prefs, "overlay:is_indicator_visible",       "");
   apply (prefs, "behavior:capture_fps",                    "");
   apply (prefs, "hotkeys:force_refresh",              "");

   EXPECT_EQ (prefs.overlay_mode, Mode::Auto);
   EXPECT_TRUE (prefs.layout.enabled);
   EXPECT_EQ (prefs.layout.align, Align::Attached);
   EXPECT_TRUE (prefs.launch_on_startup);
   EXPECT_FALSE (prefs.performance_mode);
   EXPECT_TRUE (prefs.indicator_visible);
   EXPECT_EQ (prefs.capture_fps, 15);
   EXPECT_TRUE (prefs.hotkey_scan_now.empty ());
}

TEST (Preferences, RejectsUnsupportedCaptureRates)
{
   EXPECT_EQ (folded ({ { "behavior:capture_fps", "30" } }).capture_fps, 30);
   // 1 is the rate performance mode pins to, and a standalone choice.
   EXPECT_EQ (folded ({ { "behavior:capture_fps", "1" } }).capture_fps, 1);
   EXPECT_EQ (folded ({ { "behavior:capture_fps", "2" } }).capture_fps, 15);
   EXPECT_EQ (folded ({ { "behavior:capture_fps", "12" } }).capture_fps, 15);
   EXPECT_EQ (folded ({ { "behavior:capture_fps", "60" } }).capture_fps, 15);
   EXPECT_EQ (folded ({ { "behavior:capture_fps", "fast" } }).capture_fps, 15);
}

TEST (Preferences, FoldsCaptureMode)
{
   EXPECT_EQ (folded ({ { "behavior:capture_mode", "automatic" } }).capture_mode,
      CaptureMode::Automatic);
   EXPECT_EQ (folded ({ { "behavior:capture_mode", "wgc" } }).capture_mode,
      CaptureMode::ForceWgc);
   EXPECT_EQ (folded ({ { "behavior:capture_mode", "dxgi" } }).capture_mode,
      CaptureMode::ForceDxgi);
   EXPECT_EQ (folded ({ { "behavior:capture_mode", "gdi" } }).capture_mode,
      CaptureMode::ForceGdi);
   EXPECT_EQ (folded ({ { "behavior:capture_mode", "vhs" } }).capture_mode,
      CaptureMode::Automatic);

   Preferences prefs;
   apply (prefs, "behavior:capture_mode", "gdi");
   apply (prefs, "behavior:capture_mode", "");
   EXPECT_EQ (prefs.capture_mode, CaptureMode::Automatic);
}

TEST (Preferences, ReportsWhetherAKeyWasConsumed)
{
   Preferences prefs;

   EXPECT_TRUE  (apply (prefs, "overlay:opacity", "0.5"));
   // Website-only keys are recognized so they don't log as unknown.
   EXPECT_TRUE  (apply (prefs, "tooltip:sections:header", "false"));
   EXPECT_FALSE (apply (prefs, "something:else", "1"));
   EXPECT_FALSE (apply (prefs, "tooltip:analysis:", "true"));
}
