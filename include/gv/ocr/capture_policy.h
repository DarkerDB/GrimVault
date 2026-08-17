#pragma once

namespace gv::ocr {

constexpr bool capture_tracking (bool automatic, bool anchored, bool reacquiring) noexcept
{
   return anchored || (automatic && reacquiring);
}

// Capture runs only when there is something to look at: a scan the cursor
// settle watcher (or F5) asked for, or an anchored tooltip to keep verifying.
//
// Automatic mode used to stream continuously here. It bought acquisition
// latency — a frame already in flight when a tooltip appeared — and cost a
// capture and a detector pass every frame the game was focused, whether or
// not the cursor was anywhere near an item. The settle watcher already fires
// a forced scan 100 ms after the cursor stops, and a forced scan skips the
// stability gate, so acquisition completes from that burst alone.
constexpr bool capture_active (bool enabled, bool forced, bool tracking) noexcept
{
   return enabled && (forced || tracking);
}

constexpr bool capture_targeted (bool has_window, bool forced, bool tracking) noexcept
{
   return has_window || forced || tracking;
}

} // namespace gv::ocr
