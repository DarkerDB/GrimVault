#include <gv/core/environment.h>
#include <gv/core/logger.h>
#include <gv/ocr/capture_policy.h>
#include <gv/ocr/evidence.h>
#include <gv/ocr/pipeline.h>
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

Pipeline::Impl::Impl (capture::CaptureService& c, vision::TooltipDetector& d, LanguageRegistry& r,
                      Config cfg)
    : capture (c),
      detector (d),
      registry (r),
      config (std::move (cfg)),
      evidence (config.evidence_dir, config.evidence_max_bytes),
      capture_fps (std::clamp (config.capture_fps, minimum_capture_fps, 60.0)),
      capture_mode (c.mode ()),
      language (config.language)
{
}

void Pipeline::Impl::queue_scans (int requested)
{
   int pending = force_scans.load (std::memory_order_relaxed);
   while (pending < requested &&
          !force_scans.compare_exchange_weak (pending, requested, std::memory_order_relaxed)) {}
}

std::chrono::milliseconds Pipeline::Impl::current_interval () const
{
   const bool active = tracking.load (std::memory_order_relaxed);
   const double fps = frame_fps (
      capture_fps.load (std::memory_order_relaxed), config.performance_fps, config.tracking_fps,
      config.performance_tracking_fps, performance_mode.load (std::memory_order_relaxed), active);

   return std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::duration<double> (1.0 / fps));
}

std::chrono::milliseconds Pipeline::Impl::detection_interval () const
{
   const double fps =
      detector_fps (capture_fps.load (std::memory_order_relaxed), config.performance_fps,
                    performance_mode.load (std::memory_order_relaxed));
   return std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::duration<double> (1.0 / fps));
}

Pipeline::Pipeline (capture::CaptureService& capture, vision::TooltipDetector& detector,
                    LanguageRegistry& registry, Config config)
{
   impl_ = std::make_unique<Impl> (capture, detector, registry, std::move (config));
}

Pipeline::~Pipeline ()
{
   stop ();
}

void Pipeline::on_anchor (AnchorCallback cb)
{
   impl_->anchor_cb = std::move (cb);
}
void Pipeline::on_anchor_lost (AnchorLostCallback cb)
{
   impl_->anchor_lost_cb = std::move (cb);
}
void Pipeline::on_sample (SampleCallback cb)
{
   impl_->sample_cb = std::move (cb);
}

void Pipeline::set_active_window (void* hwnd)
{
   if (impl_->window.exchange (hwnd) != hwnd) {
      core::log::vision.event ("capture_target", {
                                                    { "state", hwnd ? "acquired" : "released" },
                                                 });
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
   const double bounded = std::clamp (fps, minimum_capture_fps, 60.0);
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
void Pipeline::set_language (std::string locale)
{
   const auto family = family_of (locale);
   {
      std::lock_guard lock { impl_->language_lock };
      if (impl_->language == family && impl_->locale == locale) return;
      impl_->language = family;
      impl_->locale = std::move (locale);
   }
   impl_->force_scans.store (0, std::memory_order_relaxed);
   impl_->generation.fetch_add (1, std::memory_order_relaxed);
   impl_->reset_requested.store (true, std::memory_order_relaxed);
   core::Logger::info ("pipeline: OCR language → {}", family_dir (family));
}

bool Pipeline::is_current (std::uint64_t value) const noexcept
{
   return impl_->generation.load (std::memory_order_relaxed) == value;
}
void Pipeline::set_detect_only (bool on)
{
   impl_->detect_only.store (on);
}
void Pipeline::request_immediate_scan ()
{
   if (!impl_->enabled.load (std::memory_order_relaxed)) return;
   constexpr int k_forced_burst = 2;
   impl_->queue_scans (k_forced_burst);
}

void Pipeline::record_evidence (std::uint64_t generation, std::string event,
                                std::unordered_map<std::string, std::string> fields)
{
   impl_->evidence.event (generation, std::move (event), std::move (fields));
}

core::Result<void> Pipeline::start (TooltipCallback on_tooltip)
{
   if (impl_->running.exchange (true)) {
      return core::fail (
         core::Error::make (core::ErrorKind::Internal, "pipeline: already running"));
   }

   impl_->callback = std::move (on_tooltip);

   const auto warm_started = std::chrono::steady_clock::now ();
   LanguageFamily warm_family;
   {
      std::lock_guard lock { impl_->language_lock };
      warm_family = impl_->language;
   }
   auto warm = impl_->registry.acquire (warm_family);
   const auto warm_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                           std::chrono::steady_clock::now () - warm_started)
                           .count ();
   core::log::ocr.event ("model_prewarm", {
                                             { "family", std::string { family_dir (warm_family) } },
                                             { "elapsed_ms", std::to_string (warm_ms) },
                                             { "ok", warm.has_value () ? "1" : "0" },
                                          });
   if (!warm.has_value ()) {
      core::Logger::warn ("pipeline: OCR prewarm failed: {}", warm.error ().message);
   }
   impl_->capture_thread = std::thread { [this] { impl_->capture_loop (); } };
   impl_->vision_thread = std::thread { [this] { impl_->vision_loop (); } };
   impl_->ocr_thread = std::thread { [this] { impl_->ocr_loop (); } };

   core::Logger::info (
      "pipeline: started (detector_fps={:.1f}, performance_fps={:.1f}, "
      "tracking_fps={:.1f}, performance_tracking_fps={:.1f})",
      impl_->capture_fps.load (std::memory_order_relaxed), impl_->config.performance_fps,
      impl_->config.tracking_fps, impl_->config.performance_tracking_fps);
   return {};
}

void Pipeline::stop () noexcept
{
   if (!impl_) return;
   if (!impl_->running.exchange (false)) return;

   if (impl_->capture_thread.joinable ()) impl_->capture_thread.join ();
   if (impl_->vision_thread.joinable ()) impl_->vision_thread.join ();
   if (impl_->ocr_thread.joinable ()) impl_->ocr_thread.join ();

   core::Logger::info ("pipeline: stopped");
}

}  // namespace gv::ocr
