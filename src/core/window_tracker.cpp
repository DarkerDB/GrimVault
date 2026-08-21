#include <gv/core/window_tracker.h>
#include <gv/core/logger.h>

#include <Windows.h>
#include <ShellScalingApi.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
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

   bool class_matches (HWND hwnd, const std::wstring& want)
   {
      if (want.empty ()) return true;

      wchar_t buf [128] {};
      const int len = ::GetClassNameW (hwnd, buf, 128);
      if (len <= 0) return false;
      return want == std::wstring_view { buf, static_cast<std::size_t> (len) };
   }

   bool process_matches (HWND hwnd, const std::wstring& want)
   {
      if (want.empty ()) return true;

      DWORD pid = 0;
      ::GetWindowThreadProcessId (hwnd, &pid);
      if (!pid) return false;

      HANDLE proc = ::OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!proc) return false;

      wchar_t buf [MAX_PATH] {};
      DWORD   len = MAX_PATH;
      const bool ok = ::QueryFullProcessImageNameW (proc, 0, buf, &len);
      ::CloseHandle (proc);
      if (!ok) return false;

      std::wstring_view path { buf, len };
      const auto slash = path.find_last_of (L"\\/");
      const auto name  = (slash == std::wstring_view::npos)
         ? path
         : path.substr (slash + 1);

      return ::CompareStringOrdinal (
         name.data (), static_cast<int> (name.size ()),
         want.data (), static_cast<int> (want.size ()),
         /*bIgnoreCase=*/ TRUE) == CSTR_EQUAL;
   }

   bool client_rect_screen (HWND hwnd, RECT& out)
   {
      RECT client {};
      POINT origin {};
      if (!::GetClientRect (hwnd, &client) || !::ClientToScreen (hwnd, &origin)) {
         return false;
      }

      out = {
         origin.x,
         origin.y,
         origin.x + client.right - client.left,
         origin.y + client.bottom - client.top,
      };
      return out.right > out.left && out.bottom > out.top;
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
   std::optional<WindowEvent>      last_emitted;

   HWINEVENTHOOK                   h_location = nullptr;
   HWINEVENTHOOK                   h_foreground = nullptr;
   HWINEVENTHOOK                   h_min_start  = nullptr;
   HWINEVENTHOOK                   h_min_end    = nullptr;

   std::wstring                    needle;
   std::wstring                    want_class;
   std::wstring                    want_process;

   HWINEVENTHOOK                   h_destroy = nullptr;
   UINT_PTR                        reconcile_timer = 0;

   // Single instance lookup for the WinEventProc trampoline. Only one tracker
   // is needed in practice (the game window); if that changes, switch to a
   // hook→instance map.
   static Impl*                    s_instance;

   // Full candidate check. Title alone is spoofable by any window that
   // happens to mention the game (browser tab, Discord); class + process
   // pin the match to the actual game window.
   bool window_matches (HWND hwnd)
   {
      return class_matches (hwnd, want_class)
         && process_matches (hwnd, want_process)
         && title_matches (hwnd, needle);
   }

   bool target_is_valid (HWND hwnd) const
   {
      return hwnd && ::IsWindow (hwnd)
         && class_matches (hwnd, want_class)
         && process_matches (hwnd, want_process);
   }

   static bool same_rect (const WindowRect& a, const WindowRect& b)
   {
      return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
   }

   static bool same_event (const WindowEvent& a, const WindowEvent& b)
   {
      return a.hwnd == b.hwnd
         && same_rect (a.bounds, b.bounds)
         && same_rect (a.monitor, b.monitor)
         && a.scale == b.scale
         && a.visible == b.visible
         && a.focused == b.focused;
   }

   // The target window is gone (destroyed or unreadable). Tell the consumer
   // so overlays hide, and clear the lock so late acquisition re-arms for
   // the next game launch.
   void emit_gone (HWND hwnd)
   {
      WindowEvent ev;
      ev.hwnd    = hwnd;
      ev.visible = false;
      ev.focused = false;

      {
         std::lock_guard lk { lock };
         if (target == hwnd) target = nullptr;
         if (last_emitted && same_event (*last_emitted, ev)) return;
         last_emitted = ev;
      }

      if (handler) {
         try { handler (ev); }
         catch (...) { Logger::error ("window_tracker: handler threw"); }
      }
   }

   void emit (HWND hwnd)
   {
      if (!hwnd) return;

      WindowEvent ev;
      ev.hwnd = hwnd;

      RECT r {};
      if (!::IsWindow (hwnd) || !client_rect_screen (hwnd, r)) {
         emit_gone (hwnd);
         return;
      }

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

      {
         std::lock_guard lk { lock };
         if (last_emitted && same_event (*last_emitted, ev)) return;
         last_emitted = ev;
      }

      if (handler) {
         try { handler (ev); }
         catch (...) { Logger::error ("window_tracker: handler threw"); }
      }
   }

   HWND find_candidate ()
   {
      // Prefer the foreground game when multiple matching windows exist.
      if (HWND foreground = ::GetForegroundWindow ();
          foreground && window_matches (foreground)) {
         return foreground;
      }

      struct ProbeCtx { Impl* self; HWND found; } ctx { this, nullptr };
      ::EnumWindows ([] (HWND hwnd, LPARAM lp) -> BOOL {
         auto* c = reinterpret_cast<ProbeCtx*> (lp);
         if (!::IsWindowVisible (hwnd)) return TRUE;
         if (!c->self->window_matches (hwnd)) return TRUE;
         c->found = hwnd;
         return FALSE;
      }, reinterpret_cast<LPARAM> (&ctx));
      return ctx.found;
   }

   void reconcile ()
   {
      HWND current;
      { std::lock_guard lk { lock }; current = target; }

      if (current && !target_is_valid (current)) {
         Logger::info ("window_tracker: tracked game window became invalid; reacquiring");
         emit_gone (current);
         current = nullptr;
      }

      // A game may keep its startup HWND alive while replacing or hiding the
      // actual render window. Prefer a newly matching foreground/visible HWND
      // even when the old handle is still technically valid.
      if (HWND preferred = find_candidate (); preferred && preferred != current) {
         { std::lock_guard lk { lock }; target = preferred; }
         Logger::info ("window_tracker: rebound game window hwnd={} -> {}",
            reinterpret_cast<std::uintptr_t> (current),
            reinterpret_cast<std::uintptr_t> (preferred));
         emit (preferred);
         return;
      }

      if (!current) {
         return;
      }

      // Hooks are the fast path; this deduplicated snapshot catches missed
      // focus, minimize, geometry, and visibility transitions.
      emit (current);
   }

   void on_event (DWORD event, HWND hwnd, LONG id_object)
   {
      if (id_object != OBJID_WINDOW) return;

      HWND cur_target;
      { std::lock_guard lk { lock }; cur_target = target; }

      // Game window destroyed: notify + unlock so the next launch re-arms.
      if (event == EVENT_OBJECT_DESTROY) {
         if (cur_target && hwnd == cur_target) emit_gone (cur_target);
         return;
      }

      // Foreground change: if the new foreground is the game, lock & emit;
      // if we already have a target and lost it, emit a focus-lost event
      // for the same target so the consumer can hide the overlay.
      if (event == EVENT_SYSTEM_FOREGROUND) {
         if (hwnd && window_matches (hwnd)) {
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
      if (cur_target && hwnd == cur_target) {
         emit (cur_target);
         return;
      }

      // Late acquisition: if we don't have a target yet and the event is on
      // a window that fully matches, lock onto it. Cheap because the check
      // is bypassed for events on non-target windows once locked.
      if (!cur_target && hwnd && window_matches (hwnd)) {
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

      h_destroy = ::SetWinEventHook (
         EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
         nullptr, &win_event_proc, 0, 0,
         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

      if (!h_location || !h_foreground || !h_min_start || !h_min_end || !h_destroy) {
         Logger::error ("window_tracker: failed to activate one or more event listeners");
      } else {
         Logger::info ("window_tracker: event listeners active (target='{}' class='{}' process='{}')",
            std::string { config.title_substring },
            std::string { config.window_class },
            std::string { config.process_name });
      }

      if (config.reconcile_interval_ms > 0) {
         reconcile_timer = ::SetTimer (nullptr, 0, config.reconcile_interval_ms, nullptr);
         if (!reconcile_timer) {
            Logger::warn ("window_tracker: failed to start reconciliation timer");
         } else {
            Logger::info ("window_tracker: reconciliation poll={}ms",
               config.reconcile_interval_ms);
         }
      }

      { std::lock_guard lk { ready_mtx }; set = true; }
      ready.notify_one ();

      if (config.emit_on_start) reconcile ();

      MSG msg;
      while (::GetMessage (&msg, nullptr, 0, 0) > 0) {
         if (msg.message == WM_QUIT) break;
         if (msg.message == WM_TIMER && msg.wParam == reconcile_timer) {
            reconcile ();
            continue;
         }
         ::TranslateMessage (&msg);
         ::DispatchMessage  (&msg);
      }

      if (reconcile_timer) {
         ::KillTimer (nullptr, reconcile_timer);
         reconcile_timer = 0;
      }

      if (h_location)   ::UnhookWinEvent (h_location);
      if (h_foreground) ::UnhookWinEvent (h_foreground);
      if (h_min_start)  ::UnhookWinEvent (h_min_start);
      if (h_min_end)    ::UnhookWinEvent (h_min_end);
      if (h_destroy)    ::UnhookWinEvent (h_destroy);

      h_location = h_foreground = h_min_start = h_min_end = h_destroy = nullptr;
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
   impl->config       = std::move (cfg);
   impl->handler      = std::move (on_event);
   impl->needle       = utf8_to_wide (impl->config.title_substring);
   impl->want_class   = utf8_to_wide (impl->config.window_class);
   impl->want_process = utf8_to_wide (impl->config.process_name);
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
