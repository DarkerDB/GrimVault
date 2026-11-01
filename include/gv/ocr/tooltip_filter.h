#pragma once

#include <gv/capture/frame.h>
#include <gv/ocr/tooltip_state.h>

#include <chrono>
#include <mutex>
#include <string_view>
#include <vector>

namespace gv::ocr {

// GrimVault's own card is drawn over the game, so desktop capture sees it
// and the detector reads it as a tooltip. Accepting it replaced the item
// the player was hovering and dismissed the card that had just appeared.
// The UI reports where the card is; the vision thread drops detections
// that mostly sit inside it.
class OverlayExclusion
{
  public:
   using Clock = std::chrono::steady_clock;

   // Hidden cards stay excluded for frames captured within this window:
   // desktop duplication can deliver a frame composed before the hide.
   static constexpr std::chrono::milliseconds k_hide_grace { 150 };

   // Share of a detection's area inside the card that marks it as the card.
   static constexpr double k_covered_ratio = 0.5;

   // Desktop (physical) pixels. An empty rect hides the card as of `now`.
   void update (const capture::Rect& desktop, Clock::time_point now = Clock::now ());

   // `box` is frame-local; origin is the frame's top-left in desktop pixels.
   bool covers (const capture::Rect& box, int origin_x, int origin_y,
                Clock::time_point captured_at) const;

  private:
   mutable std::mutex lock_;
   capture::Rect area_;
   bool visible_ = false;
   Clock::time_point hidden_at_;
};

// Every real tooltip has a title and at least one line under it. One line
// (or none) is a chat line, a button label, or detector noise; the API
// rejected all of them, and only a cut-short real tooltip ever read as two.
inline constexpr int k_min_tooltip_lines = 2;

bool looks_like_tooltip (std::string_view text) noexcept;

// OCR-rejected image identities must not remain eligible tracking targets.
// Entries expire so a transient incomplete OCR read can be retried.
class RejectedTooltips
{
  public:
   using Clock = std::chrono::steady_clock;
   static constexpr std::chrono::seconds k_retry_after { 2 };
   static constexpr std::size_t k_capacity = 32;

   // Input: the observation whose OCR was not a tooltip. Output: suppression
   // until now + 2s. Example: a one-line button is ignored on the next frame.
   void reject (const TooltipObservation& observation, Clock::time_point now = Clock::now ());

   // Match content and size, never coordinates alone. Example: new item text
   // at a rejected button's position remains eligible immediately.
   bool contains (const TooltipObservation& observation, int max_bits = 16, int max_size_px = 8,
                  Clock::time_point now = Clock::now ()) const;

  private:
   struct Entry {
      TooltipObservation observation;
      Clock::time_point expires;
   };
   mutable std::mutex lock_;
   std::vector<Entry> entries_;
};

}
