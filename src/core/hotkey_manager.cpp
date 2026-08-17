#include <gv/core/hotkey_manager.h>
#include <gv/core/logger.h>

#include <Windows.h>

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cctype>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace gv::core {

namespace {

   enum class MouseButton : UINT {
      none,
      left,
      right,
      middle,
      x1,
      x2,
   };

   struct Accelerator {
      UINT        modifiers = MOD_NOREPEAT;
      UINT        key       = 0;
      MouseButton mouse     = MouseButton::none;

      bool is_mouse () const { return mouse != MouseButton::none; }
      bool has_input () const { return key != 0 || is_mouse (); }
   };

   struct Binding {
      int                    id;
      Accelerator            input;
      HotkeyManager::Handler handler;
      std::string            accelerator;
   };

   UINT named_key (std::string_view name)
   {
      static constexpr std::pair<std::string_view, UINT> keys [] {
         { "esc", VK_ESCAPE },
         { "escape", VK_ESCAPE },
         { "space", VK_SPACE },
         { "tab", VK_TAB },
         { "return", VK_RETURN },
         { "enter", VK_RETURN },
         { "backspace", VK_BACK },
         { "delete", VK_DELETE },
         { "insert", VK_INSERT },
         { "home", VK_HOME },
         { "end", VK_END },
         { "pageup", VK_PRIOR },
         { "pagedown", VK_NEXT },
         { "up", VK_UP },
         { "down", VK_DOWN },
         { "left", VK_LEFT },
         { "right", VK_RIGHT },
         { "printscreen", VK_SNAPSHOT },
         { "pause", VK_PAUSE },
         { "capslock", VK_CAPITAL },
         { "numlock", VK_NUMLOCK },
         { "scrolllock", VK_SCROLL },
         { "menu", VK_APPS },
         { "backquote", VK_OEM_3 },
         { "minus", VK_OEM_MINUS },
         { "equal", VK_OEM_PLUS },
         { "leftbracket", VK_OEM_4 },
         { "rightbracket", VK_OEM_6 },
         { "backslash", VK_OEM_5 },
         { "semicolon", VK_OEM_1 },
         { "quote", VK_OEM_7 },
         { "comma", VK_OEM_COMMA },
         { "period", VK_OEM_PERIOD },
         { "slash", VK_OEM_2 },
         { "nummultiply", VK_MULTIPLY },
         { "numadd", VK_ADD },
         { "numsubtract", VK_SUBTRACT },
         { "numdecimal", VK_DECIMAL },
         { "numdivide", VK_DIVIDE },
         { "browserback", VK_BROWSER_BACK },
         { "browserforward", VK_BROWSER_FORWARD },
         { "browserrefresh", VK_BROWSER_REFRESH },
         { "browserstop", VK_BROWSER_STOP },
         { "browsersearch", VK_BROWSER_SEARCH },
         { "browserfavorites", VK_BROWSER_FAVORITES },
         { "browserhome", VK_BROWSER_HOME },
         { "medianext", VK_MEDIA_NEXT_TRACK },
         { "mediaprevious", VK_MEDIA_PREV_TRACK },
         { "mediastop", VK_MEDIA_STOP },
         { "mediaplaypause", VK_MEDIA_PLAY_PAUSE },
         { "volumemute", VK_VOLUME_MUTE },
         { "volumedown", VK_VOLUME_DOWN },
         { "volumeup", VK_VOLUME_UP },
         { "launchmail", VK_LAUNCH_MAIL },
         { "launchmedia", VK_LAUNCH_MEDIA_SELECT },
         { "launchapp1", VK_LAUNCH_APP1 },
         { "launchapp2", VK_LAUNCH_APP2 },
      };

      for (const auto& [key_name, key] : keys) {
         if (name == key_name) return key;
      }

      if (name.size () == 4 && name.starts_with ("num")
          && name [3] >= '0' && name [3] <= '9') {
         return VK_NUMPAD0 + name [3] - '0';
      }

      return 0;
   }

   MouseButton named_mouse (std::string_view name)
   {
      if (name == "mouse1") return MouseButton::left;
      if (name == "mouse2") return MouseButton::right;
      if (name == "mouse3") return MouseButton::middle;
      if (name == "mouse4") return MouseButton::x1;
      if (name == "mouse5") return MouseButton::x2;
      return MouseButton::none;
   }

   bool add_modifier (UINT& modifiers, UINT modifier)
   {
      if ((modifiers & modifier) != 0) return false;
      modifiers |= modifier;
      return true;
   }

   bool parse_accelerator (std::string_view value, Accelerator& accelerator)
   {
      accelerator = {};

      std::string lower { value };
      if (lower.empty () || lower.back () == '+') return false;
      for (auto& character : lower) {
         character = static_cast<char> (std::tolower (static_cast<unsigned char> (character)));
      }

      std::size_t offset = 0;
      while (offset < lower.size ()) {
         const auto next = lower.find ('+', offset);
         const auto part = std::string_view { lower }.substr (
            offset, next == std::string::npos ? std::string::npos : next - offset);
         offset = next == std::string::npos ? lower.size () : next + 1;

         if (part == "ctrl" || part == "control") {
            if (!add_modifier (accelerator.modifiers, MOD_CONTROL)) return false;
            continue;
         }
         if (part == "shift") {
            if (!add_modifier (accelerator.modifiers, MOD_SHIFT)) return false;
            continue;
         }
         if (part == "alt") {
            if (!add_modifier (accelerator.modifiers, MOD_ALT)) return false;
            continue;
         }
         if (part == "win" || part == "meta") {
            if (!add_modifier (accelerator.modifiers, MOD_WIN)) return false;
            continue;
         }
         if (accelerator.has_input ()) return false;

         if (const auto mouse = named_mouse (part); mouse != MouseButton::none) {
            accelerator.mouse = mouse;
            continue;
         }
         if (part.size () >= 2 && part [0] == 'f') {
            int number = 0;
            const auto first = part.data () + 1;
            const auto last = part.data () + part.size ();
            const auto [end, error] = std::from_chars (first, last, number);
            if (error != std::errc {} || end != last || number < 1 || number > 24) return false;
            accelerator.key = VK_F1 + number - 1;
            continue;
         }
         if (part.size () == 1
             && ((part [0] >= 'a' && part [0] <= 'z')
                 || (part [0] >= '0' && part [0] <= '9'))) {
            accelerator.key = static_cast<UINT> (
               std::toupper (static_cast<unsigned char> (part [0])));
            continue;
         }

         accelerator.key = named_key (part);
         if (accelerator.key == 0) return false;
      }

      return accelerator.has_input ();
   }

   UINT active_modifiers ()
   {
      UINT modifiers = MOD_NOREPEAT;
      if ((::GetAsyncKeyState (VK_CONTROL) & 0x8000) != 0) modifiers |= MOD_CONTROL;
      if ((::GetAsyncKeyState (VK_SHIFT) & 0x8000) != 0) modifiers |= MOD_SHIFT;
      if ((::GetAsyncKeyState (VK_MENU) & 0x8000) != 0) modifiers |= MOD_ALT;
      if ((::GetAsyncKeyState (VK_LWIN) & 0x8000) != 0
          || (::GetAsyncKeyState (VK_RWIN) & 0x8000) != 0) {
         modifiers |= MOD_WIN;
      }
      return modifiers;
   }

   MouseButton mouse_from_message (WPARAM message, LPARAM data)
   {
      if (message == WM_LBUTTONDOWN) return MouseButton::left;
      if (message == WM_RBUTTONDOWN) return MouseButton::right;
      if (message == WM_MBUTTONDOWN) return MouseButton::middle;
      if (message != WM_XBUTTONDOWN) return MouseButton::none;

      const auto mouse = reinterpret_cast<const MSLLHOOKSTRUCT*> (data);
      return HIWORD (mouse->mouseData) == XBUTTON1 ? MouseButton::x1 : MouseButton::x2;
   }

}

struct HotkeyManager::Impl
{
   static constexpr UINT k_msg_bind   = WM_USER + 1;
   static constexpr UINT k_msg_unbind = WM_USER + 2;
   static constexpr UINT k_msg_quit   = WM_USER + 3;
   static constexpr UINT k_msg_mouse  = WM_USER + 4;

   struct BindRequest {
      std::string                 id;
      Accelerator                input;
      std::string                 accelerator;
      Handler                     handler;
      std::promise<Result<void>>* promise;
   };

   struct UnbindRequest {
      std::string         id;
      std::promise<void>* promise;
   };

   inline static Impl* active_mouse_hook = nullptr;

   std::thread                              pump;
   std::atomic<DWORD>                       pump_tid { 0 };
   std::unordered_map<std::string, Binding> bindings;
   int                                      next_id = 1;
   HHOOK                                    mouse_hook = nullptr;

   static LRESULT CALLBACK mouse_hook_proc (int code, WPARAM message, LPARAM data)
   {
      if (code == HC_ACTION && active_mouse_hook) {
         const auto mouse = mouse_from_message (message, data);
         if (mouse != MouseButton::none) {
            ::PostThreadMessage (
               active_mouse_hook->pump_tid.load (),
               k_msg_mouse,
               static_cast<WPARAM> (mouse),
               static_cast<LPARAM> (active_modifiers ()));
         }
      }
      return ::CallNextHookEx (nullptr, code, message, data);
   }

   Result<void> ensure_mouse_hook (std::string_view accelerator)
   {
      if (mouse_hook) return {};
      if (active_mouse_hook && active_mouse_hook != this) {
         return fail (Error::make (ErrorKind::Internal, "hotkey: mouse hook already active"));
      }

      active_mouse_hook = this;
      mouse_hook = ::SetWindowsHookExW (
         WH_MOUSE_LL, mouse_hook_proc, ::GetModuleHandleW (nullptr), 0);
      if (mouse_hook) return {};

      active_mouse_hook = nullptr;
      return fail (Error::make (
         ErrorKind::Permission,
         "hotkey: mouse hook failed for '{}' (err={})",
         accelerator,
         ::GetLastError ()));
   }

   void release_mouse_hook ()
   {
      if (!mouse_hook) return;
      ::UnhookWindowsHookEx (mouse_hook);
      mouse_hook = nullptr;
      if (active_mouse_hook == this) active_mouse_hook = nullptr;
   }

   void release_unused_mouse_hook ()
   {
      for (const auto& [_, binding] : bindings) {
         if (binding.input.is_mouse ()) return;
      }
      release_mouse_hook ();
   }

   bool mouse_conflicts (std::string_view id, const Accelerator& input) const
   {
      for (const auto& [binding_id, binding] : bindings) {
         if (binding_id != id
             && binding.input.mouse == input.mouse
             && binding.input.modifiers == input.modifiers) {
            return true;
         }
      }
      return false;
   }

   void apply_bind (BindRequest& request)
   {
      auto previous = bindings.find (request.id);
      if (previous != bindings.end ()
          && previous->second.input.key == request.input.key
          && previous->second.input.mouse == request.input.mouse
          && previous->second.input.modifiers == request.input.modifiers) {
         previous->second.handler = std::move (request.handler);
         previous->second.accelerator = std::move (request.accelerator);
         request.promise->set_value ({});
         return;
      }

      int hotkey_id = 0;
      if (request.input.is_mouse ()) {
         if (mouse_conflicts (request.id, request.input)) {
            request.promise->set_value (fail (Error::make (
               ErrorKind::Permission,
               "hotkey: '{}' is already registered",
               request.accelerator)));
            return;
         }

         auto hooked = ensure_mouse_hook (request.accelerator);
         if (!hooked) {
            request.promise->set_value (fail (hooked.error ()));
            return;
         }
      } else {
         hotkey_id = next_id++;
         if (!::RegisterHotKey (
               nullptr, hotkey_id, request.input.modifiers, request.input.key)) {
            request.promise->set_value (fail (Error::make (
               ErrorKind::Permission,
               "hotkey: RegisterHotKey failed for '{}' (err={})",
               request.accelerator,
               ::GetLastError ())));
            return;
         }
      }

      if (previous != bindings.end () && previous->second.id != 0) {
         ::UnregisterHotKey (nullptr, previous->second.id);
      }

      bindings [request.id] = Binding {
         .id          = hotkey_id,
         .input       = request.input,
         .handler     = std::move (request.handler),
         .accelerator = std::move (request.accelerator),
      };

      release_unused_mouse_hook ();
      Logger::info ("hotkey: bound {} to {}", request.id, bindings [request.id].accelerator);
      request.promise->set_value ({});
   }

   void apply_unbind (UnbindRequest& request)
   {
      if (auto binding = bindings.find (request.id); binding != bindings.end ()) {
         if (binding->second.id != 0) ::UnregisterHotKey (nullptr, binding->second.id);
         bindings.erase (binding);
         release_unused_mouse_hook ();
         Logger::info ("hotkey: unbound {}", request.id);
      }
      request.promise->set_value ();
   }

   void invoke (Binding& binding)
   {
      if (!binding.handler) return;
      try {
         binding.handler ();
      } catch (...) {
         Logger::error ("hotkey: handler threw");
      }
   }

   void on_hotkey (int hotkey_id)
   {
      for (auto& [_, binding] : bindings) {
         if (binding.id == hotkey_id) {
            invoke (binding);
            return;
         }
      }
   }

   void on_mouse (MouseButton mouse, UINT modifiers)
   {
      for (auto& [_, binding] : bindings) {
         if (binding.input.mouse == mouse && binding.input.modifiers == modifiers) {
            invoke (binding);
            return;
         }
      }
   }

   void pump_run (std::condition_variable& ready, std::mutex& ready_mutex, bool& ready_state)
   {
      MSG init;
      ::PeekMessage (&init, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      pump_tid.store (::GetCurrentThreadId ());

      {
         std::lock_guard lock { ready_mutex };
         ready_state = true;
      }
      ready.notify_one ();

      MSG message;
      while (::GetMessage (&message, nullptr, 0, 0) > 0) {
         if (message.message == WM_HOTKEY) {
            on_hotkey (static_cast<int> (message.wParam));
         } else if (message.message == k_msg_bind) {
            apply_bind (*reinterpret_cast<BindRequest*> (message.lParam));
         } else if (message.message == k_msg_unbind) {
            apply_unbind (*reinterpret_cast<UnbindRequest*> (message.lParam));
         } else if (message.message == k_msg_mouse) {
            on_mouse (
               static_cast<MouseButton> (message.wParam),
               static_cast<UINT> (message.lParam));
         } else if (message.message == k_msg_quit) {
            break;
         } else {
            ::TranslateMessage (&message);
            ::DispatchMessage (&message);
         }
      }

      for (auto& [_, binding] : bindings) {
         if (binding.id != 0) ::UnregisterHotKey (nullptr, binding.id);
      }
      bindings.clear ();
      release_mouse_hook ();
   }
};

HotkeyManager::HotkeyManager (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}

HotkeyManager::~HotkeyManager ()
{
   if (!impl_) return;

   if (const DWORD thread_id = impl_->pump_tid.load (); thread_id != 0) {
      ::PostThreadMessage (thread_id, Impl::k_msg_quit, 0, 0);
   }

   if (impl_->pump.joinable ()) impl_->pump.join ();
}

bool HotkeyManager::supports (std::string_view accelerator)
{
   Accelerator parsed;
   return parse_accelerator (accelerator, parsed);
}

Result<std::unique_ptr<HotkeyManager>> HotkeyManager::create ()
{
   auto impl = std::make_unique<Impl> ();

   std::condition_variable ready;
   std::mutex ready_mutex;
   bool ready_state = false;

   impl->pump = std::thread { [manager = impl.get (), &ready, &ready_mutex, &ready_state] {
      manager->pump_run (ready, ready_mutex, ready_state);
   }};

   std::unique_lock lock { ready_mutex };
   ready.wait (lock, [&] { return ready_state; });

   return std::unique_ptr<HotkeyManager> { new HotkeyManager (std::move (impl)) };
}

Result<void> HotkeyManager::bind (
   std::string_view id,
   std::string_view accelerator,
   Handler handler)
{
   Accelerator parsed;
   if (!parse_accelerator (accelerator, parsed)) {
      return fail (Error::make (
         ErrorKind::InvalidArgument,
         "hotkey: cannot parse accelerator '{}'",
         accelerator));
   }

   const DWORD thread_id = impl_->pump_tid.load ();
   if (thread_id == 0) {
      return fail (Error::make (ErrorKind::Internal, "hotkey: pump thread not running"));
   }

   std::promise<Result<void>> promise;
   auto future = promise.get_future ();

   Impl::BindRequest request {
      .id          = std::string { id },
      .input       = parsed,
      .accelerator = std::string { accelerator },
      .handler     = std::move (handler),
      .promise     = &promise,
   };

   if (!::PostThreadMessage (
         thread_id,
         Impl::k_msg_bind,
         0,
         reinterpret_cast<LPARAM> (&request))) {
      return fail (Error::make (ErrorKind::Internal, "hotkey: PostThreadMessage(bind) failed"));
   }

   return future.get ();
}

void HotkeyManager::unbind (std::string_view id)
{
   const DWORD thread_id = impl_->pump_tid.load ();
   if (thread_id == 0) return;

   std::promise<void> promise;
   auto future = promise.get_future ();

   Impl::UnbindRequest request {
      .id      = std::string { id },
      .promise = &promise,
   };

   if (!::PostThreadMessage (
         thread_id,
         Impl::k_msg_unbind,
         0,
         reinterpret_cast<LPARAM> (&request))) {
      return;
   }

   future.get ();
}

}
