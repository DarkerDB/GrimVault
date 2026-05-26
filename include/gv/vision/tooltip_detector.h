#pragma once

#include <gv/capture/frame.h>
#include <gv/core/result.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace gv::vision {

struct TooltipBox {
   capture::Rect rect;
   float         confidence = 0.0f;
   int           class_id   = 0;
};

// YOLO-style ONNX tooltip detector. Loads tooltip.onnx, runs inference on
// each Frame, emits zero or more bounding boxes around the in-game tooltip
// region(s). Non-Max Suppression is applied internally.
class TooltipDetector
{
public:
   TooltipDetector ();
   ~TooltipDetector ();

   core::Result<void> initialize (const std::filesystem::path& onnx_path);

   core::Result<std::vector<TooltipBox>> detect (const capture::Frame& frame);

   // Confidence threshold for keeping a detection. Defaults to 0.45.
   void  set_threshold (float t) noexcept;
   float threshold () const noexcept;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::vision
