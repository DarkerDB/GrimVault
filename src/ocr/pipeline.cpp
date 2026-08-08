#include <gv/ocr/pipeline.h>
#include <gv/core/logger.h>
#include <gv/core/spsc_queue.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace gv::ocr {

namespace {

   // Segment a tooltip crop into horizontal text-line bands via row-ink
   // profile. The recognizer is a single-line CRNN (48x320) — feeding it
   // the whole multi-line card squashes every row into one 48px band and
   // yields garbage. The lookup contract (§4.2) wants the whole tooltip
   // newline-separated, so each band is recognized separately and rejoined.
   // Threshold stays low (80) because artifact-red text has ~90 luma.
   std::vector<cv::Range> line_bands (const cv::Mat& crop)
   {
      cv::Mat gray;
      cv::cvtColor (crop, gray, crop.channels () == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);

      cv::Mat bright;
      cv::threshold (gray, bright, 80, 255, cv::THRESH_BINARY);

      // Profile only interior columns — the tooltip frame's vertical border
      // lines run the full height, register ink on every row, and weld all
      // text lines into one giant band (which then OCRs as garbage).
      const int margin   = std::max (2, bright.cols / 25);
      cv::Mat   interior = bright (cv::Range::all (), cv::Range (margin, bright.cols - margin));

      cv::Mat rowsum;
      cv::reduce (interior, rowsum, 1, cv::REDUCE_SUM, CV_32S);

      const int min_ink = interior.cols * 255 / 50;   // >2% of the row lit

      std::vector<cv::Range> bands;
      int top = -1, end = -1, gap = 0;

      auto flush = [&] {
         if (top >= 0 && end - top >= 6) {
            bands.emplace_back (std::max (0, top - 2), std::min (crop.rows, end + 3));
         }
         top = -1; end = -1; gap = 0;
      };

      for (int y = 0; y < rowsum.rows; ++y) {
         if (rowsum.at<int> (y) > min_ink) {
            if (top < 0) top = y;
            end = y;
            gap = 0;
         } else if (top >= 0 && ++gap > 3) {
            flush ();
         }
      }
      flush ();

      return bands;
   }

