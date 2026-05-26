#pragma once

#include <memory>
#include <string_view>

namespace gv::core {

// RAII single-instance guard backed by a named Win32 mutex. Construct once
// at app start; on destruction the mutex is released.
//
// Usage:
//    auto guard = SingleInstanceGuard::acquire ("GrimVault");
//    if (!guard) {
//       // another instance already owns the mutex — bring it forward and exit
//    }
class SingleInstanceGuard
{
public:
   ~SingleInstanceGuard ();

   SingleInstanceGuard (const SingleInstanceGuard&)            = delete;
   SingleInstanceGuard& operator= (const SingleInstanceGuard&) = delete;
   SingleInstanceGuard (SingleInstanceGuard&&)                 noexcept;
   SingleInstanceGuard& operator= (SingleInstanceGuard&&)      noexcept;

   // Try to acquire the mutex named `mutex_name`. Returns the guard on first
   // acquisition; returns nullptr if another instance already holds it.
   static std::unique_ptr<SingleInstanceGuard> acquire (std::string_view mutex_name);

   // Best-effort: broadcast a custom registered window message so a running
   // instance (which subscribed via window_message_id ()) can pop its window.
   static void notify_existing (std::string_view broadcast_message);

   // The message id this guard listens for on the foreground/hidden window.
   // Both producer and consumer must call this with the same key.
   static unsigned int window_message_id (std::string_view broadcast_message);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;

   explicit SingleInstanceGuard (std::unique_ptr<Impl> impl);
};

} // namespace gv::core
