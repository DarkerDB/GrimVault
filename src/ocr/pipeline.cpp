#include <gv/ocr/pipeline.h>
#include <gv/ocr/capture_policy.h>
#include <gv/ocr/preprocessor.h>
#include <gv/core/environment.h>
#include <gv/core/logger.h>
#include <gv/vision/gem_detector.h>
#include <gv/vision/tooltip_tracker.h>
#include <gv/core/spsc_queue.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
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

std::string pixel_hash (const cv::Mat& image)
{
   // Stable FNV-1a over dimensions, type, and visible pixel bytes. This is an
   // exact-capture dedupe key, not a security primitive.
   std::uint64_t hash = 14695981039346656037ull;
   const auto mix = [&hash] (const void* data, std::size_t size) {
      const auto* bytes = static_cast<const unsigned char*> (data);
      for (std::size_t i = 0; i < size; ++i) {
         hash ^= bytes [i];
         hash *= 1099511628211ull;
      }
   };
   const int header[] { image.rows, image.cols, image.type () };
   mix (header, sizeof (header));
   for (int row = 0; row < image.rows; ++row)
      mix (image.ptr (row), static_cast<std::size_t> (image.cols) * image.elemSize ());
   std::ostringstream out;
   out << std::hex << std::setfill ('0') << std::setw (16) << hash;
   return out.str ();
}

struct SampleLine {
   cv::Mat      image;
   std::size_t  source_band = 0;
   bool         title = false;
   std::string  prediction;
   float        confidence = 0.0f;
};

void persist_sample (const std::filesystem::path& inbox,
                     std::uint64_t generation, LanguageFamily family,
                     const capture::Rect& rect, const cv::Mat& tooltip,
                     std::vector<SampleLine> lines, std::string prediction,
                     float confidence)
{
   if (inbox.empty () || tooltip.empty ()) return;
   cv::Mat owned = tooltip.clone ();
   const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::system_clock::now ().time_since_epoch ()).count ();
   std::thread { [inbox, generation, family, rect, stamp, tooltip = std::move (owned),
                  lines = std::move (lines), prediction = std::move (prediction),
                  confidence] () mutable {
      try {
         std::error_code ec;
         std::filesystem::create_directories (inbox, ec);
         static std::atomic<std::uint64_t> sequence { 0 };
         const auto id = std::to_string (stamp) + "-g" + std::to_string (generation)
            + "-s" + std::to_string (sequence.fetch_add (1));
         const auto dir = inbox / id;
         if (!std::filesystem::create_directory (dir, ec)) return;

         cv::imwrite ((dir / "tooltip.png").string (), tooltip);
         nlohmann::json line_json = nlohmann::json::array ();
         for (std::size_t i = 0; i < lines.size (); ++i) {
            const auto filename = "line-" + (i < 10 ? std::string { "0" } : std::string {})
               + std::to_string (i) + ".png";
            cv::imwrite ((dir / filename).string (), lines [i].image);
            line_json.push_back ({
               { "index", i }, { "source_band", lines [i].source_band },
               { "title", lines [i].title }, { "prediction", lines [i].prediction },
               { "confidence", lines [i].confidence }, { "file", filename },
               { "width", lines [i].image.cols }, { "height", lines [i].image.rows },
               { "pixel_hash", pixel_hash (lines [i].image) },
            });
         }
         nlohmann::json metadata {
            { "schema", 1 }, { "id", id }, { "captured_unix_ms", stamp },
            { "generation", generation }, { "language", std::string { family_dir (family) } },
            { "prediction", prediction }, { "confidence", confidence },
            { "verified", false }, { "tooltip_file", "tooltip.png" },
            { "tooltip_pixel_hash", pixel_hash (tooltip) },
            { "rect", { { "x", rect.x }, { "y", rect.y },
                          { "width", rect.w }, { "height", rect.h } } },
            { "lines", std::move (line_json) },
         };
         std::ofstream output { dir / "metadata.json", std::ios::binary };
         output << metadata.dump (2) << '\n';
      } catch (const std::exception& e) {
         core::Logger::warn ("OCR sample persistence failed: {}", e.what ());
      }
   } }.detach ();
}

} // namespace

