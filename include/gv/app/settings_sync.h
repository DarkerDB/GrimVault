#pragma once

#include <QObject>
#include <QString>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <string_view>

namespace gv::api  { class DDBClient; }
namespace gv::auth { class Session;        }
namespace gv::db   { class UserSettingsRepo; }

namespace gv::app {

// Key prefixes for the settings the dashboard owns. Both the sync poller
// (which unsets a key the dashboard dropped) and account scoping in main ()
// (which clears the previous account's values on a switch) key off this one
// list, so a new managed family is added in a single place.
inline constexpr std::string_view managed_setting_prefixes [] = {
   "behavior:", "collection:", "hotkeys:", "overlay:", "pricing:", "tooltip:"
};

inline bool is_managed_setting (std::string_view key)
{
   return std::any_of (
      std::begin (managed_setting_prefixes), std::end (managed_setting_prefixes),
      [key] (std::string_view prefix) { return key.starts_with (prefix); });
}

// Background poller that mirrors dashboard-controlled settings from
// /v2/grimvault/settings into the local UserSettingsRepo. The dashboard is
// the source of truth — local writes for these keys will be overwritten on
// the next poll.
//
// Driven by a single-shot QTimer on the Qt main thread; each tick spawns a
// short-lived worker thread to do the blocking HTTP call. On success the
// next tick fires after `interval`. On transient failure the next tick
// uses exponential backoff up to `backoff_cap`.
//
// Lifecycle: construct in the GUI process, call start () once the user is
// signed in, stop () before app exit. Safe to start/stop repeatedly across
// sign-in / sign-out transitions.
class SettingsSync : public QObject
{
   Q_OBJECT

public:
   struct Config {
      // Cadence between successful polls. This is the whole propagation
      // latency of a dashboard change: there is no push channel, so a
      // player who moves a slider waits at most this long to see it. The
      // request is a small conditional GET on a warm connection, so the
      // cost of a short interval is close to nothing; main () shortens it
      // further on dev, where the round trip is a local container.
      std::chrono::seconds interval { 30 };

      // Backoff floor (first failure) and cap (every failure after that
      // doubles up to the cap).
      std::chrono::seconds backoff_floor { 15 };
      std::chrono::seconds backoff_cap   { 300 };
   };

   SettingsSync (gv::api::DDBClient* api,
                 gv::auth::Session*       session,
                 gv::db::UserSettingsRepo* repo,
                 Config                   cfg    = {},
                 QObject*                 parent = nullptr);
   ~SettingsSync () override;

   // Idempotent. Kicks an immediate poll, then schedules the next.
   void start ();

   // Stops the timer + ignores any in-flight worker result.
   void stop  ();

   // Fire one cycle now, independent of the schedule. No-op when stopped.
   void poll_now ();

signals:
   void settings_changed (QString key, QString value);
   void poll_succeeded   (int num_changed);
   void poll_failed      (QString message);
   void authentication_required ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::app
