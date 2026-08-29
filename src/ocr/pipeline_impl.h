#pragma once

#include <gv/ocr/evidence.h>
#include <gv/ocr/pipeline.h>
#include <gv/ocr/tooltip_state.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gv::ocr {

struct Pipeline::Impl {
   Impl (capture::CaptureService& capture, vision::TooltipDetector& detector,
         LanguageRegistry& registry, Config config);

   capture::CaptureService& capture;
   vision::TooltipDetector& detector;
   LanguageRegistry& registry;
   Config config;
   Evidence evidence;
   std::atomic<double> capture_fps;
   std::atomic<capture::CaptureMode> capture_mode { capture::CaptureMode::Automatic };
   std::atomic<bool> running { false };
   std::atomic<bool> enabled { true };
   std::atomic<bool> automatic { true };
   std::atomic<bool> performance_mode { false };
   std::atomic<bool> detect_only { false };
   std::atomic<bool> tracking { false };
   std::atomic<bool> reset_requested { false };
   std::atomic<std::uint64_t> generation { 0 };
   std::atomic<void*> window { nullptr };
   std::mutex language_lock;
   LanguageFamily language;
   std::string locale { "en" };
   std::thread capture_thread;
   std::thread vision_thread;
   std::thread ocr_thread;
   std::mutex capture_lock;
   std::optional<capture::Frame> pending_frame;

   struct VisionOut {
      capture::Frame frame;
      std::vector<vision::TooltipBox> boxes;
      std::uint64_t generation = 0;
      TooltipObservation observation;
      bool refresh = false;
   };

   std::mutex vision_lock;
   std::optional<VisionOut> pending_vision;
   TooltipCallback callback;
   AnchorCallback anchor_cb;
   AnchorLostCallback anchor_lost_cb;
   SampleCallback sample_cb;
   std::atomic<long long> last_detect_ms { 0 };
   std::atomic<int> force_scans { 0 };

   void queue_scans (int requested);
   std::chrono::milliseconds current_interval () const;
   std::chrono::milliseconds detection_interval () const;
   void capture_loop ();
   void vision_loop ();
   void ocr_loop ();
};

}
