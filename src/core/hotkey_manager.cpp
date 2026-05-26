#include <gv/core/hotkey_manager.h>
#include <gv/core/logger.h>

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace gv::core {

namespace {

   struct Binding {
      int                       id;
      HotkeyManager::Handler    handler;
      std::string               accelerator;
   };

   // Parse "Ctrl+Shift+F6" -> (MOD_CONTROL | MOD_SHIFT, VK_F6).
   bool parse_accelerator (std::string_view s, UINT& mods, UINT& vk)
   {
      mods = 0;
      vk   = 0;

      std::string lower { s };
      for (auto& c : lower) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));

      std::size_t i = 0;
      while (i < lower.size ()) {
         auto next = lower.find ('+', i);
         std::string part = lower.substr (i, next == std::string::npos ? std::string::npos : next - i);
         i = (next == std::string::npos) ? lower.size () : next + 1;

         if      (part == "ctrl" || part == "control") mods |= MOD_CONTROL;
         else if (part == "shift")                     mods |= MOD_SHIFT;
         else if (part == "alt")                       mods |= MOD_ALT;
         else if (part == "win" || part == "meta")     mods |= MOD_WIN;
         else if (part.size () >= 2 && part [0] == 'f') {
            try { vk = VK_F1 + std::stoi (part.substr (1)) - 1; }
            catch (...) { return false; }
         } else if (part.size () == 1) {
            vk = static_cast<UINT> (std::toupper (static_cast<unsigned char> (part [0])));
         } else {
            return false;
         }
      }

      return vk != 0;
   }

} // namespace

struct HotkeyManager::Impl
{
   // Cross-thread protocol: callers post these messages to the pump thread,
   // which is the only thread that touches `bindings` and Win32's
   // RegisterHotKey/UnregisterHotKey. Synchronous: the caller blocks on the
   // future the request carries until the pump fulfills it.
   static constexpr UINT k_msg_bind     = WM_USER + 1;
   static constexpr UINT k_msg_unbind   = WM_USER + 2;
   static constexpr UINT k_msg_quit     = WM_USER + 3;

   struct BindRequest {
      std::string                       id;
      UINT                              mods;
      UINT                              vk;
      std::string                       accelerator;
      Handler                           handler;
      std::promise<Result<void>>*       promise;
   };

   struct UnbindRequest {
      std::string                       id;
      std::promise<void>*               promise;
   };

   std::thread                                pump;
   std::atomic<DWORD>                         pump_tid  { 0 };

   // Pump-thread-only state. No mutex: the pump is the sole reader/writer
   // for these fields. Callers go through k_msg_bind / k_msg_unbind.
   std::unordered_map<std::string, Binding>   bindings;
   int                                        next_id = 1;

   void apply_bind (BindRequest& req)
   {
      // Unregister any prior hotkey with the same id before allocating a new one.
      if (auto it = bindings.find (req.id); it != bindings.end ()) {
         ::UnregisterHotKey (nullptr, it->second.id);
         bindings.erase (it);
      }

      const int hkid = next_id++;

      if (!::RegisterHotKey (nullptr, hkid, req.mods, req.vk)) {
         const DWORD err = ::GetLastError ();
         req.promise->set_value (fail (Error::make (ErrorKind::Permission,
            "hotkey: RegisterHotKey failed for '{}' (err={})", req.accelerator, err)));
         return;
      }

      bindings [req.id] = Binding {
         .id          = hkid,
         .handler     = std::move (req.handler),
         .accelerator = std::move (req.accelerator),
      };

      Logger::info ("hotkey: bound {} → {}", req.id, bindings [req.id].accelerator);
      req.promise->set_value ({});
   }

   void apply_unbind (UnbindRequest& req)
   {
      if (auto it = bindings.find (req.id); it != bindings.end ()) {
         ::UnregisterHotKey (nullptr, it->second.id);
         bindings.erase (it);
         Logger::info ("hotkey: unbound {}", req.id);
      }
      req.promise->set_value ();
   }

