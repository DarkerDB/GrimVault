#pragma once

#include <gv/capture/capture_service.h>
#include <gv/capture/frame.h>
#include <gv/core/result.h>
#include <gv/ocr/language_registry.h>
#include <gv/vision/tooltip_detector.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace gv::ocr {

struct RecognizedTooltip {
   std::uint64_t generation = 0;
   capture::Rect rect;
   std::string   text;
   std::unordered_map<std::string, std::string> gems;
   float         confidence = 0.0f;
   capture::CaptureBackend backend = capture::CaptureBackend::Unknown;
   bool          preliminary = false; // title-only; full tooltip follows
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
      double                      anchored_fps       = 60.0;
      double                      anchored_burst_fps = 120.0;
      double                      capture_fps        = 15.0;
      std::chrono::milliseconds   anchored_burst     { 150 };
      double                      idle_fps           = 3.0;
      std::chrono::milliseconds   idle_window        { 2000 };
      int                         stability_frames   = 2;
      float                       stability_iou      = 0.9f;
      int                         pin_near_edge_px   = 48;
      int                         pin_right_edge_px  = 32;
      int                         identity_bits      = 14;
      int                         identity_detail_px = 4;
      int                         search_free_px     = 16;
      int                         search_pinned_px   = 3;
      int                         cursor_reset_px    = 140;
      LanguageFamily              language           = LanguageFamily::Latin;
      // When non-empty, persist only detector-selected tooltip crops and
      // their segmented OCR lines for later, human-verified model training.
      std::filesystem::path       sample_inbox;
   };

   // Anchoring (docs/architecture/anchoring.md): once a tooltip settles,
   // the pipeline emits its anchor — the cursor->tooltip offset and exact
   // size — and the UI draws from cursor math until the anchor is lost.
   struct AnchorEvent {
      std::uint64_t generation = 0;
      int  offset_x = 0;      // tooltip top-left minus cursor, physical px
      int  offset_y = 0;
      bool locked_x = false;  // offset measured while not pinned at an edge
      bool locked_y = false;
      bool pinned_x = false;
      bool pinned_y = false;
      int  pin_x    = 0;      // fixed game-relative coordinate while pinned
      int  pin_y    = 0;
      int  w        = 0;
      int  h        = 0;
   };

   using TooltipCallback    = std::function<void (const RecognizedTooltip&)>;
   using ActivityCallback   = std::function<void ()>;
   using AnchorCallback     = std::function<void (const AnchorEvent&)>;

   // immediate = vision confirmed disappearance (or cursor jump), so
   // hide now. False remains available for future soft-loss sources.
   using AnchorLostCallback = std::function<void (bool immediate)>;

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

   // Anchor established / updated (offsets lock as the tooltip unpins) and
   // anchor lost, fired from the vision thread. Set before start().
   void on_anchor      (AnchorCallback cb);
   void on_anchor_lost (AnchorLostCallback cb);

   void set_active_window (void* hwnd);   // HWND or nullptr to capture monitor

   // Runtime policy. Disabled means no capture, detection, OCR, or API work.
   // Manual keeps the pipeline armed but captures only after an immediate scan.
   void set_enabled   (bool on);
   void set_automatic (bool on);

   // Maximum full-frame GPU readbacks per second. Applies immediately.
   void set_capture_fps (double fps);

   // Capture-backend policy. Applied on the capture thread before the next
   // frame; a mode whose backend is unavailable is logged and capture stays
   // on the current backend.
   void set_capture_mode (capture::CaptureMode mode);

   void set_language (LanguageFamily f);

   // True only while `generation` still belongs to the current tooltip.
   bool is_current (std::uint64_t generation) const noexcept;

   // Stop after detection: boxes still flow to on_debug_boxes / on_activity,
   // but nothing is dispatched to OCR (no recognition, no lookup, no
   // overlay). Dev aid for tuning the detector in isolation.
   void set_detect_only (bool on);

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
