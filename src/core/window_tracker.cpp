#include <gv/core/window_tracker.h>
#include <gv/core/logger.h>

#include <Windows.h>
#include <ShellScalingApi.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#pragma comment (lib, "Shcore.lib")

namespace gv::core {

namespace {

   std::wstring utf8_to_wide (std::string_view s)
   {
      if (s.empty ()) return {};
      const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.data (),
         static_cast<int> (s.size ()), nullptr, 0);
      std::wstring out (static_cast<std::size_t> (n), L'\0');
      ::MultiByteToWideChar (CP_UTF8, 0, s.data (),
         static_cast<int> (s.size ()), out.data (), n);
      return out;
   }

   bool title_matches (HWND hwnd, const std::wstring& needle)
   {
      wchar_t buf [256] {};
      const int len = ::GetWindowTextW (hwnd, buf, 256);
      if (len <= 0) return false;
      std::wstring_view title { buf, static_cast<std::size_t> (len) };
      return title.find (needle) != std::wstring_view::npos;
   }

} // namespace

struct WindowTracker::Impl
{
   Config                          config;
   Handler                         handler;

   std::thread                     pump;
   std::atomic<bool>               running   { false };
   std::atomic<DWORD>              pump_tid  { 0 };

   std::mutex                      lock;
   HWND                            target = nullptr;

   HWINEVENTHOOK                   h_location = nullptr;
   HWINEVENTHOOK                   h_foreground = nullptr;
   HWINEVENTHOOK                   h_min_start  = nullptr;
   HWINEVENTHOOK                   h_min_end    = nullptr;

   std::wstring                    needle;

   // Single instance lookup for the WinEventProc trampoline. Only one tracker
   // is needed in practice (the game window); if that changes, switch to a
   // hook→instance map.
   static Impl*                    s_instance;

