#include <gv/update/update_service.h>
#include <gv/core/logger.h>
#include <gv/update/appcast_url.h>
#include <gv/core/version.h>

#include <winsparkle/winsparkle.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace gv::update {

namespace {

   UpdateService* g_instance = nullptr;

   void on_found      () { if (g_instance) emit g_instance->update_available (); }
   void on_dismiss    () { if (g_instance) emit g_instance->update_dismissed (); }
   void on_error      () { if (g_instance) emit g_instance->update_error     (); }
   void on_shutdown   () { if (g_instance) emit g_instance->shutdown_requested (); }

} // namespace

UpdateService::UpdateService (QObject* parent) : QObject (parent)
{
   g_instance = this;

   check_timer_.setInterval (60 * 60 * 1000);
   connect (&check_timer_, &QTimer::timeout, this, &UpdateService::check_now_silent);

   win_sparkle_set_appcast_url    (gv::update::appcast_url);
   win_sparkle_set_app_details    (
      L"DDB", L"GrimVault",
      reinterpret_cast<const wchar_t*> (
         QString::fromLatin1 (gv::core::version::string).utf16 ())
   );

   win_sparkle_set_did_find_update_callback     (&on_found);
   win_sparkle_set_did_not_find_update_callback (&on_dismiss);
   win_sparkle_set_update_cancelled_callback    (&on_dismiss);
   win_sparkle_set_error_callback               (&on_error);

   // Without this, WinSparkle launches the installer over a still-running
   // tray app and the extract fails on the loaded Qt DLLs.
   win_sparkle_set_shutdown_request_callback    (&on_shutdown);
}

UpdateService::~UpdateService ()
{
   stop ();
   g_instance = nullptr;
}

void UpdateService::start ()
{
   if (initialized_) return;

   if (appcast_url == nullptr || std::strlen (appcast_url) == 0) {
      core::Logger::info (
         "update: no appcast URL baked in (dev build?) — skipping updater init"
      );
      return;
   }

   if (appcast_pubkey == nullptr || std::strlen (appcast_pubkey) == 0) {
      core::Logger::error (
         "update: ed25519 public key not embedded — REFUSING to enable auto-updates. "
         "Rebuild with -DGRIMVAULT_APPCAST_PUBKEY=<base64-32-byte-key> to ship updates."
      );
      return;
   }

   win_sparkle_set_eddsa_public_key (appcast_pubkey);

   // Dashboard settings are the consent surface. Keep WinSparkle's internal
   // scheduler off so it neither asks again nor competes with check_timer_.
   win_sparkle_set_automatic_check_for_updates (0);
   win_sparkle_init ();
   initialized_ = true;
   core::Logger::info (
      "update: WinSparkle initialized (appcast={}, signature verification ON, "
      "dashboard-controlled scheduling ON)",
      gv::update::appcast_url
   );
}

void UpdateService::stop () noexcept
{
   check_timer_.stop ();
   if (!initialized_) return;
   win_sparkle_cleanup ();
   initialized_ = false;
}

void UpdateService::check_now_with_ui ()
{
   start ();
   if (initialized_) win_sparkle_check_update_with_ui ();
}

void UpdateService::check_now_silent ()
{
   start ();
   if (initialized_) win_sparkle_check_update_without_ui ();
}

void UpdateService::set_check_interval_seconds (int seconds)
{
   constexpr int min_seconds = 60 * 60;
   constexpr int max_seconds = std::numeric_limits<int>::max () / 1000;
   check_timer_.setInterval (std::clamp (seconds, min_seconds, max_seconds) * 1000);
}

void UpdateService::set_automatic_checks_enabled (bool enabled)
{
   if (!enabled) {
      check_timer_.stop ();
      return;
   }

   start ();
   if (!initialized_ || check_timer_.isActive ()) return;

   check_timer_.start ();

   // Match WinSparkle's normal startup behavior without racing a setting
   // change made before the event loop begins.
   QTimer::singleShot (0, this, [this] {
      if (check_timer_.isActive ()) check_now_silent ();
   });
}

} // namespace gv::update
