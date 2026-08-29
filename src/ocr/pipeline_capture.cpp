#include <gv/core/environment.h>
#include <gv/core/logger.h>
#include <gv/ocr/capture_policy.h>
#include <gv/ocr/evidence.h>
#include <gv/ocr/preprocessor.h>
#include <gv/ocr/tooltip_state.h>
#include <gv/vision/gem_detector.h>
#include <gv/vision/tooltip_tracker.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <thread>

#include "pipeline_impl.h"

namespace gv::ocr {

void Pipeline::Impl::capture_loop ()
{
   void* current_target = nullptr;
   bool session_active = false;
   int continuous_errors = 0;
   auto continuous_backoff = continuous_backoff_min;
   auto continuous_retry_at = std::chrono::steady_clock::now ();
   auto applied_mode = capture.mode ();
   int dropped_frames = 0;
   auto last_drop_report = std::chrono::steady_clock::now ();

   const auto rearm_continuous = [&] {
      continuous_errors = 0;
      continuous_backoff = continuous_backoff_min;
      continuous_retry_at = std::chrono::steady_clock::now ();
   };

   const auto degrade_continuous = [&] (const core::Error& cause) {
      if (session_active) {
         capture.stop_continuous ();
         session_active = false;
      }
      if (capture.demote (cause)) {
         rearm_continuous ();
         return;
      }
      continuous_errors = 0;
      continuous_retry_at = std::chrono::steady_clock::now () + continuous_backoff;
      continuous_backoff = next_continuous_backoff (continuous_backoff);
   };

   while (running.load (std::memory_order_relaxed)) {
      const auto want_mode = capture_mode.load (std::memory_order_relaxed);
      if (want_mode != applied_mode) {
         if (session_active) {
            capture.stop_continuous ();
            session_active = false;
         }
         if (auto r = capture.set_mode (want_mode); !r.has_value ()) {
            core::Logger::warn ("pipeline: capture mode {} rejected: {}",
                                capture::capture_mode_name (want_mode), r.error ().message);
         }
         applied_mode = want_mode;
         current_target = nullptr;
         rearm_continuous ();
      }

      void* now_target = window.load ();
      const bool forced = force_scans.load (std::memory_order_relaxed) > 0;
      const bool auto_scan = automatic.load (std::memory_order_relaxed);

      if (!capture_active (enabled.load (std::memory_order_relaxed), auto_scan,
                           tracking.load (std::memory_order_relaxed), forced)) {
         capture.stop_continuous ();
         session_active = false;
         std::this_thread::sleep_for (std::chrono::milliseconds (100));
         continue;
      }

      if (!capture_targeted (now_target != nullptr, forced)) {
         capture.stop_continuous ();
         session_active = false;
         std::this_thread::sleep_for (std::chrono::milliseconds (250));
         continue;
      }

      const bool target_changed = now_target != current_target;
      if (target_changed) rearm_continuous ();

      const bool retry_due = std::chrono::steady_clock::now () >= continuous_retry_at;
      if (capture.supports_continuous () && retry_due && (!session_active || target_changed)) {
         if (session_active) capture.stop_continuous ();
         auto r = capture.start_continuous (now_target, now_target != nullptr);
         session_active = r.has_value ();
         if (session_active) {
            continuous_errors = 0;
         } else {
            core::Logger::warn ("pipeline: continuous start failed: {}", r.error ().message);
            degrade_continuous (r.error ());
         }
      }
      current_target = now_target;

      core::Result<capture::Frame> frame_res =
         core::fail (core::Error { core::ErrorKind::Capture, "init" });

      if (session_active) {
         frame_res = capture.latest_frame (std::chrono::milliseconds (200));
         if (frame_res.has_value ()) {
            continuous_errors = 0;
         } else if (++continuous_errors >= continuous_error_limit) {
            core::Logger::warn ("pipeline: continuous capture failed repeatedly: {}",
                                frame_res.error ().message);
            degrade_continuous (frame_res.error ());
         }
      } else {
         frame_res =
            now_target ? capture.capture_window (now_target) : capture.capture_monitor (nullptr);
      }

      if (frame_res.has_value () && !frame_res->empty ()) {
         std::lock_guard lock { capture_lock };
         if (pending_frame.has_value ()) ++dropped_frames;
         pending_frame = std::move (*frame_res);
      }

      if (const auto now = std::chrono::steady_clock::now ();
          now - last_drop_report > std::chrono::seconds { 10 }) {
         if (dropped_frames > 0) {
            core::log::vision.event ("frame_drops",
                                     {
                                        { "dropped", std::to_string (dropped_frames) },
                                        { "window_s", "10" },
                                     });
            dropped_frames = 0;
         }
         last_drop_report = now;
      }

      std::this_thread::sleep_for (current_interval ());
   }

   if (session_active) {
      capture.stop_continuous ();
   }
}

}