   // Trim a line band to its horizontal ink extent. Tooltip text is mostly
   // centered, so a band spanning the full crop carries wide empty margins;
   // the recognizer clamps width to its 320px input and the actual glyphs
   // get squashed into misreads ("Frock" → "Froc").
   cv::Mat trim_cols (const cv::Mat& line)
   {
      cv::Mat gray, bright, colsum;
      cv::cvtColor (line, gray, line.channels () == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
      cv::threshold (gray, bright, 80, 255, cv::THRESH_BINARY);
      cv::reduce (bright, colsum, 0, cv::REDUCE_SUM, CV_32S);

      int x0 = -1, x1 = -1;
      for (int x = 0; x < colsum.cols; ++x) {
         if (colsum.at<int> (x) > 0) {
            if (x0 < 0) x0 = x;
            x1 = x;
         }
      }

      if (x0 < 0) return line;
      x0 = std::max (0, x0 - 4);
      x1 = std::min (line.cols, x1 + 5);
      return line (cv::Range::all (), cv::Range (x0, x1));
   }

   // Split a trimmed line into chunks the recognizer can read at natural
   // aspect (≤ ~6:1 for the 48x320 input), cutting at whitespace valleys so
   // words stay intact. Long rows ("Required Class: Wizard, Cleric, …")
   // otherwise get compressed past legibility.
   std::vector<cv::Range> col_chunks (const cv::Mat& line)
   {
      const int max_w = line.rows * 6;
      if (line.cols <= max_w) return { cv::Range (0, line.cols) };

      cv::Mat gray, bright, colsum;
      cv::cvtColor (line, gray, line.channels () == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
      cv::threshold (gray, bright, 80, 255, cv::THRESH_BINARY);
      cv::reduce (bright, colsum, 0, cv::REDUCE_SUM, CV_32S);

      std::vector<cv::Range> chunks;
      int start = 0, gap_run = 0, last_cut = -1;

      for (int x = 0; x < line.cols; ++x) {
         if (colsum.at<int> (x) == 0) {
            if (++gap_run >= 3) last_cut = x - gap_run / 2;
         } else {
            gap_run = 0;
         }

         if (x - start >= max_w) {
            const int cut = (last_cut > start + line.rows) ? last_cut : x;
            chunks.emplace_back (start, cut);
            start    = cut;
            last_cut = -1;
         }
      }

      if (start < line.cols) chunks.emplace_back (start, line.cols);
      return chunks;
   }

   float iou (const capture::Rect& a, const capture::Rect& b) noexcept
   {
      const int ix1 = std::max (a.x, b.x);
      const int iy1 = std::max (a.y, b.y);
      const int ix2 = std::min (a.x + a.w, b.x + b.w);
      const int iy2 = std::min (a.y + a.h, b.y + b.h);

      const int iw = std::max (0, ix2 - ix1);
      const int ih = std::max (0, iy2 - iy1);

      const int  inter = iw * ih;
      const int  ua    = a.w * a.h + b.w * b.h - inter;

      return ua > 0 ? static_cast<float> (inter) / ua : 0.0f;
   }

} // namespace

struct Pipeline::Impl
{
   Impl (capture::CaptureService& c, vision::TooltipDetector& d, LanguageRegistry& r, Config cfg)
      : capture (c), detector (d), registry (r), config (std::move (cfg))
   {}

   capture::CaptureService&  capture;
   vision::TooltipDetector&  detector;
   LanguageRegistry&         registry;
   Config                    config;

   std::atomic<bool>         running   { false };
   std::atomic<void*>        window    { nullptr };
   std::atomic<LanguageFamily> language { LanguageFamily::Latin };

   std::thread               capture_thread;
   std::thread               vision_thread;
   std::thread               ocr_thread;

   core::SpscQueue<capture::Frame> capture_q  { 4 };
   struct VisionOut {
      capture::Frame                     frame;
      std::vector<vision::TooltipBox>    boxes;
   };
   core::SpscQueue<VisionOut>  vision_q  { 4 };

   TooltipCallback           callback;
   ActivityCallback          activity;

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
      const double fps = idle ? config.idle_fps : config.active_fps;

      return std::chrono::duration_cast<std::chrono::milliseconds> (
         std::chrono::duration<double> (1.0 / fps));
   }

   void capture_loop ()
   {
      const bool can_continuous = capture.current ().supports_continuous ();

      void* current_target  = nullptr;
      bool  session_active  = false;
      bool  continuous_ok   = true;   // flips false after a failed (re)start

      while (running.load (std::memory_order_relaxed)) {
         void* now_target = window.load ();

         // No game window: the pipeline sleeps outright instead of running
         // detection against monitor frames — that's pure CPU burn with
         // nothing to find. A forced scan (F5) still grabs one monitor
         // frame so desktop testing works without the game.
         if (now_target == nullptr && !force_scan.load (std::memory_order_relaxed)) {
            if (session_active) {
               capture.current ().stop_continuous ();
               session_active = false;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (250));
            continue;
         }

         // (Re)start the continuous session when the target changed or the
         // session was torn down during a no-window pause.
         if (can_continuous && continuous_ok
               && (!session_active || now_target != current_target)) {
            if (session_active) capture.current ().stop_continuous ();
            current_target = now_target;
            auto r = capture.current ().start_continuous (
               current_target, /*is_window=*/ current_target != nullptr);
            session_active = r.has_value ();
            if (!session_active) {
               core::Logger::warn ("pipeline: continuous start failed: {}; falling back to per-call",
                  r.error ().message);
               continuous_ok = false;
            }
         }

         core::Result<capture::Frame> frame_res = core::fail (core::Error {
            core::ErrorKind::Capture, "init" });

         if (session_active) {
            frame_res = capture.current ().latest_frame (std::chrono::milliseconds (200));
         } else {
            frame_res = now_target
               ? capture.current ().capture_window  (now_target)
               : capture.current ().capture_monitor (nullptr);
         }

         if (frame_res.has_value () && !frame_res->empty ()) {
            while (running.load () && !capture_q.try_push (std::move (*frame_res))) {
               std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
         }

         // Pace BOTH paths. The continuous path returns the latest frame as
         // fast as the game renders; without this sleep the detector runs at
         // game fps instead of active_fps/idle_fps.
         std::this_thread::sleep_for (current_interval ());
      }

      if (session_active) {
         capture.current ().stop_continuous ();
      }
   }

   void vision_loop ()
   {
      // Stability gate state — owned exclusively by this loop.
      std::vector<vision::TooltipBox> last_boxes;
      int                              stable_count = 0;

      // Refractory state: the last box dispatched to OCR and when, plus the
      // last UI pulse. Release-speed detection revisits the same stable
      // tooltip several times per second — without these, every frame
      // re-fires OCR + API lookup and restarts the badge animation.
      capture::Rect                          last_sent {};
      std::chrono::steady_clock::time_point  last_sent_at {};
      std::chrono::steady_clock::time_point  last_pulse {};

      while (running.load (std::memory_order_relaxed)) {
         auto* head = capture_q.front ();
         if (!head) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
         }

         capture::Frame frame = std::move (*head);
         capture_q.pop ();

         // Consume the force flag up front — leaving it latched on a fruitless
         // scan would hold the capture loop out of its no-window sleep.
         const bool forced = force_scan.exchange (false);

         const auto t0 = std::chrono::steady_clock::now ();
         auto boxes_res = detector.detect (frame);
         last_detect_ms.store (std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - t0).count ());

         if (!boxes_res.has_value () || boxes_res->empty ()) {
            stable_count = 0;
            last_boxes.clear ();
            continue;
         }

         mark_activity ();

         const auto now = std::chrono::steady_clock::now ();

         if (activity && now - last_pulse >= std::chrono::milliseconds (700)) {
            last_pulse = now;
            activity ();
         }

         // Stability: only forward to OCR if the same box persists.
         bool stable = false;
         if (!last_boxes.empty ()) {
            for (const auto& nb : *boxes_res) {
               for (const auto& ob : last_boxes) {
                  if (iou (nb.rect, ob.rect) >= config.stability_iou) {
                     stable = true;
                     break;
                  }
               }
               if (stable) break;
            }
         }

         if (stable) {
            ++stable_count;
         } else {
            stable_count = 1;
         }

         last_boxes = *boxes_res;

         if (!forced && stable_count < config.stability_frames) continue;

         // Refractory: a stable box that hasn't moved was already OCR'd and
         // looked up — re-dispatching identical work every frame spams the
         // API. Re-send only when the box moves or the result ages out.
         const bool unchanged =
            iou (boxes_res->front ().rect, last_sent) >= config.stability_iou
            && now - last_sent_at < std::chrono::milliseconds (1500);

         if (!forced && unchanged) continue;

         last_sent    = boxes_res->front ().rect;
         last_sent_at = now;

         // Construct ONCE outside the retry loop. Building the VisionOut as
         // the try_push argument re-moved `frame` on every retry, so a full
         // queue pushed a hollowed-out frame whose null data blew an OpenCV
         // assert (and abort) in the OCR thread's cv::Mat constructor.
         VisionOut out { std::move (frame), *boxes_res };
         while (running.load () && !vision_q.try_push (std::move (out))) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
         }
      }
   }

   void ocr_loop ()
   {
      while (running.load (std::memory_order_relaxed)) {
         auto* head = vision_q.front ();
         if (!head) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
         }

         VisionOut item = std::move (*head);
         vision_q.pop ();

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
            const auto bands = line_bands (crop);

            // GRIMVAULT_OCR_DEBUG=1 → dump crop + bands to %TEMP%\grimvault-ocr
            // for offline segmentation tuning.
            static const bool dump_bands = [] {
               const char* e = std::getenv ("GRIMVAULT_OCR_DEBUG");
               return e && *e && std::string_view { e } != "0";
            } ();

            if (dump_bands) {
               static std::atomic<int> seq { 0 };
               const int  i   = seq++;
               const auto dir = std::filesystem::temp_directory_path () / "grimvault-ocr";
               std::error_code ec;
               std::filesystem::create_directories (dir, ec);
               cv::imwrite ((dir / (std::to_string (i) + "_crop.png")).string (), crop);
               int b = 0;
               for (const auto& band : bands) {
                  cv::imwrite ((dir / (std::to_string (i) + "_band" + std::to_string (b++) + ".png")).string (),
                     trim_cols (crop (band, cv::Range::all ())));
               }
            }

            std::string text;
            float       conf_sum = 0.0f;
            int         conf_n   = 0;

            for (const auto& band : bands) {
               const cv::Mat line = trim_cols (crop (band, cv::Range::all ()));

               std::string line_text;
               for (const auto& chunk : col_chunks (line)) {
                  auto res = (*rec)->read (line (cv::Range::all (), chunk));
                  if (!res.has_value () || res->text.empty ()) continue;

                  if (!line_text.empty ()) line_text.push_back (' ');
                  line_text += res->text;
                  conf_sum  += res->confidence;
                  ++conf_n;
               }

               if (line_text.empty ()) continue;
               if (!text.empty ()) text.push_back ('\n');
               text += line_text;
            }

            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
               std::chrono::steady_clock::now () - t0).count ();

            core::Logger::info ("pipeline: timings detect={}ms ocr={}ms lines={}/{}",
               last_detect_ms.load (), ms, conf_n, bands.size ());

            if (text.empty ()) continue;

            if (callback) {
               try {
                  callback (RecognizedTooltip {
                     .rect        = box.rect,
                     .text        = std::move (text),
                     .confidence  = conf_n ? conf_sum / conf_n : 0.0f,
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

void Pipeline::set_active_window (void* hwnd) { impl_->window.store (hwnd); }
void Pipeline::set_language (LanguageFamily f) { impl_->language.store (f); }
void Pipeline::request_immediate_scan ()
{
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
   impl_->capture_thread = std::thread { [this] { impl_->capture_loop (); } };
   impl_->vision_thread  = std::thread { [this] { impl_->vision_loop ();  } };
   impl_->ocr_thread     = std::thread { [this] { impl_->ocr_loop ();     } };

   core::Logger::info ("pipeline: started (active_fps={:.1f})", impl_->config.active_fps);
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
