#pragma once

#include <gv/core/result.h>

#include <functional>
#include <memory>
#include <string>

namespace gv::core {

struct WindowRect {
   int x = 0;
   int y = 0;
   int w = 0;
   int h = 0;
};

// Snapshot of the tracked window's state. Delivered to the WindowTracker
// handler from the pump thread; handlers must marshal back to their own
// thread before touching Qt or other thread-affine objects.
struct WindowEvent {
   void*      hwnd     = nullptr;    // opaque HWND
   WindowRect bounds   {};           // window rect in virtual-desktop coords
   WindowRect monitor  {};           // work-area of the containing monitor
   double     scale    = 1.0;        // DPI scale (1.0 == 96 DPI)
   bool       visible  = false;      // visible AND not minimized
   bool       focused  = false;      // GetForegroundWindow() == hwnd
};

// Tracks a single Win32 window by title substring and emits a WindowEvent
// whenever it moves, resizes, gains/loses focus, or is minimized/restored.
//
// Owns a dedicated message-pump thread because SetWinEventHook with
// WINEVENT_OUTOFCONTEXT delivers events through the registering thread's
// message queue. The handler runs on that pump thread.
class WindowTracker
{
public:
   using Handler = std::function<void (const WindowEvent&)>;

   struct Config {
      std::string title_substring = "Dark and Darker";
      bool        emit_on_start   = true;     // FindWindowW probe at start
   };

   ~WindowTracker ();

   WindowTracker (const WindowTracker&)            = delete;
   WindowTracker& operator= (const WindowTracker&) = delete;

   static Result<std::unique_ptr<WindowTracker>> create (Config cfg, Handler on_event);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;

   explicit WindowTracker (std::unique_ptr<Impl> impl);
};

} // namespace gv::core
