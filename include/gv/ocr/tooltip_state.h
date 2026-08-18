#pragma once

#include <gv/capture/frame.h>

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace gv::ocr {

struct TooltipIdentity {
   std::array<std::uint64_t, 16> bits {};
   int width = 0;
   int height = 0;

   static std::optional<TooltipIdentity> read (
      const cv::Mat& frame, const capture::Rect& box);
   int distance (const TooltipIdentity& other) const noexcept;
   bool same (const TooltipIdentity& other, int max_bits, int max_size_px) const noexcept;
   std::uint64_t key () const noexcept;
   cv::Mat image () const;
};

enum class TooltipTransition : std::uint8_t {
   None,
   Candidate,
   Same,
   Acquired,
   Replaced,
   Lost,
};

class TooltipState
{
public:
   struct Config {
      int stable_frames = 2;
      int missing_frames = 2;
      int identity_bits = 16;
      int identity_size_px = 8;
   };

   explicit TooltipState (Config config = {});

   TooltipTransition observe (std::optional<TooltipIdentity> identity, bool force = false);
   void reset () noexcept;
   bool active () const noexcept;
   const std::optional<TooltipIdentity>& current () const noexcept;

private:
   Config config_;
   std::optional<TooltipIdentity> current_;
   std::optional<TooltipIdentity> candidate_;
   int stable_ = 0;
   int missing_ = 0;
};

}
