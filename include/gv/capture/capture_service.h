#pragma once

#include <gv/capture/i_capture_strategy.h>
#include <gv/core/result.h>

#include <memory>
#include <string_view>
#include <vector>

namespace gv::capture {

// Orchestrates one or more ICaptureStrategy implementations. Picks the best
// available strategy at startup (the probe ladder) and exposes a stable
// facade for the rest of the app.
//
// Diagnostics page can ask CaptureService to enumerate available strategies
// and switch between them at runtime (gv.capture.switch-strategy command).
class CaptureService
{
public:
   ~CaptureService ();

   CaptureService (const CaptureService&)            = delete;
   CaptureService& operator= (const CaptureService&) = delete;

   // Create and initialize the highest-scoring available strategy.
   static core::Result<std::unique_ptr<CaptureService>> create ();

   // The strategy currently producing frames.
   ICaptureStrategy& current () noexcept;

   // Switch to a different strategy by name (e.g. for diagnostics).
   core::Result<void> switch_to (std::string_view strategy_name);

   // Names of all strategies known to this build.
   std::vector<std::string_view> available () const;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;

   explicit CaptureService (std::unique_ptr<Impl> impl);
};

} // namespace gv::capture
