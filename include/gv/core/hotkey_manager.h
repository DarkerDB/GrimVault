#pragma once

#include <gv/core/result.h>

#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace gv::core {

// Global-hotkey manager. Wraps Win32 RegisterHotKey on a dedicated message-pump
// thread so hotkey events fire even when the app isn't focused.
//
// Accelerators use Qt-style strings: "F5", "Ctrl+Shift+P", "Alt+F6".
//
// Bind from settings:
//    auto hk = HotkeyManager::create ();
//    for (const auto& [action, accel] : hotkeys_repo.all ().value ()) {
//       hk->bind (action, accel, [action] { ... });
//    }
class HotkeyManager
{
public:
   using Handler = std::function<void()>;

   ~HotkeyManager ();

   HotkeyManager (const HotkeyManager&)            = delete;
   HotkeyManager& operator= (const HotkeyManager&) = delete;

   static Result<std::unique_ptr<HotkeyManager>> create ();

   // Register an accelerator. Replaces any prior binding with the same id.
   // Re-binding while the hotkey is held has unspecified ordering with the
   // in-flight handler.
   Result<void> bind   (std::string_view id, std::string_view accelerator, Handler handler);
   void         unbind (std::string_view id);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;

   explicit HotkeyManager (std::unique_ptr<Impl> impl);
};

} // namespace gv::core
