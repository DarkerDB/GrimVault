#pragma once

#include <gv/capture/i_capture_strategy.h>

#include <memory>

namespace gv::capture {

// Windows.Graphics.Capture strategy. When supported (Win10 build 20348+ /
// Win11 21H2+), opts out of the yellow capture border by setting
// IsBorderRequired = false. On older Windows 10 (17134..20347) it falls
// back to standard WGC with the border visible.
//
// IsCursorCaptureEnabled is set to false to avoid drawing the mouse cursor
// over the game UI (cleaner OCR input).
class WgcStrategy : public ICaptureStrategy
{
public:
   // True when this Windows build exposes GraphicsCaptureSession
   // .IsBorderRequired, i.e. WGC can capture without the yellow border.
   // Static so the default ladder can be ordered before any instance is
   // initialized.
   static bool borderless_capture_supported () noexcept;

   WgcStrategy ();
   ~WgcStrategy () override;

   core::Result<void>  initialize ()                       override;
   void                shutdown   ()         noexcept       override;
   core::Result<Frame> capture_window  (void* window)       override;
   core::Result<Frame> capture_monitor (void* monitor)      override;

   bool                supports_continuous () const noexcept override;
   core::Result<void>  start_continuous (void* target, bool is_window) override;
   core::Result<Frame> latest_frame (std::chrono::milliseconds timeout) override;
   void                stop_continuous  () noexcept override;

   std::string_view name   () const noexcept override;
   std::string_view reason () const noexcept override;

   // True iff IsBorderRequired is available and being set to false. The
   // probe ladder calls this AFTER initialize() to decide whether to keep
   // this instance or fall through to a fallback.
   bool borderless () const noexcept;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::capture