struct Pipeline::Impl
{
   Impl (capture::CaptureService& c, vision::TooltipDetector& d, LanguageRegistry& r, Config cfg)
      : capture (c), detector (d), registry (r), config (std::move (cfg)),
        capture_fps (std::clamp (config.capture_fps, 1.0, 60.0)),
        capture_mode (c.mode ())
   {}

   capture::CaptureService&  capture;
   vision::TooltipDetector&  detector;
   LanguageRegistry&         registry;
   Config                    config;
   std::atomic<double>       capture_fps;
   std::atomic<capture::CaptureMode> capture_mode { capture::CaptureMode::Automatic };

   std::atomic<bool>         running   { false };
   std::atomic<bool>         enabled   { true };
   std::atomic<bool>         automatic { true };
   std::atomic<bool>         detect_only { false };
   std::atomic<bool>         anchored { false };
   std::atomic<bool>         reacquiring { false };
   std::atomic<bool>         reset_requested { false };
   std::atomic<std::uint64_t> generation { 0 };
   std::atomic<void*>        window    { nullptr };
   std::atomic<LanguageFamily> language { LanguageFamily::Latin };

   std::thread               capture_thread;
   std::thread               vision_thread;
   std::thread               ocr_thread;

   core::SpscQueue<capture::Frame> capture_q  { 4 };
   struct VisionOut {
      capture::Frame                     frame;
      std::vector<vision::TooltipBox>    boxes;
      std::uint64_t                      generation = 0;
   };
   core::SpscQueue<VisionOut>  vision_q  { 4 };

   TooltipCallback           callback;
   ActivityCallback          activity;
   AnchorCallback            anchor_cb;
   AnchorLostCallback        anchor_lost_cb;

   std::atomic<long long>    last_detect_ms { 0 };

   // One-shot bypass of the stability gate; set by request_immediate_scan,
   // consumed and cleared by vision_loop.
   std::atomic<bool>         force_scan { false };

   // Idle policy state: steady_clock tick of the last tooltip detection (or
   // forced scan). capture_loop drops to idle_fps once it ages past
   // idle_window — without this the continuous (WGC) path forwards frames
   // at the game's render rate and the detector burns a core.
   std::atomic<std::chrono::steady_clock::rep> last_activity {
      std::chrono::steady_clock::now ().time_since_epoch ().count () };
   std::atomic<std::chrono::steady_clock::rep> burst_until { 0 };

   void mark_activity ()
   {
      last_activity.store (
         std::chrono::steady_clock::now ().time_since_epoch ().count (),
         std::memory_order_relaxed);
   }

