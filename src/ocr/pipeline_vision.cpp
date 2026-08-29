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

namespace {

std::string_view relation_name (TooltipRelation relation)
{
   switch (relation) {
      case TooltipRelation::Same:
         return "same";
      case TooltipRelation::Ambiguous:
         return "ambiguous";
      case TooltipRelation::Different:
         return "different";
      default:
         return "missing";
   }
}

std::string_view presence_name (vision::TooltipPresence presence)
{
   switch (presence) {
      case vision::TooltipPresence::Present:
         return "present";
      case vision::TooltipPresence::Changed:
         return "changed";
      case vision::TooltipPresence::Absent:
         return "absent";
      default:
         return "uncertain";
   }
}

struct VisionHealth {
   std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now ();
   std::uint64_t frames = 0;
   std::uint64_t scans = 0;
   std::uint64_t boxes = 0;
   std::uint64_t empty = 0;
   std::uint64_t errors = 0;
   std::uint64_t identity_failures = 0;
   std::uint64_t tracked = 0;
   std::uint64_t accepted = 0;
   std::uint64_t cursor_valid = 0;
   long long detect_ms_total = 0;
   long long detect_ms_max = 0;
   capture::CaptureBackend backend = capture::CaptureBackend::Unknown;
   std::string latest_error;

   void record (const capture::Frame& frame)
   {
      ++frames;
      cursor_valid += frame.cursor.valid ? 1 : 0;
      backend = frame.backend;
   }

   void record_detection (long long detect_ms)
   {
      ++scans;
      detect_ms_total += detect_ms;
      detect_ms_max = std::max (detect_ms_max, detect_ms);
   }

   void report_if_due (bool active)
   {
      const auto now = std::chrono::steady_clock::now ();
      if (now - started < std::chrono::seconds { 10 }) return;
      core::log::vision.event (
         "pipeline_health",
         {
            { "active", active ? "1" : "0" },
            { "frames", std::to_string (frames) },
            { "scans", std::to_string (scans) },
            { "boxes", std::to_string (boxes) },
            { "empty", std::to_string (empty) },
            { "errors", std::to_string (errors) },
            { "identity_failures", std::to_string (identity_failures) },
            { "tracked", std::to_string (tracked) },
            { "accepted", std::to_string (accepted) },
            { "cursor_valid", std::to_string (cursor_valid) },
            { "backend", std::string { capture::backend_name (backend) } },
            { "detect_ms_avg", std::to_string (scans ? detect_ms_total / scans : 0) },
            { "detect_ms_max", std::to_string (detect_ms_max) },
            { "latest_error", latest_error.empty () ? "none" : latest_error },
         });
      *this = VisionHealth {};
   }
};

}

