#pragma once

#include <gv/capture/frame.h>
#include <gv/core/result.h>

#include <chrono>
#include <string_view>

namespace gv::capture {

class ICaptureStrategy
{
public:
   virtual ~ICaptureStrategy () = default;

   // One-time setup (create D3D11 device, open WGC session, etc.).
   virtual core::Result<void> initialize () = 0;

   // Release all resources. Safe to call multiple times.
   virtual void shutdown () noexcept = 0;

   // Capture the full content of a window. window is HWND on Windows.
   virtual core::Result<Frame> capture_window  (void* window)  = 0;

   // Capture a monitor's content. monitor is HMONITOR on Windows.
   virtual core::Result<Frame> capture_monitor (void* monitor) = 0;

   // Stable name (e.g. "wgc.borderless", "wgc.bordered", "dxgi.duplication",
   // "gdi.bitblt", "fake"). Used by Diagnostics + the probe selector.
   virtual std::string_view name () const noexcept = 0;

   // Human-readable explanation of why this strategy is in use (e.g.
   // "Windows 11; IsBorderRequired supported"). Shown in the Diagnostics page.
   virtual std::string_view reason () const noexcept = 0;

   // --- Optional continuous capture --------------------------------------
   //
   // Strategies that maintain a persistent capture session (WGC has a
   // FramePool callback model) override these for lower per-frame latency.
   // Default impls report unsupported; pipeline falls back to per-call
   // capture_window / capture_monitor.

   virtual bool supports_continuous () const noexcept { return false; }

   virtual core::Result<void> start_continuous (void* target, bool is_window)
   {
      (void) target; (void) is_window;
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "continuous capture not supported by this strategy"));
   }

   virtual core::Result<Frame> latest_frame (std::chrono::milliseconds timeout)
   {
      (void) timeout;
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "continuous capture not active"));
   }

   virtual void stop_continuous () noexcept {}
};

} // namespace gv::capture
