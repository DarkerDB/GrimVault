#pragma once

#include <gv/capture/capture_service.h>
#include <gv/capture/frame.h>
#include <gv/core/result.h>
#include <gv/ocr/language_registry.h>
#include <gv/vision/tooltip_detector.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace gv::ocr {

struct RecognizedTooltip {
   capture::Rect rect;
   std::string   text;
   float         confidence = 0.0f;
   std::chrono::steady_clock::time_point captured_at;
};

// Real-time OCR pipeline:
//
//    [capture thread]    FrameSource ---SpscQueue<Frame>--->
//    [vision worker]     TooltipDetector ---SpscQueue<Boxes>--->
//    [ocr workers]       Recognizer ---SpscQueue<Recognized>--->
//    [callback]          on_tooltip (called on the OCR worker thread)
//
// Stability gate: a tooltip box must persist for N consecutive vision
// frames at IoU >= 0.9 before triggering recognition (configurable).
//
// Idle policy: if no tooltips are detected for `idle_window`, vision fps
// drops to `idle_fps`. Motion or focus change resumes `active_fps`.
//
// The pipeline owns all worker threads. start() spawns them; stop() joins.
class Pipeline
{
public:
   struct Config {
      double                      active_fps         = 15.0;
      double                      idle_fps           = 3.0;
      std::chrono::milliseconds   idle_window        { 2000 };
      int                         stability_frames   = 2;
      float                       stability_iou      = 0.9f;
      LanguageFamily              language           = LanguageFamily::Latin;
   };

   using TooltipCallback  = std::function<void (const RecognizedTooltip&)>;
   using ActivityCallback = std::function<void ()>;

   Pipeline (
      capture::CaptureService& capture,
      vision::TooltipDetector& detector,
      LanguageRegistry&        registry,
      Config                   config = {}
   );

   ~Pipeline ();

   core::Result<void> start (TooltipCallback on_tooltip);
   void               stop  () noexcept;

   // Fired from the vision thread the moment a tooltip box is detected —
   // well before OCR + lookup finish — so the UI can signal "scanning" with
   // no perceived lag. Set before start().
   void on_activity (ActivityCallback cb);

   void set_active_window (void* hwnd);   // HWND or nullptr to capture monitor

   void set_language (LanguageFamily f);

   // Bypass the stability gate for one OCR cycle. The next frame with any
   // detected tooltip box is forwarded to OCR regardless of accumulated
   // stability count. Used by the manual-scan hotkey and the mouse-still
   // trigger.
   void request_immediate_scan ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ocr
