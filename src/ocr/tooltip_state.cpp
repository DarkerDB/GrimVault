#include <gv/ocr/tooltip_state.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <bit>

namespace gv::ocr {

namespace {

bool changed_by_ratio (int first, int second, double ratio) noexcept
{
   const auto smaller = std::min (first, second);
   const auto larger = std::max (first, second);
   return smaller <= 0 || static_cast<double> (larger) / smaller >= ratio;
}

int motion_error (
   const TooltipObservation& current,
   const TooltipObservation& next) noexcept
{
   const auto box_x = next.box.x - current.box.x;
   const auto box_y = next.box.y - current.box.y;
   if (!current.cursor.valid || !next.cursor.valid)
      return std::max (std::abs (box_x), std::abs (box_y));

   const auto cursor_x = next.cursor.x - current.cursor.x;
   const auto cursor_y = next.cursor.y - current.cursor.y;
   const auto error_x = std::min (std::abs (box_x), std::abs (box_x - cursor_x));
   const auto error_y = std::min (std::abs (box_y), std::abs (box_y - cursor_y));
   return std::max (error_x, error_y);
}

}

std::optional<TooltipIdentity> TooltipIdentity::read (
   const cv::Mat& frame, const capture::Rect& box)
{
   cv::Rect roi {
      box.x + 8,
      box.y + 8,
      std::max (0, box.w - 16),
      std::max (0, box.h - 16),
   };
   roi &= cv::Rect { 0, 0, frame.cols, frame.rows };
   if (roi.width < 32 || roi.height < 32) return std::nullopt;

   cv::Mat gray;
   cv::cvtColor (frame (roi), gray,
      frame.channels () == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);

   cv::Mat normalized;
   cv::resize (gray, normalized, { 32, 32 }, 0.0, 0.0, cv::INTER_AREA);

   const auto mean = cv::mean (normalized) [0];
   TooltipIdentity identity;
   for (int index = 0; index < normalized.rows * normalized.cols; ++index) {
      auto& word = identity.bits [static_cast<std::size_t> (index / 64)];
      word <<= 1;
      word |= normalized.at<std::uint8_t> (index / normalized.cols, index % normalized.cols)
         > mean;
   }
   return identity;
}

int TooltipIdentity::distance (const TooltipIdentity& other) const noexcept
{
   int result = 0;
   for (std::size_t index = 0; index < bits.size (); ++index)
      result += std::popcount (bits [index] ^ other.bits [index]);
   return result;
}

bool TooltipIdentity::same (const TooltipIdentity& other, int max_bits) const noexcept
{
   return distance (other) <= max_bits;
}

std::uint64_t TooltipIdentity::key () const noexcept
{
   std::uint64_t hash = 14695981039346656037ull;
   for (const auto word : bits) {
      hash ^= word;
      hash *= 1099511628211ull;
   }
   return hash;
}

cv::Mat TooltipIdentity::image () const
{
   cv::Mat result { 32, 32, CV_8UC1 };
   for (int index = 0; index < result.rows * result.cols; ++index) {
      const auto word = bits [static_cast<std::size_t> (index / 64)];
      const auto shift = 63 - index % 64;
      result.at<std::uint8_t> (index / result.cols, index % result.cols)
         = ((word >> shift) & 1) ? 255 : 0;
   }
   return result;
}

std::optional<TooltipObservation> TooltipObservation::read (
   const cv::Mat& frame,
   const capture::Rect& box,
   const capture::CursorPos& cursor)
{
   auto identity = TooltipIdentity::read (frame, box);
   if (!identity.has_value ()) return std::nullopt;
   return TooltipObservation { std::move (*identity), box, cursor };
}

bool TooltipObservation::cacheable (
   const TooltipObservation& other, int max_bits, int max_size_px) const noexcept
{
   return std::abs (box.w - other.box.w) <= max_size_px
      && std::abs (box.h - other.box.h) <= max_size_px
      && identity.same (other.identity, max_bits);
}

TooltipState::TooltipState (Config config) : config_ (config) {}

TooltipUpdate TooltipState::compare (
   const TooltipObservation& current,
   const TooltipObservation& next) const noexcept
{
   TooltipUpdate update;
   update.identity_distance = current.identity.distance (next.identity);
   update.size_changed = changed_by_ratio (current.box.w, next.box.w, config_.size_ratio)
      || changed_by_ratio (current.box.h, next.box.h, config_.size_ratio);
   update.position_unexplained = motion_error (current, next) > config_.position_px;
   update.relation = update.size_changed || update.identity_distance > config_.identity_bits
      ? TooltipRelation::Different
      : update.position_unexplained ? TooltipRelation::Ambiguous : TooltipRelation::Same;
   return update;
}

bool TooltipState::agrees (
   const TooltipObservation& current,
   const TooltipObservation& next) const noexcept
{
   const auto update = compare (current, next);
   return !update.size_changed
      && !update.position_unexplained
      && update.identity_distance <= config_.identity_bits;
}

TooltipUpdate TooltipState::observe (
   std::optional<TooltipObservation> observation, bool force)
{
   TooltipUpdate update;
   if (!observation.has_value ()) {
      candidate_.reset ();
      stable_ = 0;
      if (!current_.has_value () || ++missing_ < std::max (1, config_.missing_frames))
         return update;
      current_.reset ();
      missing_ = 0;
      update.transition = TooltipTransition::Lost;
      return update;
   }

   missing_ = 0;
   update.relation = TooltipRelation::Different;
   if (current_.has_value ()) update = compare (*current_, *observation);
   if (!force && update.relation == TooltipRelation::Same) {
      current_ = std::move (observation);
      candidate_.reset ();
      stable_ = 0;
      update.transition = TooltipTransition::Same;
      return update;
   }

   const bool stable = candidate_.has_value () && agrees (*candidate_, *observation);
   candidate_ = std::move (observation);
   stable_ = stable ? stable_ + 1 : 1;
   if (!force && stable_ < std::max (1, config_.stable_frames)) {
      update.transition = TooltipTransition::Candidate;
      return update;
   }

   const bool replaced = current_.has_value ();
   current_ = std::move (candidate_);
   candidate_.reset ();
   stable_ = 0;
   update.transition = replaced ? TooltipTransition::Replaced : TooltipTransition::Acquired;
   return update;
}

void TooltipState::reset () noexcept
{
   current_.reset ();
   candidate_.reset ();
   stable_ = 0;
   missing_ = 0;
}

bool TooltipState::active () const noexcept { return current_.has_value (); }

const std::optional<TooltipObservation>& TooltipState::current () const noexcept
{
   return current_;
}

}
