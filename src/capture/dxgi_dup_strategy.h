#pragma once

#include <gv/capture/i_capture_strategy.h>

#include <memory>

namespace gv::capture {

// DXGI Desktop Duplication. Available on Windows 8+ (build 9200). Captures
// an entire monitor at a time; window capture requires cropping to the
// window's bounds on that monitor. Used when WGC is unavailable.
class DxgiDuplicationStrategy : public ICaptureStrategy
{
public:
   DxgiDuplicationStrategy ();
   ~DxgiDuplicationStrategy () override;

   core::Result<void>  initialize ()                      override;
   void                shutdown   ()         noexcept      override;
   core::Result<Frame> capture_window  (void* window)      override;
   core::Result<Frame> capture_monitor (void* monitor)     override;

   std::string_view name   () const noexcept override;
   std::string_view reason () const noexcept override;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::capture