void Pipeline::Impl::vision_loop ()
{
   TooltipState state { {
      .stable_frames = config.stability_frames,
      .missing_frames = config.missing_frames,
      .identity_bits = config.identity_bits,
      .position_px = config.identity_position_px,
      .size_ratio = config.identity_size_ratio,
   } };
   vision::Anchor anchor;
   std::uint64_t anchor_generation = 0;
   auto last_detection = std::chrono::steady_clock::now () - detection_interval ();
   VisionHealth health;

   const auto emit_anchor = [this, &anchor_generation] (const vision::Anchor& value) {
      if (!anchor_cb) return;
      anchor_cb (AnchorEvent {
         .generation = anchor_generation,
         .offset_x = value.offset_x,
         .offset_y = value.offset_y,
         .locked_x = value.locked_x,
         .locked_y = value.locked_y,
         .pinned_x = value.axis_x != vision::AxisPin::Free,
         .pinned_y = value.axis_y != vision::AxisPin::Free,
         .pin_x = value.pin_x,
         .pin_y = value.pin_y,
         .w = value.w,
         .h = value.h,
      });
   };

   const auto nearest_to_cursor =
      [] (const std::vector<vision::TooltipBox>& boxes,
          const capture::CursorPos& cursor) -> const vision::TooltipBox* {
      const auto* selected = &boxes.front ();
      if (!cursor.valid || boxes.size () == 1) return selected;
      long best = -1;
      for (const auto& box : boxes) {
         const long dx = cursor.x < box.rect.x                ? box.rect.x - cursor.x
                         : cursor.x > box.rect.x + box.rect.w ? cursor.x - box.rect.x - box.rect.w
                                                              : 0;
         const long dy = cursor.y < box.rect.y                ? box.rect.y - cursor.y
                         : cursor.y > box.rect.y + box.rect.h ? cursor.y - box.rect.y - box.rect.h
                                                              : 0;
         const long distance = dx * dx + dy * dy;
         if (best < 0 || distance < best) {
            best = distance;
            selected = &box;
         }
      }
      return selected;
   };

   const auto lose = [&] (std::string reason) {
      if (!state.active ()) return;
      const auto lost_generation = anchor_generation;
      state.reset ();
      anchor = {};
      tracking.store (false, std::memory_order_relaxed);
      generation.fetch_add (1, std::memory_order_relaxed);
      anchor_generation = 0;
      if (anchor_lost_cb) anchor_lost_cb (true);
      core::log::vision.event ("tooltip_lost",
                               {
                                  { "generation", std::to_string (lost_generation) },
                                  { "reason", reason },
                               });
      evidence.event (lost_generation, "lost",
                      {
                         { "reason", reason },
                      });
   };

   while (running.load (std::memory_order_relaxed)) {
      std::optional<capture::Frame> next;
      {
         std::lock_guard lock { capture_lock };
         next = std::move (pending_frame);
         pending_frame.reset ();
      }
      if (!next.has_value ()) {
         std::this_thread::sleep_for (std::chrono::milliseconds (5));
         continue;
      }

      capture::Frame frame = std::move (*next);
      frame.cursor = frame.local_cursor ();
      health.report_if_due (state.active ());
      health.record (frame);

      if (reset_requested.exchange (false, std::memory_order_relaxed)) {
         lose ("runtime_policy");
         state.reset ();
         if (!enabled.load (std::memory_order_relaxed)) continue;
      }

      int pending = force_scans.load (std::memory_order_relaxed);
      while (pending > 0 &&
             !force_scans.compare_exchange_weak (pending, pending - 1, std::memory_order_relaxed)) {
      }
      const bool forced = pending > 0;

      cv::Mat image {
         frame.height,
         frame.width,
         CV_8UC4,
         frame.data.get (),
         static_cast<std::size_t> (frame.stride),
      };

      const auto now = std::chrono::steady_clock::now ();
      bool detection_due =
         forced || !state.active () || now - last_detection >= detection_interval ();
      bool content_changed = false;
      bool sensitive = false;

      if (state.active () && !forced && !anchor.fingerprint.empty ()) {
         const int pred_x = anchor.axis_x != vision::AxisPin::Free ? anchor.pin_x
                            : frame.cursor.valid ? frame.cursor.x + anchor.offset_x
                                                 : anchor.pin_x;
         const int pred_y = anchor.axis_y != vision::AxisPin::Free ? anchor.pin_y
                            : frame.cursor.valid ? frame.cursor.y + anchor.offset_y
                                                 : anchor.pin_y;
         sensitive = anchor.identity_cursor.valid && frame.cursor.valid &&
                     std::max (std::abs (frame.cursor.x - anchor.identity_cursor.x),
                               std::abs (frame.cursor.y - anchor.identity_cursor.y)) >=
                        config.identity_position_px;
         const auto tracked =
            vision::TooltipTracker::track (image, anchor, pred_x, pred_y, sensitive);

         if (tracked.presence == vision::TooltipPresence::Present) {
            ++health.tracked;
            state.confirm ();
            anchor.update (tracked.box, frame.cursor, frame.width, frame.height,
                           config.pin_near_edge_px, config.pin_right_edge_px);
            emit_anchor (anchor);
            continue;
         } else {
            content_changed = tracked.presence == vision::TooltipPresence::Changed;
            core::log::vision.event (
               "tooltip_tracking",
               {
                  { "generation", std::to_string (anchor_generation) },
                  { "presence", std::string (presence_name (tracked.presence)) },
                  { "frame_confidence", fmt::format ("{:.3f}", tracked.frame_confidence) },
                  { "content_confidence", fmt::format ("{:.3f}", tracked.content_confidence) },
               });
            detection_due = true;
         }
      }

      if (!detection_due) continue;

      const auto started = std::chrono::steady_clock::now ();
      auto detected = detector.detect (frame);
      last_detection = now;
      const auto detect_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                std::chrono::steady_clock::now () - started)
                                .count ();
      last_detect_ms.store (detect_ms);
      health.record_detection (detect_ms);

      std::optional<capture::Rect> selected;
      std::optional<capture::Rect> detector_box;
      std::optional<TooltipObservation> observation;
      bool refined = false;
      if (!detected.has_value ()) {
         ++health.errors;
         health.latest_error = detected.error ().message;
      } else if (detected->empty ()) {
         ++health.empty;
      } else {
         health.boxes += detected->size ();
      }
      if (detected.has_value () && !detected->empty ()) {
         const auto* box = nearest_to_cursor (*detected, frame.cursor);
         detector_box = box->rect;
         const auto selection = vision::TooltipTracker::select (image, box->rect);
         selected = selection.rect;
         refined = selection.refined;
         observation = TooltipObservation::read (image, *selected, frame.cursor);
         if (!observation.has_value ()) ++health.identity_failures;
      }

      if (!forced && state.active () && selected.has_value () && !anchor.fingerprint.empty ()) {
         const auto recovered =
            vision::TooltipTracker::rebase (image, anchor, *selected, sensitive);
         content_changed =
            content_changed || recovered.presence == vision::TooltipPresence::Changed;
         if (!content_changed && recovered.presence == vision::TooltipPresence::Present) {
            state.confirm ();
            anchor.update (*selected, frame.cursor, frame.width, frame.height,
                           config.pin_near_edge_px, config.pin_right_edge_px);
            core::log::vision.event (
               "tooltip_recovered",
               {
                  { "generation", std::to_string (anchor_generation) },
                  { "frame_confidence", fmt::format ("{:.3f}", recovered.frame_confidence) },
                  { "content_confidence", fmt::format ("{:.3f}", recovered.content_confidence) },
               });
            emit_anchor (anchor);
            continue;
         }
      }

      const bool was_active = state.active ();
      const auto previous_generation = anchor_generation;
      const auto update = state.observe (observation, forced, content_changed);
      const auto transition = update.transition;

      if (!observation.has_value () || transition == TooltipTransition::Candidate) {
         static const std::vector<vision::TooltipBox> empty;
         std::string reason;
         if (!detected.has_value ()) reason = "detector_error: " + detected.error ().message;
         else if (detected->empty ()) reason = "no_detection";
         else if (!observation.has_value ()) reason = "identity_failed";
         else reason = "candidate_" + std::string (relation_name (update.relation));
         evidence.observe (frame, image, detected.has_value () ? *detected : empty, reason);
      }

      if (transition == TooltipTransition::Lost) {
         static const std::vector<vision::TooltipBox> empty;
         anchor = {};
         tracking.store (false, std::memory_order_relaxed);
         generation.fetch_add (1, std::memory_order_relaxed);
         anchor_generation = 0;
         if (anchor_lost_cb) anchor_lost_cb (true);
         evidence.snapshot (previous_generation, "lost", image,
                            detected.has_value () ? *detected : empty);
         core::log::vision.event ("tooltip_lost",
                                  {
                                     { "generation", std::to_string (previous_generation) },
                                     { "reason", "detector_misses" },
                                  });
         evidence.event (previous_generation, "lost",
                         {
                            { "reason", "detector_misses" },
                         });
         continue;
      }

      if (!observation.has_value () || !selected.has_value ()) continue;

      if (transition == TooltipTransition::Same) {
         anchor.update (*selected, frame.cursor, frame.width, frame.height, config.pin_near_edge_px,
                        config.pin_right_edge_px);
         emit_anchor (anchor);
         continue;
      }

      if (transition == TooltipTransition::Candidate) {
         if (state.active ()) emit_anchor (anchor);
         continue;
      }
      if (transition != TooltipTransition::Acquired && transition != TooltipTransition::Replaced)
         continue;

      if (was_active) {
         if (anchor_lost_cb) anchor_lost_cb (false);
         evidence.event (previous_generation, "replaced",
                         {
                            { "identity_distance", std::to_string (update.identity_distance) },
                            { "size_changed", update.size_changed ? "1" : "0" },
                            { "position_unexplained", update.position_unexplained ? "1" : "0" },
                         });
      }

      anchor_generation = generation.fetch_add (1, std::memory_order_relaxed) + 1;
      ++health.accepted;
      tracking.store (true, std::memory_order_relaxed);
      anchor.acquire (*selected, frame.cursor, frame.width, frame.height, config.pin_near_edge_px,
                      config.pin_right_edge_px);
      vision::TooltipTracker::remember (image, *selected, anchor);
      force_scans.store (0, std::memory_order_relaxed);

      core::log::vision.event (
         "tooltip_accepted",
         {
            { "generation", std::to_string (anchor_generation) },
            { "transition", transition == TooltipTransition::Replaced ? "replaced" : "acquired" },
            { "identity", std::to_string (observation->identity.key ()) },
            { "relation", std::string (relation_name (update.relation)) },
            { "identity_distance", std::to_string (update.identity_distance) },
            { "size_changed", update.size_changed ? "1" : "0" },
            { "position_unexplained", update.position_unexplained ? "1" : "0" },
            { "x", std::to_string (selected->x) },
            { "y", std::to_string (selected->y) },
            { "w", std::to_string (selected->w) },
            { "h", std::to_string (selected->h) },
            { "detector_x", std::to_string (detector_box->x) },
            { "detector_y", std::to_string (detector_box->y) },
            { "detector_w", std::to_string (detector_box->w) },
            { "detector_h", std::to_string (detector_box->h) },
            { "detections", std::to_string (detected->size ()) },
            { "refined", refined ? "1" : "0" },
            { "detect_ms", std::to_string (last_detect_ms.load ()) },
         });

      emit_anchor (anchor);

      cv::Rect crop_rect {
         selected->x,
         selected->y,
         selected->w,
         selected->h,
      };
      crop_rect &= cv::Rect { 0, 0, image.cols, image.rows };
      if (crop_rect.area () <= 0) continue;
      evidence.begin (anchor_generation, frame, image, *detected, *selected, image (crop_rect),
                      observation->identity.image (), observation->identity.key ());

      if (detect_only.load (std::memory_order_relaxed)) continue;

      VisionOut output {
         std::move (frame),
         { vision::TooltipBox { .rect = *selected } },
         anchor_generation,
         *observation,
         forced,
      };
      {
         std::lock_guard lock { vision_lock };
         pending_vision = std::move (output);
      }
   }
}

}