   void on_hotkey (int hkid)
   {
      for (auto& [_, b] : bindings) {
         if (b.id == hkid) {
            if (b.handler) {
               try { b.handler (); }
               catch (...) { Logger::error ("hotkey: handler threw"); }
            }
            return;
         }
      }
   }

   void pump_run (std::condition_variable& ready, std::mutex& ready_mtx, bool& set)
   {
      MSG init;
      ::PeekMessage (&init, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      pump_tid.store (::GetCurrentThreadId ());

      { std::lock_guard lk { ready_mtx }; set = true; }
      ready.notify_one ();

      MSG msg;
      while (::GetMessage (&msg, nullptr, 0, 0) > 0) {
         if (msg.message == WM_HOTKEY) {
            on_hotkey (static_cast<int> (msg.wParam));
         } else if (msg.message == k_msg_bind) {
            auto* req = reinterpret_cast<BindRequest*> (msg.lParam);
            apply_bind (*req);
         } else if (msg.message == k_msg_unbind) {
            auto* req = reinterpret_cast<UnbindRequest*> (msg.lParam);
            apply_unbind (*req);
         } else if (msg.message == k_msg_quit) {
            break;
         } else {
            ::TranslateMessage (&msg);
            ::DispatchMessage  (&msg);
         }
      }

      // Drain bindings on the same thread that registered them.
      for (auto& [_, b] : bindings) {
         ::UnregisterHotKey (nullptr, b.id);
      }
      bindings.clear ();
   }
};

HotkeyManager::HotkeyManager (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}

HotkeyManager::~HotkeyManager ()
{
   if (!impl_) return;

   if (const DWORD tid = impl_->pump_tid.load (); tid != 0) {
      ::PostThreadMessage (tid, Impl::k_msg_quit, 0, 0);
   }

   if (impl_->pump.joinable ()) impl_->pump.join ();
}

Result<std::unique_ptr<HotkeyManager>> HotkeyManager::create ()
{
   auto impl = std::make_unique<Impl> ();

   std::condition_variable ready;
   std::mutex              ready_mtx;
   bool                    is_ready = false;

   impl->pump = std::thread { [p = impl.get (), &ready, &ready_mtx, &is_ready] {
      p->pump_run (ready, ready_mtx, is_ready);
   }};

   std::unique_lock lk { ready_mtx };
   ready.wait (lk, [&] { return is_ready; });

   return std::unique_ptr<HotkeyManager> { new HotkeyManager (std::move (impl)) };
}

Result<void> HotkeyManager::bind (std::string_view id, std::string_view accelerator, Handler handler)
{
   UINT mods, vk;
   if (!parse_accelerator (accelerator, mods, vk)) {
      return fail (Error::make (ErrorKind::InvalidArgument,
         "hotkey: cannot parse accelerator '{}'", accelerator));
   }

   const DWORD tid = impl_->pump_tid.load ();
   if (tid == 0) {
      return fail (Error::make (ErrorKind::Internal, "hotkey: pump thread not running"));
   }

   std::promise<Result<void>> promise;
   auto future = promise.get_future ();

   Impl::BindRequest req {
      .id          = std::string { id },
      .mods        = mods,
      .vk          = vk,
      .accelerator = std::string { accelerator },
      .handler     = std::move (handler),
      .promise     = &promise,
   };

   if (!::PostThreadMessage (tid, Impl::k_msg_bind, 0,
         reinterpret_cast<LPARAM> (&req))) {
      return fail (Error::make (ErrorKind::Internal,
         "hotkey: PostThreadMessage(bind) failed"));
   }

   return future.get ();
}

void HotkeyManager::unbind (std::string_view id)
{
   const DWORD tid = impl_->pump_tid.load ();
   if (tid == 0) return;

   std::promise<void> promise;
   auto future = promise.get_future ();

   Impl::UnbindRequest req {
      .id      = std::string { id },
      .promise = &promise,
   };

   if (!::PostThreadMessage (tid, Impl::k_msg_unbind, 0,
         reinterpret_cast<LPARAM> (&req))) {
      return;
   }

   future.get ();
}

} // namespace gv::core