   std::chrono::milliseconds current_interval () const
   {
      const auto last = std::chrono::steady_clock::time_point {
         std::chrono::steady_clock::duration { last_activity.load (std::memory_order_relaxed) } };
      const bool idle = (std::chrono::steady_clock::now () - last) > config.idle_window;
      // Keep expensive acquisition at active_fps, then raise only the cheap
      // anchored presence/identity checks to game-like cadence.
      const bool tracking = anchored.load (std::memory_order_relaxed)
         || reacquiring.load (std::memory_order_relaxed);
      const bool bursting = tracking
         && std::chrono::steady_clock::now ().time_since_epoch ().count ()
            < burst_until.load (std::memory_order_relaxed);
      const double desired_fps = std::max (1.0, tracking
         ? (bursting ? config.anchored_burst_fps : config.anchored_fps)
         : (idle ? config.idle_fps : config.active_fps));
      const double fps = std::min (
         desired_fps,
         capture_fps.load (std::memory_order_relaxed));

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
         const bool forced = force_scan.load (std::memory_order_relaxed);
         const bool auto_scan = automatic.load (std::memory_order_relaxed);
         const bool tracking = capture_tracking (
            auto_scan,
            anchored.load (std::memory_order_relaxed),
            reacquiring.load (std::memory_order_relaxed));

         if (!capture_active (
               enabled.load (std::memory_order_relaxed),
               auto_scan,
               forced,
               tracking)) {
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
         if (!capture_targeted (now_target != nullptr, forced, tracking)) {
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
            if (!capture_q.try_push (std::move (*frame_res))) ++dropped_frames;
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

         // Pace BOTH paths. The continuous path returns the latest frame as
         // fast as the game renders; without this sleep the detector runs at
         // game fps instead of active_fps/idle_fps.
         std::this_thread::sleep_for (current_interval ());
      }

      if (session_active) {
         capture.stop_continuous ();
      }
   }

   void vision_loop ()
   {
      // Anchoring state machine (docs/architecture/anchoring.md §2):
      //
      //    IDLE ──detector hit──> SETTLING ──2 agreeing refines──> ANCHORED
      //     ^                        │                                │
      //     └────────hit lost────────┴────presence / identity change──┘
      //
      // Owned exclusively by this loop; the UI hears about it via queued
      // anchor / anchor-lost events.
      enum class Track { Idle, Settling, Anchored };
      Track track = Track::Idle;

      capture::Rect     prev_box {};        // SETTLING: previous refined box
      capture::CursorPos prev_cursor {};
      bool              have_prev = false;
      int               stable_frames = 0;
      int               identity_streak = 0;

      vision::Anchor    anchor;
      std::uint64_t     anchor_generation = 0;
      capture::Rect     last_box {};        // ANCHORED: freshest known box
      capture::Rect     observed_box {};    // vision-only; never drives presentation
      std::chrono::steady_clock::time_point reacquire_started {};

      struct Metrics {
         int frames = 0;
         long long age_us = 0;
         long long locate_us = 0;
         int max_hash_bits = 0;
         int max_detail_pixels = 0;
      } metrics;

      std::chrono::steady_clock::time_point last_pulse {};

      const auto emit_anchor = [this, &anchor_generation] (const vision::Anchor& a) {
         if (anchor_cb) {
            anchor_cb (AnchorEvent {
               .generation = anchor_generation,
               .offset_x = a.offset_x, .offset_y = a.offset_y,
               .locked_x = a.locked_x, .locked_y = a.locked_y,
               .pinned_x = a.axis_x != vision::AxisPin::Free,
               .pinned_y = a.axis_y != vision::AxisPin::Free,
               .pin_x = a.pin_x, .pin_y = a.pin_y,
               .w = a.w, .h = a.h });
         }
      };

      // Reset baseline: cursor at the frame that anchored. A jump past
      // cursor_reset_px from here means the hover moved off the item, so drop
      // the anchor and hard-hide (no grace) before the fingerprint drifts.
      capture::CursorPos anchored_cursor {};

      const auto lose = [&] (const char* why, bool immediate = true) {
         if (track == Track::Anchored) {
            core::Logger::debug ("anchoring: {} ({})", immediate ? "reset" : "lost", why);
            core::log::vision.event ("anchor_lost", {
               { "reason", why },
               { "immediate", immediate ? "1" : "0" },
            });
            if (anchor_lost_cb) anchor_lost_cb (immediate);
         }
         track           = Track::Idle;
         anchored.store (false, std::memory_order_relaxed);
         reacquiring.store (false, std::memory_order_relaxed);
         generation.fetch_add (1, std::memory_order_relaxed);
         have_prev       = false;
         stable_frames   = 0;
         identity_streak = 0;
         anchored_cursor = {};
      };

      // Cursor->tooltip offset per axis, locking only when the box is not
      // pinned against a clamp edge on that axis (§3, estimator).
      const auto measure = [] (vision::Anchor& a, const capture::Rect& box,
                               const capture::CursorPos& c, int fw, int fh) {
         if (!c.valid) return;
         if (!a.locked_x && box.x > 2 && box.x + box.w < fw - 2) {
            a.offset_x = box.x - c.x;
            a.locked_x = true;
         }
         if (!a.locked_y && box.y > 2 && box.y + box.h < fh - 2) {
            a.offset_y = box.y - c.y;
            a.locked_y = true;
         }
      };

      const auto classify_pins = [this] (vision::Anchor& a,
                                     const capture::Rect& box, int fw, int fh) {
         const auto x = box.x <= config.pin_near_edge_px
            ? vision::AxisPin::Low
            : (fw - (box.x + box.w) <= config.pin_right_edge_px
               ? vision::AxisPin::High : vision::AxisPin::Free);
         const auto y = box.y <= config.pin_near_edge_px
            ? vision::AxisPin::Low
            : (fh - (box.y + box.h) <= config.pin_near_edge_px
               ? vision::AxisPin::High : vision::AxisPin::Free);
         if (x != vision::AxisPin::Free && a.axis_x == vision::AxisPin::Free)
            a.pin_x = box.x;
         if (y != vision::AxisPin::Free && a.axis_y == vision::AxisPin::Free)
            a.pin_y = box.y;
         a.axis_x = x;
         a.axis_y = y;
      };

      const auto nearest_to_cursor = [] (
         const std::vector<vision::TooltipBox>& boxes,
         const capture::CursorPos& cursor) -> const vision::TooltipBox* {
         const auto* pick = &boxes.front ();
         if (!cursor.valid || boxes.size () == 1) return pick;
         long best = -1;
         for (const auto& candidate : boxes) {
            const long dx = candidate.rect.x + candidate.rect.w / 2 - cursor.x;
            const long dy = candidate.rect.y + candidate.rect.h / 2 - cursor.y;
            const long distance = dx * dx + dy * dy;
            if (best < 0 || distance < best) {
               best = distance;
               pick = &candidate;
            }
         }
         return pick;
      };

      const std::filesystem::path diagnostic_dir = [] {
         return std::filesystem::path {
            core::environment::get ("GRIMVAULT_ANCHOR_DIAGNOSTICS")
         };
      } ();
      const auto dump_diagnostic = [&diagnostic_dir] (
         std::string event, const cv::Mat& image) {
         if (diagnostic_dir.empty () || image.empty ()) return;
         cv::Mat owned = image.clone ();
         const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now ().time_since_epoch ()).count ();
         std::thread { [dir = diagnostic_dir, event = std::move (event),
                        stamp, image = std::move (owned)] () mutable {
            try {
               std::error_code ec;
               std::filesystem::create_directories (dir, ec);
               cv::imwrite ((dir / (std::to_string (stamp) + "_" + event + ".png")).string (),
                            image);

               std::vector<std::filesystem::directory_entry> files;
               for (std::filesystem::directory_iterator it { dir, ec }, end;
                    !ec && it != end; it.increment (ec)) {
                  if (it->is_regular_file () && it->path ().extension () == ".png")
                     files.push_back (*it);
               }
               constexpr std::size_t keep = 40;
               if (files.size () > keep) {
                  std::sort (files.begin (), files.end (), [] (const auto& a, const auto& b) {
                     return a.last_write_time () < b.last_write_time ();
                  });
                  for (std::size_t i = 0; i < files.size () - keep; ++i)
                     std::filesystem::remove (files [i].path (), ec);
               }
            } catch (...) {
               // Diagnostics must never affect tracking or process lifetime.
            }
         } }.detach ();
      };

      while (running.load (std::memory_order_relaxed)) {
         auto* head = capture_q.front ();
         if (!head) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
         }

         capture::Frame frame = std::move (*head);
         capture_q.pop ();

         // Capture strategies retain the cursor in desktop coordinates so a
         // frame is self-describing across monitors. Vision and anchoring are
         // frame-relative, so normalize exactly once at the stage boundary.
         frame.cursor = frame.local_cursor ();

         if (reset_requested.exchange (false, std::memory_order_relaxed)) {
            lose ("runtime policy changed");
            if (!enabled.load (std::memory_order_relaxed)) continue;
         }

         // Consume the force flag up front — leaving it latched on a fruitless
         // scan would hold the capture loop out of its no-window sleep.
         const bool forced = force_scan.exchange (false);

         cv::Mat bgra { frame.height, frame.width, CV_8UC4,
                        frame.data.get (),
                        static_cast<std::size_t> (frame.stride) };

         const auto build_anchor = [&] (const capture::Rect& box) {
            vision::Anchor fresh;
            fresh.w = box.w;
            fresh.h = box.h;
            fresh.fingerprint = vision::TooltipTracker::fingerprint (
               bgra, box, fresh.fp_dx, fresh.fp_dy);
            fresh.content_hash = vision::TooltipTracker::content_hash (bgra, box);
            fresh.detail_thumbnail = vision::TooltipTracker::detail_thumbnail (bgra, box);
            measure (fresh, box, frame.cursor, frame.width, frame.height);
            classify_pins (fresh, box, frame.width, frame.height);
            return fresh;
         };

         bool dispatch = false;   // OCR this frame's last_box at loop tail

         if (track == Track::Anchored) {
            mark_activity ();

            const auto process_now = std::chrono::steady_clock::now ();
            const auto age_us = std::chrono::duration_cast<std::chrono::microseconds> (
               process_now - frame.timestamp).count ();
            ++metrics.frames;
            metrics.age_us += std::max<long long> (0, age_us);

            // Substantial cursor travel since the last frame (a flick, not
            // slow drift within a large item) = the hover is leaving; reset
            // immediately and re-acquire fresh. Baseline slides per frame.
            if (frame.cursor.valid && anchored_cursor.valid) {
               const long dx = frame.cursor.x - anchored_cursor.x;
               const long dy = frame.cursor.y - anchored_cursor.y;
               if (dx != 0 || dy != 0) {
                  burst_until.store (
                     (std::chrono::steady_clock::now () + config.anchored_burst)
                        .time_since_epoch ().count (),
                     std::memory_order_relaxed);
               }
               if (dx * dx + dy * dy
                   > static_cast<long> (config.cursor_reset_px) * config.cursor_reset_px) {
                  lose ("cursor jump", /*immediate=*/ true);
                  continue;
               }
            }
            if (frame.cursor.valid) anchored_cursor = frame.cursor;

            if ((!anchor.locked_x && anchor.axis_x == vision::AxisPin::Free)
                || (!anchor.locked_y && anchor.axis_y == vision::AxisPin::Free)) {
               // Acquired while pinned: keep refining around the last box
               // until the cursor moves inward and the offset can lock.
               auto refined = vision::TooltipTracker::refine (bgra, last_box);
               if (!refined.has_value ()) {
                  lose ("refine failed while unlocked");
                  continue;
               }
               last_box = *refined;
               classify_pins (anchor, last_box, frame.width, frame.height);
               measure (anchor, last_box, frame.cursor, frame.width, frame.height);
               emit_anchor (anchor);
            } else {
               const int max_x = std::max (0, frame.width  - anchor.w);
               const int max_y = std::max (0, frame.height - anchor.h);
               const bool pinned_x = anchor.axis_x != vision::AxisPin::Free;
               const bool pinned_y = anchor.axis_y != vision::AxisPin::Free;
               const int px = pinned_x ? anchor.pin_x : frame.cursor.valid
                  ? std::clamp (frame.cursor.x + anchor.offset_x, 0, max_x) : last_box.x;
               const int py = pinned_y ? anchor.pin_y : frame.cursor.valid
                  ? std::clamp (frame.cursor.y + anchor.offset_y, 0, max_y) : last_box.y;

               {
                  // Observe the actual frame within a small translation
                  // radius solely to sample identity. Presentation continues
                  // to use px/py from cursor math (or the fixed pin).
                  const int vision_x = pinned_x ? observed_box.x : px;
                  const int vision_y = pinned_y ? observed_box.y : py;
                  const auto locate_started = std::chrono::steady_clock::now ();
                  const auto observed = vision::TooltipTracker::locate (
                     bgra, anchor, vision_x, vision_y,
                     pinned_x ? config.search_pinned_px : config.search_free_px,
                     pinned_y ? config.search_pinned_px : config.search_free_px);
                  metrics.locate_us += std::chrono::duration_cast<std::chrono::microseconds> (
                     std::chrono::steady_clock::now () - locate_started).count ();
                  const int hash_bits = observed.has_value ()
                     ? std::popcount (vision::TooltipTracker::content_hash (bgra, *observed)
                        ^ anchor.content_hash)
                     : 64;
                  int detail_pixels = 1024;
                  if (observed.has_value () && !anchor.detail_thumbnail.empty ()) {
                     const auto detail = vision::TooltipTracker::detail_thumbnail (
                        bgra, *observed);
                     if (!detail.empty ()) {
                        cv::Mat delta, changed;
                        cv::absdiff (detail, anchor.detail_thumbnail, delta);
                        cv::threshold (delta, changed, 8, 255, cv::THRESH_BINARY);
                        detail_pixels = cv::countNonZero (changed);
                     }
                  }
                  metrics.max_hash_bits = std::max (metrics.max_hash_bits, hash_bits);
                  metrics.max_detail_pixels = std::max (
                     metrics.max_detail_pixels, detail_pixels);
                  const bool region_changed = hash_bits >= config.identity_bits
                     && detail_pixels >= config.identity_detail_px;
                  identity_streak = region_changed ? identity_streak + 1 : 0;
                  if (identity_streak >= std::max (1, config.identity_frames)) {
                     core::log::vision.event ("replacement_candidate", {
                        { "hash_bits", std::to_string (hash_bits) },
                        { "detail_px", std::to_string (detail_pixels) },
                        { "located", observed.has_value () ? "1" : "0" },
                        { "frame_age_us", std::to_string (std::max<long long> (0, age_us)) },
                     });
                     dump_diagnostic ("replacement_candidate", bgra);
                     core::Logger::debug (
                        "anchoring: replacement candidate; entering settle gate");
                     if (anchor_lost_cb) anchor_lost_cb (true);
                     track = Track::Settling;
                     anchored.store (false, std::memory_order_relaxed);
                     reacquiring.store (true, std::memory_order_relaxed);
                     generation.fetch_add (1, std::memory_order_relaxed);
                     reacquire_started = std::chrono::steady_clock::now ();
                     burst_until.store (
                        (std::chrono::steady_clock::now () + config.anchored_burst)
                           .time_since_epoch ().count (),
                        std::memory_order_relaxed);
                     have_prev = false;
                     stable_frames = 0;
                     identity_streak = 0;
                     anchored_cursor = {};
                     continue;
                  }
                  if (observed.has_value ()) observed_box = *observed;
               }

               last_box = { px, py, anchor.w, anchor.h };
            }

            if (metrics.frames >= 120) {
               core::log::vision.event ("anchor_metrics", {
                  { "frames", std::to_string (metrics.frames) },
                  { "avg_frame_age_us", std::to_string (metrics.age_us / metrics.frames) },
                  { "avg_locate_us", std::to_string (metrics.locate_us / metrics.frames) },
                  { "max_hash_bits", std::to_string (metrics.max_hash_bits) },
                  { "max_detail_px", std::to_string (metrics.max_detail_pixels) },
               });
               metrics = {};
            }

            if (forced) dispatch = true;
         } else {
            const auto t0 = std::chrono::steady_clock::now ();
            auto boxes_res = detector.detect (frame);
            last_detect_ms.store (std::chrono::duration_cast<std::chrono::milliseconds> (
               std::chrono::steady_clock::now () - t0).count ());

            if (!boxes_res.has_value () || boxes_res->empty ()) {
               if (reacquiring.load (std::memory_order_relaxed)) {
                  core::log::vision.event ("replacement_rejected", {
                     { "reason", "no_detection" },
                     { "elapsed_ms", std::to_string (
                        std::chrono::duration_cast<std::chrono::milliseconds> (
                           std::chrono::steady_clock::now () - reacquire_started).count ()) },
                  });
               }
               track     = Track::Idle;
               reacquiring.store (false, std::memory_order_relaxed);
               have_prev = false;
               stable_frames = 0;
               continue;
            }

            mark_activity ();

            const auto now = std::chrono::steady_clock::now ();
            if (activity && now - last_pulse >= std::chrono::milliseconds (700)) {
               last_pulse = now;
               activity ();
            }

            // Multiple hits (item-compare side tooltips): anchor the box
            // nearest the cursor.
            const auto& c = frame.cursor;
            const auto* pick = nearest_to_cursor (*boxes_res, c);

            auto refined = vision::TooltipTracker::refine (bgra, pick->rect);
            if (!refined.has_value ()) {
               // Mid fade-in or a ghost: stay/enter SETTLING and try the
               // next frame.
               track     = Track::Settling;
               have_prev = false;
               stable_frames = 0;
               continue;
            }

            const capture::Rect relative {
               refined->x - (c.valid ? c.x : 0),
               refined->y - (c.valid ? c.y : 0),
               refined->w,
               refined->h,
            };
            const capture::Rect previous_relative {
               prev_box.x - (prev_cursor.valid ? prev_cursor.x : 0),
               prev_box.y - (prev_cursor.valid ? prev_cursor.y : 0),
               prev_box.w,
               prev_box.h,
            };
            const bool agrees = have_prev
               && capture::intersection_over_union (relative, previous_relative)
                  >= std::clamp (config.stability_iou, 0.0f, 1.0f);
            stable_frames = agrees ? stable_frames + 1 : 1;
            const bool settled = forced
               || stable_frames >= std::max (1, config.stability_frames);

            if (!settled) {
               track       = Track::Settling;
               prev_box    = *refined;
               prev_cursor = c;
               have_prev   = true;
               continue;
            }

            anchor = build_anchor (*refined);
            anchor_generation = generation.fetch_add (1, std::memory_order_relaxed) + 1;

            last_box        = *refined;
            observed_box    = *refined;
            track           = Track::Anchored;
            const bool was_reacquiring = reacquiring.load (std::memory_order_relaxed);
            const auto reacquire_ms = was_reacquiring
               ? std::chrono::duration_cast<std::chrono::milliseconds> (
                    std::chrono::steady_clock::now () - reacquire_started).count ()
               : 0;
            anchored.store (true, std::memory_order_relaxed);
            reacquiring.store (false, std::memory_order_relaxed);
            metrics = {};
            have_prev       = false;
            stable_frames   = 0;
            identity_streak = 0;
            anchored_cursor = c;
            core::Logger::debug (
               "anchoring: anchored {},{} {}x{} offset {},{} locked {}/{}",
               refined->x, refined->y, refined->w, refined->h,
               anchor.offset_x, anchor.offset_y, anchor.locked_x, anchor.locked_y);
            core::log::vision.event ("anchor_acquired", {
               { "x", std::to_string (refined->x) },
               { "y", std::to_string (refined->y) },
               { "w", std::to_string (refined->w) },
               { "h", std::to_string (refined->h) },
               { "pin_x", anchor.axis_x == vision::AxisPin::Free ? "free"
                  : (anchor.axis_x == vision::AxisPin::Low ? "low" : "high") },
               { "pin_y", anchor.axis_y == vision::AxisPin::Free ? "free"
                  : (anchor.axis_y == vision::AxisPin::Low ? "low" : "high") },
               { "reacquire_ms", std::to_string (reacquire_ms) },
            });
            if (was_reacquiring) dump_diagnostic ("replacement_settled", bgra);

            emit_anchor (anchor);
            dispatch = true;
         }

         if (!dispatch || detect_only.load (std::memory_order_relaxed)) continue;

         // OCR fires once per anchor (plus manual re-scans), on the settled
         // refined box — the crop is exact, not the detector's coarse guess.
         VisionOut out { std::move (frame),
                         { vision::TooltipBox { .rect = last_box } },
                         anchor_generation };
         (void) vision_q.try_push (std::move (out));
      }
   }

   void ocr_loop ()
   {
      std::uint64_t last_completed_generation = 0;
      while (running.load (std::memory_order_relaxed)) {
         auto* head = vision_q.front ();
         if (!head) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
         }

         VisionOut item = std::move (*head);
         vision_q.pop ();

         if (item.generation != generation.load (std::memory_order_relaxed)
             || item.generation == last_completed_generation) continue;

         if (!item.frame.data || item.frame.empty ()) {
            core::Logger::warn ("pipeline: dropping frame with no pixel data");
            continue;
         }

         auto rec = registry.acquire (language.load ());
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
            std::vector<SampleLine> sample_lines;

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
               if (language.load () == LanguageFamily::English && is_title) {
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
                  const auto chunks = language.load () == LanguageFamily::English
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
               if (language.load () == LanguageFamily::Latin)
                  canonicalize_latin (line_text);
               if (!config.sample_inbox.empty ()) {
                  sample_lines.push_back (SampleLine {
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
                  if (auto family = vision::detect_gem_family (raw_line); family.has_value ()) {
                     gems [line_text] = *family;
                  }
               }
               if (!text.empty ()) text.push_back ('\n');
               text += line_text;

               if (!preliminary_sent && is_title
                   && language.load () == LanguageFamily::English
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
               { "family", std::string { family_dir (language.load ()) } },
               { "segment_us", std::to_string (segment_us) },
               { "total_ms", std::to_string (ms) },
               { "lines", std::to_string (conf_n) },
               { "bands", std::to_string (bands.size ()) },
               { "confidence", fmt::format ("{:.3f}",
                  conf_n ? conf_sum / conf_n : 0.0f) },
            });

            if (item.generation != generation.load (std::memory_order_relaxed)) continue;
            if (!config.sample_inbox.empty ()) {
               persist_sample (config.sample_inbox, item.generation, language.load (),
                  box.rect, crop, std::move (sample_lines), text,
                  conf_n ? conf_sum / conf_n : 0.0f);
            }
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
               item.generation, family_dir (language.load ()),
               conf_n ? conf_sum / conf_n : 0.0f, printable);

            if (callback) {
               try {
                  callback (RecognizedTooltip {
                     .generation  = item.generation,
                     .rect        = box.rect,
                     .text        = std::move (text),
                     .gems        = std::move (gems),
                     .confidence  = conf_n ? conf_sum / conf_n : 0.0f,
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

void Pipeline::on_activity (ActivityCallback cb) { impl_->activity = std::move (cb); }
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
   impl_->force_scan.store (false, std::memory_order_relaxed);
   impl_->generation.fetch_add (1, std::memory_order_relaxed);
   impl_->reset_requested.store (true, std::memory_order_relaxed);
}
void Pipeline::set_automatic (bool on)
{
   if (impl_->automatic.exchange (on) == on) return;
   impl_->force_scan.store (false, std::memory_order_relaxed);
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
void Pipeline::set_language (LanguageFamily f) { impl_->language.store (f); }

bool Pipeline::is_current (std::uint64_t value) const noexcept
{
   return impl_->generation.load (std::memory_order_relaxed) == value;
}
void Pipeline::set_detect_only (bool on) { impl_->detect_only.store (on); }
void Pipeline::request_immediate_scan ()
{
   if (!impl_->enabled.load (std::memory_order_relaxed)) return;
   // Wake from idle pacing too, or a forced scan waits up to 1/idle_fps.
   impl_->mark_activity ();
   impl_->force_scan.store (true);
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
      "pipeline: started (capture_fps={:.1f}, active_fps={:.1f}, anchored_fps={:.1f}, burst_fps={:.1f})",
      impl_->capture_fps.load (std::memory_order_relaxed),
      impl_->config.active_fps, impl_->config.anchored_fps,
      impl_->config.anchored_burst_fps);
   return {};
}

void Pipeline::stop () noexcept
{
   if (!impl_) return;
   if (!impl_->running.exchange (false)) return;
   impl_->anchored.store (false, std::memory_order_relaxed);

   if (impl_->capture_thread.joinable ()) impl_->capture_thread.join ();
   if (impl_->vision_thread.joinable  ()) impl_->vision_thread.join  ();
   if (impl_->ocr_thread.joinable     ()) impl_->ocr_thread.join     ();

   core::Logger::info ("pipeline: stopped");
}

} // namespace gv::ocr
