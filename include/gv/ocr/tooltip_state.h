#pragma once

#include <gv/capture/frame.h>

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace gv::ocr {

struct TooltipIdentity {
   std::array<std::uint64_t, 16> bits {};

   static std::optional<TooltipIdentity> read (
      const cv::Mat& frame, const capture::Rect& box);
   int distance (const TooltipIdentity& other) const noexcept;
   bool same (const TooltipIdentity& other, int max_bits) const noexcept;
   std::uint64_t key () const noexcept;
   cv::Mat image () const;
};

struct TooltipObservation {
   TooltipIdentity identity;
   capture::Rect box;
   capture::CursorPos cursor;

   static std::optional<TooltipObservation> read (
      const cv::Mat& frame,
      const capture::Rect& box,
      const capture::CursorPos& cursor);
   bool cacheable (
      const TooltipObservation& other, int max_bits, int max_size_px) const noexcept;
};

enum class TooltipRelation : std::uint8_t {
   Missing,
   Same,
   Ambiguous,
   Different,
};

enum class TooltipTransition : std::uint8_t {
   None,
   Candidate,
   Same,
   Acquired,
   Replaced,
   Lost,
};

struct TooltipUpdate {
   TooltipTransition transition = TooltipTransition::None;
   TooltipRelation relation = TooltipRelation::Missing;
   int identity_distance = 0;
   bool size_changed = false;
   bool position_unexplained = false;
};

class TooltipState
{
public:
   struct Config {
      int stable_frames = 2;
      int missing_frames = 2;
      int identity_bits = 16;
      int position_px = 24;
      double size_ratio = 1.2;
   };

   explicit TooltipState (Config config = {});

   TooltipUpdate observe (std::optional<TooltipObservation> observation, bool force = false);
   void reset () noexcept;
   bool active () const noexcept;
   const std::optional<TooltipObservation>& current () const noexcept;

private:
   TooltipUpdate compare (
      const TooltipObservation& current,
      const TooltipObservation& next) const noexcept;
   bool agrees (
      const TooltipObservation& current,
      const TooltipObservation& next) const noexcept;

   Config config_;
   std::optional<TooltipObservation> current_;
   std::optional<TooltipObservation> candidate_;
   int stable_ = 0;
   int missing_ = 0;
};

}
