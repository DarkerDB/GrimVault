#pragma once

namespace gv::ocr {

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
   return performance && performance_fps < capture_fps
      ? performance_fps
      : capture_fps;
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
