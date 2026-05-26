#include <gv/core/single_instance.h>
#include <gv/core/logger.h>

#include <Windows.h>

#include <string>

namespace gv::core {

namespace {

   std::wstring widen (std::string_view s)
   {
      if (s.empty ()) return {};
      const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.data (),
         static_cast<int> (s.size ()), nullptr, 0);
      std::wstring out (static_cast<std::size_t> (n), L'\0');
      ::MultiByteToWideChar (CP_UTF8, 0, s.data (),
         static_cast<int> (s.size ()), out.data (), n);
      return out;
   }

} // namespace

struct SingleInstanceGuard::Impl
{
   HANDLE mutex = nullptr;
};

SingleInstanceGuard::SingleInstanceGuard (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}

SingleInstanceGuard::SingleInstanceGuard (SingleInstanceGuard&&) noexcept            = default;
SingleInstanceGuard& SingleInstanceGuard::operator= (SingleInstanceGuard&&) noexcept = default;

SingleInstanceGuard::~SingleInstanceGuard ()
{
   if (impl_ && impl_->mutex) {
      ::ReleaseMutex (impl_->mutex);
      ::CloseHandle  (impl_->mutex);
   }
}

std::unique_ptr<SingleInstanceGuard> SingleInstanceGuard::acquire (std::string_view mutex_name)
{
   const auto wname = L"Global\\" + widen (mutex_name);

   HANDLE h = ::CreateMutexW (nullptr, TRUE, wname.c_str ());
   const DWORD err = ::GetLastError ();

   if (h && err == ERROR_ALREADY_EXISTS) {
      // Mutex existed; another instance has it. Close our handle and bail.
      ::CloseHandle (h);
      return nullptr;
   }

   if (!h) {
      Logger::error ("single_instance: CreateMutexW failed (err={})", err);
      return nullptr;
   }

   auto impl = std::make_unique<Impl> ();
   impl->mutex = h;
   return std::unique_ptr<SingleInstanceGuard> { new SingleInstanceGuard (std::move (impl)) };
}

unsigned int SingleInstanceGuard::window_message_id (std::string_view broadcast_message)
{
   const auto wmsg = widen (broadcast_message);
   return static_cast<unsigned int> (::RegisterWindowMessageW (wmsg.c_str ()));
}

void SingleInstanceGuard::notify_existing (std::string_view broadcast_message)
{
   const UINT msg = window_message_id (broadcast_message);
   if (msg == 0) return;

   // Broadcast to all top-level windows; the existing instance's listener
   // window will pick this up and bring itself to the foreground.
   ::PostMessageW (HWND_BROADCAST, msg, 0, 0);
}

} // namespace gv::core
