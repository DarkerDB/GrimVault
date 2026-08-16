#pragma once

namespace gv::ocr {

constexpr bool capture_tracking (bool automatic, bool anchored, bool reacquiring) noexcept
{
   return anchored || (automatic && reacquiring);
}

constexpr bool capture_active (
   bool enabled, bool automatic, bool forced, bool tracking, bool performance_mode = false) noexcept
{
   return enabled && ((automatic && !performance_mode) || forced || tracking);
}

constexpr bool capture_targeted (bool has_window, bool forced, bool tracking) noexcept
{
   return has_window || forced || tracking;
}

} // namespace gv::ocr
