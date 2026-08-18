#include <gv/ocr/pipeline.h>
#include <gv/ocr/capture_policy.h>
#include <gv/ocr/evidence.h>
#include <gv/ocr/preprocessor.h>
#include <gv/ocr/tooltip_state.h>
#include <gv/core/environment.h>
#include <gv/core/logger.h>
#include <gv/vision/gem_detector.h>
#include <gv/vision/tooltip_tracker.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace gv::ocr {

namespace {

void replace_all (std::string& text, std::string_view from, std::string_view to)
{
   for (std::size_t pos = 0; (pos = text.find (from, pos)) != std::string::npos;) {
      text.replace (pos, from.size (), to);
      pos += to.size ();
   }
}

// The game's serif face draws dotted Latin glyphs with an accent-like stroke,
// and the multilingual Paddle model often chooses the accented codepoint with
// high confidence. Item lookup wants semantic ASCII, not a faithful encoding
// of that font quirk. Keep this intentionally conservative and Latin-only.
void canonicalize_latin (std::string& text)
{
   constexpr std::pair<std::string_view, std::string_view> replacements[] = {
      { "á", "a" }, { "à", "a" }, { "â", "a" }, { "ä", "a" },
      { "é", "e" }, { "è", "e" }, { "ê", "e" }, { "ë", "e" },
      { "í", "i" }, { "ì", "i" }, { "î", "i" }, { "ï", "i" },
      { "ó", "o" }, { "ò", "o" }, { "ô", "o" }, { "ö", "o" },
      { "ú", "u" }, { "ù", "u" }, { "û", "u" }, { "ü", "u" },
      { "−", "-" }, { "–", "-" }, { "’", "'" },
   };
   for (const auto& [from, to] : replacements) replace_all (text, from, to);
}

std::string_view relation_name (TooltipRelation relation)
{
   switch (relation) {
      case TooltipRelation::Same: return "same";
      case TooltipRelation::Ambiguous: return "ambiguous";
      case TooltipRelation::Different: return "different";
      default: return "missing";
   }
}

std::string_view presence_name (vision::TooltipPresence presence)
{
   switch (presence) {
      case vision::TooltipPresence::Present: return "present";
      case vision::TooltipPresence::Changed: return "changed";
      case vision::TooltipPresence::Absent: return "absent";
      default: return "uncertain";
   }
}

} // namespace

struct Pipeline::Impl
{
   Impl (capture::CaptureService& c, vision::TooltipDetector& d, LanguageRegistry& r, Config cfg)
      : capture (c), detector (d), registry (r), config (std::move (cfg)),
        evidence (config.evidence_dir, config.evidence_max_bytes),
        capture_fps (std::clamp (config.capture_fps, 1.0, 60.0)),
        capture_mode (c.mode ())
   {}

   capture::CaptureService&  capture;
   vision::TooltipDetector&  detector;
   LanguageRegistry&         registry;
   Config                    config;
   Evidence                  evidence;
   std::atomic<double>       capture_fps;
   std::atomic<capture::CaptureMode> capture_mode { capture::CaptureMode::Automatic };

   std::atomic<bool>         running   { false };
   std::atomic<bool>         enabled   { true };
   std::atomic<bool>         automatic { true };
   std::atomic<bool>         performance_mode { false };
   std::atomic<bool>         detect_only { false };
   std::atomic<bool>         tracking { false };
   std::atomic<bool>         reset_requested { false };
   std::atomic<std::uint64_t> generation { 0 };
   std::atomic<void*>        window    { nullptr };
   std::atomic<LanguageFamily> language { LanguageFamily::Latin };

   std::thread               capture_thread;
   std::thread               vision_thread;
   std::thread               ocr_thread;

   std::mutex capture_lock;
   std::optional<capture::Frame> pending_frame;
   struct VisionOut {
      capture::Frame                     frame;
      std::vector<vision::TooltipBox>    boxes;
      std::uint64_t                      generation = 0;
      TooltipObservation                 observation;
      bool                               refresh = false;
   };
   std::mutex vision_lock;
   std::optional<VisionOut> pending_vision;

   TooltipCallback           callback;
   AnchorCallback            anchor_cb;
   AnchorLostCallback        anchor_lost_cb;

   std::atomic<long long>    last_detect_ms { 0 };

   std::atomic<int>          force_scans { 0 };

   void queue_scans (int requested)
   {
      int pending = force_scans.load (std::memory_order_relaxed);
      while (pending < requested && !force_scans.compare_exchange_weak (
         pending, requested, std::memory_order_relaxed)) {}
   }

