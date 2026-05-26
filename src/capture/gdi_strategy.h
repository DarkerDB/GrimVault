#pragma once

#include <gv/capture/i_capture_strategy.h>

#include <memory>

namespace gv::capture {

// GDI BitBlt. Ultimate fallback — works on Vista+, even on systems without
// D3D11. Won't correctly capture hardware-accelerated fullscreen surfaces
// (DXGI exclusive fullscreen), but borderless-windowed games are fine.
class GdiBitBltStrategy : public ICaptureStrategy
{
public:
   GdiBitBltStrategy ();
   ~GdiBitBltStrategy () override;

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
