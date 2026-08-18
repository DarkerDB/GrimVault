#include <gv/ocr/tooltip_state.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <bit>

namespace gv::ocr {

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
   identity.width = box.w;
   identity.height = box.h;
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

bool TooltipIdentity::same (
   const TooltipIdentity& other, int max_bits, int max_size_px) const noexcept
{
   return std::abs (width - other.width) <= max_size_px
      && std::abs (height - other.height) <= max_size_px
      && distance (other) <= max_bits;
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

TooltipState::TooltipState (Config config) : config_ (config) {}

TooltipTransition TooltipState::observe (
   std::optional<TooltipIdentity> identity, bool force)
{
   if (!identity.has_value ()) {
      candidate_.reset ();
      stable_ = 0;
      if (!current_.has_value () || ++missing_ < std::max (1, config_.missing_frames))
         return TooltipTransition::None;
      current_.reset ();
      missing_ = 0;
      return TooltipTransition::Lost;
   }

   missing_ = 0;
   if (!force && current_.has_value () && current_->same (
         *identity, config_.identity_bits, config_.identity_size_px)) {
      current_ = std::move (identity);
      candidate_.reset ();
      stable_ = 0;
      return TooltipTransition::Same;
   }

   const bool agrees = candidate_.has_value () && candidate_->same (
      *identity, config_.identity_bits, config_.identity_size_px);
   candidate_ = std::move (identity);
   stable_ = agrees ? stable_ + 1 : 1;
   if (!force && stable_ < std::max (1, config_.stable_frames))
      return TooltipTransition::Candidate;

   const bool replaced = current_.has_value ();
   current_ = std::move (candidate_);
   candidate_.reset ();
   stable_ = 0;
   return replaced ? TooltipTransition::Replaced : TooltipTransition::Acquired;
}

void TooltipState::reset () noexcept
{
   current_.reset ();
   candidate_.reset ();
   stable_ = 0;
   missing_ = 0;
}

bool TooltipState::active () const noexcept { return current_.has_value (); }

const std::optional<TooltipIdentity>& TooltipState::current () const noexcept
{
   return current_;
}

}
