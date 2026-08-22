#include <gv/capture/capture_service.h>
#include <gv/core/logger.h>

#include "dxgi_dup_strategy.h"
#include "gdi_strategy.h"
#include "wgc_strategy.h"

#include <string>
#include <utility>

namespace gv::capture {

namespace {

   enum class Availability { Unknown, Available, Unavailable };

   bool matches (std::string_view actual, std::string_view requested)
   {
      if (actual == requested) return true;

      if ((requested == "wgc" || requested == "wgc.borderless"
             || requested == "wgc.bordered")
            && actual.starts_with ("wgc.")) return true;

      if ((requested == "dxgi" || requested == "dxgi.duplication")
            && actual == "dxgi.duplication") return true;

      if ((requested == "gdi" || requested == "gdi.bitblt")
            && actual == "gdi.bitblt") return true;

      return false;
   }

   CaptureBackend backend (std::string_view name) noexcept
   {
      if (name.starts_with ("wgc"))  return CaptureBackend::Wgc;
      if (name.starts_with ("dxgi")) return CaptureBackend::Dxgi;
      if (name.starts_with ("gdi"))  return CaptureBackend::Gdi;
      return CaptureBackend::Unknown;
   }

   bool automatic (std::string_view name) noexcept
   {
      return backend (name) != CaptureBackend::Gdi;
   }

   CaptureService::Strategies default_strategies ()
   {
      // Where WGC cannot suppress Windows' yellow capture border (no
      // IsBorderRequired — Windows 10), prefer the borderless backends and
      // keep bordered WGC as the last automatic resort.
      CaptureService::Strategies strategies;
      auto wgc = std::make_unique<WgcStrategy> ();

      if (WgcStrategy::borderless_capture_supported ()) {
         strategies.push_back (std::move (wgc));
         strategies.push_back (std::make_unique<DxgiDuplicationStrategy> ());
      } else {
         strategies.push_back (std::make_unique<DxgiDuplicationStrategy> ());
         strategies.push_back (std::move (wgc));
      }

      strategies.push_back (std::make_unique<GdiBitBltStrategy> ());

      return strategies;
   }

} // namespace

struct CaptureService::Impl
{
   struct Entry {
      std::unique_ptr<ICaptureStrategy> strategy;
      Availability                      availability = Availability::Unknown;
   };

   std::vector<Entry> entries;
   Config             config;
   CaptureMode        mode = CaptureMode::Automatic;
   std::size_t        active = 0;
   int                failures = 0;
   bool               fallback_exhausted = false;
   bool               have_target = false;
   bool               target_is_window = false;
   void*              target = nullptr;

   explicit Impl (Strategies strategies, Config value) : config (value)
   {
      entries.reserve (strategies.size ());
      for (auto& strategy : strategies) {
         entries.push_back (Entry { .strategy = std::move (strategy) });
      }
   }

   ICaptureStrategy& current () noexcept
   {
      return *entries [active].strategy;
   }

   const ICaptureStrategy& current () const noexcept
   {
      return *entries [active].strategy;
   }

   bool pinned () const noexcept
   {
      return mode != CaptureMode::Automatic;
   }

   core::Result<void> initialize (std::size_t index)
   {
      auto& entry = entries [index];
      auto result = entry.strategy->initialize ();
      entry.availability = result.has_value ()
         ? Availability::Available
         : Availability::Unavailable;

      if (!result.has_value ()) entry.strategy->shutdown ();
      return result;
   }

   bool restore_preferred ()
   {
      if (active == 0) return automatic (current ().name ());

      const auto previous = std::string { current ().name () };
      for (std::size_t i = 0; i < active; ++i) {
         if (!automatic (entries [i].strategy->name ())) continue;
         auto initialized = initialize (i);
         if (!initialized.has_value ()) continue;

         current ().stop_continuous ();
         current ().shutdown ();
         active = i;
         core::Logger::info (
            "capture: restored preferred strategy {} for new target; previous={}",
            current ().name (), previous);
         return true;
      }

      return automatic (current ().name ());
   }

   void update_target (void* value, bool is_window)
   {
      if (have_target && target == value && target_is_window == is_window) return;

      have_target        = true;
      target             = value;
      target_is_window   = is_window;
      failures           = 0;
      fallback_exhausted = false;

      // DXGI availability can change when the game moves to another adapter
      // or monitor. Let later strategies be probed again for the new target.
      for (std::size_t i = active + 1; i < entries.size (); ++i) {
         entries [i].availability = Availability::Unknown;
      }

      if (!pinned ()) restore_preferred ();
   }

   void note_success () noexcept
   {
      failures = 0;
      fallback_exhausted = false;
   }

   bool activate_next (const core::Error& cause)
   {
      if (fallback_exhausted) return false;

      if (pinned ()) {
         failures = 0;
         fallback_exhausted = true;
         core::Logger::error (
            "capture: {} failed and capture mode {} pins it: {}",
            current ().name (), capture_mode_name (mode), cause.message);
         return false;
      }

      const auto previous = std::string { current ().name () };
      for (std::size_t i = active + 1; i < entries.size (); ++i) {
         if (!automatic (entries [i].strategy->name ())) continue;
         auto initialized = initialize (i);
         if (!initialized.has_value ()) {
            core::Logger::warn ("capture: {} unavailable during failover: {}",
               entries [i].strategy->name (), initialized.error ().message);
            continue;
         }

         current ().stop_continuous ();
         current ().shutdown ();
         active = i;
         failures = 0;
         fallback_exhausted = false;

         core::Logger::warn (
            "capture: switched from {} to {} after repeated failures: {}",
            previous, current ().name (), cause.message);
         return true;
      }

      failures = 0;
      fallback_exhausted = true;
      core::Logger::error (
         "capture: {} failed and no later strategy is available: {}",
         previous, cause.message);
      return false;
   }

