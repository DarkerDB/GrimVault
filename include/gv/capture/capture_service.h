#pragma once

#include <gv/capture/i_capture_strategy.h>
#include <gv/core/result.h>

#include <chrono>
#include <memory>
#include <string_view>
#include <vector>

namespace gv::capture {

// Orchestrates one or more ICaptureStrategy implementations. Picks the best
// available strategy at startup, then advances through the same ladder after
// repeated runtime capture failures.
//
// Diagnostics page can ask CaptureService to enumerate available strategies
// and switch between them at runtime (gv.capture.switch-strategy command).
class CaptureService
{
public:
   struct Config {
      int failure_threshold = 3;
   };

   using Strategies = std::vector<std::unique_ptr<ICaptureStrategy>>;

   ~CaptureService ();

   CaptureService (const CaptureService&)            = delete;
   CaptureService& operator= (const CaptureService&) = delete;

   // Create and initialize the highest-scoring available strategy.
   static core::Result<std::unique_ptr<CaptureService>> create ();

   // Inject an ordered strategy ladder. Used by deterministic tests and by
   // alternate hosts that provide their own capture backend.
   static core::Result<std::unique_ptr<CaptureService>> create (
      Strategies strategies,
      Config     config
   );

   // The strategy currently producing frames.
   ICaptureStrategy& current () noexcept;

   core::Result<Frame> capture_window  (void* window);
   core::Result<Frame> capture_monitor (void* monitor);

   bool               supports_continuous () const noexcept;
   core::Result<void> start_continuous (
      void* target,
      bool  is_window
   );
   core::Result<Frame> latest_frame (std::chrono::milliseconds timeout);
   void                stop_continuous () noexcept;

   // Switch to a different strategy by name (e.g. for diagnostics).
   core::Result<void> switch_to (std::string_view strategy_name);

   // Strategies that initialized successfully during this process. Unprobed
   // candidates are omitted instead of being reported as available.
   std::vector<std::string_view> available () const;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;

   explicit CaptureService (std::unique_ptr<Impl> impl);
};

} // namespace gv::capture
