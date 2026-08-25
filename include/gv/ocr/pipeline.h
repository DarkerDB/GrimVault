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
#include <opencv2/core/mat.hpp>
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

struct TooltipSample {
   std::uint64_t generation = 0;
   capture::Rect rect;
   cv::Mat image;
   std::string locale;
   std::string text;
   float confidence = 0.0f;
   capture::CaptureBackend backend = capture::CaptureBackend::Unknown;
};

class Pipeline
{
public:
   struct Config {
      double                      capture_fps        = 15.0;
      double                      performance_fps    = 3.0;
      double                      tracking_fps       = 60.0;
      double                      performance_tracking_fps = 30.0;
      int                         stability_frames   = 2;
      int                         missing_frames     = 2;
      int                         pin_near_edge_px   = 48;
      int                         pin_right_edge_px  = 32;
      int                         identity_bits      = 16;
      int                         identity_size_px   = 8;
      int                         identity_position_px = 24;
      double                      identity_size_ratio = 1.2;
      LanguageFamily              language           = LanguageFamily::Latin;
      std::filesystem::path       evidence_dir;
      std::uintmax_t              evidence_max_bytes = 250ull * 1024ull * 1024ull;
      std::filesystem::path       collector_dir;
   };

   // Once a tooltip settles,
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
   using AnchorCallback     = std::function<void (const AnchorEvent&)>;
   using SampleCallback     = std::function<void (TooltipSample)>;

   // immediate = vision confirmed disappearance, so
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

   // Anchor established / updated (offsets lock as the tooltip unpins) and
   // anchor lost, fired from the vision thread. Set before start().
   void on_anchor      (AnchorCallback cb);
   void on_anchor_lost (AnchorLostCallback cb);
   void on_sample      (SampleCallback cb);

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

   void set_performance_mode (bool on);

   void set_language (std::string locale);

   // True only while `generation` still belongs to the current tooltip.
   bool is_current (std::uint64_t generation) const noexcept;

   void set_detect_only (bool on);

   void request_immediate_scan ();
   void record_evidence (
      std::uint64_t generation,
      std::string event,
      std::unordered_map<std::string, std::string> fields = {});

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ocr
