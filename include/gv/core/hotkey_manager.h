#pragma once

#include <gv/core/result.h>

#include <functional>
#include <memory>
#include <string_view>

namespace gv::core {

class HotkeyManager
{
public:
   using Handler = std::function<void()>;

   ~HotkeyManager ();

   HotkeyManager (const HotkeyManager&)            = delete;
   HotkeyManager& operator= (const HotkeyManager&) = delete;

   static bool supports (std::string_view accelerator);
   static Result<std::unique_ptr<HotkeyManager>> create ();

   Result<void> bind   (std::string_view id, std::string_view accelerator, Handler handler);
   void         unbind (std::string_view id);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;

   explicit HotkeyManager (std::unique_ptr<Impl> impl);
};

}