   template <typename Capture>
   core::Result<Frame> capture (Capture&& call)
   {
      while (true) {
         auto result = call (current ());
         if (result.has_value ()) {
            result->backend = backend (current ().name ());
            note_success ();
            return result;
         }

         ++failures;
         if (failures < config.failure_threshold || fallback_exhausted) return result;

         const auto cause = result.error ();
         if (!activate_next (cause)) return result;
      }
   }
};

CaptureService::CaptureService (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}

CaptureService::~CaptureService ()
{
   if (!impl_) return;
   for (auto& entry : impl_->entries) entry.strategy->shutdown ();
}

core::Result<std::unique_ptr<CaptureService>> CaptureService::create ()
{
   return create (default_strategies (), Config {});
}

core::Result<std::unique_ptr<CaptureService>> CaptureService::create (
   Strategies strategies,
   Config     config
) {
   if (strategies.empty ()) {
      return core::fail (core::Error::make (
         core::ErrorKind::InvalidArgument, "capture: strategy ladder is empty"));
   }
   if (config.failure_threshold < 1) {
      return core::fail (core::Error::make (
         core::ErrorKind::InvalidArgument,
         "capture: failure threshold must be at least one"));
   }

   auto impl = std::make_unique<Impl> (std::move (strategies), config);
   std::string last_error;

   for (std::size_t i = 0; i < impl->entries.size (); ++i) {
      if (!automatic (impl->entries [i].strategy->name ())) continue;
      auto initialized = impl->initialize (i);
      if (initialized.has_value ()) {
         impl->active = i;
         core::Logger::info ("capture: probe selected {}: {}",
            impl->current ().name (), impl->current ().reason ());
         return std::unique_ptr<CaptureService> {
            new CaptureService (std::move (impl)) };
      }

      last_error = initialized.error ().message;
      core::Logger::warn ("capture: {} unavailable: {}",
         impl->entries [i].strategy->name (), last_error);
   }

   return core::fail (core::Error::make (
      core::ErrorKind::Capture,
      "capture: no working strategy on this system; last error: {}",
      last_error));
}

ICaptureStrategy& CaptureService::current () noexcept
{
   return impl_->current ();
}

core::Result<Frame> CaptureService::capture_window (void* window)
{
   impl_->update_target (window, true);
   return impl_->capture ([window] (ICaptureStrategy& strategy) {
      return strategy.capture_window (window);
   });
}

core::Result<Frame> CaptureService::capture_monitor (void* monitor)
{
   impl_->update_target (monitor, false);
   return impl_->capture ([monitor] (ICaptureStrategy& strategy) {
      return strategy.capture_monitor (monitor);
   });
}

bool CaptureService::supports_continuous () const noexcept
{
   return impl_->current ().supports_continuous ();
}

core::Result<void> CaptureService::start_continuous (void* target, bool is_window)
{
   impl_->update_target (target, is_window);
   auto result = impl_->current ().start_continuous (target, is_window);
   if (result.has_value ()) impl_->note_success ();
   return result;
}

core::Result<Frame> CaptureService::latest_frame (std::chrono::milliseconds timeout)
{
   auto result = impl_->current ().latest_frame (timeout);
   if (result.has_value ()) {
      result->backend = backend (impl_->current ().name ());
      impl_->note_success ();
   }
   return result;
}

void CaptureService::stop_continuous () noexcept
{
   impl_->current ().stop_continuous ();
}

bool CaptureService::demote (const core::Error& cause)
{
   return impl_->activate_next (cause);
}

core::Result<void> CaptureService::switch_to (std::string_view strategy_name)
{
   for (std::size_t i = 0; i < impl_->entries.size (); ++i) {
      auto& candidate = *impl_->entries [i].strategy;
      if (!matches (candidate.name (), strategy_name)) continue;
      if (i == impl_->active) return {};

      auto initialized = impl_->initialize (i);
      if (!initialized.has_value ()) return core::fail (initialized.error ());

      impl_->current ().stop_continuous ();
      impl_->current ().shutdown ();
      impl_->active = i;
      impl_->failures = 0;
      impl_->fallback_exhausted = false;
      impl_->have_target = false;

      core::Logger::info ("capture: manually switched to {}: {}",
         impl_->current ().name (), impl_->current ().reason ());
      return {};
   }

   return core::fail (core::Error::make (
      core::ErrorKind::InvalidArgument,
      "capture: unknown strategy '{}'", strategy_name));
}

core::Result<void> CaptureService::set_mode (CaptureMode mode)
{
   if (mode == impl_->mode) return {};

   if (mode == CaptureMode::Automatic) {
      if (!impl_->restore_preferred ()) {
         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "capture: no automatic backend is available"));
      }
      impl_->mode = mode;
      impl_->failures = 0;
      impl_->fallback_exhausted = false;
      core::Logger::info ("capture: mode automatic; strategy {}",
         impl_->current ().name ());
      return {};
   }

   if (auto switched = switch_to (capture_mode_name (mode));
       !switched.has_value ()) {
      return switched;
   }

   impl_->mode = mode;
   core::Logger::info ("capture: mode {} pins strategy {}",
      capture_mode_name (mode), impl_->current ().name ());
   return {};
}

CaptureMode CaptureService::mode () const noexcept
{
   return impl_->mode;
}

std::vector<std::string_view> CaptureService::available () const
{
   std::vector<std::string_view> result;
   for (const auto& entry : impl_->entries) {
      if (entry.availability == Availability::Available) {
         result.push_back (entry.strategy->name ());
      }
   }
   return result;
}

} // namespace gv::capture
