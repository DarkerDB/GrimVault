#include <gv/update/update_service.h>
#include <gv/core/logger.h>
#include <gv/update/appcast_url.h>
#include <gv/core/version.h>

#include <winsparkle/winsparkle.h>

#include <atomic>
#include <cstring>

namespace gv::update {

namespace {

   UpdateService*           g_instance = nullptr;
   std::atomic<bool>        g_started  { false };

   void on_found      () { if (g_instance) emit g_instance->update_available (); }
   void on_dismiss    () { if (g_instance) emit g_instance->update_dismissed (); }
   void on_error      () { if (g_instance) emit g_instance->update_error     (); }

} // namespace

UpdateService::UpdateService (QObject* parent) : QObject (parent)
{
   g_instance = this;

   win_sparkle_set_appcast_url    (gv::update::appcast_url);
   win_sparkle_set_app_details    (
      L"DarkerDB", L"GrimVault",
      reinterpret_cast<const wchar_t*> (
         QString::fromLatin1 (gv::core::version::string).utf16 ())
   );

   win_sparkle_set_did_find_update_callback     (&on_found);
   win_sparkle_set_did_not_find_update_callback (&on_dismiss);
   win_sparkle_set_update_cancelled_callback    (&on_dismiss);
   win_sparkle_set_error_callback               (&on_error);
}

UpdateService::~UpdateService ()
{
   stop ();
   g_instance = nullptr;
}

void UpdateService::start ()
{
   if (g_started.exchange (true)) return;

   if (appcast_url == nullptr || std::strlen (appcast_url) == 0) {
      core::Logger::info (
         "update: no appcast URL baked in (dev build?) — skipping auto-update init"
      );
      g_started.store (false);
      return;
   }

   if (appcast_pubkey == nullptr || std::strlen (appcast_pubkey) == 0) {
      core::Logger::error (
         "update: ed25519 public key not embedded — REFUSING to enable auto-updates. "
         "Rebuild with -DGRIMVAULT_APPCAST_PUBKEY=<base64-32-byte-key> to ship updates."
      );
      g_started.store (false);
      return;
   }

   win_sparkle_set_eddsa_public_key (appcast_pubkey);
   win_sparkle_init ();
   core::Logger::info (
      "update: WinSparkle initialized (appcast={}, signature verification ON)",
      gv::update::appcast_url
   );
}

void UpdateService::stop () noexcept
{
   if (!g_started.exchange (false)) return;
   win_sparkle_cleanup ();
}

void UpdateService::check_now_with_ui () { win_sparkle_check_update_with_ui ();    }
void UpdateService::check_now_silent  () { win_sparkle_check_update_without_ui (); }

void UpdateService::set_check_interval_seconds (int seconds)
{
   win_sparkle_set_update_check_interval (seconds);
}

} // namespace gv::update
