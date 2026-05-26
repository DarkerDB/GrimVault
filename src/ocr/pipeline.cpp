#include <gv/ocr/pipeline.h>
#include <gv/core/logger.h>
#include <gv/core/spsc_queue.h>

#include <opencv2/imgproc.hpp>

#include <atomic>
#include <thread>

namespace gv::ocr {

namespace {

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

   // One-shot bypass of the stability gate; set by request_immediate_scan,
   // consumed and cleared by vision_loop.
   std::atomic<bool>         force_scan { false };

   void capture_loop ()
   {
      auto active_interval = std::chrono::duration_cast<std::chrono::milliseconds> (
         std::chrono::duration<double> (1.0 / config.active_fps));

      void* current_target = window.load ();
      bool  using_continuous = false;

      // Try continuous mode if the strategy supports it.
      if (capture.current ().supports_continuous ()) {
         auto r = capture.current ().start_continuous (current_target, /*is_window=*/ current_target != nullptr);
         using_continuous = r.has_value ();
         if (!using_continuous) {
            core::Logger::warn ("pipeline: continuous start failed: {}; falling back to per-call",
               r.error ().message);
         }
      }

      while (running.load (std::memory_order_relaxed)) {
         void* now_target = window.load ();
         if (using_continuous && now_target != current_target) {
            capture.current ().stop_continuous ();
            current_target = now_target;
            auto r = capture.current ().start_continuous (current_target, current_target != nullptr);
            if (!r.has_value ()) {
               core::Logger::warn ("pipeline: continuous re-start failed: {}", r.error ().message);
               using_continuous = false;
            }
         }

         core::Result<capture::Frame> frame_res = core::fail (core::Error {
            core::ErrorKind::Capture, "init" });

         if (using_continuous) {
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

         if (!using_continuous) {
            std::this_thread::sleep_for (active_interval);
         }
      }

      if (using_continuous) {
         capture.current ().stop_continuous ();
      }
   }

   void vision_loop ()
   {
      // Stability gate state — owned exclusively by this loop.
      std::vector<vision::TooltipBox> last_boxes;
      int                              stable_count = 0;

      while (running.load (std::memory_order_relaxed)) {
         auto* head = capture_q.front ();
         if (!head) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
         }

         capture::Frame frame = std::move (*head);
         capture_q.pop ();

         auto boxes_res = detector.detect (frame);

         if (!boxes_res.has_value () || boxes_res->empty ()) {
            stable_count = 0;
            last_boxes.clear ();
            continue;
         }

         const bool forced = force_scan.exchange (false);

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

         while (running.load () && !vision_q.try_push (VisionOut { std::move (frame), *boxes_res })) {
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

            auto res = (*rec)->read (crop);
            if (!res.has_value ()) {
               core::Logger::warn ("pipeline: ocr read failed: {}", res.error ().message);
               continue;
            }

            if (callback) {
               callback (RecognizedTooltip {
                  .rect        = box.rect,
                  .text        = std::move (res->text),
                  .confidence  = res->confidence,
                  .captured_at = item.frame.timestamp,
               });
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

void Pipeline::set_active_window (void* hwnd) { impl_->window.store (hwnd); }
void Pipeline::set_language (LanguageFamily f) { impl_->language.store (f); }
void Pipeline::request_immediate_scan ()       { impl_->force_scan.store (true); }

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
