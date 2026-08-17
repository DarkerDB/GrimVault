#pragma once

#include <gv/capture/frame.h>
#include <gv/core/result.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace gv::vision {

// Shipped detector exports, produced by packages/scry/training/detection.
//
// The name carries the two parameters that distinguish them — architecture
// and input edge — because a models/ directory holding several .onnx files
// should be readable without loading any of them. Everything else about the
// run (depth, width, epochs, corpus size) rides in ONNX metadata.
//
// Exporting at another size means editing here; the size itself is read from
// the graph, so nothing downstream hardcodes 416.
namespace model_files {

   inline constexpr const char* tooltip_full = "tooltip-yolox-nano-416.onnx";

} // namespace model_files

struct TooltipBox {
   capture::Rect rect;
   float         confidence = 0.0f;
   int           class_id   = 0;
};

class TooltipDetector
{
public:
   TooltipDetector ();
   ~TooltipDetector ();

   core::Result<void> initialize (const std::filesystem::path& onnx_path);

   core::Result<std::vector<TooltipBox>> detect (const capture::Frame& frame);


   void  set_threshold (float t) noexcept;
   float threshold () const noexcept;

   // "DirectML" or "CPU" for the session as it stands right now. Worth
   // asking rather than trusting the startup log: a DirectML inference that
   // throws once reloads the session on CPU for the rest of the run, and the
   // only other trace of that is a warn line.
   std::string_view backend () const noexcept;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::vision
