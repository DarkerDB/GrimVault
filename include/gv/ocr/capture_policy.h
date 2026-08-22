#pragma once

#include <algorithm>
#include <chrono>

namespace gv::ocr {

inline constexpr double minimum_capture_fps = 3.0;

inline constexpr int continuous_error_limit = 3;

inline constexpr std::chrono::milliseconds continuous_backoff_min { 2000 };
inline constexpr std::chrono::milliseconds continuous_backoff_max { 60000 };

constexpr std::chrono::milliseconds next_continuous_backoff (
   std::chrono::milliseconds current) noexcept
{
   return std::min (current * 2, continuous_backoff_max);
}

constexpr bool capture_active (
   bool enabled, bool automatic, bool tracking, bool forced) noexcept
{
   return enabled && (automatic || tracking || forced);
}

constexpr bool capture_targeted (bool has_window, bool forced) noexcept
{
   return has_window || forced;
}

constexpr double detector_fps (double capture_fps, double performance_fps,
                               bool performance) noexcept
{
   return std::max (
      performance ? performance_fps : capture_fps,
      minimum_capture_fps);
}

constexpr double frame_fps (
   double detector,
   double performance_detector,
   double tracker,
   double performance_tracker,
   bool performance,
   bool tracking) noexcept
{
   return detector_fps (
      tracking ? tracker : detector,
      tracking ? performance_tracker : performance_detector,
      performance);
}

}