   void emit (HWND hwnd)
   {
      if (!hwnd) return;

      WindowEvent ev;
      ev.hwnd = hwnd;

      RECT r {};
      if (!::GetWindowRect (hwnd, &r)) return;

      ev.bounds = { r.left, r.top, r.right - r.left, r.bottom - r.top };

      HMONITOR mon = ::MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
      if (mon) {
         MONITORINFO mi { .cbSize = sizeof (MONITORINFO) };
         if (::GetMonitorInfoW (mon, &mi)) {
            ev.monitor = {
               mi.rcWork.left,
               mi.rcWork.top,
               mi.rcWork.right  - mi.rcWork.left,
               mi.rcWork.bottom - mi.rcWork.top,
            };
         }
         UINT dx = 96, dy = 96;
         if (::GetDpiForMonitor (mon, MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK) {
            ev.scale = static_cast<double> (dx) / 96.0;
         }
      }

      ev.visible = ::IsWindowVisible (hwnd) && !::IsIconic (hwnd);
      ev.focused = ::GetForegroundWindow () == hwnd;

      if (handler) {
         try { handler (ev); }
         catch (...) { Logger::error ("window_tracker: handler threw"); }
      }
   }

   void on_event (DWORD event, HWND hwnd, LONG id_object)
   {
      if (id_object != OBJID_WINDOW) return;

      // Foreground change: if the new foreground is the game, lock & emit;
      // if we already have a target and lost it, emit a focus-lost event
      // for the same target so the consumer can hide the overlay.
      if (event == EVENT_SYSTEM_FOREGROUND) {
         HWND cur_target;
         { std::lock_guard lk { lock }; cur_target = target; }

         if (hwnd && title_matches (hwnd, needle)) {
            { std::lock_guard lk { lock }; target = hwnd; }
            emit (hwnd);
            return;
         }

         if (cur_target) {
            emit (cur_target);
         }
         return;
      }

      // For move/resize/minimize: only emit when the event is on our target.
      HWND cur_target;
      { std::lock_guard lk { lock }; cur_target = target; }

      if (cur_target && hwnd == cur_target) {
         emit (cur_target);
         return;
      }

      // Late acquisition: if we don't have a target yet and the event is on
      // a window whose title matches, lock onto it. Cheap because title check
      // is bypassed for events on non-target windows once locked.
      if (!cur_target && hwnd && title_matches (hwnd, needle)) {
         { std::lock_guard lk { lock }; target = hwnd; }
         emit (hwnd);
      }
   }

   static void CALLBACK win_event_proc (
      HWINEVENTHOOK, DWORD event, HWND hwnd,
      LONG id_object, LONG, DWORD, DWORD)
   {
      if (Impl* self = s_instance) {
         self->on_event (event, hwnd, id_object);
      }
   }

   void pump_run (std::condition_variable& ready, std::mutex& ready_mtx, bool& set)
   {
      // Establish a message queue for this thread.
      MSG init;
      ::PeekMessage (&init, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      pump_tid.store (::GetCurrentThreadId ());

      h_location = ::SetWinEventHook (
         EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
         nullptr, &win_event_proc, 0, 0,
         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

      h_foreground = ::SetWinEventHook (
         EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
         nullptr, &win_event_proc, 0, 0,
         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

      h_min_start = ::SetWinEventHook (
         EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART,
         nullptr, &win_event_proc, 0, 0,
         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

      h_min_end = ::SetWinEventHook (
         EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND,
         nullptr, &win_event_proc, 0, 0,
         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

      if (!h_location || !h_foreground || !h_min_start || !h_min_end) {
         Logger::error ("window_tracker: failed to install one or more hooks");
      } else {
         Logger::info ("window_tracker: hooks installed (target='{}')",
            std::string { config.title_substring });
      }

      // Initial probe — if the game is already running we won't get a
      // foreground event for it.
      if (config.emit_on_start) {
         HWND found = ::FindWindowW (nullptr, needle.c_str ());
         if (found) {
            { std::lock_guard lk { lock }; target = found; }
            emit (found);
         }
      }

      { std::lock_guard lk { ready_mtx }; set = true; }
      ready.notify_one ();

      MSG msg;
      while (::GetMessage (&msg, nullptr, 0, 0) > 0) {
         if (msg.message == WM_QUIT) break;
         ::TranslateMessage (&msg);
         ::DispatchMessage  (&msg);
      }

      if (h_location)   ::UnhookWinEvent (h_location);
      if (h_foreground) ::UnhookWinEvent (h_foreground);
      if (h_min_start)  ::UnhookWinEvent (h_min_start);
      if (h_min_end)    ::UnhookWinEvent (h_min_end);

      h_location = h_foreground = h_min_start = h_min_end = nullptr;
   }
};

WindowTracker::Impl* WindowTracker::Impl::s_instance = nullptr;

WindowTracker::WindowTracker (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}

WindowTracker::~WindowTracker ()
{
   if (!impl_) return;

   impl_->running.store (false);

   if (const DWORD tid = impl_->pump_tid.load (); tid != 0) {
      ::PostThreadMessage (tid, WM_QUIT, 0, 0);
   }

   if (impl_->pump.joinable ()) impl_->pump.join ();

   if (Impl::s_instance == impl_.get ()) {
      Impl::s_instance = nullptr;
   }
}

Result<std::unique_ptr<WindowTracker>> WindowTracker::create (Config cfg, Handler on_event)
{
   if (Impl::s_instance != nullptr) {
      return fail (Error::make (ErrorKind::Internal,
         "window_tracker: another instance is already active"));
   }

   auto impl = std::make_unique<Impl> ();
   impl->config  = std::move (cfg);
   impl->handler = std::move (on_event);
   impl->needle  = utf8_to_wide (impl->config.title_substring);
   impl->running.store (true);

   Impl::s_instance = impl.get ();

   std::condition_variable ready;
   std::mutex              ready_mtx;
   bool                    is_ready = false;

   impl->pump = std::thread { [p = impl.get (), &ready, &ready_mtx, &is_ready] {
      p->pump_run (ready, ready_mtx, is_ready);
   }};

   std::unique_lock lk { ready_mtx };
   ready.wait (lk, [&] { return is_ready; });

   return std::unique_ptr<WindowTracker> { new WindowTracker (std::move (impl)) };
}

} // namespace gv::core