   std::chrono::milliseconds current_interval () const
   {
      const bool active = tracking.load (std::memory_order_relaxed);
      const double fps = std::max (1.0, frame_fps (
         capture_fps.load (std::memory_order_relaxed),
         config.performance_fps,
         config.tracking_fps,
         config.performance_tracking_fps,
         performance_mode.load (std::memory_order_relaxed),
         active));

      return std::chrono::duration_cast<std::chrono::milliseconds> (
         std::chrono::duration<double> (1.0 / fps));
   }

   std::chrono::milliseconds detection_interval () const
   {
      const double fps = std::max (1.0, detector_fps (
         capture_fps.load (std::memory_order_relaxed),
         config.performance_fps,
         performance_mode.load (std::memory_order_relaxed)));
      return std::chrono::duration_cast<std::chrono::milliseconds> (
         std::chrono::duration<double> (1.0 / fps));
   }

   void capture_loop ()
   {
      void* current_target  = nullptr;
      bool  session_active  = false;
      bool  continuous_ok   = true;   // flips false after a failed (re)start
      int   continuous_errors = 0;
      auto  applied_mode    = capture.mode ();
      int   dropped_frames  = 0;
      auto  last_drop_report = std::chrono::steady_clock::now ();

      while (running.load (std::memory_order_relaxed)) {
         // The service is owned by this thread once the loop runs, so mode
         // changes from the settings bridge land here, between frames. A
         // rejected mode is not retried — the service kept its previous
         // strategy and the next settings change re-arms the check.
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
            applied_mode      = want_mode;
            current_target    = nullptr;
            continuous_ok     = true;
            continuous_errors = 0;
         }

         void* now_target = window.load ();
         const bool forced = force_scans.load (std::memory_order_relaxed) > 0;
         const bool auto_scan = automatic.load (std::memory_order_relaxed);

         if (!capture_active (
               enabled.load (std::memory_order_relaxed),
               auto_scan,
               tracking.load (std::memory_order_relaxed),
               forced)) {
            if (session_active) {
               capture.stop_continuous ();
               session_active = false;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
            continue;
         }

         // No game window: the pipeline sleeps outright instead of running
         // detection against monitor frames — that's pure CPU burn with
         // nothing to find. A forced scan (F5) still grabs one monitor
         // frame so desktop testing works without the game.
         if (!capture_targeted (now_target != nullptr, forced)) {
            if (session_active) {
               capture.stop_continuous ();
               session_active = false;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (250));
            continue;
         }

         // (Re)start the continuous session when the target changed or the
         // session was torn down during a no-window pause.
         const bool target_changed = now_target != current_target;
         if (target_changed) {
            continuous_ok = true;
            continuous_errors = 0;
         }
         const bool can_continuous = capture.supports_continuous ();
         if (can_continuous && continuous_ok
               && (!session_active || target_changed)) {
            if (session_active) capture.stop_continuous ();
            auto r = capture.start_continuous (
               now_target, /*is_window=*/ now_target != nullptr);
            session_active = r.has_value ();
            if (!session_active) {
               core::Logger::warn ("pipeline: continuous start failed: {}; falling back to per-call",
                  r.error ().message);
               continuous_ok = false;
            } else {
               continuous_errors = 0;
            }
         }
         current_target = now_target;

         core::Result<capture::Frame> frame_res = core::fail (core::Error {
            core::ErrorKind::Capture, "init" });

         if (session_active) {
            frame_res = capture.latest_frame (std::chrono::milliseconds (200));
            if (!frame_res.has_value ()) {
               ++continuous_errors;
               if (continuous_errors >= 3) {
                  core::Logger::warn (
                     "pipeline: continuous capture failed repeatedly; falling back to per-call");
                  capture.stop_continuous ();
                  session_active = false;
                  continuous_ok = false;
               }
            } else {
               continuous_errors = 0;
            }
         } else {
            frame_res = now_target
               ? capture.capture_window  (now_target)
               : capture.capture_monitor (nullptr);
         }

         if (frame_res.has_value () && !frame_res->empty ()) {
            std::lock_guard lock { capture_lock };
            if (pending_frame.has_value ()) ++dropped_frames;
            pending_frame = std::move (*frame_res);
         }

         if (const auto now = std::chrono::steady_clock::now ();
             now - last_drop_report > std::chrono::seconds { 10 }) {
            if (dropped_frames > 0) {
               core::log::vision.event ("frame_drops", {
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

   void vision_loop ()
   {
      TooltipState state {{
         .stable_frames = config.stability_frames,
         .missing_frames = config.missing_frames,
         .identity_bits = config.identity_bits,
         .position_px = config.identity_position_px,
         .size_ratio = config.identity_size_ratio,
      }};
      vision::Anchor anchor;
      std::uint64_t anchor_generation = 0;
      int uncertain_frames = 0;
      auto last_detection = std::chrono::steady_clock::now () - detection_interval ();

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

      const auto nearest_to_cursor = [] (
         const std::vector<vision::TooltipBox>& boxes,
         const capture::CursorPos& cursor) -> const vision::TooltipBox* {
         const auto* selected = &boxes.front ();
         if (!cursor.valid || boxes.size () == 1) return selected;
         long best = -1;
         for (const auto& box : boxes) {
            const long dx = cursor.x < box.rect.x
               ? box.rect.x - cursor.x
               : cursor.x > box.rect.x + box.rect.w
                  ? cursor.x - box.rect.x - box.rect.w
                  : 0;
            const long dy = cursor.y < box.rect.y
               ? box.rect.y - cursor.y
               : cursor.y > box.rect.y + box.rect.h
                  ? cursor.y - box.rect.y - box.rect.h
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
         uncertain_frames = 0;
         tracking.store (false, std::memory_order_relaxed);
         generation.fetch_add (1, std::memory_order_relaxed);
         anchor_generation = 0;
         if (anchor_lost_cb) anchor_lost_cb (true);
         core::log::vision.event ("tooltip_lost", {
            { "generation", std::to_string (lost_generation) },
            { "reason", reason },
         });
         evidence.event (lost_generation, "lost", {
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

         if (reset_requested.exchange (false, std::memory_order_relaxed)) {
            lose ("runtime_policy");
            state.reset ();
            if (!enabled.load (std::memory_order_relaxed)) continue;
         }

         int pending = force_scans.load (std::memory_order_relaxed);
         while (pending > 0 && !force_scans.compare_exchange_weak (
            pending, pending - 1, std::memory_order_relaxed)) {}
         const bool forced = pending > 0;

         cv::Mat image {
            frame.height,
            frame.width,
            CV_8UC4,
            frame.data.get (),
            static_cast<std::size_t> (frame.stride),
         };

         const auto now = std::chrono::steady_clock::now ();
         bool detection_due = forced || !state.active ()
            || now - last_detection >= detection_interval ();
         bool tracked_present = false;

         if (state.active () && !forced && !anchor.fingerprint.empty ()) {
            const int pred_x = anchor.axis_x != vision::AxisPin::Free
               ? anchor.pin_x
               : frame.cursor.valid ? frame.cursor.x + anchor.offset_x : anchor.pin_x;
            const int pred_y = anchor.axis_y != vision::AxisPin::Free
               ? anchor.pin_y
               : frame.cursor.valid ? frame.cursor.y + anchor.offset_y : anchor.pin_y;
            const auto tracked = vision::TooltipTracker::track (
               image, anchor, pred_x, pred_y);

            if (tracked.presence == vision::TooltipPresence::Present) {
               uncertain_frames = 0;
               tracked_present = true;
               anchor.update (
                  tracked.box, frame.cursor, frame.width, frame.height,
                  config.pin_near_edge_px, config.pin_right_edge_px);
               if (!detection_due) {
                  emit_anchor (anchor);
                  continue;
               }
            } else {
               core::log::vision.event ("tooltip_tracking", {
                  { "generation", std::to_string (anchor_generation) },
                  { "presence", std::string (presence_name (tracked.presence)) },
                  { "frame_confidence", fmt::format ("{:.3f}", tracked.frame_confidence) },
                  { "tail_confidence", fmt::format ("{:.3f}", tracked.tail_confidence) },
                  { "hash_distance", std::to_string (tracked.hash_distance) },
                  { "detail_distance", std::to_string (tracked.detail_distance) },
               });

               const bool strong = tracked.presence != vision::TooltipPresence::Uncertain;
               uncertain_frames = strong ? 0 : uncertain_frames + 1;
               if (strong || uncertain_frames >= 2) {
                  static const std::vector<vision::TooltipBox> empty;
                  const auto lost_generation = anchor_generation;
                  const auto reason = "tracker_" + std::string (
                     presence_name (tracked.presence));
                  lose (reason);
                  evidence.snapshot (
                     lost_generation,
                     reason,
                     image,
                     empty);
               }
               detection_due = true;
            }
         }

         if (!detection_due) continue;

         const auto started = std::chrono::steady_clock::now ();
         auto detected = detector.detect (frame);
         last_detection = now;
         last_detect_ms.store (std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - started).count ());

         std::optional<capture::Rect> selected;
         std::optional<TooltipObservation> observation;
         bool refined = false;
         if (detected.has_value () && !detected->empty ()) {
            const auto* box = nearest_to_cursor (*detected, frame.cursor);
            observation = TooltipObservation::read (image, box->rect, frame.cursor);
            const auto selection = vision::TooltipTracker::select (image, box->rect);
            selected = selection.rect;
            refined = selection.refined;
         }

         if (tracked_present && !observation.has_value ()) {
            static const std::vector<vision::TooltipBox> empty;
            evidence.observe (
               frame,
               image,
               detected.has_value () ? *detected : empty,
               "detector_miss_while_tracked");
            emit_anchor (anchor);
            continue;
         }

         const bool was_active = state.active ();
         const auto previous_generation = anchor_generation;
         const auto update = state.observe (observation, forced);
         const auto transition = update.transition;

         if (!observation.has_value () || transition == TooltipTransition::Candidate) {
            static const std::vector<vision::TooltipBox> empty;
            std::string reason;
            if (!detected.has_value ())
               reason = "detector_error: " + detected.error ().message;
            else if (detected->empty ())
               reason = "no_detection";
            else if (!observation.has_value ())
               reason = "identity_failed";
            else
               reason = "candidate_" + std::string (relation_name (update.relation));
            evidence.observe (
               frame, image, detected.has_value () ? *detected : empty, reason);
         }

         if (transition == TooltipTransition::Lost) {
            static const std::vector<vision::TooltipBox> empty;
            anchor = {};
            uncertain_frames = 0;
            tracking.store (false, std::memory_order_relaxed);
            generation.fetch_add (1, std::memory_order_relaxed);
            anchor_generation = 0;
            if (anchor_lost_cb) anchor_lost_cb (true);
            evidence.snapshot (
               previous_generation,
               "lost",
               image,
               detected.has_value () ? *detected : empty);
            core::log::vision.event ("tooltip_lost", {
               { "generation", std::to_string (previous_generation) },
               { "reason", "detector_misses" },
            });
            evidence.event (previous_generation, "lost", {
               { "reason", "detector_misses" },
            });
            continue;
         }

         if (!observation.has_value () || !selected.has_value ()) continue;

         if (transition == TooltipTransition::Same) {
            anchor.update (
               *selected, frame.cursor, frame.width, frame.height,
               config.pin_near_edge_px, config.pin_right_edge_px);
            vision::TooltipTracker::remember (image, *selected, anchor);
            uncertain_frames = 0;
            emit_anchor (anchor);
            continue;
         }

         if (transition == TooltipTransition::Candidate) {
            if (tracked_present) emit_anchor (anchor);
            continue;
         }
         if (transition != TooltipTransition::Acquired
             && transition != TooltipTransition::Replaced) continue;

         if (was_active) {
            if (anchor_lost_cb) anchor_lost_cb (true);
            evidence.event (previous_generation, "replaced", {
               { "identity_distance", std::to_string (
                  update.identity_distance) },
               { "size_changed", update.size_changed ? "1" : "0" },
               { "position_unexplained", update.position_unexplained ? "1" : "0" },
            });
         }

         anchor_generation = generation.fetch_add (1, std::memory_order_relaxed) + 1;
         tracking.store (true, std::memory_order_relaxed);
         anchor.acquire (
            *selected, frame.cursor, frame.width, frame.height,
            config.pin_near_edge_px, config.pin_right_edge_px);
         uncertain_frames = 0;
         vision::TooltipTracker::remember (image, *selected, anchor);
         force_scans.store (0, std::memory_order_relaxed);

         core::log::vision.event ("tooltip_accepted", {
            { "generation", std::to_string (anchor_generation) },
            { "transition", transition == TooltipTransition::Replaced
               ? "replaced"
               : "acquired" },
            { "identity", std::to_string (observation->identity.key ()) },
            { "relation", std::string (relation_name (update.relation)) },
            { "identity_distance", std::to_string (update.identity_distance) },
            { "size_changed", update.size_changed ? "1" : "0" },
            { "position_unexplained", update.position_unexplained ? "1" : "0" },
            { "x", std::to_string (selected->x) },
            { "y", std::to_string (selected->y) },
            { "w", std::to_string (selected->w) },
            { "h", std::to_string (selected->h) },
            { "detector_x", std::to_string (observation->box.x) },
            { "detector_y", std::to_string (observation->box.y) },
            { "detector_w", std::to_string (observation->box.w) },
            { "detector_h", std::to_string (observation->box.h) },
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
         evidence.begin (
            anchor_generation,
            frame,
            image,
            *detected,
            *selected,
            image (crop_rect),
            observation->identity.image (),
            observation->identity.key (),
            refined);

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

   void ocr_loop ()
   {
      struct Cached {
         TooltipObservation observation;
         LanguageFamily family = LanguageFamily::English;
         std::string text;
         std::unordered_map<std::string, std::string> gems;
         float confidence = 0.0f;
      };
      std::vector<Cached> cache;
      std::uint64_t last_completed_generation = 0;
      while (running.load (std::memory_order_relaxed)) {
         std::optional<VisionOut> next;
         {
            std::lock_guard lock { vision_lock };
            next = std::move (pending_vision);
            pending_vision.reset ();
         }
         if (!next.has_value ()) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
         }

         VisionOut item = std::move (*next);

         if (item.generation != generation.load (std::memory_order_relaxed)
             || item.generation == last_completed_generation) continue;

         if (!item.frame.data || item.frame.empty ()) {
            core::Logger::warn ("pipeline: dropping frame with no pixel data");
            continue;
         }

         const auto family = language.load (std::memory_order_relaxed);
         const auto identity_key = item.observation.identity.key ();
         if (!item.refresh) {
            const auto found = std::find_if (cache.begin (), cache.end (), [&] (const Cached& value) {
               return value.family == family && value.observation.cacheable (
                  item.observation, config.identity_bits, config.identity_size_px);
            });
            if (found != cache.end ()) {
               evidence.event (item.generation, "ocr_cache_hit", {
                  { "identity", std::to_string (identity_key) },
               });
               core::log::ocr.event ("cache_hit", {
                  { "generation", std::to_string (item.generation) },
                  { "identity", std::to_string (identity_key) },
               });
               last_completed_generation = item.generation;
               if (callback) {
                  try {
                     callback (RecognizedTooltip {
                        .generation = item.generation,
                        .rect = item.boxes.front ().rect,
                        .text = found->text,
                        .gems = found->gems,
                        .confidence = found->confidence,
                        .backend = item.frame.backend,
                        .preliminary = false,
                        .captured_at = item.frame.timestamp,
                     });
                  } catch (const std::exception& error) {
                     core::Logger::error (
                        "pipeline: tooltip callback threw: {}", error.what ());
                  }
               }
               continue;
            }
         }

         evidence.event (item.generation, "ocr_started", {
            { "identity", std::to_string (identity_key) },
         });
         auto rec = registry.acquire (family);
         if (!rec.has_value ()) {
            core::Logger::error ("pipeline: language acquire failed: {}", rec.error ().message);
            continue;
         }

         cv::Mat bgra { item.frame.height, item.frame.width, CV_8UC4,
                        item.frame.data.get (),
                        static_cast<std::size_t> (item.frame.stride) };

         for (const auto& box : item.boxes) {
            cv::Rect cv_box { box.rect.x, box.rect.y, box.rect.w, box.rect.h };
            cv_box &= cv::Rect (0, 0, bgra.cols, bgra.rows);

            if (cv_box.area () <= 0) continue;

            cv::Mat crop = bgra (cv_box).clone ();

            // Cut the tooltip's painted border before segmenting — same
            // trim the old Tesseract pipeline applied.
            if (crop.cols > 24 && crop.rows > 24) {
               crop = crop (cv::Rect (6, 6, crop.cols - 12, crop.rows - 12));
            }

            const auto t0    = std::chrono::steady_clock::now ();
            const auto bands = preprocess::line_bands (crop);
            const auto segmented_at = std::chrono::steady_clock::now ();

            // GRIMVAULT_OCR_DEBUG=1 → dump crop + bands to %TEMP%\grimvault-ocr
            // for offline segmentation tuning.
            static const bool dump_bands = [] {
               const auto value = core::environment::get ("GRIMVAULT_OCR_DEBUG");
               return !value.empty () && value != "0";
            } ();

            int dump_index = -1;
            std::filesystem::path dump_dir;
            if (dump_bands) {
               static std::atomic<int> seq { 0 };
               dump_index = seq++;
               dump_dir = std::filesystem::temp_directory_path () / "grimvault-ocr";
               std::error_code ec;
               std::filesystem::create_directories (dump_dir, ec);
               cv::imwrite ((dump_dir / (std::to_string (dump_index) + "_crop.png")).string (), crop);
               int b = 0;
               for (const auto& band : bands) {
                  cv::imwrite ((dump_dir / (std::to_string (dump_index) + "_band" + std::to_string (b++) + ".png")).string (),
                     preprocess::trim_cols (crop (band, cv::Range::all ())));
               }
            }

            std::string text;
            std::unordered_map<std::string, std::string> gems;
            float       conf_sum = 0.0f;
            int         conf_n   = 0;
            bool        preliminary_sent = false;
            std::vector<EvidenceLine> evidence_lines;

            std::size_t band_index = 0;
            std::size_t source_band_index = 0;
            for (const auto& band : bands) {
               const auto source_index = source_band_index++;
               if (item.generation != generation.load (std::memory_order_relaxed)) break;
               // Rule classification must see the original tooltip-wide
               // band. Once tightly column-trimmed, an ordinary title can
               // occupy >50% of its row and masquerade as a separator.
               cv::Mat raw_line = crop (band, cv::Range::all ());
               const bool is_rule = preprocess::is_horizontal_rule (raw_line);
               // Artifact tooltips can expose the decorated panel's top edge
               // as a tiny standalone band before the actual title. In
               // contrast, a title merged with its lower separator is tall.
               // Skip only the thin standalone form without consuming the
               // title slot.
               if (band_index == 0 && is_rule && raw_line.rows <= 20) continue;
               const bool is_title = band_index == 0;
               // The title and its lower ornament can be merged into one
               // geometric band. Never reject the first band as a rule
               // before removing that ornament, or the first stat is
               // promoted to the title. Later bands are safe to classify at
               // tooltip width, before column trimming.
               if (!is_title && is_rule) continue;
               // A failed title recognition must not cause the first stat to
               // become the title on the next iteration. Band identity is
               // geometric, not conditional on OCR success.
               ++band_index;
               if (is_title) raw_line = preprocess::trim_title_rule (raw_line);
               cv::Mat line = preprocess::trim_cols (raw_line);
               if (dump_index >= 0) {
                  cv::imwrite ((dump_dir / (std::to_string (dump_index) + "_input"
                     + std::to_string (source_index) + ".png")).string (), line);
               }

               std::string line_text;
               float line_confidence = 0.0f;
               int line_confidence_n = 0;
               const std::vector<cv::Range> whole_line {
                  cv::Range { 0, line.cols }
               };
               if (family == LanguageFamily::English && is_title) {
                  // The font-trained model learns spaces and punctuation as
                  // CTC classes. Feed the complete title once; geometric word
                  // splitting created false boundaries inside serif names.
                  auto full = (*rec)->read (line, /*title=*/ true);
                  if (full.has_value ()) {
                     line_text = full->text;
                     line_confidence = full->confidence;
                     line_confidence_n = 1;
                     conf_sum += full->confidence;
                     ++conf_n;
                  }
               } else {
                  const auto chunks = family == LanguageFamily::English
                     ? whole_line : preprocess::col_chunks (line);
                  for (const auto& chunk : chunks) {
                     if (item.generation != generation.load (std::memory_order_relaxed)) break;
                     auto res = (*rec)->read (line (cv::Range::all (), chunk));
                     if (!res.has_value () || res->text.empty ()) continue;

                     if (!line_text.empty ()) line_text.push_back (' ');
                     line_text += res->text;
                     conf_sum  += res->confidence;
                     ++conf_n;
                     line_confidence += res->confidence;
                     ++line_confidence_n;
                  }
               }

               if (line_confidence_n > 1) line_confidence /= line_confidence_n;
               if (family == LanguageFamily::Latin)
                  canonicalize_latin (line_text);
               if (evidence.enabled ()) {
                  evidence_lines.push_back (EvidenceLine {
                     .image = line.clone (), .source_band = source_index,
                     .title = is_title, .prediction = line_text,
                     .confidence = line_confidence,
                  });
               }
               if (line_text.empty ()) continue;
               if (!is_title && std::any_of (
                     line_text.begin (), line_text.end (), [] (char ch) {
                        return std::isdigit (static_cast<unsigned char> (ch)) != 0;
                     })) {
                  if (auto gem_family = vision::detect_gem_family (raw_line); gem_family.has_value ()) {
                     gems [line_text] = *gem_family;
                  }
               }
               if (!text.empty ()) text.push_back ('\n');
               text += line_text;

               if (!preliminary_sent && is_title
                   && family == LanguageFamily::English
                   && line_text.size () >= 2 && line_confidence >= 0.65f
                   && item.generation == generation.load (std::memory_order_relaxed)
                   && callback) {
                  preliminary_sent = true;
                  core::log::ocr.event ("title_ready", {
                     { "generation", std::to_string (item.generation) },
                     { "confidence", fmt::format ("{:.3f}", line_confidence) },
                     { "text", line_text },
                  });
                  try {
                     callback (RecognizedTooltip {
                        .generation  = item.generation,
                        .rect        = box.rect,
                        .text        = line_text,
                        .confidence  = line_confidence,
                        .backend     = item.frame.backend,
                        .preliminary = true,
                        .captured_at = item.frame.timestamp,
                     });
                  } catch (const std::exception& e) {
                     core::Logger::error ("pipeline: preliminary callback threw: {}", e.what ());
                  }
               }
            }

            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
               std::chrono::steady_clock::now () - t0).count ();
            const auto segment_us = std::chrono::duration_cast<std::chrono::microseconds> (
               segmented_at - t0).count ();

            core::Logger::info ("pipeline: timings detect={}ms ocr={}ms lines={}/{}",
               last_detect_ms.load (), ms, conf_n, bands.size ());
            core::log::ocr.event ("recognition", {
               { "generation", std::to_string (item.generation) },
               { "family", std::string { family_dir (family) } },
               { "segment_us", std::to_string (segment_us) },
               { "total_ms", std::to_string (ms) },
               { "lines", std::to_string (conf_n) },
               { "bands", std::to_string (bands.size ()) },
               { "confidence", fmt::format ("{:.3f}",
                  conf_n ? conf_sum / conf_n : 0.0f) },
            });

            const float confidence = conf_n ? conf_sum / conf_n : 0.0f;
            evidence.ocr (
               item.generation, crop, evidence_lines, text, confidence);
            if (item.generation != generation.load (std::memory_order_relaxed)) continue;
            if (text.empty ()) continue;
            last_completed_generation = item.generation;

            std::string printable = text;
            std::size_t newline = 0;
            while ((newline = printable.find ('\n', newline)) != std::string::npos) {
               printable.replace (newline, 1, "\\n");
               newline += 2;
            }
            core::Logger::info (
               "OCR result generation={} family={} confidence={:.3f} text=\"{}\"",
               item.generation, family_dir (family),
               confidence, printable);

            if (cache.size () == 128) cache.erase (cache.begin ());
            cache.push_back (Cached {
               .observation = item.observation,
               .family = family,
               .text = text,
               .gems = gems,
               .confidence = confidence,
            });

            if (callback) {
               try {
                  callback (RecognizedTooltip {
                     .generation  = item.generation,
                     .rect        = box.rect,
                     .text        = std::move (text),
                     .gems        = std::move (gems),
                     .confidence  = confidence,
                     .backend     = item.frame.backend,
                     .preliminary = false,
                     .captured_at = item.frame.timestamp,
                  });
               } catch (const std::exception& e) {
                  core::Logger::error ("pipeline: tooltip callback threw: {}", e.what ());
               }
            }
         }
      }
   }
};

Pipeline::Pipeline (
   capture::CaptureService& capture,
   vision::TooltipDetector& detector,
   LanguageRegistry&        registry,
   Config                   config
) {
   impl_           = std::make_unique<Impl> (capture, detector, registry, std::move (config));
   impl_->language = impl_->config.language;
}

Pipeline::~Pipeline () { stop (); }

void Pipeline::on_anchor (AnchorCallback cb) { impl_->anchor_cb = std::move (cb); }
void Pipeline::on_anchor_lost (AnchorLostCallback cb) { impl_->anchor_lost_cb = std::move (cb); }

void Pipeline::set_active_window (void* hwnd)
{
   if (impl_->window.exchange (hwnd) != hwnd) {
      impl_->reset_requested.store (true, std::memory_order_relaxed);
   }
}
void Pipeline::set_enabled (bool on)
{
   if (impl_->enabled.exchange (on) == on) return;
   impl_->force_scans.store (0, std::memory_order_relaxed);
   impl_->generation.fetch_add (1, std::memory_order_relaxed);
   impl_->reset_requested.store (true, std::memory_order_relaxed);
}
void Pipeline::set_automatic (bool on)
{
   if (impl_->automatic.exchange (on) == on) return;
   impl_->force_scans.store (0, std::memory_order_relaxed);
   impl_->generation.fetch_add (1, std::memory_order_relaxed);
   impl_->reset_requested.store (true, std::memory_order_relaxed);
}
void Pipeline::set_capture_fps (double fps)
{
   const double bounded = std::clamp (fps, 1.0, 60.0);
   if (impl_->capture_fps.exchange (bounded, std::memory_order_relaxed) == bounded) return;
   core::Logger::info ("pipeline: capture rate → {:.0f} fps", bounded);
}
void Pipeline::set_capture_mode (capture::CaptureMode mode)
{
   if (impl_->capture_mode.exchange (mode, std::memory_order_relaxed) == mode) return;
   core::Logger::info ("pipeline: capture mode → {}", capture::capture_mode_name (mode));
}
void Pipeline::set_performance_mode (bool on)
{
   if (impl_->performance_mode.exchange (on, std::memory_order_relaxed) == on) return;
   impl_->force_scans.store (0, std::memory_order_relaxed);
   impl_->reset_requested.store (true, std::memory_order_relaxed);
   core::Logger::info ("pipeline: performance mode {}", on ? "enabled" : "disabled");
}
void Pipeline::set_language (LanguageFamily f)
{
   if (impl_->language.exchange (f, std::memory_order_relaxed) == f) return;
   impl_->force_scans.store (0, std::memory_order_relaxed);
   impl_->generation.fetch_add (1, std::memory_order_relaxed);
   impl_->reset_requested.store (true, std::memory_order_relaxed);
   core::Logger::info ("pipeline: OCR language → {}", family_dir (f));
}

bool Pipeline::is_current (std::uint64_t value) const noexcept
{
   return impl_->generation.load (std::memory_order_relaxed) == value;
}
void Pipeline::set_detect_only (bool on) { impl_->detect_only.store (on); }
void Pipeline::request_immediate_scan ()
{
   if (!impl_->enabled.load (std::memory_order_relaxed)) return;
   constexpr int k_forced_burst = 2;
   impl_->queue_scans (k_forced_burst);
}

void Pipeline::record_evidence (
   std::uint64_t generation,
   std::string event,
   std::unordered_map<std::string, std::string> fields)
{
   impl_->evidence.event (generation, std::move (event), std::move (fields));
}

core::Result<void> Pipeline::start (TooltipCallback on_tooltip)
{
   if (impl_->running.exchange (true)) {
      return core::fail (core::Error::make (core::ErrorKind::Internal, "pipeline: already running"));
   }

   impl_->callback       = std::move (on_tooltip);

   const auto warm_started = std::chrono::steady_clock::now ();
   const auto warm_family = impl_->language.load (std::memory_order_relaxed);
   auto warm = impl_->registry.acquire (warm_family);
   const auto warm_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::steady_clock::now () - warm_started).count ();
   core::log::ocr.event ("model_prewarm", {
      { "family", std::string { family_dir (warm_family) } },
      { "elapsed_ms", std::to_string (warm_ms) },
      { "ok", warm.has_value () ? "1" : "0" },
   });
   if (!warm.has_value ()) {
      core::Logger::warn ("pipeline: OCR prewarm failed: {}", warm.error ().message);
   }
   impl_->capture_thread = std::thread { [this] { impl_->capture_loop (); } };
   impl_->vision_thread  = std::thread { [this] { impl_->vision_loop ();  } };
   impl_->ocr_thread     = std::thread { [this] { impl_->ocr_loop ();     } };

   core::Logger::info (
      "pipeline: started (detector_fps={:.1f}, performance_fps={:.1f}, "
      "tracking_fps={:.1f}, performance_tracking_fps={:.1f})",
      impl_->capture_fps.load (std::memory_order_relaxed),
      impl_->config.performance_fps,
      impl_->config.tracking_fps,
      impl_->config.performance_tracking_fps);
   return {};
}

void Pipeline::stop () noexcept
{
   if (!impl_) return;
   if (!impl_->running.exchange (false)) return;

   if (impl_->capture_thread.joinable ()) impl_->capture_thread.join ();
   if (impl_->vision_thread.joinable  ()) impl_->vision_thread.join  ();
   if (impl_->ocr_thread.joinable     ()) impl_->ocr_thread.join     ();

   core::Logger::info ("pipeline: stopped");
}

} // namespace gv::ocr
