#include <gv/ocr/tooltip_filter.h>

#include <algorithm>
#include <cctype>

namespace gv::ocr {

void OverlayExclusion::update (const capture::Rect& desktop, Clock::time_point now)
{
   std::lock_guard guard { lock_ };
   if (desktop.empty ()) {
      if (visible_) hidden_at_ = now;
      visible_ = false;
      return;
   }
   area_ = desktop;
   visible_ = true;
}

bool OverlayExclusion::covers (const capture::Rect& box, int origin_x, int origin_y,
                               Clock::time_point captured_at) const
{
   if (box.empty ()) return false;

   capture::Rect area;
   {
      std::lock_guard guard { lock_ };
      if (area_.empty ()) return false;
      if (!visible_ && captured_at > hidden_at_ + k_hide_grace) return false;
      area = area_;
   }

   const int left = std::max (box.x, area.x - origin_x);
   const int top = std::max (box.y, area.y - origin_y);
   const int right = std::min (box.x + box.w, area.x - origin_x + area.w);
   const int bottom = std::min (box.y + box.h, area.y - origin_y + area.h);
   if (right <= left || bottom <= top) return false;

   const double inside = static_cast<double> (right - left) * (bottom - top);
   return inside >= k_covered_ratio * static_cast<double> (box.w) * box.h;
}

bool looks_like_tooltip (std::string_view text) noexcept
{
   int lines = 0;
   bool content = false;
   for (const char ch : text) {
      if (ch == '\n') {
         lines += content ? 1 : 0;
         content = false;
      } else if (!std::isspace (static_cast<unsigned char> (ch))) {
         content = true;
      }
   }
   lines += content ? 1 : 0;
   return lines >= k_min_tooltip_lines;
}

void RejectedTooltips::reject (const TooltipObservation& observation, Clock::time_point now)
{
   std::lock_guard guard { lock_ };
   std::erase_if (entries_, [now] (const Entry& entry) { return entry.expires <= now; });

   if (entries_.size () == k_capacity) {
      entries_.erase (entries_.begin ());
   }

   entries_.push_back ({ observation, now + k_retry_after });
}

bool RejectedTooltips::contains (const TooltipObservation& observation, int max_bits,
                                 int max_size_px, Clock::time_point now) const
{
   std::lock_guard guard { lock_ };
   return std::any_of (entries_.begin (), entries_.end (), [&] (const Entry& entry) {
      return entry.expires > now &&
             entry.observation.cacheable (observation, max_bits, max_size_px);
   });
}

}
